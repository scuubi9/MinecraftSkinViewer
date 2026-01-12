// ==============================
// File: src/main.cpp
// ==============================
#include <windows.h>
#include <shellapi.h>
#include <wincodec.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>

#include <wrl/client.h>
#include "DirectXMath.h"

#include <cstdint>
#include <vector>
#include <string>
#include <string_view>
#include <filesystem>
#include <optional>
#include <algorithm>
#include <stdexcept>
#include <cmath>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include <cstring>
#include <cstddef>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

// ------------------------------
// Small helpers
// ------------------------------
static void ThrowIfFailed(HRESULT hr, const char* msg) {
  if (FAILED(hr)) {
    throw std::runtime_error(std::string(msg) + " (hr=0x" + std::to_string((uint32_t)hr) + ")");
  }
}

static std::string NarrowFromWide(std::wstring_view w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string out(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), out.data(), n, nullptr, nullptr);
  return out;
}

static std::wstring WideFromUtf8(std::string_view s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring out(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
  return out;
}

static float Clamp(float v, float lo, float hi) { return std::max(lo, std::min(hi, v)); }

// ------------------------------
// D3D state
// ------------------------------
struct D3DState {
  HWND hwnd = nullptr;

  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> ctx;
  ComPtr<IDXGISwapChain> swap;
  ComPtr<ID3D11RenderTargetView> rtv;
  ComPtr<ID3D11Texture2D> depth;
  ComPtr<ID3D11DepthStencilView> dsv;

  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps;
  ComPtr<ID3D11InputLayout> il;
  ComPtr<ID3D11Buffer> vb;
  ComPtr<ID3D11Buffer> ib;
  ComPtr<ID3D11Buffer> cb;
  ComPtr<ID3D11SamplerState> samp;
  ComPtr<ID3D11RasterizerState> rs;
  ComPtr<ID3D11DepthStencilState> dsDefault;
  ComPtr<ID3D11BlendState> blendAlpha;

  UINT ibCountBase = 0;
  UINT ibCountOverlay = 0;
  UINT ibOffsetOverlay = 0;

  int fbW = 1280;
  int fbH = 720;
};

// ------------------------------
// Skin + analysis
// ------------------------------
struct SkinInfo {
  std::wstring path;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t scale = 1;   // 1 for 64x*, 2 for 128x128, etc.
  bool legacy64x32 = false;
  bool hasAlpha = true;

  std::vector<uint8_t> rgba; // width * height * 4 (RGBA)
  ComPtr<ID3D11ShaderResourceView> srv;
};

struct UvRectPx {
  int x = 0, y = 0, w = 0, h = 0; // pixel-space
};

struct BoxUv {
  UvRectPx top, bottom, right, front, left, back;
};

static bool AnyNonTransparent(const SkinInfo& s, const UvRectPx& r) {
  if (s.rgba.empty() || s.width == 0 || s.height == 0) return false;
  int x0 = std::max(0, r.x), y0 = std::max(0, r.y);
  int x1 = std::min((int)s.width, r.x + r.w);
  int y1 = std::min((int)s.height, r.y + r.h);
  if (x1 <= x0 || y1 <= y0) return false;

  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      const size_t idx = (size_t)(y * s.width + x) * 4;
      if (idx + 3 < s.rgba.size() && s.rgba[idx + 3] != 0) return true;
    }
  }
  return false;
}

static UvRectPx ScaleRect(const UvRectPx& r, uint32_t scale) {
  return UvRectPx{ (int)(r.x * (int)scale), (int)(r.y * (int)scale), (int)(r.w * (int)scale), (int)(r.h * (int)scale) };
}

static BoxUv ScaleBoxUv(const BoxUv& b, uint32_t scale) {
  return BoxUv{
    ScaleRect(b.top, scale),
    ScaleRect(b.bottom, scale),
    ScaleRect(b.right, scale),
    ScaleRect(b.front, scale),
    ScaleRect(b.left, scale),
    ScaleRect(b.back, scale),
  };
}

