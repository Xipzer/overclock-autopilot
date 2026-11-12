// The built-in GPU load, for warming a shared water loop: `shakedown.exe --heat`.
// A heavy pixel shader drawn off-screen, over and over, on the graphics card with the most memory. No window.
// It runs as its own process so the graphics driver is never loaded into the tray program, and it uses almost no
// CPU (it sleeps while the card works), because the CPU is what is being tested.
// It ends when the program that started it lets go of the "heat" event, or is closed.
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>

#define HEAT_NAME L"Local\\ShakedownHeat"

static const GUID heatFactoryId = {0x770aae78, 0xf26f, 0x4dba, {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}}; // IDXGIFactory1
static const char heatVertex[] = "float4 main(uint id : SV_VertexID) : SV_Position { float2 p = float2((id << 1) & 2, id & 2); return float4(p * 2 - 1, 0, 1); }";
static const char heatPixel[] =
    "float4 main(float4 pos : SV_Position) : SV_Target {"
    "  float2 z = pos.xy * 0.001; float a = 0;"
    "  for (int i = 0; i < 384; i++) { z = float2(sin(z.x * 1.7 + z.y), cos(z.y * 1.3 - z.x)) * 1.01 + 0.001 * i; a += dot(z, z); }"
    "  return float4(frac(a), z, 1); }";

typedef HRESULT(WINAPI *HeatFactory)(REFIID, void **);
typedef HRESULT(WINAPI *HeatDevice)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT, ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
typedef HRESULT(WINAPI *HeatCompile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);

static int heatMain(void) {
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll"), d3d = LoadLibraryW(L"d3d11.dll"), compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    HeatFactory mkFactory = dxgi ? (HeatFactory)GetProcAddress(dxgi, "CreateDXGIFactory1") : NULL;
    HeatDevice mkDevice = d3d ? (HeatDevice)GetProcAddress(d3d, "D3D11CreateDevice") : NULL;
    HeatCompile compile = compiler ? (HeatCompile)GetProcAddress(compiler, "D3DCompile") : NULL;
    IDXGIFactory1 *factory = NULL;
    IDXGIAdapter1 *adapter = NULL, *best = NULL;
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    ID3DBlob *vsCode = NULL, *psCode = NULL;
    ID3D11VertexShader *vs = NULL;
    ID3D11PixelShader *ps = NULL;
    ID3D11Texture2D *target = NULL;
    ID3D11RenderTargetView *view = NULL;
    ID3D11Query *done = NULL;
    SIZE_T most = 0;
    if (!mkFactory || !mkDevice || !compile || mkFactory(&heatFactoryId, (void **)&factory) != S_OK) return 3;
    // The card to warm is the one with the most memory of its own, not the one built into the CPU.
    for (UINT i = 0; IDXGIFactory1_EnumAdapters1(factory, i, &adapter) == S_OK; i++) {
        DXGI_ADAPTER_DESC1 desc;
        if (IDXGIAdapter1_GetDesc1(adapter, &desc) == S_OK && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && desc.DedicatedVideoMemory > most) {
            if (best) IDXGIAdapter1_Release(best);
            best = adapter, most = desc.DedicatedVideoMemory;
        } else {
            IDXGIAdapter1_Release(adapter);
        }
    }
    if (!best || mkDevice((IDXGIAdapter *)best, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, NULL, &ctx) != S_OK) return 4;
    if (compile(heatVertex, sizeof(heatVertex) - 1, NULL, NULL, NULL, "main", "vs_5_0", 0, 0, &vsCode, NULL) != S_OK ||
        compile(heatPixel, sizeof(heatPixel) - 1, NULL, NULL, NULL, "main", "ps_5_0", 0, 0, &psCode, NULL) != S_OK)
        return 5;
    D3D11_TEXTURE2D_DESC td = {4096, 4096, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0};
    D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
    D3D11_VIEWPORT vp = {0, 0, 4096, 4096, 0, 1};
    if (ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vsCode), ID3D10Blob_GetBufferSize(vsCode), NULL, &vs) != S_OK ||
        ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(psCode), ID3D10Blob_GetBufferSize(psCode), NULL, &ps) != S_OK ||
        ID3D11Device_CreateTexture2D(dev, &td, NULL, &target) != S_OK || ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)target, NULL, &view) != S_OK ||
        ID3D11Device_CreateQuery(dev, &qd, &done) != S_OK)
        return 6;
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &view, NULL);
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    DWORD lastSeen = GetTickCount();
    for (;;) {
        // A few frames are queued, then this thread sleeps until the card has finished them.
        for (int i = 0; i < 3; i++) ID3D11DeviceContext_Draw(ctx, 3, 0);
        ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)done);
        ID3D11DeviceContext_Flush(ctx);
        for (int waited = 0; ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)done, NULL, 0, 0) == S_FALSE && waited < 5000; waited++) Sleep(1);
        HANDLE owner = OpenEventW(SYNCHRONIZE, FALSE, HEAT_NAME);
        if (owner) CloseHandle(owner), lastSeen = GetTickCount();
        else if (GetTickCount() - lastSeen > 3000) break;
        if (ID3D11Device_GetDeviceRemovedReason(dev) != S_OK) return 7;
    }
    return 0;
}
