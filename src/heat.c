// The built-in GPU load, for warming a shared water loop: `shakedown.exe --heat`.
// A heavy compute shader, dispatched over and over, on the graphics card with the most memory. No window.
// It runs as its own process so the graphics driver is never loaded into the tray program, and it uses almost no
// CPU (it sleeps while the card works), because the CPU is what is being tested.
// It ends when the program that started it lets go of the "heat" event, or is closed.
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <mmsystem.h>

#define HEAT_NAME L"Local\\ShakedownHeat"

static const GUID heatFactoryId = {0x770aae78, 0xf26f, 0x4dba, {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}}; // IDXGIFactory1
// Dense multiply-adds in a compute shader, the usual way to make a graphics card draw its full power.
static const char heatShader[] =
    "RWStructuredBuffer<float4> sink : register(u0);"
    "[numthreads(256, 1, 1)] void main(uint3 id : SV_DispatchThreadID) {"
    "  float4 a = float4(id.x * 1e-7, 0.5, 0.25, 0.125), b = a.yzwx, c = a.zwxy, d = a.wxyz;"
    "  [loop] for (int i = 0; i < 2048; i++) { a = mad(a, b, c); b = mad(b, c, d); c = mad(c, d, a); d = mad(d, a, b); }"
    "  sink[id.x & 65535] = a + b + c + d; }";

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
    ID3DBlob *code = NULL;
    ID3D11ComputeShader *cs = NULL;
    ID3D11Buffer *buffer = NULL;
    ID3D11UnorderedAccessView *uav = NULL;
    ID3D11Query *done = NULL, *more = NULL;
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
    if (compile(heatShader, sizeof(heatShader) - 1, NULL, NULL, NULL, "main", "cs_5_0", 0, 0, &code, NULL) != S_OK) return 5;
    D3D11_BUFFER_DESC bd = {65536 * 16, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 16};
    D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
    if (ID3D11Device_CreateComputeShader(dev, ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code), NULL, &cs) != S_OK ||
        ID3D11Device_CreateBuffer(dev, &bd, NULL, &buffer) != S_OK || ID3D11Device_CreateUnorderedAccessView(dev, (ID3D11Resource *)buffer, NULL, &uav) != S_OK ||
        ID3D11Device_CreateQuery(dev, &qd, &done) != S_OK || ID3D11Device_CreateQuery(dev, &qd, &more) != S_OK)
        return 6;
    ID3D11DeviceContext_CSSetShader(ctx, cs, NULL, 0);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 1, &uav, NULL);
    // The CPU under test is fully loaded, so this thread must win the little CPU it needs, or the card sits idle.
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    timeBeginPeriod(1);
    // Two batches in flight so the card always has work queued. A batch is sized to take about 40 ms; between
    // batches this thread sleeps, so it costs the CPU almost nothing.
    ID3D11Query *fence[2] = {done, more};
    int groups = 4096, slot = 0;
    LARGE_INTEGER freq, began[2], now;
    QueryPerformanceFrequency(&freq);
    for (int k = 0; k < 2; k++) {
        ID3D11DeviceContext_Dispatch(ctx, groups, 1, 1);
        ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)fence[k]);
        QueryPerformanceCounter(&began[k]);
    }
    ID3D11DeviceContext_Flush(ctx);
    DWORD lastSeen = GetTickCount();
    for (;;) {
        while (ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)fence[slot], NULL, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_FALSE) Sleep(2);
        QueryPerformanceCounter(&now);
        double ms = (double)(now.QuadPart - began[slot].QuadPart) * 1000.0 / (double)freq.QuadPart / 2;
        if (ms < 30 && groups < (1 << 22)) groups += groups / 4;
        else if (ms > 60 && groups > 1024) groups -= groups / 5;
        ID3D11DeviceContext_Dispatch(ctx, groups, 1, 1);
        ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)fence[slot]);
        ID3D11DeviceContext_Flush(ctx);
        QueryPerformanceCounter(&began[slot]);
        slot ^= 1;
        HANDLE owner = OpenEventW(SYNCHRONIZE, FALSE, HEAT_NAME);
        if (owner) CloseHandle(owner), lastSeen = GetTickCount();
        else if (GetTickCount() - lastSeen > 3000) break;
        if (ID3D11Device_GetDeviceRemovedReason(dev) != S_OK) return 7;
    }
    timeEndPeriod(1);
    return 0;
}