// Base layer coordinates (64x64 reference).
static BoxUv UV_Head() {
  return BoxUv{
    /*top*/    { 8,  0, 8, 8},
    /*bottom*/ {16,  0, 8, 8},
    /*right*/  { 0,  8, 8, 8},
    /*front*/  { 8,  8, 8, 8},
    /*left*/   {16,  8, 8, 8},
    /*back*/   {24,  8, 8, 8},
  };
}
static BoxUv UV_Hat() {
  return BoxUv{
    {40,  0, 8, 8},
    {48,  0, 8, 8},
    {32,  8, 8, 8},
    {40,  8, 8, 8},
    {48,  8, 8, 8},
    {56,  8, 8, 8},
  };
}
static BoxUv UV_Torso() {
  return BoxUv{
    {20, 16, 8, 4},   // top
    {28, 16, 8, 4},   // bottom
    {16, 20, 4, 12},  // right
    {20, 20, 8, 12},  // front
    {28, 20, 4, 12},  // left
    {32, 20, 8, 12},  // back
  };
}
static BoxUv UV_Jacket() {
  return BoxUv{
    {20, 32, 8, 4},
    {28, 32, 8, 4},
    {16, 36, 4, 12},
    {20, 36, 8, 12},
    {28, 36, 4, 12},
    {32, 36, 8, 12},
  };
}
static BoxUv UV_RightLeg() {
  return BoxUv{
    { 4, 16, 4, 4},
    { 8, 16, 4, 4},
    { 0, 20, 4, 12},
    { 4, 20, 4, 12},
    { 8, 20, 4, 12},
    {12, 20, 4, 12},
  };
}
static BoxUv UV_RightLegPants() {
  return BoxUv{
    { 4, 32, 4, 4},
    { 8, 32, 4, 4},
    { 0, 36, 4, 12},
    { 4, 36, 4, 12},
    { 8, 36, 4, 12},
    {12, 36, 4, 12},
  };
}
static BoxUv UV_RightArm() {
  return BoxUv{
    {44, 16, 4, 4},
    {48, 16, 4, 4},
    {40, 20, 4, 12},
    {44, 20, 4, 12},
    {48, 20, 4, 12},
    {52, 20, 4, 12},
  };
}
static BoxUv UV_RightSleeve() {
  return BoxUv{
    {44, 32, 4, 4},
    {48, 32, 4, 4},
    {40, 36, 4, 12},
    {44, 36, 4, 12},
    {48, 36, 4, 12},
    {52, 36, 4, 12},
  };
}
static BoxUv UV_LeftLeg() {
  return BoxUv{
    {20, 48, 4, 4},
    {24, 48, 4, 4},
    {16, 52, 4, 12},
    {20, 52, 4, 12},
    {24, 52, 4, 12},
    {28, 52, 4, 12},
  };
}
static BoxUv UV_LeftLegPants() {
  return BoxUv{
    { 4, 48, 4, 4},
    { 8, 48, 4, 4},
    { 0, 52, 4, 12},
    { 4, 52, 4, 12},
    { 8, 52, 4, 12},
    {12, 52, 4, 12},
  };
}
static BoxUv UV_LeftArm() {
  return BoxUv{
    {36, 48, 4, 4},
    {40, 48, 4, 4},
    {32, 52, 4, 12},
    {36, 52, 4, 12},
    {40, 52, 4, 12},
    {44, 52, 4, 12},
  };
}
static BoxUv UV_LeftSleeve() {
  return BoxUv{
    {52, 48, 4, 4},
    {56, 48, 4, 4},
    {48, 52, 4, 12},
    {52, 52, 4, 12},
    {56, 52, 4, 12},
    {60, 52, 4, 12},
  };
}

// ---- Slim (Alex) arm UVs (64x64+) ----
static BoxUv UV_RightArmSlim() {
  return BoxUv{
    /*top*/    {44, 16, 3, 4},
    /*bottom*/ {47, 16, 3, 4},
    /*right*/  {40, 20, 4, 12},
    /*front*/  {44, 20, 3, 12},
    /*left*/   {47, 20, 4, 12},
    /*back*/   {51, 20, 3, 12},
  };
}
static BoxUv UV_RightSleeveSlim() {
  return BoxUv{
    /*top*/    {44, 32, 3, 4},
    /*bottom*/ {47, 32, 3, 4},
    /*right*/  {40, 36, 4, 12},
    /*front*/  {44, 36, 3, 12},
    /*left*/   {47, 36, 4, 12},
    /*back*/   {51, 36, 3, 12},
  };
}
static BoxUv UV_LeftArmSlim() {
  return BoxUv{
    /*top*/    {36, 48, 3, 4},
    /*bottom*/ {39, 48, 3, 4},
    /*right*/  {32, 52, 4, 12},
    /*front*/  {36, 52, 3, 12},
    /*left*/   {39, 52, 4, 12},
    /*back*/   {43, 52, 3, 12},
  };
}
static BoxUv UV_LeftSleeveSlim() {
  return BoxUv{
    /*top*/    {52, 48, 3, 4},
    /*bottom*/ {55, 48, 3, 4},
    /*right*/  {48, 52, 4, 12},
    /*front*/  {52, 52, 3, 12},
    /*left*/   {55, 52, 4, 12},
    /*back*/   {59, 52, 3, 12},
  };
}

// ------------------------------
// Geometry
// ------------------------------
struct Vertex {
  XMFLOAT3 pos;
  XMFLOAT3 nrm;
  XMFLOAT2 uv;
};

static XMFLOAT2 UVFromPx(int px, int py, uint32_t texW, uint32_t texH) {
  return XMFLOAT2((float)px / (float)texW, (float)py / (float)texH);
}

static void AddFace(
  std::vector<Vertex>& v,
  std::vector<uint32_t>& i,
  const XMFLOAT3& p0, const XMFLOAT3& p1, const XMFLOAT3& p2, const XMFLOAT3& p3,
  const XMFLOAT3& n,
  const UvRectPx& r,
  uint32_t texW, uint32_t texH
) {
  XMFLOAT2 uv0 = UVFromPx(r.x,       r.y,       texW, texH);
  XMFLOAT2 uv1 = UVFromPx(r.x + r.w, r.y,       texW, texH);
  XMFLOAT2 uv2 = UVFromPx(r.x + r.w, r.y + r.h, texW, texH);
  XMFLOAT2 uv3 = UVFromPx(r.x,       r.y + r.h, texW, texH);

  uint32_t base = (uint32_t)v.size();
  v.push_back(Vertex{p0, n, uv0});
  v.push_back(Vertex{p1, n, uv1});
  v.push_back(Vertex{p2, n, uv2});
  v.push_back(Vertex{p3, n, uv3});

  i.push_back(base + 0);
  i.push_back(base + 1);
  i.push_back(base + 2);

  i.push_back(base + 0);
  i.push_back(base + 2);
  i.push_back(base + 3);
}

