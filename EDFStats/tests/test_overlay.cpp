// renders the EDFStats overlay into a real D3D11 window with fake data and saves frames as BMP
#include "edfstats.cpp"

static void SaveBmp(IDXGISwapChain *sc, const char *path) {
    ID3D11Texture2D *bb = nullptr; sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bb);
    D3D11_TEXTURE2D_DESC d; bb->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    ID3D11Texture2D *st = nullptr; g_dev->CreateTexture2D(&d, nullptr, &st);
    g_ctx->CopyResource(st, bb);
    D3D11_MAPPED_SUBRESOURCE m; g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &m);
    FILE *f = fopen(path, "wb");
    BITMAPFILEHEADER fh{}; BITMAPINFOHEADER ih{};
    ih.biSize = sizeof ih; ih.biWidth = d.Width; ih.biHeight = -(LONG)d.Height; ih.biPlanes = 1; ih.biBitCount = 32;
    fh.bfType = 0x4D42; fh.bfOffBits = sizeof fh + sizeof ih; fh.bfSize = fh.bfOffBits + d.Width * d.Height * 4;
    fwrite(&fh, sizeof fh, 1, f); fwrite(&ih, sizeof ih, 1, f);
    std::vector<uint8_t> row(d.Width * 4);
    for (UINT y = 0; y < d.Height; ++y) {
        const uint8_t *src = (const uint8_t *)m.pData + y * m.RowPitch;
        for (UINT x = 0; x < d.Width; ++x) { row[x*4] = src[x*4+2]; row[x*4+1] = src[x*4+1]; row[x*4+2] = src[x*4]; row[x*4+3] = 255; }
        fwrite(row.data(), 1, row.size(), f);
    }
    fclose(f); g_ctx->Unmap(st, 0); st->Release(); bb->Release();
}
static ID3D11Device *tDev; static ID3D11DeviceContext *tCtx;
static void Frame(IDXGISwapChain *sc) {
    ID3D11Texture2D *bb = nullptr; sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bb);
    ID3D11RenderTargetView *rtv = nullptr; tDev->CreateRenderTargetView(bb, nullptr, &rtv);
    const float col[4] = { 0.30f, 0.36f, 0.42f, 1 }; tCtx->ClearRenderTargetView(rtv, col);   // stand-in for the game picture
    rtv->Release(); bb->Release();
    OverlayFrame(sc);
}
int main(int argc, char **argv) {
    InitializeCriticalSection(&g_cs); InitializeCriticalSection(&g_viewCs);
    g_log = stdout; std::string out = argv[1];
    if (argc > 2 && !strcmp(argv[2], "en")) g_en = true;   // optional second argument: language
    WNDCLASSEXA wc{ sizeof wc }; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(nullptr); wc.lpszClassName = "EDFStatsTest";
    RegisterClassExA(&wc);
    RECT r{ 0, 0, 1600, 900 }; AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND w = CreateWindowExA(0, wc.lpszClassName, "EDFStats overlay test", WS_OVERLAPPEDWINDOW, 0, 0, r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    DXGI_SWAP_CHAIN_DESC d{}; d.BufferCount = 1; d.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.BufferDesc.Width = 1600; d.BufferDesc.Height = 900;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; d.OutputWindow = w; d.SampleDesc.Count = 1; d.Windowed = TRUE; d.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain *sc = nullptr; ID3D11Device *dev = nullptr; ID3D11DeviceContext *ctx = nullptr;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &d, &sc, &dev, nullptr, &ctx))) { puts("no d3d"); return 1; }
    tDev = dev; tCtx = ctx;

    // fake mission: three players
    g_missionStartMs = GetTickCount64() - 263000;
    PStats a; a.cls = "Engineer"; a.dmg = 14157; a.kills = 17; a.parts = 1; a.best = 266; a.taken = 394; a.deaths = 1; a.dist = 900; a.ffNpc = 531; a.selfDmg = 40;
    a.dps.Add(GetTickCount64() - 300, 520); a.dps.Add(GetTickCount64() - 100, 300); a.dps.Add(GetTickCount64() - 3000, 900);
    PStats b; b.cls = "HeavyArmor"; b.dmg = 13575; b.kills = 9; b.parts = 8; b.best = 60; b.taken = 40; b.dist = 716; b.heal = 0; b.dps.peak1 = 2497;
    PStats c; c.cls = "PaleWing"; c.dmg = 9100; c.kills = 21; c.best = 812; c.taken = 1200; c.deaths = 2; c.dist = 3400; c.heal = 300;
    LiveView v;
    v.rows.push_back({ MakeSummary(a, u8"Игрок 1"), true, false });
    Row rb{ MakeSummary(b, u8"Игрок 2"), false, false }; rb.s.ffPlayers = 150; v.rows.push_back(rb);
    v.rows.push_back({ MakeSummary(c, u8"Пикирующая Маша"), false, false });
    v.dps1 = a.dps.Window(GetTickCount64(), 10); v.dps5 = a.dps.Window(GetTickCount64(), 50) / 5; v.secs = 263; v.inMission = true;
    g_view = v;

    g_showLive = true; g_showBoard = false;
    for (int i = 0; i < 3; ++i) Frame(sc);
    SaveBmp(sc, (out + "live.bmp").c_str());
    g_view.ended = true; g_view.inMission = false; g_view.savedPath = "x"; g_autoBoard = true;
    for (int i = 0; i < 3; ++i) Frame(sc);
    SaveBmp(sc, (out + "board.bmp").c_str());
    printf("overlay frames saved, dead=%d\n", (int)g_ovDead);
    return 0;
}
