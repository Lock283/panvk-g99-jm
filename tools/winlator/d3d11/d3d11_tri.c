/* d3d11_tri.c -- D3D11 test for DXVK on the Winlator stack.
 * Creates a window + swapchain (D3D11CreateDeviceAndSwapChain). HLSL is
 * compiled at startup with D3DCompile from d3dcompiler_47.dll. Each frame: clear to a frame-dependent colour, draw one
 * full-viewport triangle pair coloured by a constant buffer, Present. Every
 * CHECK_EVERY frames it copies the back buffer to a staging texture and checks
 * the centre pixel (quad colour) and a corner pixel (clear colour) against the
 * expected values. Prints feature level, adapter name, and a final verdict.
 *
 * D3D11TRI_PNG=<windows path>: write each checked frame (read back from the
 * GPU) as PNG, e.g. Z:/.../img/live/dxvk.png for the live dashboard.
 * Negative control: D3D11TRI_LIE=1 expects a wrong quad colour (must FAIL).
 * env: D3D11TRI_FRAMES (default 180), D3D11TRI_CHECK_EVERY (30).
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcommon.h>
#include <stdio.h>
#include <stdlib.h>
#include "../pngdump.h"

static const char hlsl[] =
   "cbuffer C : register(b0) { float4 col; };\n"
   "float4 vs(uint id : SV_VertexID) : SV_Position {\n"
   "  float2 p[6] = { float2(-0.5,-0.5), float2(-0.5,0.5), float2(0.5,0.5),\n"
   "                  float2(-0.5,-0.5), float2(0.5,0.5), float2(0.5,-0.5) };\n"
   "  return float4(p[id], 0.5, 1.0); }\n"
   "float4 ps() : SV_Target { return col; }\n";

typedef HRESULT (WINAPI *PFN_D3DCompile)(const void *, SIZE_T, const char *, const void *, void *, const char *,
                                          const char *, UINT, UINT, ID3DBlob **, ID3DBlob **);

static ID3DBlob *compile(PFN_D3DCompile fn, const char *entry, const char *target)
{
   ID3DBlob *code = NULL, *err = NULL;
   HRESULT hr = fn(hlsl, sizeof(hlsl) - 1, "tri.hlsl", NULL, NULL, entry, target, 0, 0, &code, &err);
   if (FAILED(hr)) {
      printf("FAILED: D3DCompile %s hr=0x%08lx %s\n", entry, (unsigned long)hr,
             err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "");
      return NULL;
   }
   return code;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
   if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
   return DefWindowProcA(h, m, w, l);
}

static int env_int(const char *n, int d) { const char *v = getenv(n); return v ? atoi(v) : d; }

int main(void)
{
   const int frames = env_int("D3D11TRI_FRAMES", 180);
   const int every = env_int("D3D11TRI_CHECK_EVERY", 30);
   const int lie = env_int("D3D11TRI_LIE", 0);
   const int W = 512, Hh = 512;
   setvbuf(stdout, NULL, _IONBF, 0);

   WNDCLASSA wc = { 0 };
   wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "d3d11tri";
   RegisterClassA(&wc);
   RECT rc = { 0, 0, W, Hh }; AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
   HWND hwnd = CreateWindowA("d3d11tri", "d3d11_tri (DXVK)", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100,
                             rc.right - rc.left, rc.bottom - rc.top, NULL, NULL, wc.hInstance, NULL);
   if (!hwnd) { printf("FAILED: CreateWindow\n"); return 1; }

   DXGI_SWAP_CHAIN_DESC sd = { 0 };
   sd.BufferCount = 2; sd.BufferDesc.Width = W; sd.BufferDesc.Height = Hh;
   sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
   sd.OutputWindow = hwnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
   D3D_FEATURE_LEVEL fls[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                               D3D_FEATURE_LEVEL_10_0 };
   ID3D11Device *dev = NULL; ID3D11DeviceContext *ctx = NULL; IDXGISwapChain *sc = NULL; D3D_FEATURE_LEVEL got = 0;
   HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, fls, 4, D3D11_SDK_VERSION,
                                              &sd, &sc, &dev, &got, &ctx);
   if (FAILED(hr)) { printf("FAILED: D3D11CreateDeviceAndSwapChain hr=0x%08lx\n", (unsigned long)hr); return 1; }
   printf("D3D11 device ok, feature level %x_%x\n", got >> 12, (got >> 8) & 0xf);

   IDXGIDevice *xd = NULL; IDXGIAdapter *ad = NULL; DXGI_ADAPTER_DESC adesc;
   if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void **)&xd)) &&
       SUCCEEDED(IDXGIDevice_GetAdapter(xd, &ad)) && SUCCEEDED(IDXGIAdapter_GetDesc(ad, &adesc)))
      printf("adapter: %ls vendor=0x%x device=0x%x vram=%lluMB\n", adesc.Description, adesc.VendorId,
             adesc.DeviceId, (unsigned long long)adesc.DedicatedVideoMemory >> 20);

   ID3D11Texture2D *bb = NULL; ID3D11RenderTargetView *rtv = NULL;
   IDXGISwapChain_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **)&bb);
   ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)bb, NULL, &rtv);

   HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
   PFN_D3DCompile D3DCompileFn = dc ? (PFN_D3DCompile)GetProcAddress(dc, "D3DCompile") : NULL;
   if (!D3DCompileFn) { printf("FAILED: no D3DCompile\n"); return 1; }
   ID3DBlob *vsb = compile(D3DCompileFn, "vs", "vs_4_0"), *psb = compile(D3DCompileFn, "ps", "ps_4_0");
   if (!vsb || !psb) return 1;
   ID3D11VertexShader *vs = NULL; ID3D11PixelShader *ps = NULL;
   hr = ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), NULL, &vs);
   if (FAILED(hr)) { printf("FAILED: CreateVertexShader 0x%08lx\n", (unsigned long)hr); return 1; }
   hr = ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb), NULL, &ps);
   if (FAILED(hr)) { printf("FAILED: CreatePixelShader 0x%08lx\n", (unsigned long)hr); return 1; }

   D3D11_BUFFER_DESC cbd = { 16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
   ID3D11Buffer *cb = NULL; ID3D11Device_CreateBuffer(dev, &cbd, NULL, &cb);

   D3D11_TEXTURE2D_DESC td; ID3D11Texture2D_GetDesc(bb, &td);
   td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
   ID3D11Texture2D *stg = NULL; ID3D11Device_CreateTexture2D(dev, &td, NULL, &stg);

   D3D11_VIEWPORT vp = { 0, 0, (float)W, (float)Hh, 0, 1 };
   int checked = 0, bad = 0;
   DWORD t0 = GetTickCount();
   for (int f = 0; f < frames; f++) {
      MSG msg; while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
      float clear[4] = { (f % 64) / 63.0f, 0.25f, 0.5f, 1.0f };
      float quad[4] = { 1.0f, (f % 32) / 31.0f, 0.0f, 1.0f };
      D3D11_MAPPED_SUBRESOURCE ms;
      ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
      memcpy(ms.pData, quad, 16);
      ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)cb, 0);

      ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
      ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
      ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, clear);
      ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
      ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
      ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &cb);
      ID3D11DeviceContext_Draw(ctx, 6, 0); /* centre quad from SV_VertexID */

      if (every > 0 && f % every == 0) {
         ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)stg, (ID3D11Resource *)bb);
         D3D11_MAPPED_SUBRESOURCE rm;
         if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)stg, 0, D3D11_MAP_READ, 0, &rm))) {
            const unsigned char *px = rm.pData;
            const unsigned char *c = px + (Hh / 2) * rm.RowPitch + (W / 2) * 4;
            const unsigned char *k = px + 4 * rm.RowPitch + 4 * 4;
            int er = 255, eg = (int)(quad[1] * 255 + 0.5f), ebl = 0;
            if (lie) eg = 255 - eg;
            int cr = (int)(clear[0] * 255 + 0.5f), cg = 64, cbv = 128;
            int okc = abs(c[0] - er) <= 2 && abs(c[1] - eg) <= 2 && abs(c[2] - ebl) <= 2;
            int okk = abs(k[0] - cr) <= 2 && abs(k[1] - cg) <= 2 && abs(k[2] - cbv) <= 2;
            checked++;
            if (!okc || !okk) bad++;
            const char *pngp = getenv("D3D11TRI_PNG");
            if (pngp) {
               unsigned char *rgb = malloc((size_t)W * Hh * 3);
               for (int y = 0; y < Hh; y++)
                  for (int x = 0; x < W; x++)
                     memcpy(rgb + ((size_t)y * W + x) * 3, px + (size_t)y * rm.RowPitch + x * 4, 3);
               pngdump_write(pngp, rgb, W, Hh);
               free(rgb);
            }
            printf("CHECK frame=%03d centre=%d,%d,%d exp=%d,%d,%d corner=%d,%d,%d exp=%d,%d,%d %s\n", f, c[0], c[1],
                   c[2], er, eg, ebl, k[0], k[1], k[2], cr, cg, cbv, (okc && okk) ? "ok" : "BAD");
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)stg, 0);
         } else {
            checked++; bad++; printf("CHECK frame=%03d map failed\n", f);
         }
      }
      hr = IDXGISwapChain_Present(sc, 1, 0);
      if (FAILED(hr)) { printf("FAILED: Present frame %d hr=0x%08lx\n", f, (unsigned long)hr); return 1; }
   }
   DWORD ms_ = GetTickCount() - t0;
   printf("D3D11SUM frames=%d checked=%d bad=%d time=%lums fps=%.1f verdict=%s\n", frames, checked, bad,
          (unsigned long)ms_, ms_ ? frames * 1000.0 / ms_ : 0.0, (checked && !bad) ? "PASS" : "FAIL");
   return (checked && !bad) ? 0 : 2;
}