static void AddBox(
  std::vector<Vertex>& v,
  std::vector<uint32_t>& i,
  XMFLOAT3 center,
  XMFLOAT3 size,
  const BoxUv& uv,
  uint32_t texW, uint32_t texH
) {
  const float hx = size.x * 0.5f;
  const float hy = size.y * 0.5f;
  const float hz = size.z * 0.5f;

  const float cx = center.x, cy = center.y, cz = center.z;

  XMFLOAT3 LBF{cx - hx, cy - hy, cz - hz};
  XMFLOAT3 RBF{cx + hx, cy - hy, cz - hz};
  XMFLOAT3 RTF{cx + hx, cy + hy, cz - hz};
  XMFLOAT3 LTF{cx - hx, cy + hy, cz - hz};

  XMFLOAT3 LBB{cx - hx, cy - hy, cz + hz};
  XMFLOAT3 RBB{cx + hx, cy - hy, cz + hz};
  XMFLOAT3 RTB{cx + hx, cy + hy, cz + hz};
  XMFLOAT3 LTB{cx - hx, cy + hy, cz + hz};

  // Top (+Y)
  AddFace(v, i, LTF, RTF, RTB, LTB, XMFLOAT3(0, 1, 0), uv.top, texW, texH);

  // Bottom (-Y)
  AddFace(v, i, LBB, RBB, RBF, LBF, XMFLOAT3(0, -1, 0), uv.bottom, texW, texH);

  // Front (+Z) (player front)
  AddFace(v, i, LTB, RTB, RBB, LBB, XMFLOAT3(0, 0, 1), uv.front, texW, texH);

  // Back (-Z) (player back)
  AddFace(v, i, RTF, LTF, LBF, RBF, XMFLOAT3(0, 0, -1), uv.back, texW, texH);

  // Right (+X)
  AddFace(v, i, RTB, RTF, RBF, RBB, XMFLOAT3(1, 0, 0), uv.right, texW, texH);

  // Left (-X)
  AddFace(v, i, LTF, LTB, LBB, LBF, XMFLOAT3(-1, 0, 0), uv.left, texW, texH);
}

struct BuiltMesh {
  std::vector<Vertex> vertices;
  std::vector<uint32_t> indicesBase;
  std::vector<uint32_t> indicesOverlay;
};

