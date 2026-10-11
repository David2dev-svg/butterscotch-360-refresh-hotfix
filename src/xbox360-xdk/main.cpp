// Butterscotch Xbox 360 — XDK Entry Point
// Uses official Xbox 360 SDK: D3D9, XAudio2, XInputGetState

#include <xtl.h>
#include <d3d9.h>
#include <d3dx9.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>

// DbgPrint is a C-linkage kernel function — declare it explicitly since
// we compile .c files as C++ and removed extern "C" wrappers.
extern "C" ULONG __cdecl DbgPrint(const char* format, ...);

// Core headers — compiled as C++ alongside the .c files (via /TP flag)
#include "runner.h"
#include "runner_keyboard.h"
#include "vm.h"
#include "data_win.h"
#include "json_reader.h"
#include "utils.h"
#include "stb_ds.h"

#include "d3d9_renderer.h"
#include "xaudio2_audio.h"
#include "xdk_file_system.h"
#include "debug_font/debug_font.h"
#include "stb_image.h"

// ===[ POSIX clock polyfill implementation ]===
double _xdk_monotonic_ms(void) {
    LARGE_INTEGER freq, counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / (double)freq.QuadPart * 1000.0;
}

// Screen dimensions (720p native)
#define SCREEN_WIDTH  1280
#define SCREEN_HEIGHT 720

static void diagLog(const char* fmt, ...);

static bool xdkGetWindowSize(int32_t* outW, int32_t* outH) {
    if (outW) *outW = SCREEN_WIDTH;
    if (outH) *outH = SCREEN_HEIGHT;
    return true;
}

static void xdkSetWindowSize(int32_t width, int32_t height) {
    diagLog("BS: window_set_size ignored on fixed 720p backbuffer requested=%dx%d", width, height);
}

static HANDLE gDiagLog = INVALID_HANDLE_VALUE;
static FILE* gDiagFile = NULL;
static bool gDiagTriedFallback = false;
static char gLastParseChunk[5] = "NONE";
static int gLastParseChunkIndex = -1;
static int gLastParseChunkTotal = 0;

struct LoadingVertex {
    float x, y, z, w;
    float u, v;
    float r, g, b, a;
};

typedef struct LoadingScreen {
    IDirect3DDevice9* dev;
    IDirect3DTexture9* splashTex;
    IDirect3DTexture9* fontTex;
    IDirect3DTexture9* whiteTex;
    IDirect3DVertexShader9* vertexShader;
    IDirect3DPixelShader9* pixelShader;
    IDirect3DVertexDeclaration9* vertexDecl;
    int splashW;
    int splashH;
    bool available;
    char stage[128];
} LoadingScreen;

static LoadingScreen gLoadingScreen;
static LoadingScreen gDiagOverlayScreen;

static bool gDiagOverlayVisible = false;
static bool gDiagOverlayComboWasDown = false;
static float gDiagOverlayFps = 0.0f;
static float gDiagOverlayDtMs = 0.0f;
static int gDiagOverlaySteps = 0;
static uint32_t gDiagOverlayFrameCount = 0;
static double gDiagOverlayWindowStart = 0.0;
static SIZE_T gDiagTotalPhys = 0;
static SIZE_T gDiagAvailPhys = 0;
static SIZE_T gDiagTotalVirtual = 0;
static SIZE_T gDiagAvailVirtual = 0;
static int gDiagControllerConnected = 0;
static int gDiagSpeedCapRemoved = 0;
static uint32_t gDiagRoomAgeFrames = 0;
static uint32_t gDiagRoomTransitionHolds = 0;

static bool diagOpenPath(const char* path, bool overwrite) {
    FILE* f = fopen(path, overwrite ? "wb" : "ab");
    if (f) {
        if (gDiagFile) fclose(gDiagFile);
        if (gDiagLog != INVALID_HANDLE_VALUE) {
            CloseHandle(gDiagLog);
            gDiagLog = INVALID_HANDLE_VALUE;
        }
        gDiagFile = f;
        {
            static bool sErrRedirected = false;
            const char* base = "bs360_refresh.log";
            size_t plen = strlen(path);
            size_t blen = strlen(base);
            if (!sErrRedirected && plen >= blen && plen < 480 && strcmp(path + plen - blen, base) == 0) {
                char errPath[512];
                memcpy(errPath, path, plen - blen);
                strcpy(errPath + plen - blen, "bs360_stderr.log");
                if (freopen(errPath, "wb", stderr)) {
                    setvbuf(stderr, NULL, _IONBF, 0);
                    fprintf(stderr, "STDERR: redirected\n");
                    sErrRedirected = true;
                }
            }
        }
        return true;
    }
    int crtErr = errno;

    HANDLE h = CreateFileA(path,
                           GENERIC_WRITE,
                           FILE_SHARE_READ,
                           NULL,
                           overwrite ? CREATE_ALWAYS : OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL,
                           NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DbgPrint("BS: open log failed at %s errno=%d gle=%lu\n", path, crtErr, GetLastError());
        return false;
    }
    if (!overwrite) SetFilePointer(h, 0, NULL, FILE_END);
    if (gDiagFile) {
        fclose(gDiagFile);
        gDiagFile = NULL;
    }
    if (gDiagLog != INVALID_HANDLE_VALUE) CloseHandle(gDiagLog);
    gDiagLog = h;
    return true;
}

static void diagLog(const char* fmt, ...) {
    char line[1024];
    va_list args;
    va_start(args, fmt);
    _vsnprintf(line, sizeof(line) - 2, fmt, args);
    va_end(args);
    line[sizeof(line) - 2] = '\0';

    size_t len = strlen(line);
    if (len == 0 || line[len - 1] != '\n') {
        line[len++] = '\n';
        line[len] = '\0';
    }

    DbgPrint("%s", line);
    if (gDiagFile) {
        fputs(line, gDiagFile);
        fflush(gDiagFile);
    }
    if (gDiagLog != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(gDiagLog, line, (DWORD)strlen(line), &written, NULL);
        FlushFileBuffers(gDiagLog);
    }
}

static void diagOpenFallback(void) {
    if (gDiagLog != INVALID_HANDLE_VALUE || gDiagFile || gDiagTriedFallback) return;
    gDiagTriedFallback = true;

    static const char* paths[] = {
        "game:\\bs360_refresh.log",
        "d:\\bs360_refresh.log",
        "hdd:\\bs360_refresh.log",
        "cache:\\bs360_refresh.log",
        "uda:\\bs360_refresh.log",
        "uda:/bs360_refresh.log",
        "usb0:\\bs360_refresh.log",
        NULL,
    };
    for (int i = 0; paths[i]; i++) {
        if (diagOpenPath(paths[i], true)) {
            diagLog("BS: fallback log opened at %s", paths[i]);
            return;
        }
    }
    DbgPrint("BS: WARNING: no writable diagnostic log path found\n");
}

static void diagOpenNextToDataWin(const char* dataWinPath) {
    if (gDiagFile || gDiagLog != INVALID_HANDLE_VALUE) {
        diagLog("BS: keeping existing log while data.win is at %s", dataWinPath);
        return;
    }

    char logPath[512];
    const char* lastSlash = strrchr(dataWinPath, '\\');
    if (!lastSlash) lastSlash = strrchr(dataWinPath, '/');
    if (lastSlash) {
        size_t dirLen = (size_t)(lastSlash - dataWinPath + 1);
        if (dirLen >= sizeof(logPath) - 32) return;
        memcpy(logPath, dataWinPath, dirLen);
        strcpy(logPath + dirLen, "bs360_refresh.log");
    } else {
        strcpy(logPath, "bs360_refresh.log");
    }

    if (diagOpenPath(logPath, true)) {
        diagLog("BS: logging to %s", logPath);
    } else {
        diagLog("BS: WARNING: failed to open log next to data.win at %s gle=%lu", logPath, GetLastError());
    }
}

static void loadingSetVertex(LoadingVertex* v, float x, float y, float u, float vv,
                             float r, float g, float b, float a) {
    v->x = x - 0.5f;
    v->y = y - 0.5f;
    v->z = 0.0f;
    v->w = 1.0f;
    v->u = u;
    v->v = vv;
    v->r = r;
    v->g = g;
    v->b = b;
    v->a = a;
}