static BuiltMesh BuildPlayerMesh(const SkinInfo& skin, bool slimArms) {
  BuiltMesh m;
  if (!skin.width || !skin.height) return m;

  const uint32_t texW = skin.width;
  const uint32_t texH = skin.height;
  const uint32_t s = skin.scale;

  auto S = [&](const BoxUv& b) { return ScaleBoxUv(b, s); };
  const bool has64 = (!skin.legacy64x32 && skin.height >= 64 * s);

  // Pixel units
  const XMFLOAT3 headSize{8, 8, 8};
  const XMFLOAT3 bodySize{8, 12, 4};
  const XMFLOAT3 legSize{4, 12, 4};

  const float armW = (has64 && slimArms) ? 3.0f : 4.0f;
  const XMFLOAT3 armSize{armW, 12, 4};

  const XMFLOAT3 headC{0, 12 + 12 + 4, 0}; // 28
  const XMFLOAT3 bodyC{0, 12 + 6, 0};      // 18

  const float armX = 4.0f + armW * 0.5f;   // body half-width + arm half-width
  const XMFLOAT3 rArmC{-armX, 12 + 6, 0};
  const XMFLOAT3 lArmC{ armX, 12 + 6, 0};

  const XMFLOAT3 rLegC{-2, 6, 0};
  const XMFLOAT3 lLegC{ 2, 6, 0};

  // Base geometry
  AddBox(m.vertices, m.indicesBase, headC, headSize, S(UV_Head()), texW, texH);
  AddBox(m.vertices, m.indicesBase, bodyC, bodySize, S(UV_Torso()), texW, texH);

  if (armW == 3.0f) AddBox(m.vertices, m.indicesBase, rArmC, armSize, S(UV_RightArmSlim()), texW, texH);
  else             AddBox(m.vertices, m.indicesBase, rArmC, armSize, S(UV_RightArm()),     texW, texH);

  AddBox(m.vertices, m.indicesBase, rLegC, legSize, S(UV_RightLeg()), texW, texH);

  if (has64) {
    if (armW == 3.0f) AddBox(m.vertices, m.indicesBase, lArmC, armSize, S(UV_LeftArmSlim()), texW, texH);
    else             AddBox(m.vertices, m.indicesBase, lArmC, armSize, S(UV_LeftArm()),     texW, texH);

    AddBox(m.vertices, m.indicesBase, lLegC, legSize, S(UV_LeftLeg()), texW, texH);
  } else {
    // legacy 64x32: left limbs mirror right (approx)
    AddBox(m.vertices, m.indicesBase, lArmC, armSize, S(UV_RightArm()), texW, texH);
    AddBox(m.vertices, m.indicesBase, lLegC, legSize, S(UV_RightLeg()), texW, texH);
  }

  // Overlay: only add if any non-transparent pixels exist
  auto Inflate = [](XMFLOAT3 size, float delta) { return XMFLOAT3(size.x + delta, size.y + delta, size.z + delta); };

  // Hat
  {
    BoxUv hat = S(UV_Hat());
    bool present =
      AnyNonTransparent(skin, hat.top) || AnyNonTransparent(skin, hat.bottom) ||
      AnyNonTransparent(skin, hat.left) || AnyNonTransparent(skin, hat.right) ||
      AnyNonTransparent(skin, hat.front) || AnyNonTransparent(skin, hat.back);
    if (present) AddBox(m.vertices, m.indicesOverlay, headC, Inflate(headSize, 0.5f), hat, texW, texH);
  }

  // Jacket
  {
    BoxUv jacket = S(UV_Jacket());
    bool present =
      AnyNonTransparent(skin, jacket.top) || AnyNonTransparent(skin, jacket.bottom) ||
      AnyNonTransparent(skin, jacket.left) || AnyNonTransparent(skin, jacket.right) ||
      AnyNonTransparent(skin, jacket.front) || AnyNonTransparent(skin, jacket.back);
    if (present) AddBox(m.vertices, m.indicesOverlay, bodyC, Inflate(bodySize, 0.5f), jacket, texW, texH);
  }

  // Right sleeve
  {
    BoxUv rs = (armW == 3.0f) ? S(UV_RightSleeveSlim()) : S(UV_RightSleeve());
    bool present =
      AnyNonTransparent(skin, rs.top) || AnyNonTransparent(skin, rs.bottom) ||
      AnyNonTransparent(skin, rs.left) || AnyNonTransparent(skin, rs.right) ||
      AnyNonTransparent(skin, rs.front) || AnyNonTransparent(skin, rs.back);
    if (present) AddBox(m.vertices, m.indicesOverlay, rArmC, Inflate(armSize, 0.5f), rs, texW, texH);
  }

  // Right pants
  {
    BoxUv rp = S(UV_RightLegPants());
    bool present =
      AnyNonTransparent(skin, rp.top) || AnyNonTransparent(skin, rp.bottom) ||
      AnyNonTransparent(skin, rp.left) || AnyNonTransparent(skin, rp.right) ||
      AnyNonTransparent(skin, rp.front) || AnyNonTransparent(skin, rp.back);
    if (present) AddBox(m.vertices, m.indicesOverlay, rLegC, Inflate(legSize, 0.5f), rp, texW, texH);
  }

  if (has64) {
    // Left sleeve
    {
      BoxUv ls = (armW == 3.0f) ? S(UV_LeftSleeveSlim()) : S(UV_LeftSleeve());
      bool present =
        AnyNonTransparent(skin, ls.top) || AnyNonTransparent(skin, ls.bottom) ||
        AnyNonTransparent(skin, ls.left) || AnyNonTransparent(skin, ls.right) ||
        AnyNonTransparent(skin, ls.front) || AnyNonTransparent(skin, ls.back);
      if (present) AddBox(m.vertices, m.indicesOverlay, lArmC, Inflate(armSize, 0.5f), ls, texW, texH);
    }

    // Left pants
    {
      BoxUv lp = S(UV_LeftLegPants());
      bool present =
        AnyNonTransparent(skin, lp.top) || AnyNonTransparent(skin, lp.bottom) ||
        AnyNonTransparent(skin, lp.left) || AnyNonTransparent(skin, lp.right) ||
        AnyNonTransparent(skin, lp.front) || AnyNonTransparent(skin, lp.back);
      if (present) AddBox(m.vertices, m.indicesOverlay, lLegC, Inflate(legSize, 0.5f), lp, texW, texH);
    }
  }

  return m;
}

static void ForceRectOpaque(SkinInfo& s, int x, int y, int w, int h) {
  const int x0 = std::max(0, x);
  const int y0 = std::max(0, y);
  const int x1 = std::min((int)s.width, x + w);
  const int y1 = std::min((int)s.height, y + h);
  if (x1 <= x0 || y1 <= y0) return;

  for (int yy = y0; yy < y1; ++yy) {
    for (int xx = x0; xx < x1; ++xx) {
      const size_t idx = (size_t)(yy * s.width + xx) * 4;
      s.rgba[idx + 3] = 255;
    }
  }
}

static void SanitizeMinecraftBaseAlpha(SkinInfo& s) {
  const int k = (int)s.scale;

  ForceRectOpaque(s, 0  * k, 0  * k, 32 * k, 16 * k); // head base
  ForceRectOpaque(s, 16 * k, 16 * k, 24 * k, 16 * k); // torso base
  ForceRectOpaque(s, 40 * k, 16 * k, 16 * k, 16 * k); // right arm base
  ForceRectOpaque(s, 0  * k, 16 * k, 16 * k, 16 * k); // right leg base

  if (!s.legacy64x32 && s.height >= (uint32_t)(64 * k)) {
    ForceRectOpaque(s, 32 * k, 48 * k, 16 * k, 16 * k); // left arm base
    ForceRectOpaque(s, 16 * k, 48 * k, 16 * k, 16 * k); // left leg base
  }
}

// ------------------------------
// WIC PNG -> RGBA8 + D3D SRV
// ------------------------------
static SkinInfo LoadSkinPngWIC(ID3D11Device* dev, const std::wstring& path) {
  SkinInfo out;
  out.path = path;

  ComPtr<IWICImagingFactory> wic;
  ThrowIfFailed(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&wic)), "CoCreateInstance(WIC)");

  ComPtr<IWICBitmapDecoder> dec;
  ThrowIfFailed(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                               WICDecodeMetadataCacheOnLoad, &dec),
                "CreateDecoderFromFilename");

  ComPtr<IWICBitmapFrameDecode> frame;
  ThrowIfFailed(dec->GetFrame(0, &frame), "GetFrame(0)");

  UINT w = 0, h = 0;
  ThrowIfFailed(frame->GetSize(&w, &h), "GetSize");
  out.width = w;
  out.height = h;

  out.legacy64x32 = (w == 64 && h == 32);
  if (w == h && (w % 64) == 0) out.scale = w / 64;
  else out.scale = 1;

  ComPtr<IWICFormatConverter> conv;
  ThrowIfFailed(wic->CreateFormatConverter(&conv), "CreateFormatConverter");
  ThrowIfFailed(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
                                 WICBitmapDitherTypeNone, nullptr, 0.0,
                                 WICBitmapPaletteTypeCustom),
                "FormatConverter.Initialize(32bppRGBA)");

  out.rgba.resize((size_t)w * (size_t)h * 4);
  ThrowIfFailed(conv->CopyPixels(nullptr, (UINT)(w * 4), (UINT)out.rgba.size(), out.rgba.data()),
                "CopyPixels");

  // Make Minecraft base layer opaque (prevents see-through skins)
  SanitizeMinecraftBaseAlpha(out);

  // Alpha detection
  out.hasAlpha = std::any_of(out.rgba.begin() + 3, out.rgba.end(),
                             [](uint8_t a) { return a != 255; });

  D3D11_TEXTURE2D_DESC td{};
  td.Width = w;
  td.Height = h;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

  D3D11_SUBRESOURCE_DATA sd{};
  sd.pSysMem = out.rgba.data();
  sd.SysMemPitch = w * 4;

  ComPtr<ID3D11Texture2D> tex;
  ThrowIfFailed(dev->CreateTexture2D(&td, &sd, &tex), "CreateTexture2D");

  D3D11_SHADER_RESOURCE_VIEW_DESC srvd{};
  srvd.Format = td.Format;
  srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
  srvd.Texture2D.MipLevels = 1;

  ThrowIfFailed(dev->CreateShaderResourceView(tex.Get(), &srvd, &out.srv), "CreateShaderResourceView");

  return out;
}

// ------------------------------
// Shaders
// ------------------------------
static const char* kShaderSrc = R"(
cbuffer CB0 : register(b0) {
  float4x4 uMVP;
};

struct VSIn {
  float3 pos : POSITION;
  float3 nrm : NORMAL;
  float2 uv  : TEXCOORD0;
};

struct VSOut {
  float4 pos : SV_POSITION;
  float2 uv  : TEXCOORD0;
};

VSOut VSMain(VSIn i) {
  VSOut o;
  o.pos = mul(float4(i.pos, 1.0), uMVP);
  o.uv = i.uv;
  return o;
}

Texture2D uTex : register(t0);
SamplerState uSamp : register(s0);

float4 PSMain(VSOut i) : SV_Target {
  return uTex.Sample(uSamp, i.uv);
}
)";

struct CB0 {
  XMFLOAT4X4 mvp;
};

static void CreateRTVAndDSV(D3DState& d) {
  ComPtr<ID3D11Texture2D> back;
  ThrowIfFailed(d.swap->GetBuffer(0, IID_PPV_ARGS(&back)), "SwapChain.GetBuffer");
  ThrowIfFailed(d.device->CreateRenderTargetView(back.Get(), nullptr, &d.rtv), "CreateRTV");

  D3D11_TEXTURE2D_DESC dd{};
  dd.Width = (UINT)d.fbW;
  dd.Height = (UINT)d.fbH;
  dd.MipLevels = 1;
  dd.ArraySize = 1;
  dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
  dd.SampleDesc.Count = 1;
  dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
  ThrowIfFailed(d.device->CreateTexture2D(&dd, nullptr, &d.depth), "CreateDepthTex");
  ThrowIfFailed(d.device->CreateDepthStencilView(d.depth.Get(), nullptr, &d.dsv), "CreateDSV");
}

static void Resize(D3DState& d, int w, int h) {
  if (!d.swap) return;
  if (w <= 0 || h <= 0) return;
  d.fbW = w; d.fbH = h;

  d.ctx->OMSetRenderTargets(0, nullptr, nullptr);
  d.rtv.Reset();
  d.dsv.Reset();
  d.depth.Reset();

  ThrowIfFailed(d.swap->ResizeBuffers(0, (UINT)w, (UINT)h, DXGI_FORMAT_UNKNOWN, 0), "ResizeBuffers");
  CreateRTVAndDSV(d);
}