static void loadingApplyState(LoadingScreen* ls) {
    IDirect3DDevice9* dev = ls->dev;
    D3DVIEWPORT9 vp;
    vp.X = 0;
    vp.Y = 0;
    vp.Width = SCREEN_WIDTH;
    vp.Height = SCREEN_HEIGHT;
    vp.MinZ = 0.0f;
    vp.MaxZ = 1.0f;
    dev->SetViewport(&vp);
    dev->SetVertexShader(ls->vertexShader);
    dev->SetPixelShader(ls->pixelShader);
    dev->SetVertexDeclaration(ls->vertexDecl);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_VIEWPORTENABLE, FALSE);
    for (DWORD sampler = 0; sampler < 8; sampler++) {
        dev->SetSamplerState(sampler, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(sampler, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(sampler, D3DSAMP_MIPFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(sampler, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(sampler, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    }
}

static IDirect3DTexture9* loadingCreateTextureFromRgba(IDirect3DDevice9* dev, const uint8_t* pixels, int w, int h) {
    IDirect3DTexture9* tex = NULL;
    if (FAILED(dev->CreateTexture(w, h, 1, 0, D3DFMT_LIN_A8R8G8B8, D3DPOOL_DEFAULT, &tex, NULL)) || !tex) {
        return NULL;
    }

    D3DLOCKED_RECT lr;
    if (FAILED(tex->LockRect(0, &lr, NULL, 0))) {
        tex->Release();
        return NULL;
    }

    for (int y = 0; y < h; y++) {
        const uint8_t* src = pixels + y * w * 4;
        DWORD* dst = (DWORD*)((uint8_t*)lr.pBits + y * lr.Pitch);
        for (int x = 0; x < w; x++) {
            uint8_t r = src[x * 4 + 0];
            uint8_t g = src[x * 4 + 1];
            uint8_t b = src[x * 4 + 2];
            uint8_t a = src[x * 4 + 3];
            if (a == 0) { r = 0; g = 0; b = 0; }
            dst[x] = D3DCOLOR_ARGB(a, r, g, b);
        }
    }

    tex->UnlockRect(0);
    return tex;
}

static IDirect3DTexture9* loadingLoadPng(IDirect3DDevice9* dev, const char* path, int* outW, int* outH) {
    int w = 0;
    int h = 0;
    int channels = 0;
    uint8_t* pixels = stbi_load(path, &w, &h, &channels, 4);
    if (!pixels) return NULL;
    IDirect3DTexture9* tex = loadingCreateTextureFromRgba(dev, pixels, w, h);
    stbi_image_free(pixels);
    if (tex) {
        *outW = w;
        *outH = h;
    }
    return tex;
}

static IDirect3DTexture9* loadingCreateFontTexture(IDirect3DDevice9* dev) {
    uint8_t* rgba = (uint8_t*)malloc(DEBUGFONT_ATLAS_W * DEBUGFONT_ATLAS_H * 4);
    if (!rgba) return NULL;
    for (int i = 0; i < DEBUGFONT_ATLAS_W * DEBUGFONT_ATLAS_H; i++) {
        uint8_t a = debugFontPixels[i];
        rgba[i * 4 + 0] = 255;
        rgba[i * 4 + 1] = 255;
        rgba[i * 4 + 2] = 255;
        rgba[i * 4 + 3] = a;
    }
    IDirect3DTexture9* tex = loadingCreateTextureFromRgba(dev, rgba, DEBUGFONT_ATLAS_W, DEBUGFONT_ATLAS_H);
    free(rgba);
    return tex;
}

static bool loadingInit(LoadingScreen* ls, IDirect3DDevice9* dev, const char* dataWinPath) {
    memset(ls, 0, sizeof(*ls));
    ls->dev = dev;
    strcpy(ls->stage, "Starting");

    static const char* vs =
        "struct VS_IN  { float4 Pos : POSITION; float2 Tex : TEXCOORD0; float4 Col : TEXCOORD1; };\n"
        "struct VS_OUT { float4 Pos : POSITION; float2 Tex : TEXCOORD0; float4 Col : TEXCOORD1; };\n"
        "VS_OUT main(VS_IN i) { VS_OUT o; o.Pos = i.Pos; o.Tex = i.Tex; o.Col = i.Col; return o; }\n";
    static const char* ps =
        "sampler2D s0 : register(s0) = sampler_state { MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; AddressU = CLAMP; AddressV = CLAMP; };\n"
        "struct PS_IN { float2 Tex : TEXCOORD0; float4 Col : TEXCOORD1; };\n"
        "float4 main(PS_IN i) : COLOR0 { return tex2D(s0, i.Tex) * i.Col; }\n";

    ID3DXBuffer* code = NULL;
    ID3DXBuffer* err = NULL;
    HRESULT hr = D3DXCompileShader(vs, (UINT)strlen(vs), NULL, NULL, "main", "vs_2_0", 0, &code, &err, NULL);
    if (FAILED(hr)) {
        if (err) err->Release();
        diagLog("LOAD: vertex shader compile failed hr=0x%08X", hr);
        return false;
    }
    dev->CreateVertexShader((const DWORD*)code->GetBufferPointer(), &ls->vertexShader);
    code->Release();

    hr = D3DXCompileShader(ps, (UINT)strlen(ps), NULL, NULL, "main", "ps_2_0", 0, &code, &err, NULL);
    if (FAILED(hr)) {
        if (err) err->Release();
        diagLog("LOAD: pixel shader compile failed hr=0x%08X", hr);
        return false;
    }
    dev->CreatePixelShader((const DWORD*)code->GetBufferPointer(), &ls->pixelShader);
    code->Release();

    static const D3DVERTEXELEMENT9 decl[] = {
        { 0,  0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        { 0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
        { 0, 24, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1 },
        D3DDECL_END()
    };
    if (FAILED(dev->CreateVertexDeclaration(decl, &ls->vertexDecl))) {
        diagLog("LOAD: vertex declaration failed");
        return false;
    }

    ls->fontTex = loadingCreateFontTexture(dev);
    if (!ls->fontTex) diagLog("LOAD: debug font texture failed");
    {
        uint8_t whitePixel[4] = { 255, 255, 255, 255 };
        ls->whiteTex = loadingCreateTextureFromRgba(dev, whitePixel, 1, 1);
    }

    char splashPath[512];
    const char* lastSlash = strrchr(dataWinPath, '\\');
    if (!lastSlash) lastSlash = strrchr(dataWinPath, '/');
    if (lastSlash) {
        size_t dirLen = (size_t)(lastSlash - dataWinPath + 1);
        if (dirLen < sizeof(splashPath) - 16) {
            memcpy(splashPath, dataWinPath, dirLen);
            strcpy(splashPath + dirLen, "splash.png");
            ls->splashTex = loadingLoadPng(dev, splashPath, &ls->splashW, &ls->splashH);
        }
    }
    diagLog("LOAD: splash %s", ls->splashTex ? "loaded" : "not found");

    ls->available = (ls->vertexShader && ls->pixelShader && ls->vertexDecl);
    return ls->available;
}

static void loadingDestroy(LoadingScreen* ls) {
    if (ls->dev) {
        ls->dev->SetTexture(0, NULL);
        ls->dev->SetVertexShader(NULL);
        ls->dev->SetPixelShader(NULL);
        ls->dev->SetVertexDeclaration(NULL);
    }
    if (ls->splashTex) ls->splashTex->Release();
    if (ls->fontTex) ls->fontTex->Release();
    if (ls->whiteTex) ls->whiteTex->Release();
    if (ls->vertexShader) ls->vertexShader->Release();
    if (ls->pixelShader) ls->pixelShader->Release();
    if (ls->vertexDecl) ls->vertexDecl->Release();
    memset(ls, 0, sizeof(*ls));
}

static void loadingDrawQuad(LoadingScreen* ls, IDirect3DTexture9* tex,
                            float x0, float y0, float x1, float y1,
                            float u0, float v0, float u1, float v1,
                            float r, float g, float b, float a) {
    LoadingVertex verts[4];
    loadingSetVertex(&verts[0], x0, y0, u0, v0, r, g, b, a);
    loadingSetVertex(&verts[1], x1, y0, u1, v0, r, g, b, a);
    loadingSetVertex(&verts[2], x1, y1, u1, v1, r, g, b, a);
    loadingSetVertex(&verts[3], x0, y1, u0, v1, r, g, b, a);
    ls->dev->SetTexture(0, tex ? tex : ls->whiteTex);
    ls->dev->DrawPrimitiveUP(D3DPT_QUADLIST, 1, verts, sizeof(LoadingVertex));
}

static void loadingDrawText(LoadingScreen* ls, const char* text, float x, float y, float scale,
                            float r, float g, float b, float a) {
    if (!ls->fontTex || !text) return;
    float penX = x;
    for (const char* p = text; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch == '\n') {
            penX = x;
            y += (float)DEBUGFONT_LINE_HEIGHT * scale;
            continue;
        }
        if (ch < DEBUGFONT_FIRST_CP || ch > DEBUGFONT_LAST_CP) ch = '?';
        const DebugFontGlyphEntry* glyph = &debugFontGlyphs[ch - DEBUGFONT_FIRST_CP];
        float gx0 = penX + (float)glyph->xoffset * scale;
        float gy0 = y + (float)glyph->yoffset * scale;
        float gx1 = gx0 + (float)glyph->w * scale;
        float gy1 = gy0 + (float)glyph->h * scale;
        float u0 = ((float)glyph->x + 0.5f) / (float)DEBUGFONT_ATLAS_W;
        float v0 = ((float)glyph->y + 0.5f) / (float)DEBUGFONT_ATLAS_H;
        float u1 = ((float)glyph->x + (float)glyph->w - 0.5f) / (float)DEBUGFONT_ATLAS_W;
        float v1 = ((float)glyph->y + (float)glyph->h - 0.5f) / (float)DEBUGFONT_ATLAS_H;
        loadingDrawQuad(ls, ls->fontTex, gx0, gy0, gx1, gy1, u0, v0, u1, v1, r, g, b, a);
        penX += (float)glyph->xadvance * scale;
    }
}

static float loadingTextWidth(const char* text, float scale) {
    float w = 0.0f;
    if (!text) return w;
    for (const char* p = text; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch < DEBUGFONT_FIRST_CP || ch > DEBUGFONT_LAST_CP) ch = '?';
        w += (float)debugFontGlyphs[ch - DEBUGFONT_FIRST_CP].xadvance * scale;
    }
    return w;
}

static void loadingDraw(LoadingScreen* ls, float progress, const char* stage) {
    if (!ls || !ls->available) return;
    if (stage && stage[0]) {
        _snprintf(ls->stage, sizeof(ls->stage) - 1, "%s", stage);
        ls->stage[sizeof(ls->stage) - 1] = '\0';
    }
    if (progress < 0.0f) progress = 0.0f;
    if (progress > 1.0f) progress = 1.0f;

    IDirect3DDevice9* dev = ls->dev;
    dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
    if (FAILED(dev->BeginScene())) return;
    loadingApplyState(ls);

    if (ls->splashTex && ls->splashW > 0 && ls->splashH > 0) {
        float scaleX = (float)SCREEN_WIDTH / (float)ls->splashW;
        float scaleY = (float)SCREEN_HEIGHT / (float)ls->splashH;
        float scale = (scaleX < scaleY) ? scaleX : scaleY;
        float w = (float)ls->splashW * scale;
        float h = (float)ls->splashH * scale;
        float x = ((float)SCREEN_WIDTH - w) * 0.5f;
        float y = ((float)SCREEN_HEIGHT - h) * 0.5f;
        loadingDrawQuad(ls, ls->splashTex, x, y, x + w, y + h, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
    }

    float barW = 720.0f;
    float barH = 18.0f;
    float barX = ((float)SCREEN_WIDTH - barW) * 0.5f;
    float barY = (float)SCREEN_HEIGHT - 96.0f;
    loadingDrawQuad(ls, NULL, barX - 3.0f, barY - 3.0f, barX + barW + 3.0f, barY + barH + 3.0f,
                    0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.70f);
    loadingDrawQuad(ls, NULL, barX, barY, barX + barW, barY + barH,
                    0.0f, 0.0f, 1.0f, 1.0f, 0.12f, 0.12f, 0.12f, 0.95f);
    loadingDrawQuad(ls, NULL, barX, barY, barX + barW * progress, barY + barH,
                    0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 0.73f, 0.18f, 1.0f);

    float textScale = 0.42f;
    float textW = loadingTextWidth(ls->stage, textScale);
    loadingDrawText(ls, ls->stage, ((float)SCREEN_WIDTH - textW) * 0.5f, barY + 30.0f,
                    textScale, 1.0f, 1.0f, 1.0f, 0.92f);

    dev->EndScene();
    dev->Present(NULL, NULL, NULL, NULL);
}

static bool diagOverlayInit(IDirect3DDevice9* dev, const char* dataWinPath) {
    bool ok = loadingInit(&gDiagOverlayScreen, dev, dataWinPath);
    if (gDiagOverlayScreen.splashTex) {
        gDiagOverlayScreen.splashTex->Release();
        gDiagOverlayScreen.splashTex = NULL;
        gDiagOverlayScreen.splashW = 0;
        gDiagOverlayScreen.splashH = 0;
    }
    return ok && gDiagOverlayScreen.fontTex && gDiagOverlayScreen.whiteTex;
}

static void diagOverlayDrawLine(const char* text, float* y, float scale, float r, float g, float b, float a) {
    loadingDrawText(&gDiagOverlayScreen, text, 22.0f, *y, scale, r, g, b, a);
    *y += (float)DEBUGFONT_LINE_HEIGHT * scale + 3.0f;
}

static float diagBytesToMb(SIZE_T bytes) {
    return (float)((double)bytes / (1024.0 * 1024.0));
}

static void diagOverlayPollSystem(void) {
    MEMORYSTATUS ms;
    memset(&ms, 0, sizeof(ms));
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatus(&ms);
    gDiagTotalPhys = ms.dwTotalPhys;
    gDiagAvailPhys = ms.dwAvailPhys;
    gDiagTotalVirtual = ms.dwTotalVirtual;
    gDiagAvailVirtual = ms.dwAvailVirtual;
}

static void diagOverlayDraw(Runner* runner, Renderer* renderer, int32_t frameW, int32_t frameH) {
    if (!gDiagOverlayVisible || !runner || !gDiagOverlayScreen.available) return;

    if (renderer && renderer->vtable && renderer->vtable->flush) {
        renderer->vtable->flush(renderer);
    }

    loadingApplyState(&gDiagOverlayScreen);

    const char* roomName = "(none)";
    int32_t roomIndex = -1;
    uint32_t roomSpeed = 0;
    uint32_t roomW = 0;
    uint32_t roomH = 0;
    if (runner->currentRoom) {
        roomName = runner->currentRoom->name ? runner->currentRoom->name : "(null)";
        roomIndex = runner->currentRoomIndex;
        roomSpeed = runner->currentRoom->speed;
        roomW = runner->currentRoom->width;
        roomH = runner->currentRoom->height;
    }

    const float x0 = 12.0f;
    const float y0 = 12.0f;
    const float x1 = 560.0f;
    const float y1 = 222.0f;
    loadingDrawQuad(&gDiagOverlayScreen, NULL, x0, y0, x1, y1,
                    0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.72f);
    loadingDrawQuad(&gDiagOverlayScreen, NULL, x0, y0, x1, y0 + 3.0f,
                    0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 0.73f, 0.18f, 0.95f);

    char line[256];
    float y = y0 + 12.0f;
    diagOverlayDrawLine("Butterscotch360-Refresh DIAG  (LB+RB)", &y, 0.42f, 1.0f, 0.90f, 0.45f, 1.0f);

    _snprintf(line, sizeof(line) - 1, "FPS %.1f  dt %.2fms  steps %d  speed %u", gDiagOverlayFps, gDiagOverlayDtMs, gDiagOverlaySteps, roomSpeed);
    line[sizeof(line) - 1] = '\0';
    diagOverlayDrawLine(line, &y, 0.36f, 1.0f, 1.0f, 1.0f, 0.95f);

    _snprintf(line, sizeof(line) - 1, "Room %d: %s", roomIndex, roomName);
    line[sizeof(line) - 1] = '\0';
    diagOverlayDrawLine(line, &y, 0.36f, 1.0f, 1.0f, 1.0f, 0.95f);

    _snprintf(line, sizeof(line) - 1, "Room %ux%u  inst %d  pending %d", roomW, roomH, (int32_t)arrlen(runner->instances), runner->pendingRoom);
    line[sizeof(line) - 1] = '\0';
    diagOverlayDrawLine(line, &y, 0.36f, 0.82f, 0.92f, 1.0f, 0.95f);

    SIZE_T usedPhys = gDiagTotalPhys > gDiagAvailPhys ? (gDiagTotalPhys - gDiagAvailPhys) : 0;
    _snprintf(line, sizeof(line) - 1, "RAM %.1f/%.1f MB  free %.1f MB",
              diagBytesToMb(usedPhys), diagBytesToMb(gDiagTotalPhys),
              diagBytesToMb(gDiagAvailPhys));
    line[sizeof(line) - 1] = '\0';
    diagOverlayDrawLine(line, &y, 0.36f, 0.75f, 1.0f, 0.75f, 0.95f);

    _snprintf(line, sizeof(line) - 1, "Virt free %.1f MB  pad %d  fast %d  roomAge %u hold %u",
              diagBytesToMb(gDiagAvailVirtual), gDiagControllerConnected, gDiagSpeedCapRemoved,
              gDiagRoomAgeFrames, gDiagRoomTransitionHolds);
    line[sizeof(line) - 1] = '\0';
    diagOverlayDrawLine(line, &y, 0.36f, 0.75f, 1.0f, 0.75f, 0.95f);

    _snprintf(line, sizeof(line) - 1, "Game %dx%d  frame %dx%d  app %dx%d", SCREEN_WIDTH, SCREEN_HEIGHT, frameW, frameH, runner->applicationWidth, runner->applicationHeight);
    line[sizeof(line) - 1] = '\0';
    diagOverlayDrawLine(line, &y, 0.36f, 0.82f, 0.92f, 1.0f, 0.95f);

    _snprintf(line, sizeof(line) - 1, "GUI %dx%d  surf auto=%d keep=%d id=%d", runner->guiWidth, runner->guiHeight,
              runner->appSurfaceAutoDraw ? 1 : 0, runner->appSurfaceKeepWindowSize ? 1 : 0, runner->applicationSurfaceId);
    line[sizeof(line) - 1] = '\0';
    diagOverlayDrawLine(line, &y, 0.36f, 0.82f, 0.92f, 1.0f, 0.95f);
}

extern "C" void Butterscotch_xdkAbort(const char* file, int line) {
    diagOpenFallback();
    diagLog("BS: FATAL abort at %s:%d lastChunk=%s index=%d/%d", file ? file : "(null)", line, gLastParseChunk, gLastParseChunkIndex, gLastParseChunkTotal);
    for (;;) {
        Sleep(1000);
    }
}

extern "C" void Butterscotch_xdkDataWinTrace(const char* fmt, ...) {
    char line[1024];
    va_list args;
    va_start(args, fmt);
    _vsnprintf(line, sizeof(line) - 1, fmt, args);
    va_end(args);
    line[sizeof(line) - 1] = '\0';
    diagLog("DW: %s", line);
}

extern "C" void Butterscotch_xdkDiagTrace(const char* fmt, ...) {
    char line[1024];
    va_list args;
    va_start(args, fmt);
    _vsnprintf(line, sizeof(line) - 1, fmt, args);
    va_end(args);
    line[sizeof(line) - 1] = '\0';
    diagLog("%s", line);
}

static void dataWinParseProgress(const char* chunkName, int chunkIndex, int totalChunks, DataWin* dataWin, void* userData) {
    (void)dataWin;
    memcpy(gLastParseChunk, chunkName, 4);
    gLastParseChunk[4] = '\0';
    gLastParseChunkIndex = chunkIndex;
    gLastParseChunkTotal = totalChunks;
    diagLog("PARSE chunk %d/%d %.4s", chunkIndex + 1, totalChunks, chunkName);
    LoadingScreen* loading = (LoadingScreen*)userData;
    if (loading && loading->available) {
        char stage[128];
        _snprintf(stage, sizeof(stage) - 1, "Loading data.win: %.4s %d/%d", chunkName, chunkIndex + 1, totalChunks);
        stage[sizeof(stage) - 1] = '\0';
        float progress = (totalChunks > 0) ? ((float)(chunkIndex + 1) / (float)totalChunks) : 0.0f;
        loadingDraw(loading, progress * 0.82f, stage);
    }
}

static DataWin* parseDataWinGuarded(const char* dataWinPath, DataWinParserOptions parseOpts) {
    DataWin* dataWin = NULL;
    unsigned int exceptionCode = 0;
    #ifdef __clang__
        dataWin = DataWin_parse(dataWinPath, parseOpts);
    
#else
    __try {
        dataWin = DataWin_parse(dataWinPath, parseOpts);
    } __except (exceptionCode = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        diagLog("BS: FATAL exception 0x%08X during DataWin_parse lastChunk=%s index=%d/%d", exceptionCode, gLastParseChunk, gLastParseChunkIndex, gLastParseChunkTotal);
        dataWin = NULL;
    }
#endif
    return dataWin;
}

static bool initFirstRoomGuarded(Runner* runner) {
    unsigned int exceptionCode = 0;
    #ifdef __clang__
        Runner_initFirstRoom(runner);
    
#else
    __try {
        Runner_initFirstRoom(runner);
    } __except (exceptionCode = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        const char* roomName = "(none)";
        int roomIndex = -1;
        if (runner && runner->currentRoom) {
            roomName = runner->currentRoom->name ? runner->currentRoom->name : "(null)";
            roomIndex = runner->currentRoomIndex;
        }
        diagLog("BS: FATAL exception 0x%08X during Runner_initFirstRoom room=%d name=%s",
            exceptionCode, roomIndex, roomName);
        return false;
    }
#endif
    return true;
}

// ===[ Controller Mapping ]===

typedef struct {
    WORD xpadButton;
    int32_t gmlKey;
} XpadMapping;

static XpadMapping* xpadMappings = NULL;
static int xpadMappingCount = 0;
static WORD prevButtons = 0;
static BYTE prevLeftTrigger = 0;
static BYTE prevRightTrigger = 0;
static bool gamepadApiEnabled = false;

static void setupDefaultMappings(void) {
    static XpadMapping defaults[] = {
        { XINPUT_GAMEPAD_DPAD_UP,    38 },  // VK_UP
        { XINPUT_GAMEPAD_DPAD_DOWN,  40 },  // VK_DOWN
        { XINPUT_GAMEPAD_DPAD_LEFT,  37 },  // VK_LEFT
        { XINPUT_GAMEPAD_DPAD_RIGHT, 39 },  // VK_RIGHT
        { XINPUT_GAMEPAD_A,          13 },  // VK_RETURN (confirm)
        { XINPUT_GAMEPAD_B,          16 },  // VK_SHIFT (cancel)
        { XINPUT_GAMEPAD_X,          17 },  // VK_CONTROL
        { XINPUT_GAMEPAD_Y,          88 },  // 'X' key
        { XINPUT_GAMEPAD_START,      27 },  // VK_ESCAPE (menu)
        { XINPUT_GAMEPAD_BACK,       27 },  // VK_ESCAPE
    };
    xpadMappingCount = sizeof(defaults) / sizeof(XpadMapping);
    xpadMappings = (XpadMapping*)malloc(sizeof(defaults));
    memcpy(xpadMappings, defaults, sizeof(defaults));
}

static const char* osTypeName(YoYoOperatingSystem osType) {
    switch (osType) {
        case OS_WINDOWS: return "windows";
        case OS_XBOX360: return "xbox360";
        case OS_XBOXONE: return "xboxone";
        case OS_SWITCH: return "switch";
        case OS_PS4: return "ps4";
        case OS_PS3: return "ps3";
        default: return "unknown";
    }
}

static bool parseOsTypeName(const char* text, YoYoOperatingSystem* out) {
    if (!text || !out) return false;
    if (_stricmp(text, "windows") == 0 || _stricmp(text, "win32") == 0) {
        *out = OS_WINDOWS;
        return true;
    }
    if (_stricmp(text, "xbox360") == 0 || _stricmp(text, "x360") == 0) {
        *out = OS_XBOX360;
        return true;
    }
    if (_stricmp(text, "xboxone") == 0 || _stricmp(text, "xbone") == 0 || _stricmp(text, "xbox") == 0) {
        *out = OS_XBOXONE;
        return true;
    }
    if (_stricmp(text, "switch") == 0) {
        *out = OS_SWITCH;
        return true;
    }
    if (_stricmp(text, "ps4") == 0) {
        *out = OS_PS4;
        return true;
    }
    if (_stricmp(text, "ps3") == 0) {
        *out = OS_PS3;
        return true;
    }
    return false;
}

static float normalizeStickAxis(SHORT value) {
    if (value >= 0) return (float)value / 32767.0f;
    return (float)value / 32768.0f;
}

static void setGamepadButton(GamepadSlot* slot, int index, bool down) {
    if (index < 0 || index >= GP_BUTTON_COUNT) return;
    slot->buttonDown[index] = down;
    slot->buttonValue[index] = down ? 1.0f : 0.0f;
}

static void pollGamepadApi(Runner* runner, const XINPUT_STATE* state, WORD buttons, bool connected) {
    if (!runner || !runner->gamepads) return;

    RunnerGamepad_beginFrame(runner->gamepads);
    GamepadSlot* slot = &runner->gamepads->slots[0];
    memcpy(slot->buttonDownPrev, slot->buttonDown, sizeof(slot->buttonDown));
    memset(slot->buttonDown, 0, sizeof(slot->buttonDown));
    memset(slot->buttonValue, 0, sizeof(slot->buttonValue));
    memset(slot->axisValue, 0, sizeof(slot->axisValue));

    if (!connected || !state) {
        slot->connected = false;
        return;
    }

    static bool loggedGamepadConnected = false;
    slot->connected = true;
    slot->jid = 0;
    strncpy(slot->description, "Xbox 360 Controller", sizeof(slot->description) - 1);
    slot->description[sizeof(slot->description) - 1] = '\0';
    strncpy(slot->guid, "xinput-xbox360", sizeof(slot->guid) - 1);
    slot->guid[sizeof(slot->guid) - 1] = '\0';

    setGamepadButton(slot, 0,  (buttons & XINPUT_GAMEPAD_A) != 0);
    setGamepadButton(slot, 1,  (buttons & XINPUT_GAMEPAD_B) != 0);
    setGamepadButton(slot, 2,  (buttons & XINPUT_GAMEPAD_X) != 0);
    setGamepadButton(slot, 3,  (buttons & XINPUT_GAMEPAD_Y) != 0);
    setGamepadButton(slot, 4,  (buttons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0);
    setGamepadButton(slot, 5,  (buttons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0);
    setGamepadButton(slot, 8,  (buttons & XINPUT_GAMEPAD_BACK) != 0);
    setGamepadButton(slot, 9,  (buttons & XINPUT_GAMEPAD_START) != 0);
    setGamepadButton(slot, 10, (buttons & XINPUT_GAMEPAD_LEFT_THUMB) != 0);
    setGamepadButton(slot, 11, (buttons & XINPUT_GAMEPAD_RIGHT_THUMB) != 0);
    setGamepadButton(slot, 12, (buttons & XINPUT_GAMEPAD_DPAD_UP) != 0);
    setGamepadButton(slot, 13, (buttons & XINPUT_GAMEPAD_DPAD_DOWN) != 0);
    setGamepadButton(slot, 14, (buttons & XINPUT_GAMEPAD_DPAD_LEFT) != 0);
    setGamepadButton(slot, 15, (buttons & XINPUT_GAMEPAD_DPAD_RIGHT) != 0);

    slot->buttonValue[6] = (float)state->Gamepad.bLeftTrigger / 255.0f;
    slot->buttonValue[7] = (float)state->Gamepad.bRightTrigger / 255.0f;
    slot->buttonDown[6] = slot->buttonValue[6] >= slot->triggerThreshold;
    slot->buttonDown[7] = slot->buttonValue[7] >= slot->triggerThreshold;

    slot->axisValue[0] = normalizeStickAxis(state->Gamepad.sThumbLX);
    slot->axisValue[1] = -normalizeStickAxis(state->Gamepad.sThumbLY);
    slot->axisValue[2] = normalizeStickAxis(state->Gamepad.sThumbRX);
    slot->axisValue[3] = -normalizeStickAxis(state->Gamepad.sThumbRY);

    for (int i = 0; i < GP_BUTTON_COUNT; i++) {
        bool wasDown = slot->buttonDownPrev[i];
        if (slot->buttonDown[i] && !wasDown) slot->buttonPressed[i] = true;
        if (!slot->buttonDown[i] && wasDown) slot->buttonReleased[i] = true;
    }
    runner->gamepads->connectedCount = 1;
    if (!loggedGamepadConnected) {
        diagLog("BS: gamepad API slot0 connected desc=%s guid=%s", slot->description, slot->guid);
        loggedGamepadConnected = true;
    }
}

static void drawRunnerFrame(Runner* runner, Renderer* renderer, int32_t gameW, int32_t gameH) {
    int32_t frameW = gameW;
    int32_t frameH = gameH;
    if (runner && !runner->appSurfaceKeepWindowSize && !runner->appSurfaceAutoDraw && runner->currentRoom &&
        runner->currentRoom->width > 0 && runner->currentRoom->height > 0 &&
        runner->currentRoom->width < (uint32_t)frameW && runner->currentRoom->height < (uint32_t)frameH) {
        frameW = (int32_t)runner->currentRoom->width;
        frameH = (int32_t)runner->currentRoom->height;
    }

    float displayScaleX;
    float displayScaleY;
    runner->renderGameW = frameW;
    runner->renderGameH = frameH;
    Runner_drawPre(runner, SCREEN_WIDTH, SCREEN_HEIGHT);
    Runner_computeViewDisplayScale(runner, frameW, frameH, &displayScaleX, &displayScaleY);
    Runner_beginFrame(runner, frameW, frameH, SCREEN_WIDTH, SCREEN_HEIGHT);
    Runner_drawViews(runner, frameW, frameH, displayScaleX, displayScaleY, false);
    renderer->vtable->endFrameInit(renderer);
    Runner_drawPost(runner, SCREEN_WIDTH, SCREEN_HEIGHT);
    Runner_drawGUI(runner, SCREEN_WIDTH, SCREEN_HEIGHT, frameW, frameH);
    diagOverlayDraw(runner, renderer, frameW, frameH);
    renderer->vtable->endFrameEnd(renderer);
}

// ===[ Main Entry Point ]===

// splits launch parameters into argv[1..] (quotes supported); argv[0] is kept
static void xdkSplitLaunchParams(const char* params, char* store, size_t storeSize, char** argv, int* argc, int maxArgs) {
    int n = 1;
    size_t w = 0;
    const char* c = params;
    while (*c && n < maxArgs) {
        while (*c == ' ' || *c == '\t') c++;
        if (!*c) break;
        char* start = store + w;
        bool quoted = false;
        if (*c == '"') { quoted = true; c++; }
        while (*c && w + 1 < storeSize) {
            if (quoted ? (*c == '"') : (*c == ' ' || *c == '\t')) break;
            store[w++] = *c++;
        }
        if (quoted && *c == '"') c++;
        store[w++] = 0;
        argv[n++] = start;
    }
    *argc = n;
}

// collapses "." and ".." segments of a backslash path, in place
static void xdkNormalizePath(char* p) {
    char tmp[512];
    char* parts[64];
    char* keepParts[64];
    int n = 0;
    int keep = 0;
    strncpy(tmp, p, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = 0;
    char* s = tmp;
    while (n < 64) {
        parts[n++] = s;
        char* b = strchr(s, '\\');
        if (!b) break;
        *b = 0;
        s = b + 1;
    }
    for (int i = 0; i < n; i++) {
        if (parts[i][0] == 0 && i > 0) continue;
        if (strcmp(parts[i], ".") == 0) continue;
        if (strcmp(parts[i], "..") == 0) { if (keep > 1) keep--; continue; }
        keepParts[keep++] = parts[i];
    }
    p[0] = 0;
    for (int i = 0; i < keep; i++) {
        if (i > 0) strcat(p, "\\");
        strcat(p, keepParts[i]);
    }
}

// game_change: builds <parent of current data.win>\<workingDir>\<file from "-game file">
static char* xdkBuildGameChangePath(const char* currentPath, const char* workingDir, const char* launchParams) {
    const char* g = strstr(launchParams, "-game");
    if (!g) return NULL;
    g += 5;
    while (*g == ' ' || *g == '\t') g++;
    if (*g == '"') g++;
    char file[260];
    size_t n = 0;
    while (*g && *g != ' ' && *g != '\t' && *g != '"' && n < sizeof(file) - 1) file[n++] = *g++;
    file[n] = 0;
    if (n == 0) return NULL;

    char parent[512];
    strncpy(parent, currentPath, sizeof(parent) - 1);
    parent[sizeof(parent) - 1] = 0;
    char* ls = strrchr(parent, '\\');
    char* lb = strrchr(parent, '/');
    char* sep = (ls > lb) ? ls : lb;
    if (sep) *sep = 0; else { parent[0] = '.'; parent[1] = 0; }

    char wd[260];
    strncpy(wd, workingDir, sizeof(wd) - 1);
    wd[sizeof(wd) - 1] = 0;
    char* w = wd;
    while (*w == '/' || *w == '\\') w++;
    size_t wl = strlen(w);
    while (wl > 0 && (w[wl - 1] == '/' || w[wl - 1] == '\\')) w[--wl] = 0;

    size_t len = strlen(parent) + strlen(w) + strlen(file) + 3;
    char* out = (char*) malloc(len);
    if (!out) return NULL;
    if (wl > 0) _snprintf(out, len, "%s\\%s\\%s", parent, w, file);
    else _snprintf(out, len, "%s\\%s", parent, file);
    out[len - 1] = 0;
    for (char* c = out; *c; c++) if (*c == '/') *c = '\\';
    xdkNormalizePath(out);
    return out;
}

VOID __cdecl main() {
    diagOpenFallback();
    diagLog("BUILD parse_guard_diag_v2 %s %s", __DATE__, __TIME__);
    diagLog("BS: guard v2 active; log is overwritten on each launch");
    diagLog("BS: 01 main() entered");

    IDirect3D9* pD3D = Direct3DCreate9(D3D_SDK_VERSION);
    if (!pD3D) { diagLog("BS: FATAL: D3D create failed"); return; }
    diagLog("BS: 02 D3D9 created");

    D3DPRESENT_PARAMETERS d3dpp;
    ZeroMemory(&d3dpp, sizeof(d3dpp));
    d3dpp.BackBufferWidth        = SCREEN_WIDTH;
    d3dpp.BackBufferHeight       = SCREEN_HEIGHT;
    d3dpp.BackBufferFormat       = D3DFMT_A8R8G8B8;
    d3dpp.FrontBufferFormat      = D3DFMT_LE_X8R8G8B8;
    d3dpp.MultiSampleType        = D3DMULTISAMPLE_NONE;
    d3dpp.BackBufferCount        = 1;
    d3dpp.EnableAutoDepthStencil = TRUE;
    d3dpp.AutoDepthStencilFormat = D3DFMT_D24S8;
    d3dpp.SwapEffect             = D3DSWAPEFFECT_DISCARD;
    d3dpp.PresentationInterval   = D3DPRESENT_INTERVAL_ONE;

    IDirect3DDevice9* pd3dDevice = NULL;
    HRESULT hr = pD3D->CreateDevice(0, D3DDEVTYPE_HAL, NULL,
                                    D3DCREATE_HARDWARE_VERTEXPROCESSING,
                                    &d3dpp, &pd3dDevice);
    if (FAILED(hr)) {
        diagLog("BS: FATAL: CreateDevice failed hr=0x%08X", hr);
        return;
    }
    diagLog("BS: 03 D3D device created");


    // ===[ Locate data.win ]===
    // On Xbox 360, game content is at game:\ (DVD/HDD) or d:\ (dev kit)
    const char* dataWinPath = NULL;

    // Try multiple paths. Use fopen() for detection since it goes through
    // the Xbox CRT which handles game:\ paths reliably on both hardware and emulators.
    static const char* searchPaths[] = {
        "game:\\data.win",
        "game:\\butterscotch\\data.win",
        "d:\\data.win",
        "d:\\butterscotch\\data.win",
        NULL,
    };

    diagLog("BS: 04 searching for data.win");
    char msg[256];
    for (int i = 0; searchPaths[i]; i++) {
        diagLog("BS: try %s", searchPaths[i]);

        FILE* testFile = fopen(searchPaths[i], "rb");
        if (testFile) {
            fclose(testFile);
            dataWinPath = searchPaths[i];
            diagOpenNextToDataWin(dataWinPath);
            diagLog("BS: 05 found data.win at %s", dataWinPath);
            break;
        }
    }

    if (!dataWinPath) {
        diagLog("BS: FATAL: data.win not found");
        for (;;) { } // hang instead of crash for debugging
    }

    char* gcOwnedPath = NULL;
    char gcArgStore[1024];
    char* gcArgv[32];
    int gcArgc = 1;
    gcArgv[0] = (char*)"default.xex";
    for (;;) {  // one iteration per game (game_change reloads here)
    bool loadingOk = loadingInit(&gLoadingScreen, pd3dDevice, dataWinPath);
    if (loadingOk) loadingDraw(&gLoadingScreen, 0.02f, "Starting Butterscotch360");

    diagLog("BS: 06 parsing data.win");
    if (loadingOk) loadingDraw(&gLoadingScreen, 0.05f, "Opening data.win");

    DataWinParserOptions parseOpts;
    memset(&parseOpts, 0, sizeof(parseOpts));
    parseOpts.parseGen8 = true;  parseOpts.parseOptn = true;  parseOpts.parseLang = true;
    parseOpts.parseExtn = true;  parseOpts.parseSond = true;  parseOpts.parseAgrp = true;
    parseOpts.parseSprt = true;  parseOpts.parseBgnd = true;  parseOpts.parsePath = true;
    parseOpts.parseScpt = true;  parseOpts.parseGlob = true;  parseOpts.parseShdr = true;
    parseOpts.parseFont = true;  parseOpts.parseTmln = true;  parseOpts.parseObjt = true;
    parseOpts.parseRoom = true;  parseOpts.parseTpag = true;  parseOpts.parseCode = true;
    parseOpts.parseVari = true;  parseOpts.parseFunc = true;  parseOpts.parseStrg = true;
    parseOpts.parseTxtr = true;  parseOpts.parseAudo = true;
    parseOpts.skipLoadingPreciseMasksForNonPreciseSprites = true;
    parseOpts.progressCallback = dataWinParseProgress;
    parseOpts.progressCallbackUserData = loadingOk ? &gLoadingScreen : NULL;
    DataWin* dataWin = parseDataWinGuarded(dataWinPath, parseOpts);

    if (!dataWin) {
        diagLog("BS: FATAL: DataWin_parse returned NULL");
        for (;;) { }
    }
    diagLog("BS: 07 data.win parsed OK");
    if (loadingOk) {
        loadingDraw(&gLoadingScreen, 1.0f, "data.win loaded");
        loadingDestroy(&gLoadingScreen);
        loadingOk = false;
    }
    if (diagOverlayInit(pd3dDevice, dataWinPath)) {
        diagLog("DIAG: overlay renderer ready; toggle with LB+RB");
    } else {
        diagLog("DIAG: overlay renderer unavailable");
    }

    diagLog("BS: game=%s", dataWin->gen8.displayName ? dataWin->gen8.displayName : "Unknown");

    // ===[ Load CONFIG.JSN (optional) ]===
    char configPath[512];
    const char* lastSlash = strrchr(dataWinPath, '\\');
    if (!lastSlash) lastSlash = strrchr(dataWinPath, '/');
    if (lastSlash) {
        size_t dirLen = (size_t)(lastSlash - dataWinPath + 1);
        memcpy(configPath, dataWinPath, dirLen);
        sprintf(configPath + dirLen, "CONFIG.JSN");
    } else {
        strcpy(configPath, "CONFIG.JSN");
    }

    JsonValue* configRoot = NULL;
    HANDLE hConfig = CreateFileA(configPath, GENERIC_READ, FILE_SHARE_READ,
                                 NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hConfig != INVALID_HANDLE_VALUE) {
        DWORD configSize = GetFileSize(hConfig, NULL);
        char* configText = (char*)malloc(configSize + 1);
        DWORD bytesRead;
        ReadFile(hConfig, configText, configSize, &bytesRead, NULL);
        CloseHandle(hConfig);
        configText[bytesRead] = '\0';
        configRoot = JsonReader_parse(configText);
        free(configText);
        diagLog("BS: Loaded CONFIG.JSN");
    }

    // ===[ Create Subsystems ]===
    diagLog("BS: 08 creating subsystems");
    XdkFileSystem* xdkFs = XdkFileSystem_create(dataWinPath);
    FileSystem* fileSystem = (FileSystem*)xdkFs;

    diagLog("BS: 09 creating renderer");
    Renderer* renderer = D3D9Renderer_create(pd3dDevice);

    diagLog("BS: 10 creating audio");
    XdkAudioSystem* xdkAudio = XdkAudioSystem_create();
    AudioSystem* audioSystem = (AudioSystem*)xdkAudio;

    diagLog("BS: 11 creating VM");
    VMContext* vm = VM_create(dataWin);
    diagLog("BS: 12 creating runner");
    Runner* runner = Runner_create(dataWin, vm, renderer, fileSystem, audioSystem);
    runner->getWindowSize = xdkGetWindowSize;
    runner->setWindowSize = xdkSetWindowSize;
    runner->osType = OS_WINDOWS;
    diagLog("BS: 12b runner created with renderer/audio initialized");
    Runner_setGameArgs(runner, gcArgv, (int32_t) gcArgc);
    diagLog("BS: game args count=%d", gcArgc);

    // Parse CONFIG.JSN options
    if (configRoot) {
        JsonValue* osTypeVal = JsonReader_getObject(configRoot, "osType");
        if (osTypeVal && JsonReader_isString(osTypeVal)) {
            YoYoOperatingSystem configuredOsType;
            const char* osText = JsonReader_getString(osTypeVal);
            if (parseOsTypeName(osText, &configuredOsType)) {
                runner->osType = configuredOsType;
            } else {
                diagLog("CONFIG.JSN: unknown osType '%s', keeping %s", osText, osTypeName(runner->osType));
            }
        }

        JsonValue* disabledArr = JsonReader_getObject(configRoot, "disabledObjects");
        if (disabledArr && JsonReader_isArray(disabledArr)) {
            sh_new_strdup(runner->disabledObjects);
            int count = JsonReader_arrayLength(disabledArr);
            for (int i = 0; i < count; i++) {
                JsonValue* elem = JsonReader_getArrayElement(disabledArr, i);
                if (elem && JsonReader_isString(elem)) {
                    const char* name = JsonReader_getString(elem);
                    shput(runner->disabledObjects, name, 1);
                }
            }
        }

        JsonValue* mappingsObj = JsonReader_getObject(configRoot, "controllerMappings");
        if (mappingsObj && JsonReader_isObject(mappingsObj)) {
            xpadMappingCount = JsonReader_objectLength(mappingsObj);
            xpadMappings = (XpadMapping*)malloc(sizeof(XpadMapping) * xpadMappingCount);
            for (int i = 0; i < xpadMappingCount; i++) {
                const char* btnStr = JsonReader_getObjectKey(mappingsObj, i);
                JsonValue* gmlVal = JsonReader_getObjectValue(mappingsObj, i);
                xpadMappings[i].xpadButton = (WORD)atoi(btnStr);
                xpadMappings[i].gmlKey = (int32_t)JsonReader_getInt(gmlVal);
            }
        }

        JsonValue* gamepadApiVal = JsonReader_getObject(configRoot, "gamepadApi");
        if (gamepadApiVal) {
            if (JsonReader_isBool(gamepadApiVal)) {
                gamepadApiEnabled = JsonReader_getBool(gamepadApiVal);
            } else if (JsonReader_isObject(gamepadApiVal)) {
                JsonValue* enabledVal = JsonReader_getObject(gamepadApiVal, "enabled");
                if (enabledVal) gamepadApiEnabled = JsonReader_getBool(enabledVal);
            }
        }
    }

    if (!xpadMappings) setupDefaultMappings();
    diagLog("BS: osType=%s (%d)", osTypeName(runner->osType), (int)runner->osType);
    diagLog("BS: gamepadApi=%s", gamepadApiEnabled ? "enabled" : "disabled");

    diagLog("BS: 13 audio OK");
    diagLog("BS: 14 renderer already initialized by Runner_create");
    diagLog("BS: 15 renderer OK");

    // Initialize first room
    diagLog("BS: 16 init first room");
    if (dataWin->gen8.roomOrderCount > 0) {
        int32_t firstRoomIndex = dataWin->gen8.roomOrder[0];
        if (firstRoomIndex >= 0 && dataWin->room.count > (uint32_t)firstRoomIndex) {
            Room* firstRoom = &dataWin->room.rooms[firstRoomIndex];
            diagLog("BS: first room idx=%d name=%s size=%ux%u objects=%u layers=%u tiles=%u",
                firstRoomIndex,
                firstRoom->name ? firstRoom->name : "(null)",
                firstRoom->width,
                firstRoom->height,
                firstRoom->gameObjectCount,
                firstRoom->layerCount,
                firstRoom->tileCount);
        } else {
            diagLog("BS: first room index out of range idx=%d roomCount=%u",
                firstRoomIndex, dataWin->room.count);
        }
    } else {
        diagLog("BS: no room order entries");
    }
    if (!initFirstRoomGuarded(runner)) {
        for (;;) { Sleep(1000); }
    }
    diagLog("BS: 17 first room OK");

    Gen8* gen8 = &dataWin->gen8;
    int32_t gameW = (int32_t)gen8->defaultWindowWidth;
    int32_t gameH = (int32_t)gen8->defaultWindowHeight;
    diagLog("BS: gameW=%d gameH=%d screenW=%d screenH=%d", gameW, gameH, SCREEN_WIDTH, SCREEN_HEIGHT);

    // Parse deferDrawToAfterAllSteps
    bool deferDraw = false;
    if (configRoot) {
        JsonValue* deferVal = JsonReader_getObject(configRoot, "deferDrawToAfterAllSteps");
        if (deferVal) deferDraw = JsonReader_getBool(deferVal);
    }

    diagLog("BS: 18 entering main loop");

    // ===[ Main Loop ]===
    LARGE_INTEGER freq, lastTime, currentTime;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&lastTime);
    LARGE_INTEGER startTime = lastTime;
    LARGE_INTEGER lastHeartbeatTime = lastTime;
    LARGE_INTEGER lastDiagSystemPollTime = lastTime;
    double accumulator = 0.0;
    uint32_t heartbeatFrame = 0;
    int32_t lastRoomId = runner->currentRoomIndex;
    diagOverlayPollSystem();

    while (!runner->shouldExit && runner->pendingWorkingDirectory == NULL) {
        QueryPerformanceCounter(&currentTime);
        double deltaTime = (double)(currentTime.QuadPart - lastTime.QuadPart) / (double)freq.QuadPart;
        lastTime = currentTime;

        // ===[ Poll Controller ]===
        XINPUT_STATE state;
        ZeroMemory(&state, sizeof(state));
        DWORD xinputResult = XInputGetState(0, &state);
        WORD buttons = 0;
        bool controllerConnected = xinputResult == ERROR_SUCCESS;
        gDiagControllerConnected = controllerConnected ? 1 : 0;
        if (controllerConnected) {
            buttons = state.Gamepad.wButtons;

            // Left thumbstick as dpad
            #define STICK_DEADZONE 16384
            SHORT lx = state.Gamepad.sThumbLX;
            SHORT ly = state.Gamepad.sThumbLY;
            if (lx < -STICK_DEADZONE) buttons |= XINPUT_GAMEPAD_DPAD_LEFT;
            if (lx >  STICK_DEADZONE) buttons |= XINPUT_GAMEPAD_DPAD_RIGHT;
            if (ly >  STICK_DEADZONE) buttons |= XINPUT_GAMEPAD_DPAD_UP;
            if (ly < -STICK_DEADZONE) buttons |= XINPUT_GAMEPAD_DPAD_DOWN;
        }

        if (gamepadApiEnabled) {
            pollGamepadApi(runner, &state, buttons, controllerConnected);
        }

        bool diagComboDown = controllerConnected &&
            ((buttons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0) &&
            ((buttons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0);
        if (diagComboDown && !gDiagOverlayComboWasDown) {
            gDiagOverlayVisible = !gDiagOverlayVisible;
            diagLog("DIAG: overlay %s", gDiagOverlayVisible ? "on" : "off");
        }
        gDiagOverlayComboWasDown = diagComboDown;

        for (int i = 0; i < xpadMappingCount; i++) {
            WORD mask = xpadMappings[i].xpadButton;
            int32_t gmlKey = xpadMappings[i].gmlKey;

            bool wasPressed = (prevButtons & mask) != 0;
            bool isPressed = (buttons & mask) != 0;

            if (isPressed && !wasPressed)
                RunnerKeyboard_onKeyDown(runner->keyboard, gmlKey);
            else if (!isPressed && wasPressed)
                RunnerKeyboard_onKeyUp(runner->keyboard, gmlKey);
        }
        prevButtons = buttons;
        prevRightTrigger = controllerConnected ? state.Gamepad.bRightTrigger : 0;

        bool speedCapRemoved = prevRightTrigger > 128;
        gDiagSpeedCapRemoved = speedCapRemoved ? 1 : 0;

        // ===[ Frame Pacing ]===
        uint32_t roomSpeed = runner->currentRoom->speed;
        double targetFrameTime = (roomSpeed > 0) ? (1.0 / roomSpeed) : (1.0 / 60.0);

        if (deltaTime > targetFrameTime * 2.0) {
            accumulator = targetFrameTime;
            diagLog("TIMING: dropped catch-up dt=%.3f target=%.3f room=%d",
                deltaTime, targetFrameTime, runner->currentRoom ? runner->currentRoomIndex : -1);
        } else {
            accumulator += deltaTime;
            double maxAccumulator = targetFrameTime * 2.0;
            if (accumulator > maxAccumulator) accumulator = maxAccumulator;
        }
        if (speedCapRemoved && targetFrameTime > accumulator) accumulator = targetFrameTime;

        int gameFramesRan = 0;
        bool heldRoomTransitionFrame = false;
        while (accumulator >= targetFrameTime) {
            if (gameFramesRan > 0)
                RunnerKeyboard_beginFrame(runner->keyboard);

            Runner_step(runner);
            bool roomChangedThisStep = false;
            if (runner->currentRoom && runner->currentRoomIndex != lastRoomId) {
                lastRoomId = runner->currentRoomIndex;
                gDiagRoomAgeFrames = 0;
                roomChangedThisStep = true;
                diagLog("ROOM_CHANGED id=%d name=%s", lastRoomId, runner->currentRoom->name ? runner->currentRoom->name : "(null)");
                XdkAudioSystem_onRoomChanged(runner->audioSystem, lastRoomId, runner->currentRoom->name);
            }

            if (!deferDraw) {
                if (roomChangedThisStep && runner->appSurfaceKeepWindowSize) {
                    heldRoomTransitionFrame = true;
                    gDiagRoomTransitionHolds++;
                } else {
                    drawRunnerFrame(runner, renderer, gameW, gameH);
                }
            }

            accumulator -= targetFrameTime;
            gameFramesRan++;
        }

        // Deferred draw: render once after all catch-up steps
        if (deferDraw && gameFramesRan > 0 && !(heldRoomTransitionFrame && runner->appSurfaceKeepWindowSize)) {
            drawRunnerFrame(runner, renderer, gameW, gameH);
        }

        // Update audio
        if (runner->audioSystem) {
            float dt = (float)deltaTime;
            if (dt < 0.0f) dt = 0.0f;
            if (dt > 0.1f) dt = 0.1f;
            runner->audioSystem->vtable->update(runner->audioSystem, dt);
        }

        if (gameFramesRan > 0) {
            gDiagRoomAgeFrames += (uint32_t)gameFramesRan;
            double elapsedForDiag = (double)(currentTime.QuadPart - startTime.QuadPart) / (double)freq.QuadPart;
            if (gDiagOverlayWindowStart <= 0.0) gDiagOverlayWindowStart = elapsedForDiag;
            gDiagOverlayFrameCount += (uint32_t)gameFramesRan;
            gDiagOverlayDtMs = (float)(deltaTime * 1000.0);
            gDiagOverlaySteps = gameFramesRan;
            double diagElapsed = elapsedForDiag - gDiagOverlayWindowStart;
            if (diagElapsed >= 0.5) {
                gDiagOverlayFps = diagElapsed > 0.0 ? (float)((double)gDiagOverlayFrameCount / diagElapsed) : 0.0f;
                gDiagOverlayFrameCount = 0;
                gDiagOverlayWindowStart = elapsedForDiag;
            }
            double sysPollElapsed = (double)(currentTime.QuadPart - lastDiagSystemPollTime.QuadPart) / (double)freq.QuadPart;
            if (sysPollElapsed >= 1.0) {
                diagOverlayPollSystem();
                lastDiagSystemPollTime = currentTime;
            }

            heartbeatFrame += (uint32_t) gameFramesRan;
            if ((heartbeatFrame % 120) < (uint32_t) gameFramesRan) {
                double elapsed = (double)(currentTime.QuadPart - startTime.QuadPart) / (double)freq.QuadPart;
                double hbElapsed = (double)(currentTime.QuadPart - lastHeartbeatTime.QuadPart) / (double)freq.QuadPart;
                double hbFps = hbElapsed > 0.0 ? 120.0 / hbElapsed : 0.0;
                lastHeartbeatTime = currentTime;
                diagLog("HB frame %u t=%.3f dt120=%.3f fps=%.2f room=%d speed=%u pending=%d inst=%d", heartbeatFrame, elapsed, hbElapsed, hbFps, runner->currentRoom ? runner->currentRoomIndex : -1, runner->currentRoom ? runner->currentRoom->speed : 0, runner->pendingRoom, (int32_t) arrlen(runner->instances));
            }
            RunnerKeyboard_beginFrame(runner->keyboard);
        }
    }

    // ===[ game_change request (Deltarune chapter switch) ]===
    char* gcNextPath = NULL;
    if (runner->pendingWorkingDirectory != NULL && runner->pendingLaunchParameters != NULL) {
        diagLog("BS: GAME_CHANGE requested dir='%s' params='%s'", runner->pendingWorkingDirectory, runner->pendingLaunchParameters);
        gcNextPath = xdkBuildGameChangePath(dataWinPath, runner->pendingWorkingDirectory, runner->pendingLaunchParameters);
        if (gcNextPath) xdkSplitLaunchParams(runner->pendingLaunchParameters, gcArgStore, sizeof(gcArgStore), gcArgv, &gcArgc, 32);
        if (gcNextPath) {
            FILE* t = fopen(gcNextPath, "rb");
            diagLog("BS: GAME_CHANGE new data.win='%s' exists=%d", gcNextPath, t ? 1 : 0);
            if (t) fclose(t); else { free(gcNextPath); gcNextPath = NULL; }
        } else {
            diagLog("BS: GAME_CHANGE could not build a path");
        }
    }

    // ===[ Cleanup ]===
    if (runner->audioSystem) {
        runner->audioSystem->vtable->destroy(runner->audioSystem);
        runner->audioSystem = NULL;
    }
    renderer->vtable->destroy(renderer);
    loadingDestroy(&gDiagOverlayScreen);
    Runner_free(runner);
    VM_free(vm);
    DataWin_free(dataWin);
    if (gcNextPath) {
        diagLog("BS: GAME_CHANGE reloading %s", gcNextPath);
        free(gcOwnedPath);
        gcOwnedPath = gcNextPath;
        dataWinPath = gcOwnedPath;
        continue;
    }
    break;
    }  // end of for(;;) game loop
    pd3dDevice->Release();
    pD3D->Release();

    free(xpadMappings);
}