static void InitD3D(D3DState& d) {
  RECT rc{};
  GetClientRect(d.hwnd, &rc);
  d.fbW = std::max(1L, rc.right - rc.left);
  d.fbH = std::max(1L, rc.bottom - rc.top);

  DXGI_SWAP_CHAIN_DESC scd{};
  scd.BufferCount = 2;
  scd.BufferDesc.Width = d.fbW;
  scd.BufferDesc.Height = d.fbH;
  scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  scd.OutputWindow = d.hwnd;
  scd.SampleDesc.Count = 1;
  scd.Windowed = TRUE;
  scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

  UINT flags = 0;
#if defined(_DEBUG)
  flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

  D3D_FEATURE_LEVEL fl{};
  ThrowIfFailed(D3D11CreateDeviceAndSwapChain(
    nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
    nullptr, 0, D3D11_SDK_VERSION,
    &scd, &d.swap, &d.device, &fl, &d.ctx
  ), "D3D11CreateDeviceAndSwapChain");

  CreateRTVAndDSV(d);

  // Compile shaders
  ComPtr<ID3DBlob> vsb, psb, err;
  HRESULT hr = D3DCompile(kShaderSrc, strlen(kShaderSrc), "skin.hlsl",
                          nullptr, nullptr, "VSMain", "vs_5_0",
                          0, 0, &vsb, &err);
  if (FAILED(hr)) {
    std::string e = err ? std::string((const char*)err->GetBufferPointer(), err->GetBufferSize()) : "VS compile failed";
    throw std::runtime_error(e);
  }

  hr = D3DCompile(kShaderSrc, strlen(kShaderSrc), "skin.hlsl",
                  nullptr, nullptr, "PSMain", "ps_5_0",
                  0, 0, &psb, &err);
  if (FAILED(hr)) {
    std::string e = err ? std::string((const char*)err->GetBufferPointer(), err->GetBufferSize()) : "PS compile failed";
    throw std::runtime_error(e);
  }

  ThrowIfFailed(d.device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &d.vs),
                "CreateVertexShader");
  ThrowIfFailed(d.device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &d.ps),
                "CreatePixelShader");

  D3D11_INPUT_ELEMENT_DESC ild[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex,pos), D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex,nrm), D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, offsetof(Vertex,uv),  D3D11_INPUT_PER_VERTEX_DATA, 0},
  };
  ThrowIfFailed(d.device->CreateInputLayout(ild, (UINT)std::size(ild),
                                           vsb->GetBufferPointer(), vsb->GetBufferSize(),
                                           &d.il),
                "CreateInputLayout");

  // Constant buffer
  D3D11_BUFFER_DESC cbd{};
  cbd.ByteWidth = sizeof(CB0);
  cbd.Usage = D3D11_USAGE_DYNAMIC;
  cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  ThrowIfFailed(d.device->CreateBuffer(&cbd, nullptr, &d.cb), "CreateCB");

  // Sampler
  D3D11_SAMPLER_DESC sd{};
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  ThrowIfFailed(d.device->CreateSamplerState(&sd, &d.samp), "CreateSampler");

  // Rasterizer
  D3D11_RASTERIZER_DESC rsd{};
  rsd.FillMode = D3D11_FILL_SOLID;
  rsd.CullMode = D3D11_CULL_BACK;
  rsd.FrontCounterClockwise = TRUE; // your mesh winding is CCW
  rsd.DepthClipEnable = TRUE;
  ThrowIfFailed(d.device->CreateRasterizerState(&rsd, &d.rs), "CreateRasterizer");

  // Depth
  D3D11_DEPTH_STENCIL_DESC dsd{};
  dsd.DepthEnable = TRUE;
  dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
  dsd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
  ThrowIfFailed(d.device->CreateDepthStencilState(&dsd, &d.dsDefault), "CreateDepthStencilState");

  // Alpha blend
  D3D11_BLEND_DESC bd{};
  bd.RenderTarget[0].BlendEnable = TRUE;
  bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
  bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
  bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
  bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
  bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
  bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  ThrowIfFailed(d.device->CreateBlendState(&bd, &d.blendAlpha), "CreateBlendState");
}

static void UploadMesh(D3DState& d, const BuiltMesh& m) {
  std::vector<uint32_t> allIdx;
  allIdx.reserve(m.indicesBase.size() + m.indicesOverlay.size());
  allIdx.insert(allIdx.end(), m.indicesBase.begin(), m.indicesBase.end());
  d.ibOffsetOverlay = (UINT)m.indicesBase.size();
  allIdx.insert(allIdx.end(), m.indicesOverlay.begin(), m.indicesOverlay.end());

  d.ibCountBase = (UINT)m.indicesBase.size();
  d.ibCountOverlay = (UINT)m.indicesOverlay.size();

  d.vb.Reset();
  d.ib.Reset();

  if (m.vertices.empty() || allIdx.empty()) return;

  D3D11_BUFFER_DESC vbd{};
  vbd.ByteWidth = (UINT)(m.vertices.size() * sizeof(Vertex));
  vbd.Usage = D3D11_USAGE_DEFAULT;
  vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;

  D3D11_SUBRESOURCE_DATA vsd{};
  vsd.pSysMem = m.vertices.data();
  ThrowIfFailed(d.device->CreateBuffer(&vbd, &vsd, &d.vb), "CreateVB");

  D3D11_BUFFER_DESC ibd{};
  ibd.ByteWidth = (UINT)(allIdx.size() * sizeof(uint32_t));
  ibd.Usage = D3D11_USAGE_DEFAULT;
  ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;

  D3D11_SUBRESOURCE_DATA isd{};
  isd.pSysMem = allIdx.data();
  ThrowIfFailed(d.device->CreateBuffer(&ibd, &isd, &d.ib), "CreateIB");
}

// ------------------------------
// Camera controls
// ------------------------------
struct Camera {
  float yaw = 0.9f;
  float pitch = 0.35f;
  float dist = 70.0f;
  XMFLOAT3 target{0, 16, 0};
};

static XMMATRIX MakeView(const Camera& c) {
  const float cp = cosf(c.pitch), sp = sinf(c.pitch);
  const float cy = cosf(c.yaw),   sy = sinf(c.yaw);

  XMVECTOR eye = XMVectorSet(
    c.target.x + c.dist * cp * sy,
    c.target.y + c.dist * sp,
    c.target.z + c.dist * cp * cy,
    1.0f
  );

  XMVECTOR at = XMVectorSet(c.target.x, c.target.y, c.target.z, 1.0f);
  XMVECTOR up = XMVectorSet(0, 1, 0, 0);
  return XMMatrixLookAtLH(eye, at, up);
}

// ------------------------------
// App state
// ------------------------------
struct App {
  D3DState d3d;
  std::optional<SkinInfo> skin;
  Camera cam;

  std::string status = "Drag & drop a Minecraft skin .png onto the window.";
  bool showOverlay = true;
  bool pointFilter = true;
  bool slimArms = false; // <-- NEW
  bool minimized = false;


  bool rotating = false;
  POINT lastMouse{};
};

static void ApplySampler(App& a) {
  D3D11_SAMPLER_DESC sd{};
  sd.Filter = a.pointFilter ? D3D11_FILTER_MIN_MAG_MIP_POINT : D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.MaxLOD = D3D11_FLOAT32_MAX;

  a.d3d.samp.Reset();
  ThrowIfFailed(a.d3d.device->CreateSamplerState(&sd, &a.d3d.samp), "CreateSamplerState(update)");
}

static void RebuildMeshIfSkinLoaded(App& a) {
  if (!a.skin) return;
  BuiltMesh mesh = BuildPlayerMesh(*a.skin, a.slimArms);
  UploadMesh(a.d3d, mesh);
}

static void LoadSkinIntoApp(App& a, const std::wstring& path) {
  try {
    SkinInfo s = LoadSkinPngWIC(a.d3d.device.Get(), path);

    const bool ok =
      (s.width == 64 && (s.height == 32 || s.height == 64)) ||
      (s.width == s.height && (s.width % 64) == 0);

    a.status = ok ? "Skin loaded." : "Loaded image, but dimensions are not typical for Minecraft skins.";

    BuiltMesh mesh = BuildPlayerMesh(s, a.slimArms);
    UploadMesh(a.d3d, mesh);

    a.skin = std::move(s);
  } catch (const std::exception& e) {
    a.skin.reset();
    a.status = std::string("Failed to load skin: ") + e.what();
  }
}

// ------------------------------
// Win32
// ------------------------------
static App* g_app = nullptr;

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
    return true;

  switch (msg) {
    case WM_SIZE: {
      if (g_app) {
        g_app->minimized = (wParam == SIZE_MINIMIZED);
        if (!g_app->minimized && g_app->d3d.swap) {
          int w = LOWORD(lParam);
          int h = HIWORD(lParam);
          Resize(g_app->d3d, w, h);
        }
      }
      return 0;
    }
    
    case WM_DROPFILES: {
      if (!g_app) return 0;
      HDROP drop = (HDROP)wParam;
      wchar_t filePath[MAX_PATH]{};
      if (DragQueryFileW(drop, 0, filePath, MAX_PATH)) {
        LoadSkinIntoApp(*g_app, filePath);
      }
      DragFinish(drop);
      return 0;
    }
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ------------------------------
// Render
// ------------------------------
static void Render(App& a) {
  auto& d = a.d3d;

  float clear[4]{ 0.08f, 0.08f, 0.10f, 1.0f };
  d.ctx->ClearRenderTargetView(d.rtv.Get(), clear);
  d.ctx->ClearDepthStencilView(d.dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

  D3D11_VIEWPORT vp{};
  vp.Width = (float)d.fbW;
  vp.Height = (float)d.fbH;
  vp.MinDepth = 0.0f;
  vp.MaxDepth = 1.0f;
  d.ctx->RSSetViewports(1, &vp);

  d.ctx->OMSetRenderTargets(1, d.rtv.GetAddressOf(), d.dsv.Get());
  d.ctx->RSSetState(d.rs.Get());
  d.ctx->OMSetDepthStencilState(d.dsDefault.Get(), 0);

  d.ctx->IASetInputLayout(d.il.Get());
  d.ctx->VSSetShader(d.vs.Get(), nullptr, 0);
  d.ctx->PSSetShader(d.ps.Get(), nullptr, 0);
  d.ctx->PSSetSamplers(0, 1, d.samp.GetAddressOf());

  UINT stride = sizeof(Vertex), offset = 0;
  if (d.vb && d.ib) {
    d.ctx->IASetVertexBuffers(0, 1, d.vb.GetAddressOf(), &stride, &offset);
    d.ctx->IASetIndexBuffer(d.ib.Get(), DXGI_FORMAT_R32_UINT, 0);
    d.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  }

  XMMATRIX view = MakeView(a.cam);
  XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(55.0f), (float)d.fbW / (float)d.fbH, 0.1f, 500.0f);
  XMMATRIX world = XMMatrixScaling(0.9f, 0.9f, 0.9f) * XMMatrixTranslation(0, 0, 0);
  XMMATRIX mvp = world * view * proj;

  CB0 cb{};
  XMStoreFloat4x4(&cb.mvp, XMMatrixTranspose(mvp));

  D3D11_MAPPED_SUBRESOURCE map{};
  ThrowIfFailed(d.ctx->Map(d.cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map), "Map(CB)");
  memcpy(map.pData, &cb, sizeof(cb));
  d.ctx->Unmap(d.cb.Get(), 0);

  d.ctx->VSSetConstantBuffers(0, 1, d.cb.GetAddressOf());

  ID3D11ShaderResourceView* srv = nullptr;
  if (a.skin) srv = a.skin->srv.Get();
  d.ctx->PSSetShaderResources(0, 1, &srv);

  // Draw base
  if (d.vb && d.ib && a.skin && d.ibCountBase > 0) {
    float blendFactor[4]{};
    d.ctx->OMSetBlendState(nullptr, blendFactor, 0xFFFFFFFF);
    d.ctx->DrawIndexed(d.ibCountBase, 0, 0);
  }

  // Draw overlay
  if (a.showOverlay && d.vb && d.ib && a.skin && d.ibCountOverlay > 0) {
    float blendFactor[4]{};
    d.ctx->OMSetBlendState(d.blendAlpha.Get(), blendFactor, 0xFFFFFFFF);
    d.ctx->DrawIndexed(d.ibCountOverlay, d.ibOffsetOverlay, 0);
    d.ctx->OMSetBlendState(nullptr, blendFactor, 0xFFFFFFFF);
  }

  // ImGui
  ImGui_ImplDX11_NewFrame();
  ImGui_ImplWin32_NewFrame();
  ImGui::NewFrame();

  ImGui::Begin("Minecraft Skin Viewer");
  ImGui::TextUnformatted(a.status.c_str());
  ImGui::Separator();

  if (a.skin) {
    ImGui::Text("File: %s", NarrowFromWide(a.skin->path).c_str());
    ImGui::Text("Size: %ux%u", a.skin->width, a.skin->height);
    ImGui::Text("Scale: %u (64px reference)", a.skin->scale);
    ImGui::Text("Format: %s", a.skin->legacy64x32 ? "Legacy 64x32" : "Modern (64x64+) / Scaled");
    ImGui::Text("Alpha: %s", a.skin->hasAlpha ? "present" : "opaque/none detected");
    ImGui::Separator();
  } else {
    ImGui::Text("No skin loaded.");
  }

  ImGui::Checkbox("Show overlay (hat/jacket/sleeves/pants)", &a.showOverlay);

  if (ImGui::Checkbox("Point filtering (pixel-crisp)", &a.pointFilter)) {
    ApplySampler(a);
  }

  // ---- NEW: Slim toggle ----
  if (ImGui::Checkbox("Slim arms (Alex)", &a.slimArms)) {
    RebuildMeshIfSkinLoaded(a);
  }

  ImGui::Separator();
  ImGui::Text("Controls:");
  ImGui::BulletText("Drag & drop a .png skin onto the window");
  ImGui::BulletText("Hold Left Mouse + drag: orbit");
  ImGui::BulletText("Mouse wheel: zoom");
  ImGui::End();

  ImGui::Render();
  ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

  d.swap->Present(1, 0);
}

// ------------------------------
// Main
// ------------------------------
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
  try {
    ThrowIfFailed(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED), "CoInitializeEx");

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"MinecraftSkinViewerWnd";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    RECT r{0, 0, 1280, 720};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);

    HWND hwnd = CreateWindowW(
      wc.lpszClassName,
      L"Minecraft Skin Viewer (DirectX 11)",
      WS_OVERLAPPEDWINDOW,
      CW_USEDEFAULT, CW_USEDEFAULT,
      r.right - r.left, r.bottom - r.top,
      nullptr, nullptr, hInst, nullptr
    );
    if (!hwnd) throw std::runtime_error("CreateWindowW failed");

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    App app;
    g_app = &app;
    app.d3d.hwnd = hwnd;

    DragAcceptFiles(hwnd, TRUE);

    InitD3D(app.d3d);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(app.d3d.device.Get(), app.d3d.ctx.Get());

    ApplySampler(app);

    MSG msg{};
  bool running = true;

  while (running) {
    // If minimized, sleep until some window message arrives.
    // If not minimized, wake up every ~16ms to render (~60fps) OR sooner if there are messages.
    DWORD timeoutMs = app.minimized ? INFINITE : 16;
    MsgWaitForMultipleObjectsEx(
      0, nullptr,
      timeoutMs,
      QS_ALLINPUT,
      MWMO_INPUTAVAILABLE
    );

    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) { running = false; break; }
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    if (!running) break;

    // Don’t render while minimized (kills the “4 billion cycles” issue)
    if (app.minimized) { app.rotating = false; continue; }

    // Camera input (avoid fighting ImGui)
    ImGuiIO& iio = ImGui::GetIO();
    if (!iio.WantCaptureMouse) {
      if (iio.MouseWheel != 0.0f) {
        app.cam.dist = Clamp(app.cam.dist - iio.MouseWheel * 4.0f, 20.0f, 200.0f);
      }

      if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (!app.rotating) {
          app.rotating = true;
          GetCursorPos(&app.lastMouse);
        } else {
          POINT p{};
          GetCursorPos(&p);
          const float dx = (float)(p.x - app.lastMouse.x);
          const float dy = (float)(p.y - app.lastMouse.y);
          app.lastMouse = p;

          app.cam.yaw += dx * 0.01f;
          app.cam.pitch = Clamp(app.cam.pitch + dy * 0.01f, -1.2f, 1.2f);
        }
      } else {
        app.rotating = false;
      }
    }

    Render(app);
  }


    DragAcceptFiles(hwnd, FALSE);

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CoUninitialize();
    return 0;
  } catch (const std::exception& e) {
    MessageBoxA(nullptr, e.what(), "Fatal error", MB_ICONERROR | MB_OK);
    return 1;
  }
}
