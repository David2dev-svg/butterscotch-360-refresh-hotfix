#include <xtl.h>
#include <d3d9.h>
#include <d3dx9.h>
#include <xgraphics.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdarg>

// Core headers — compiled as C++ alongside the .c files (via /TP flag)
#include "utils.h"
#include "text_utils.h"
#include "d3d9_renderer.h"
#include "runner.h"

#include "../image/image_decoder.h"
extern "C" unsigned long __cdecl DbgPrint(const char* format, ...);
extern "C" void Butterscotch_xdkDiagTrace(const char* fmt, ...);

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// ===[ Vertex Format ]===
// Uses FLOAT4 position (pre-transformed screen coords, z=0, w=1)
// and FLOAT4 color to avoid D3DCOLOR endianness issues on Xbox 360.
struct SpriteVertex {
    float x, y, z, w; // position (screen-space, z=0, w=1)
    float u, v;        // texcoord
    float r, g, b, a;  // color as floats
};

// ===[ HLSL Shader Source ]===
// Vertex shader: simple pass-through for pre-transformed screen-space vertices.
// Position is already in screen pixels with z=0, w=1.
// With D3DRS_VIEWPORTENABLE=FALSE, the GPU uses these directly.
static const char* g_vsSource =
    "struct VS_IN  { float4 Pos : POSITION; float2 Tex : TEXCOORD0; float4 Col : TEXCOORD1; };\n"
    "struct VS_OUT { float4 Pos : POSITION; float2 Tex : TEXCOORD0; float4 Col : TEXCOORD1; };\n"
    "VS_OUT main(VS_IN i) {\n"
    "  VS_OUT o;\n"
    "  o.Pos = i.Pos;\n"
    "  o.Tex = i.Tex;\n"
    "  o.Col = i.Col;\n"
    "  return o;\n"
    "}\n";

static const char* g_psSource =
    "sampler2D s0 : register(s0) = sampler_state {\n"
    "  MinFilter = POINT; MagFilter = POINT; MipFilter = POINT;\n"
    "  AddressU = CLAMP; AddressV = CLAMP;\n"
    "};\n"
    "struct PS_IN { float2 Tex : TEXCOORD0; float4 Col : TEXCOORD1; };\n"
    "float4 main(PS_IN i) : COLOR0 {\n"
    "  return tex2D(s0, i.Tex) * i.Col;\n"
    "}\n";

// ===[ Helpers ]===

static inline void setVertex(SpriteVertex* sv, float px, float py, float tu, float tv,
                              float cr, float cg, float cb, float ca) {
    sv->x = px - 0.5f; sv->y = py - 0.5f; sv->z = 0.0f; sv->w = 1.0f;
    sv->u = tu; sv->v = tv;
    sv->r = cr; sv->g = cg; sv->b = cb; sv->a = ca;
}

static inline float texelStart(float pos, float textureSize) {
    return (pos + 0.5f) / textureSize;
}

static inline float texelEnd(float pos, float size, float textureSize) {
    return (pos + size - 0.5f) / textureSize;
}

static inline IDirect3DDevice9* Dev(D3D9Renderer* r) {
    return (IDirect3DDevice9*)r->pd3dDevice;
}

static void d3d9DiagOnce(bool* flag, const char* fmt, ...) {
    if (*flag) return;
    *flag = true;

    char line[512];
    va_list args;
    va_start(args, fmt);
    _vsnprintf(line, sizeof(line) - 1, fmt, args);
    va_end(args);
    line[sizeof(line) - 1] = '\0';
    Butterscotch_xdkDiagTrace("%s", line);
}

static void d3d9DiagLimited(int* counter, int limit, const char* fmt, ...) {
    if (*counter >= limit) return;
    (*counter)++;

    char line[512];
    va_list args;
    va_start(args, fmt);
    _vsnprintf(line, sizeof(line) - 1, fmt, args);
    va_end(args);
    line[sizeof(line) - 1] = '\0';
    Butterscotch_xdkDiagTrace("%s", line);
}

static DWORD gmlBlendFactorToD3D(int32_t factor) {
    switch (factor) {
        case bm_zero: return D3DBLEND_ZERO;
        case bm_one: return D3DBLEND_ONE;
        case bm_src_color: return D3DBLEND_SRCCOLOR;
        case bm_inv_src_color: return D3DBLEND_INVSRCCOLOR;
        case bm_src_alpha: return D3DBLEND_SRCALPHA;
        case bm_inv_src_alpha: return D3DBLEND_INVSRCALPHA;
        case bm_dest_alpha: return D3DBLEND_DESTALPHA;
        case bm_inv_dest_alpha: return D3DBLEND_INVDESTALPHA;
        case bm_dest_color: return D3DBLEND_DESTCOLOR;
        case bm_inv_dest_color: return D3DBLEND_INVDESTCOLOR;
        case bm_src_alpha_sat: return D3DBLEND_SRCALPHASAT;
        default: return D3DBLEND_ONE;
    }
}

static void d3d9SetNormalBlend(IDirect3DDevice9* dev) {
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
}

static void resetFullBackbufferState(D3D9Renderer* dr) {
    IDirect3DDevice9* dev = Dev(dr);
    D3DVIEWPORT9 vp;
    vp.X = 0;
    vp.Y = 0;
    vp.Width = (DWORD)dr->screenW;
    vp.Height = (DWORD)dr->screenH;
    vp.MinZ = 0.0f;
    vp.MaxZ = 1.0f;
    dev->SetViewport(&vp);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
}

static void setGameTargetTransform(D3D9Renderer* dr) {
    dr->offsetX = 0.0f;
    dr->offsetY = 0.0f;
    dr->portScaleX = dr->renderScale;
    dr->portScaleY = dr->renderScale;
    dr->portOffsetX = dr->renderOffsetX;
    dr->portOffsetY = dr->renderOffsetY;
}

static void setWindowSurfaceTransform(D3D9Renderer* dr) {
    dr->offsetX = 0.0f;
    dr->offsetY = 0.0f;
    dr->portScaleX = (dr->appSurfaceW > 0) ? ((float)dr->screenW / (float)dr->appSurfaceW) : 1.0f;
    dr->portScaleY = (dr->appSurfaceH > 0) ? ((float)dr->screenH / (float)dr->appSurfaceH) : 1.0f;
    dr->portOffsetX = 0.0f;
    dr->portOffsetY = 0.0f;
}

static void setApplicationSurfaceTransform(D3D9Renderer* dr) {
    dr->offsetX = 0.0f;
    dr->offsetY = 0.0f;
    dr->portScaleX = 1.0f;
    dr->portScaleY = 1.0f;
    dr->portOffsetX = 0.0f;
    dr->portOffsetY = 0.0f;
}

static void releaseApplicationSurface(D3D9Renderer* dr) {
    if (dr->appSurfaceLevel) {
        ((IDirect3DSurface9*)dr->appSurfaceLevel)->Release();
        dr->appSurfaceLevel = NULL;
    }
    if (dr->appRenderTexture) {
        ((IDirect3DTexture9*)dr->appRenderTexture)->Release();
        dr->appRenderTexture = NULL;
    }
    if (dr->appSurfaceTexture) {
        ((IDirect3DTexture9*)dr->appSurfaceTexture)->Release();
        dr->appSurfaceTexture = NULL;
    }
    dr->appSurfaceW = 0;
    dr->appSurfaceH = 0;
    dr->appSurfaceAllocW = 0;
    dr->appSurfaceAllocH = 0;
    dr->appSurfaceResolved = false;
}

static bool bindBackbuffer(D3D9Renderer* dr) {
    IDirect3DDevice9* dev = Dev(dr);
    IDirect3DSurface9* backbuffer = NULL;
    HRESULT hr = dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer);
    if (FAILED(hr) || !backbuffer) {
        Butterscotch_xdkDiagTrace("D3D9: GetBackBuffer failed hr=0x%08X", (unsigned)hr);
        return false;
    }
    hr = dev->SetRenderTarget(0, backbuffer);
    backbuffer->Release();
    if (FAILED(hr)) {
        Butterscotch_xdkDiagTrace("D3D9: SetRenderTarget(backbuffer) failed hr=0x%08X", (unsigned)hr);
        return false;
    }
    return true;
}

static void resolveApplicationSurface(D3D9Renderer* dr) {
    if (!dr->appSurfaceTexture || dr->appSurfaceW <= 0 || dr->appSurfaceH <= 0) return;
    if (dr->appSurfaceResolved) return;

    IDirect3DDevice9* dev = Dev(dr);
    dev->Resolve(D3DRESOLVE_RENDERTARGET0, NULL,
                 (IDirect3DBaseTexture9*)dr->appSurfaceTexture,
                 NULL, 0, 0, NULL, 1.0f, 0, NULL);
    dr->appSurfaceResolved = true;
}

static void applyPointSampling(IDirect3DDevice9* dev) {
    for (DWORD sampler = 0; sampler < 8; sampler++) {
        dev->SetSamplerState(sampler, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(sampler, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(sampler, D3DSAMP_MIPFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(sampler, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(sampler, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    }
}

// Convert Butterscotch BGR color + alpha to float RGBA
static inline void bgrToFloatColor(uint32_t bgr, float alpha, float* outR, float* outG, float* outB, float* outA) {
    *outR = (float)(bgr & 0xFF) / 255.0f;
    *outG = (float)((bgr >> 8) & 0xFF) / 255.0f;
    *outB = (float)((bgr >> 16) & 0xFF) / 255.0f;
    *outA = alpha;
}

// ===[ Batch Flush ]===

static void flushBatch(D3D9Renderer* dr) {
    if (dr->quadCount == 0) return;

    IDirect3DDevice9* dev = Dev(dr);

    // Bind texture
    if (dr->currentTextureIndex >= 0 && (uint32_t)dr->currentTextureIndex < dr->textureCount) {
        dev->SetTexture(0, (IDirect3DBaseTexture9*)dr->textures[dr->currentTextureIndex]);
    } else {
        dev->SetTexture(0, (IDirect3DBaseTexture9*)dr->whiteTexture);
    }

    // Draw using DrawPrimitiveUP — simpler than managing a vertex buffer for 2D
    int32_t vertCount = dr->quadCount * D3D9_VERTS_PER_QUAD;
    // We use QUADLIST (Xbox 360 extension) — 4 verts per quad, no index buffer needed
    dev->DrawPrimitiveUP(D3DPT_QUADLIST, dr->quadCount,
                         dr->vertexData, sizeof(SpriteVertex));

    dr->quadCount = 0;
}

static bool loadTextureBytes(D3D9Renderer* dr, uint32_t index, const uint8_t* bytes, int byteSize, const char* label);
static bool loadExternalTexturePage(D3D9Renderer* dr, uint32_t index);
static bool d3d9SetRenderTarget(Renderer* renderer, int32_t surfaceID);
static void d3d9DrawSurface(Renderer* renderer, int32_t surfaceID, int32_t srcLeft, int32_t srcTop, int32_t srcWidth, int32_t srcHeight, float x, float y, float xscale, float yscale, float angleDeg, uint32_t color, float alpha);

static void releaseTexturePage(D3D9Renderer* dr, uint32_t index) {
    if (!dr || index >= dr->textureCount || !dr->textures || !dr->textures[index]) return;
    if (dr->currentTextureIndex == (int32_t)index) {
        flushBatch(dr);
        dr->currentTextureIndex = -1;
        Dev(dr)->SetTexture(0, NULL);
    }
    ((IDirect3DTexture9*)dr->textures[index])->Release();
    dr->textures[index] = NULL;
    dr->textureWidths[index] = 0;
    dr->textureHeights[index] = 0;
    if (dr->textureLastUsedFrame) dr->textureLastUsedFrame[index] = 0;
    if (dr->loadedTexturePages > 0) dr->loadedTexturePages--;
}

static void ensureTextureCacheRoom(D3D9Renderer* dr) {
    const uint32_t maxLoadedPages = 12;
    while (dr->loadedTexturePages >= maxLoadedPages) {
        uint32_t victim = UINT_MAX;
        uint32_t oldest = UINT_MAX;
        for (uint32_t i = 0; i < dr->originalTexturePageCount && i < dr->textureCount; i++) {
            if (!dr->textures[i]) continue;
            if ((int32_t)i == dr->currentTextureIndex) continue;
            uint32_t age = dr->textureLastUsedFrame ? dr->textureLastUsedFrame[i] : 0;
            if (age < oldest) {
                oldest = age;
                victim = i;
            }
        }
        if (victim == UINT_MAX) break;
        releaseTexturePage(dr, victim);
    }
}

static bool ensureTexturePageLoaded(D3D9Renderer* dr, uint32_t textureIndex) {
    if (!dr || textureIndex >= dr->textureCount) return false;
    if (dr->textures[textureIndex]) {
        if (dr->textureLastUsedFrame) dr->textureLastUsedFrame[textureIndex] = dr->frameCounter;
        return true;
    }

    DataWin* dw = dr->base.dataWin;
    if (!dw || textureIndex >= dw->txtr.count) return false;

    Texture* txtr = &dw->txtr.textures[textureIndex];
    ensureTextureCacheRoom(dr);

    bool ok = false;
    if (txtr->blobData && txtr->blobSize > 0) {
        ok = loadTextureBytes(dr, textureIndex, txtr->blobData, (int)txtr->blobSize, "data.win");
    } else if (txtr->present) {
        ok = loadExternalTexturePage(dr, textureIndex);
    }

    if (ok) {
        dr->loadedTexturePages++;
        if (dr->textureLastUsedFrame) dr->textureLastUsedFrame[textureIndex] = dr->frameCounter;
    }
    return ok;
}

static void ensureTexture(D3D9Renderer* dr, int32_t textureIndex) {
    if (dr->currentTextureIndex != textureIndex) {
        flushBatch(dr);
        dr->currentTextureIndex = textureIndex;
    }
}

static SpriteVertex* allocQuad(D3D9Renderer* dr) {
    if (dr->quadCount >= D3D9_MAX_QUADS) {
        flushBatch(dr);
    }
    SpriteVertex* v = (SpriteVertex*)(dr->vertexData + dr->quadCount * D3D9_VERTS_PER_QUAD * sizeof(SpriteVertex));
    dr->quadCount++;
    return v;
}

static bool readWholeFile(const char* path, uint8_t** outData, int* outSize) {
    if (!path || !outData || !outSize) return false;
    *outData = NULL;
    *outSize = 0;

    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 64 * 1024 * 1024) {
        fclose(f);
        return false;
    }

    uint8_t* data = (uint8_t*)malloc((size_t)size);
    if (!data) {
        fclose(f);
        return false;
    }
    size_t got = fread(data, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) {
        free(data);
        return false;
    }
    *outData = data;
    *outSize = (int)size;
    return true;
}

static bool loadTextureBytes(D3D9Renderer* dr, uint32_t index, const uint8_t* bytes, int byteSize, const char* label) {
    if (!bytes || byteSize <= 0 || index >= dr->textureCount) return false;

    int w, h, channels;
    //uint8_t* pixels = stbi_load_from_memory(bytes, byteSize, &w, &h, &channels, 4);
    uint8_t* pixels = ImageDecoder_decodeToRgba(bytes, (size_t) byteSize, true, &w, &h);
    if (!pixels) {
        Butterscotch_xdkDiagTrace("D3D9: failed to decode texture page %u from %s bytes=%d", index, label ? label : "(memory)", byteSize);
        return false;
    }

    IDirect3DDevice9* dev = Dev(dr);
    IDirect3DTexture9* tex = NULL;
    HRESULT hr = dev->CreateTexture(w, h, 1, 0, D3DFMT_LIN_A8R8G8B8, D3DPOOL_DEFAULT, &tex, NULL);
    if (FAILED(hr) || !tex) {
        Butterscotch_xdkDiagTrace("D3D9: CreateTexture failed page=%u %dx%d hr=0x%08X", index, w, h, (unsigned)hr);
        stbi_image_free(pixels);
        return false;
    }

    D3DLOCKED_RECT lr;
    hr = tex->LockRect(0, &lr, NULL, 0);
    if (FAILED(hr)) {
        Butterscotch_xdkDiagTrace("D3D9: LockRect failed page=%u hr=0x%08X", index, (unsigned)hr);
        tex->Release();
        stbi_image_free(pixels);
        return false;
    }
    for (int y2 = 0; y2 < h; y2++) {
        uint8_t* src = pixels + y2 * w * 4;
        DWORD* dst = (DWORD*)((uint8_t*)lr.pBits + y2 * lr.Pitch);
        for (int x2 = 0; x2 < w; x2++) {
            uint8_t r = src[x2 * 4 + 0];
            uint8_t g = src[x2 * 4 + 1];
            uint8_t b = src[x2 * 4 + 2];
            uint8_t a = src[x2 * 4 + 3];
            if (a == 0) { r = 0; g = 0; b = 0; }
            dst[x2] = D3DCOLOR_ARGB(a, r, g, b);
        }
    }
    tex->UnlockRect(0);
    stbi_image_free(pixels);

    dr->textures[index] = tex;
    dr->textureWidths[index] = w;
    dr->textureHeights[index] = h;
    Butterscotch_xdkDiagTrace("D3D9: loaded texture page %u %dx%d from %s", index, w, h, label ? label : "(memory)");
    return true;
}

static bool loadExternalTexturePage(D3D9Renderer* dr, uint32_t index) {
    char path[256];
    const char* formats[] = {
        "game:\\texture_%u.png",
        "game:\\texture_%u.PNG",
        "game:\\texture_page_%u.png",
        "game:\\texture_page_%u.PNG",
        "game:\\textures\\texture_%u.png",
        "game:\\textures\\texture_%u.PNG",
        "game:\\textures\\texture_page_%u.png",
        "game:\\textures\\texture_page_%u.PNG",
    };

    for (uint32_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
        _snprintf(path, sizeof(path), formats[i], index);
        path[sizeof(path) - 1] = '\0';
        uint8_t* data = NULL;
        int size = 0;
        if (!readWholeFile(path, &data, &size)) continue;
        bool ok = loadTextureBytes(dr, index, data, size, path);
        free(data);
        if (ok) return true;
    }

    Butterscotch_xdkDiagTrace("D3D9: external texture page %u missing; tried texture_%u.png / texture_page_%u.png", index, index, index);
    return false;
}

// Transform a game-space point to screen-space pixels (with pillarbox centering)
static inline void transformPoint(D3D9Renderer* dr, float inX, float inY, float* outX, float* outY) {
    // Game-space: view → port mapping
    *outX = dr->portOffsetX + (inX - dr->offsetX) * dr->portScaleX;
    *outY = dr->portOffsetY + (inY - dr->offsetY) * dr->portScaleY;
}

static inline int32_t floorInt(float v) {
    int32_t i = (int32_t)v;
    return (v < (float)i) ? i - 1 : i;
}

static inline int32_t ceilInt(float v) {
    int32_t i = (int32_t)v;
    return (v > (float)i) ? i + 1 : i;
}

static void d3d9DrawSurface(Renderer* renderer, int32_t surfaceID, int32_t srcLeft, int32_t srcTop,
                            int32_t srcWidth, int32_t srcHeight, float x, float y,
                            float xscale, float yscale, float angleDeg, uint32_t color, float alpha);

// ===[ Vtable Implementations ]===

static void d3d9Init(Renderer* renderer, DataWin* dataWin) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    IDirect3DDevice9* dev = Dev(dr);
    renderer->dataWin = dataWin;

    // Allocate CPU vertex staging buffer
    dr->vertexData = (uint8_t*)malloc(D3D9_MAX_QUADS * D3D9_VERTS_PER_QUAD * sizeof(SpriteVertex));

    // Compile shaders from source
    ID3DXBuffer* pCode = NULL;
    ID3DXBuffer* pErr = NULL;

    HRESULT hr = D3DXCompileShader(g_vsSource, (UINT)strlen(g_vsSource),
                                   NULL, NULL, "main", "vs_2_0", 0, &pCode, &pErr, NULL);
    if (FAILED(hr)) {
        OutputDebugStringA("VS compile failed: ");
        if (pErr) OutputDebugStringA((const char*)pErr->GetBufferPointer());
        if (pErr) pErr->Release();
        return;
    }
    dev->CreateVertexShader((const DWORD*)pCode->GetBufferPointer(),
                            (IDirect3DVertexShader9**)&dr->pVertexShader);
    pCode->Release();

    hr = D3DXCompileShader(g_psSource, (UINT)strlen(g_psSource),
                           NULL, NULL, "main", "ps_2_0", 0, &pCode, &pErr, NULL);
    if (FAILED(hr)) {
        OutputDebugStringA("PS compile failed: ");
        if (pErr) OutputDebugStringA((const char*)pErr->GetBufferPointer());
        if (pErr) pErr->Release();
        return;
    }
    dev->CreatePixelShader((const DWORD*)pCode->GetBufferPointer(),
                           (IDirect3DPixelShader9**)&dr->pPixelShader);
    pCode->Release();

    // Create vertex declaration
    static const D3DVERTEXELEMENT9 decl[] = {
        { 0,  0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        { 0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
        { 0, 24, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1 },
        D3DDECL_END()
    };
    dev->CreateVertexDeclaration(decl, (IDirect3DVertexDeclaration9**)&dr->pVertexDecl);

    // Create 1x1 white texture for primitives
    IDirect3DTexture9* whiteTex = NULL;
    dev->CreateTexture(1, 1, 1, 0, D3DFMT_LIN_A8R8G8B8, D3DPOOL_DEFAULT, &whiteTex, NULL);
    if (whiteTex) {
        D3DLOCKED_RECT lr;
        whiteTex->LockRect(0, &lr, NULL, 0);
        *(DWORD*)lr.pBits = 0xFFFFFFFF;
        whiteTex->UnlockRect(0);
    }
    dr->whiteTexture = whiteTex;

    // TXTR pages can be huge in fan builds. Decode/upload them on demand.
    dr->textureCount = dataWin->txtr.count;
    dr->textures = (void**)calloc(dr->textureCount, sizeof(void*));
    dr->textureWidths = (int32_t*)calloc(dr->textureCount, sizeof(int32_t));
    dr->textureHeights = (int32_t*)calloc(dr->textureCount, sizeof(int32_t));
    dr->textureLastUsedFrame = (uint32_t*)calloc(dr->textureCount, sizeof(uint32_t));
    dr->loadedTexturePages = 0;
    dr->frameCounter = 1;
    Butterscotch_xdkDiagTrace("D3D9: texture pages will be loaded lazily count=%u", dr->textureCount);

    dr->originalTexturePageCount = dataWin->txtr.count;
    dr->originalTpagCount = dataWin->tpag.count;
    dr->originalSpriteCount = dataWin->sprt.count;

    dr->currentTextureIndex = -1;
    dr->quadCount = 0;
}

static void d3d9Destroy(Renderer* renderer) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    for (uint32_t i = 0; i < dr->textureCount; i++) {
        if (dr->textures[i]) ((IDirect3DTexture9*)dr->textures[i])->Release();
    }
    free(dr->textures);
    free(dr->textureWidths);
    free(dr->textureHeights);
    free(dr->textureLastUsedFrame);
    free(dr->vertexData);
    if (dr->whiteTexture) ((IDirect3DTexture9*)dr->whiteTexture)->Release();
    releaseApplicationSurface(dr);
    if (dr->pVertexShader) ((IDirect3DVertexShader9*)dr->pVertexShader)->Release();
    if (dr->pPixelShader) ((IDirect3DPixelShader9*)dr->pPixelShader)->Release();
    if (dr->pVertexDecl) ((IDirect3DVertexDeclaration9*)dr->pVertexDecl)->Release();
    free(dr);
}

static void d3d9BeginFrame(Renderer* renderer, int32_t gameW, int32_t gameH, int32_t windowW, int32_t windowH) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    IDirect3DDevice9* dev = Dev(dr);

    dr->gameW = gameW;
    dr->gameH = gameH;
    dr->screenW = windowW;
    dr->screenH = windowH;
    dr->frameCounter++;
    if (dr->frameCounter == 0) dr->frameCounter = 1;

    // Fit the game inside the 720p backbuffer. On Xbox 360, 1080p backbuffers
    // can fail to allocate; keep 720p and rely on point sampling for crispness.
    float scaleX = (float)windowW / (float)gameW;
    float scaleY = (float)windowH / (float)gameH;
    float fitScale = (scaleX < scaleY) ? scaleX : scaleY;
    dr->renderScale = fitScale;
    dr->renderOffsetX = ((float)windowW - (float)gameW * dr->renderScale) * 0.5f;
    dr->renderOffsetY = ((float)windowH - (float)gameH * dr->renderScale) * 0.5f;

    // Debug: print once
    static bool printedOnce = false;
    if (!printedOnce) {
        DbgPrint("BS: renderScale=%d/1000 offsetX=%d offsetY=%d gameW=%d gameH=%d screenW=%d screenH=%d\n",
            (int)(dr->renderScale * 1000), (int)dr->renderOffsetX, (int)dr->renderOffsetY,
            gameW, gameH, windowW, windowH);
        printedOnce = true;
    }

    dev->BeginScene();

    if (renderer->runner && renderer->runner->usingAppSurface && dr->appSurfaceLevel) {
        d3d9SetRenderTarget(renderer, APPLICATION_SURFACE_ID);
        dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
    } else {
        bindBackbuffer(dr);
        resetFullBackbufferState(dr);
        dr->renderingToApplicationSurface = false;
        setGameTargetTransform(dr);
        dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
    }

    // Set shared render state
    dev->SetVertexShader((IDirect3DVertexShader9*)dr->pVertexShader);
    dev->SetPixelShader((IDirect3DPixelShader9*)dr->pPixelShader);
    dev->SetVertexDeclaration((IDirect3DVertexDeclaration9*)dr->pVertexDecl);


    // Alpha blending
    d3d9SetNormalBlend(dev);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE,
        D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
        D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);

    // No depth testing for 2D
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);

    // Disable viewport transform — we use pre-transformed screen-space vertices
    dev->SetRenderState(D3DRS_VIEWPORTENABLE, FALSE);

    // Point filtering — pixel-perfect for 2D sprite games like Undertale.
    // Force every sampler each frame because GML shader state can be sticky.
    applyPointSampling(dev);
}

static void d3d9EndFrame(Renderer* renderer) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    flushBatch(dr);
    Dev(dr)->EndScene();
    Dev(dr)->Present(NULL, NULL, NULL, NULL);
}

static void d3d9EndFrameInit(Renderer* renderer) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    flushBatch(dr);
    if (renderer->runner && renderer->runner->usingAppSurface && dr->appSurfaceLevel && dr->renderingToApplicationSurface) {
        resolveApplicationSurface(dr);
        bindBackbuffer(dr);
        resetFullBackbufferState(dr);
        dr->renderingToApplicationSurface = false;
        setGameTargetTransform(dr);
        Dev(dr)->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
        applyPointSampling(Dev(dr));
        if (renderer->runner->appSurfaceAutoDraw) {
            d3d9DrawSurface(renderer, APPLICATION_SURFACE_ID, 0, 0, -1, -1, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0xFFFFFF, 1.0f);
        }
    }
}

static void d3d9EndFrameEnd(Renderer* renderer) {
    d3d9EndFrame(renderer);
}

static void d3d9BeginView(Renderer* renderer, int32_t viewX, int32_t viewY, int32_t viewW, int32_t viewH,
                           int32_t portX, int32_t portY, int32_t portW, int32_t portH, float viewAngle) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    IDirect3DDevice9* dev = Dev(dr);
    (void)viewAngle; // TODO: support rotated views

    float targetScaleX = dr->renderingToApplicationSurface ? 1.0f : dr->renderScale;
    float targetScaleY = dr->renderingToApplicationSurface ? 1.0f : dr->renderScale;
    float targetOffsetX = dr->renderingToApplicationSurface ? 0.0f : dr->renderOffsetX;
    float targetOffsetY = dr->renderingToApplicationSurface ? 0.0f : dr->renderOffsetY;
    int32_t targetW = dr->renderingToApplicationSurface ? dr->gameW : dr->screenW;
    int32_t targetH = dr->renderingToApplicationSurface ? dr->gameH : dr->screenH;

    float screenPortX = targetOffsetX + (float)portX * targetScaleX;
    float screenPortY = targetOffsetY + (float)portY * targetScaleY;
    float screenPortW = (float)portW * targetScaleX;
    float screenPortH = (float)portH * targetScaleY;

    int32_t scLeft = floorInt(screenPortX);
    int32_t scTop = floorInt(screenPortY);
    int32_t scRight = ceilInt(screenPortX + screenPortW);
    int32_t scBottom = ceilInt(screenPortY + screenPortH);
    if (scLeft < 0) scLeft = 0;
    if (scTop < 0) scTop = 0;
    if (scRight > targetW) scRight = targetW;
    if (scBottom > targetH) scBottom = targetH;

    // Set viewport to the screen-space port rectangle for scissor alignment.
    D3DVIEWPORT9 vp;
    vp.X = (DWORD)scLeft;
    vp.Y = (DWORD)scTop;
    vp.Width = max(1, (DWORD)(scRight - scLeft));
    vp.Height = max(1, (DWORD)(scBottom - scTop));
    vp.MinZ = 0.0f;
    vp.MaxZ = 1.0f;
    dev->SetViewport(&vp);

    // Enable scissor test to clip rendering to the port rectangle.
    // This prevents sprites from bleeding outside their intended viewport
    // (e.g. characters appearing outside dialogue box borders in Undertale).
    RECT scissor;
    scissor.left = scLeft;
    scissor.top = scTop;
    scissor.right = scRight;
    scissor.bottom = scBottom;
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
    dev->SetScissorRect(&scissor);

    // Store view transform for point mapping
    dr->offsetX = (float)viewX;
    dr->offsetY = (float)viewY;
    dr->portScaleX = screenPortW / (float)viewW;
    dr->portScaleY = screenPortH / (float)viewH;
    dr->portOffsetX = screenPortX;
    dr->portOffsetY = screenPortY;

    // No projection matrix needed — vertices are pre-transformed screen coords.
    // D3DRS_VIEWPORTENABLE=FALSE means the GPU uses positions directly as pixels.
}

static void d3d9EndView(Renderer* renderer) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    flushBatch(dr);
    Dev(dr)->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
}

static void d3d9ApplyProjection(Renderer* renderer, const Matrix4f* worldToClip) {
    (void)renderer;
    (void)worldToClip;
}

static void d3d9BeginGUI(Renderer* renderer, int32_t guiW, int32_t guiH, int32_t portX, int32_t portY, int32_t portW, int32_t portH) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    IDirect3DDevice9* dev = Dev(dr);

    int32_t scLeft = portX < 0 ? 0 : portX;
    int32_t scTop = portY < 0 ? 0 : portY;
    int32_t scRight = portX + portW;
    int32_t scBottom = portY + portH;
    if (scRight > dr->screenW) scRight = dr->screenW;
    if (scBottom > dr->screenH) scBottom = dr->screenH;

    RECT scissor;
    scissor.left = scLeft;
    scissor.top = scTop;
    scissor.right = scRight;
    scissor.bottom = scBottom;
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
    dev->SetScissorRect(&scissor);

    dr->offsetX = 0.0f;
    dr->offsetY = 0.0f;
    dr->portScaleX = (float)portW / (float)guiW;
    dr->portScaleY = (float)portH / (float)guiH;
    dr->portOffsetX = (float)portX;
    dr->portOffsetY = (float)portY;
}

static void d3d9EndGUI(Renderer* renderer) {
    d3d9EndView(renderer);
}

// ===[ Sprite Drawing ]===

static void d3d9DrawSprite(Renderer* renderer, int32_t tpagIndex, float x, float y,
                            float originX, float originY, float xscale, float yscale,
                            float angleDeg, uint32_t color, float alpha) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    DataWin* dw = renderer->dataWin;

    if (0 > tpagIndex || (uint32_t)tpagIndex >= dw->tpag.count) {
        static int invalidTpagLog = 0;
        d3d9DiagLimited(&invalidTpagLog, 64, "D3D9: drawSprite invalid tpag=%d count=%u x=%.2f y=%.2f",
                        tpagIndex, dw->tpag.count, x, y);
        return;
    }
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    int32_t texPageId = tpag->texturePageId;
    if (0 > texPageId || (uint32_t)texPageId >= dr->textureCount) {
        static int invalidTextureLog = 0;
        d3d9DiagLimited(&invalidTextureLog, 64, "D3D9: drawSprite invalid texPage=%d textureCount=%u tpag=%d",
                        texPageId, dr->textureCount, tpagIndex);
        return;
    }
    ensureTexturePageLoaded(dr, (uint32_t)texPageId);
    if (!dr->textures[texPageId]) {
        static int nullTextureLog = 0;
        d3d9DiagLimited(&nullTextureLog, 64, "D3D9: drawSprite null texture page=%d tpag=%d", texPageId, tpagIndex);
        return;
    }

    ensureTexture(dr, texPageId);

    float texW = (float)dr->textureWidths[texPageId];
    float texH = (float)dr->textureHeights[texPageId];
    if (texW <= 0 || texH <= 0) return;

    int roomIndex = renderer->runner ? renderer->runner->currentRoomIndex : -1;
    if (renderer->drawPhase == RENDER_PHASE_WORLD && (roomIndex >= 288 || roomIndex < 8)) {
        static int worldSpriteLog = 0;
        int limit = roomIndex >= 288 ? 96 : 120;
        if ((roomIndex >= 288 || tpag->sourceWidth >= 64 || tpag->sourceHeight >= 64 || x <= 8.0f || y <= 8.0f) && worldSpriteLog < limit) {
            d3d9DiagLimited(&worldSpriteLog, limit,
                            "D3D9WORLD: sprite tpag=%d texPage=%d tex=%dx%d tpagSrc=%d,%d %dx%d target=%d,%d bound=%dx%d dst=%.2f,%.2f origin=%.2f,%.2f scale=%.3f,%.3f angle=%.2f room=%d",
                            tpagIndex, texPageId, (int)texW, (int)texH,
                            tpag->sourceX, tpag->sourceY, tpag->sourceWidth, tpag->sourceHeight,
                            tpag->targetX, tpag->targetY, tpag->boundingWidth, tpag->boundingHeight,
                            x, y, originX, originY, xscale, yscale, angleDeg,
                            roomIndex);
        }
    }

    // UV coordinates on the texture atlas
    float u0 = texelStart((float)tpag->sourceX, texW);
    float v0 = texelStart((float)tpag->sourceY, texH);
    float u1 = texelEnd((float)tpag->sourceX, (float)tpag->sourceWidth, texW);
    float v1 = texelEnd((float)tpag->sourceY, (float)tpag->sourceHeight, texH);

    // Quad corners in local space (before transform)
    float localX0 = (float)tpag->targetX - originX;
    float localY0 = (float)tpag->targetY - originY;
    float localX1 = localX0 + (float)tpag->sourceWidth;
    float localY1 = localY0 + (float)tpag->sourceHeight;

    // Scale
    localX0 *= xscale; localY0 *= yscale;
    localX1 *= xscale; localY1 *= yscale;

    float cr, cg, cb, ca;
    bgrToFloatColor(color, alpha, &cr, &cg, &cb, &ca);

    // Build 4 corners
    float cx[4], cy[4];
    if (angleDeg != 0.0f) {
        float rad = -angleDeg * (3.14159265f / 180.0f);
        float cosA = cosf(rad);
        float sinA = sinf(rad);

        float lx[4] = { localX0, localX1, localX1, localX0 };
        float ly[4] = { localY0, localY0, localY1, localY1 };
        for (int i = 0; i < 4; i++) {
            cx[i] = lx[i] * cosA - ly[i] * sinA;
            cy[i] = lx[i] * sinA + ly[i] * cosA;
        }
    } else {
        cx[0] = localX0; cy[0] = localY0;
        cx[1] = localX1; cy[1] = localY0;
        cx[2] = localX1; cy[2] = localY1;
        cx[3] = localX0; cy[3] = localY1;
    }

    // Transform to screen space
    SpriteVertex* v = allocQuad(dr);
    float sx, sy;
    for (int i = 0; i < 4; i++) {
        transformPoint(dr, x + cx[i], y + cy[i], &sx, &sy);
        v[i].x = sx - 0.5f;
        v[i].y = sy - 0.5f;
        v[i].z = 0.0f;
        v[i].w = 1.0f;
        v[i].r = cr; v[i].g = cg; v[i].b = cb; v[i].a = ca;
    }
    v[0].u = u0; v[0].v = v0;
    v[1].u = u1; v[1].v = v0;
    v[2].u = u1; v[2].v = v1;
    v[3].u = u0; v[3].v = v1;
}

static void d3d9DrawSpritePart(Renderer* renderer, int32_t tpagIndex,
                                int32_t srcOffX, int32_t srcOffY, int32_t srcW, int32_t srcH,
                                float x, float y, float xscale, float yscale,
                                float angleDeg, float pivotX, float pivotY,
                                uint32_t color, float alpha) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    DataWin* dw = renderer->dataWin;

    if (0 > tpagIndex || (uint32_t)tpagIndex >= dw->tpag.count) {
        static int invalidTpagLog = 0;
        d3d9DiagLimited(&invalidTpagLog, 64, "D3D9: drawSpritePart invalid tpag=%d count=%u src=%d,%d %dx%d dst=%.2f,%.2f",
                        tpagIndex, dw->tpag.count, srcOffX, srcOffY, srcW, srcH, x, y);
        return;
    }
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    int32_t texPageId = tpag->texturePageId;
    if (0 > texPageId || (uint32_t)texPageId >= dr->textureCount) {
        static int invalidTextureLog = 0;
        d3d9DiagLimited(&invalidTextureLog, 64, "D3D9: drawSpritePart invalid texPage=%d textureCount=%u tpag=%d",
                        texPageId, dr->textureCount, tpagIndex);
        return;
    }
    ensureTexturePageLoaded(dr, (uint32_t)texPageId);
    if (!dr->textures[texPageId]) {
        static int nullTextureLog = 0;
        d3d9DiagLimited(&nullTextureLog, 64, "D3D9: drawSpritePart null texture page=%d tpag=%d", texPageId, tpagIndex);
        return;
    }

    ensureTexture(dr, texPageId);

    float texW = (float)dr->textureWidths[texPageId];
    float texH = (float)dr->textureHeights[texPageId];
    if (texW <= 0 || texH <= 0) return;

    int roomIndex = renderer->runner ? renderer->runner->currentRoomIndex : -1;
    if (renderer->drawPhase == RENDER_PHASE_POST || renderer->drawPhase == RENDER_PHASE_WORLD || roomIndex >= 288) {
        static int partLog = 0;
        int limit = roomIndex >= 288 ? 128 : 180;
        if ((roomIndex >= 288 || srcW >= 16 || srcH >= 16 || x <= 4.0f || y <= 4.0f) && partLog < limit) {
            d3d9DiagLimited(&partLog, limit,
                            "D3D9PART: phase=%d tpag=%d texPage=%d tex=%dx%d tpagSrc=%d,%d %dx%d target=%d,%d bound=%dx%d srcOff=%d,%d src=%dx%d dst=%.2f,%.2f scale=%.2f,%.2f room=%d",
                            renderer->drawPhase,
                            tpagIndex, texPageId, (int)texW, (int)texH,
                            tpag->sourceX, tpag->sourceY, tpag->sourceWidth, tpag->sourceHeight,
                            tpag->targetX, tpag->targetY, tpag->boundingWidth, tpag->boundingHeight,
                            srcOffX, srcOffY, srcW, srcH,
                            x, y, xscale, yscale,
                            roomIndex);
        }
    }

    float u0 = texelStart((float)(tpag->sourceX + srcOffX), texW);
    float v0 = texelStart((float)(tpag->sourceY + srcOffY), texH);
    float u1 = texelEnd((float)(tpag->sourceX + srcOffX), (float)srcW, texW);
    float v1 = texelEnd((float)(tpag->sourceY + srcOffY), (float)srcH, texH);

    float drawW = (float)srcW * xscale;
    float drawH = (float)srcH * yscale;

    float cr, cg, cb, ca;
    bgrToFloatColor(color, alpha, &cr, &cg, &cb, &ca);

    SpriteVertex* v = allocQuad(dr);
    float qx[4] = { x, x + drawW, x + drawW, x };
    float qy[4] = { y, y, y + drawH, y + drawH };
    float cx[4], cy[4];

    if (angleDeg != 0.0f) {
        float rad = -angleDeg * (3.14159265f / 180.0f);
        float cosA = cosf(rad);
        float sinA = sinf(rad);
        for (int i = 0; i < 4; i++) {
            float dx = qx[i] - pivotX;
            float dy = qy[i] - pivotY;
            cx[i] = cosA * dx - sinA * dy + pivotX;
            cy[i] = sinA * dx + cosA * dy + pivotY;
        }
    } else {
        for (int i = 0; i < 4; i++) {
            cx[i] = qx[i];
            cy[i] = qy[i];
        }
    }

    float sx[4], sy[4];
    for (int i = 0; i < 4; i++) {
        transformPoint(dr, cx[i], cy[i], &sx[i], &sy[i]);
    }

    setVertex(&v[0], sx[0], sy[0], u0, v0, cr, cg, cb, ca);
    setVertex(&v[1], sx[1], sy[1], u1, v0, cr, cg, cb, ca);
    setVertex(&v[2], sx[2], sy[2], u1, v1, cr, cg, cb, ca);
    setVertex(&v[3], sx[3], sy[3], u0, v1, cr, cg, cb, ca);
}

static void d3d9DrawSpritePos(Renderer* renderer, int32_t tpagIndex, float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4, float alpha) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    DataWin* dw = renderer->dataWin;

    if (0 > tpagIndex || (uint32_t)tpagIndex >= dw->tpag.count) {
        static int invalidTpagLog = 0;
        d3d9DiagLimited(&invalidTpagLog, 64, "D3D9: drawSpritePos invalid tpag=%d count=%u", tpagIndex, dw->tpag.count);
        return;
    }
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    int32_t texPageId = tpag->texturePageId;
    if (0 > texPageId || (uint32_t)texPageId >= dr->textureCount) {
        static int invalidTextureLog = 0;
        d3d9DiagLimited(&invalidTextureLog, 64, "D3D9: drawSpritePos invalid texPage=%d textureCount=%u tpag=%d",
                        texPageId, dr->textureCount, tpagIndex);
        return;
    }
    ensureTexturePageLoaded(dr, (uint32_t)texPageId);
    if (!dr->textures[texPageId]) {
        static int nullTextureLog = 0;
        d3d9DiagLimited(&nullTextureLog, 64, "D3D9: drawSprite null texture page=%d tpag=%d", texPageId, tpagIndex);
        return;
    }

    ensureTexture(dr, texPageId);

    float texW = (float)dr->textureWidths[texPageId];
    float texH = (float)dr->textureHeights[texPageId];
    if (texW <= 0 || texH <= 0) return;

    float u0 = texelStart((float)tpag->sourceX, texW);
    float v0 = texelStart((float)tpag->sourceY, texH);
    float u1 = texelEnd((float)tpag->sourceX, (float)tpag->sourceWidth, texW);
    float v1 = texelEnd((float)tpag->sourceY, (float)tpag->sourceHeight, texH);

    float cr, cg, cb, ca;
    bgrToFloatColor(renderer->drawColor, alpha, &cr, &cg, &cb, &ca);

    SpriteVertex* v = allocQuad(dr);
    float sx, sy;
    transformPoint(dr, x1, y1, &sx, &sy); setVertex(&v[0], sx, sy, u0, v0, cr, cg, cb, ca);
    transformPoint(dr, x2, y2, &sx, &sy); setVertex(&v[1], sx, sy, u1, v0, cr, cg, cb, ca);
    transformPoint(dr, x3, y3, &sx, &sy); setVertex(&v[2], sx, sy, u1, v1, cr, cg, cb, ca);
    transformPoint(dr, x4, y4, &sx, &sy); setVertex(&v[3], sx, sy, u0, v1, cr, cg, cb, ca);
}

static void d3d9DrawRectangle(Renderer* renderer, float x1, float y1, float x2, float y2,
                               uint32_t color, float alpha, bool outline) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;

    if (outline) {
        // Draw 4 lines as thin rectangles
        float lw = 1.0f;
        d3d9DrawRectangle(renderer, x1, y1, x2, y1 + lw, color, alpha, false); // top
        d3d9DrawRectangle(renderer, x1, y2 - lw, x2, y2, color, alpha, false); // bottom
        d3d9DrawRectangle(renderer, x1, y1, x1 + lw, y2, color, alpha, false); // left
        d3d9DrawRectangle(renderer, x2 - lw, y1, x2, y2, color, alpha, false); // right
        return;
    }

    ensureTexture(dr, -1); // white texture

    float cr, cg, cb, ca;
    bgrToFloatColor(color, alpha, &cr, &cg, &cb, &ca);
    SpriteVertex* v = allocQuad(dr);

    float sx0, sy0, sx1, sy1;
    transformPoint(dr, x1, y1, &sx0, &sy0);
    transformPoint(dr, x2, y2, &sx1, &sy1);

    setVertex(&v[0], sx0, sy0, 0, 0, cr, cg, cb, ca);
    setVertex(&v[1], sx1, sy0, 1, 0, cr, cg, cb, ca);
    setVertex(&v[2], sx1, sy1, 1, 1, cr, cg, cb, ca);
    setVertex(&v[3], sx0, sy1, 0, 1, cr, cg, cb, ca);
}

static void d3d9DrawLine(Renderer* renderer, float x1, float y1, float x2, float y2,
                          float width, uint32_t color, float alpha) {
    // Draw line as a thin rotated rectangle
    float dx = x2 - x1;
    float dy = y2 - y1;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.001f) return;

    float nx = -dy / len * width * 0.5f;
    float ny = dx / len * width * 0.5f;

    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    ensureTexture(dr, -1);
    float cr, cg, cb, ca;
    bgrToFloatColor(color, alpha, &cr, &cg, &cb, &ca);

    SpriteVertex* v = allocQuad(dr);
    float sx, sy;

    transformPoint(dr, x1 + nx, y1 + ny, &sx, &sy); setVertex(&v[0], sx, sy, 0, 0, cr, cg, cb, ca);
    transformPoint(dr, x2 + nx, y2 + ny, &sx, &sy); setVertex(&v[1], sx, sy, 1, 0, cr, cg, cb, ca);
    transformPoint(dr, x2 - nx, y2 - ny, &sx, &sy); setVertex(&v[2], sx, sy, 1, 1, cr, cg, cb, ca);
    transformPoint(dr, x1 - nx, y1 - ny, &sx, &sy); setVertex(&v[3], sx, sy, 0, 1, cr, cg, cb, ca);
}

static void d3d9DrawLineColor(Renderer* renderer, float x1, float y1, float x2, float y2,
                               float width, uint32_t color1, uint32_t color2, float alpha) {
    float dx = x2 - x1;
    float dy = y2 - y1;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.001f) return;

    float nx = -dy / len * width * 0.5f;
    float ny = dx / len * width * 0.5f;

    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    ensureTexture(dr, -1);
    float c1r, c1g, c1b, c1a;
    bgrToFloatColor(color1, alpha, &c1r, &c1g, &c1b, &c1a);
    float c2r, c2g, c2b, c2a;
    bgrToFloatColor(color2, alpha, &c2r, &c2g, &c2b, &c2a);

    SpriteVertex* v = allocQuad(dr);
    float sx, sy;

    transformPoint(dr, x1 + nx, y1 + ny, &sx, &sy); setVertex(&v[0], sx, sy, 0, 0, c1r, c1g, c1b, c1a);
    transformPoint(dr, x2 + nx, y2 + ny, &sx, &sy); setVertex(&v[1], sx, sy, 1, 0, c2r, c2g, c2b, c2a);
    transformPoint(dr, x2 - nx, y2 - ny, &sx, &sy); setVertex(&v[2], sx, sy, 1, 1, c2r, c2g, c2b, c2a);
    transformPoint(dr, x1 - nx, y1 - ny, &sx, &sy); setVertex(&v[3], sx, sy, 0, 1, c1r, c1g, c1b, c1a);
}

static void d3d9DrawRectangleColor(Renderer* renderer, float x1, float y1, float x2, float y2,
                                   uint32_t color1, uint32_t color2, uint32_t color3, uint32_t color4,
                                   float alpha, bool outline) {
    (void)color2;
    (void)color3;
    (void)color4;
    d3d9DrawRectangle(renderer, x1, y1, x2, y2, color1, alpha, outline);
}

static void d3d9DrawTriangle(Renderer* renderer, float x1, float y1, float x2, float y2, float x3, float y3, bool outline) {
    if (outline) {
        d3d9DrawLine(renderer, x1, y1, x2, y2, 1.0f, renderer->drawColor, renderer->drawAlpha);
        d3d9DrawLine(renderer, x2, y2, x3, y3, 1.0f, renderer->drawColor, renderer->drawAlpha);
        d3d9DrawLine(renderer, x3, y3, x1, y1, 1.0f, renderer->drawColor, renderer->drawAlpha);
        return;
    }
    d3d9DrawRectangle(renderer, x1, y1, x2, y2, renderer->drawColor, renderer->drawAlpha, false);
}

static void d3d9DrawText(Renderer* renderer, const char* text, float x, float y,
                          float xscale, float yscale, float angleDeg, float lineSeparation) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    DataWin* dw = renderer->dataWin;
    int32_t fontIndex = renderer->drawFont;
    if (0 > fontIndex || (uint32_t)fontIndex >= dw->font.count) return;

    Font* font = &dw->font.fonts[fontIndex];
    uint32_t color = renderer->drawColor;
    float alpha = renderer->drawAlpha;

    // Resolve font texture page
    int32_t fontTpagIndex = font->tpagIndex;
    if (0 > fontTpagIndex) return;

    TexturePageItem* fontTpag = &dw->tpag.items[fontTpagIndex];
    int16_t pageId = fontTpag->texturePageId;
    if (0 > pageId || dr->textureCount <= (uint32_t)pageId) return;
    ensureTexturePageLoaded(dr, (uint32_t)pageId);
    if (!dr->textures[pageId]) return;

    float texW = (float)dr->textureWidths[pageId];
    float texH = (float)dr->textureHeights[pageId];
    if (texW <= 0 || texH <= 0) return;

    float cr, cg, cb, ca;
    bgrToFloatColor(color, alpha, &cr, &cg, &cb, &ca);

    // Preprocess: convert # to \n (and \# to literal #)
    PreprocessedText processedText = TextUtils_preprocessGmlText(text);
    const char* processed = processedText.text;
    int32_t textLen = (int32_t)strlen(processed);

    // Count lines
    int32_t lineCount = TextUtils_countLines(processed, textLen);
    float lineStride = 0.0f <= lineSeparation ? lineSeparation : TextUtils_lineStride(font);

    // Vertical alignment offset
    float totalHeight = (float)lineCount * lineStride;
    float valignOffset = 0;
    if (renderer->drawValign == 1) valignOffset = -totalHeight / 2.0f;
    else if (renderer->drawValign == 2) valignOffset = -totalHeight;

    float fontScaleX = xscale * font->scaleX;
    float fontScaleY = yscale * font->scaleY;

    // Build rotation transform (if needed)
    float cosA = 1.0f, sinA = 0.0f;
    bool hasRotation = (angleDeg != 0.0f);
    if (hasRotation) {
        float rad = -angleDeg * (3.14159265f / 180.0f);
        cosA = cosf(rad);
        sinA = sinf(rad);
    }

    ensureTexture(dr, (int32_t)pageId);

    // Iterate through lines
    float cursorY = valignOffset;
    int32_t lineStart = 0;

    for (int32_t lineIdx = 0; lineCount > lineIdx; lineIdx++) {
        // Find end of current line
        int32_t lineEnd = lineStart;
        while (textLen > lineEnd && !TextUtils_isNewlineChar(processed[lineEnd])) {
            lineEnd++;
        }
        int32_t lineLen = lineEnd - lineStart;

        // Horizontal alignment offset for this line
        float lineWidth = TextUtils_measureLineWidth(font, processed + lineStart, lineLen);
        float halignOffset = 0;
        if (renderer->drawHalign == 1) halignOffset = -lineWidth / 2.0f;
        else if (renderer->drawHalign == 2) halignOffset = -lineWidth;

        float cursorX = halignOffset;

        // Render each glyph
        int32_t pos = 0;
        while (lineLen > pos) {
            uint16_t ch = TextUtils_decodeUtf8(processed + lineStart, lineLen, &pos);
            FontGlyph* glyph = TextUtils_findGlyph(font, ch);
            if (!glyph) continue;
            if (glyph->sourceWidth == 0 || glyph->sourceHeight == 0) {
                cursorX += glyph->shift;
                continue;
            }

            // Compute UVs from glyph position in the font's atlas
            float u0 = texelStart((float)(fontTpag->sourceX + glyph->sourceX), texW);
            float v0 = texelStart((float)(fontTpag->sourceY + glyph->sourceY), texH);
            float u1 = texelEnd((float)(fontTpag->sourceX + glyph->sourceX), (float)glyph->sourceWidth, texW);
            float v1 = texelEnd((float)(fontTpag->sourceY + glyph->sourceY), (float)glyph->sourceHeight, texH);

            // Local quad position
            float localX0 = cursorX + glyph->offset;
            float localY0 = cursorY;
            float localX1 = localX0 + (float)glyph->sourceWidth;
            float localY1 = localY0 + (float)glyph->sourceHeight;

            // Scale
            float sx0 = localX0 * fontScaleX;
            float sy0 = localY0 * fontScaleY;
            float sx1 = localX1 * fontScaleX;
            float sy1 = localY1 * fontScaleY;

            // Build 4 corners (with optional rotation)
            float cx[4], cy[4];
            if (hasRotation) {
                float lx[4] = { sx0, sx1, sx1, sx0 };
                float ly[4] = { sy0, sy0, sy1, sy1 };
                for (int i = 0; i < 4; i++) {
                    cx[i] = lx[i] * cosA - ly[i] * sinA;
                    cy[i] = lx[i] * sinA + ly[i] * cosA;
                }
            } else {
                cx[0] = sx0; cy[0] = sy0;
                cx[1] = sx1; cy[1] = sy0;
                cx[2] = sx1; cy[2] = sy1;
                cx[3] = sx0; cy[3] = sy1;
            }

            SpriteVertex* v = allocQuad(dr);
            float screenX, screenY;
            for (int i = 0; i < 4; i++) {
                transformPoint(dr, x + cx[i], y + cy[i], &screenX, &screenY);
                v[i].x = screenX - 0.5f;
                v[i].y = screenY - 0.5f;
                v[i].z = 0.0f;
                v[i].w = 1.0f;
                v[i].r = cr; v[i].g = cg; v[i].b = cb; v[i].a = ca;
            }
            v[0].u = u0; v[0].v = v0;
            v[1].u = u1; v[1].v = v0;
            v[2].u = u1; v[2].v = v1;
            v[3].u = u0; v[3].v = v1;

            // Advance cursor (shift + kerning)
            cursorX += glyph->shift;
            if (lineLen > pos) {
                int32_t savedPos = pos;
                uint16_t nextCh = TextUtils_decodeUtf8(processed + lineStart, lineLen, &pos);
                pos = savedPos;
                cursorX += TextUtils_getKerningOffset(glyph, nextCh);
            }
        }

        cursorY += lineStride;

        // Advance past the newline
        if (textLen > lineEnd) {
            lineStart = TextUtils_skipNewline(processed, lineEnd, textLen);
        } else {
            lineStart = lineEnd;
        }
    }

    PreprocessedText_free(processedText);
}

static void d3d9DrawTextColor(Renderer* renderer, const char* text, float x, float y,
                              float xscale, float yscale, float angleDeg,
                              int32_t c1, int32_t c2, int32_t c3, int32_t c4,
                              float alpha, float lineSeparation) {
    uint32_t previousColor = renderer->drawColor;
    float previousAlpha = renderer->drawAlpha;
    (void)c2;
    (void)c3;
    (void)c4;
    renderer->drawColor = (uint32_t)c1;
    renderer->drawAlpha = alpha;
    d3d9DrawText(renderer, text, x, y, xscale, yscale, angleDeg, lineSeparation);
    renderer->drawColor = previousColor;
    renderer->drawAlpha = previousAlpha;
}

static void d3d9Flush(Renderer* renderer) {
    flushBatch((D3D9Renderer*)renderer);
}

static void d3d9ClearScreen(Renderer* renderer, uint32_t color, float alpha) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    flushBatch(dr);
    uint8_t r = (uint8_t)(color & 0xFF);
    uint8_t g = (uint8_t)((color >> 8) & 0xFF);
    uint8_t b = (uint8_t)((color >> 16) & 0xFF);
    uint8_t a = (uint8_t)(alpha * 255.0f);
    Dev(dr)->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(a, r, g, b), 1.0f, 0);
}

// ===[ Dynamic sprites captured from the application surface ]===
// sprite_create_from_surface: resolve the top-left area of the application surface that covers
// the requested rect into a texture, then register page + TPAG + sprite like the GL backend does.
static uint8_t* gDynFree = NULL;
static uint32_t gDynFreeCap = 0;
static D3D9Renderer* gDynOwner = NULL;

static void dynEnsureCap(D3D9Renderer* dr) {
    if (gDynOwner != dr) {
        free(gDynFree);
        gDynFree = NULL;
        gDynFreeCap = 0;
        gDynOwner = dr;
    }
    if (gDynFreeCap < dr->textureCount) {
        uint8_t* n = (uint8_t*)realloc(gDynFree, dr->textureCount);
        if (!n) return;
        memset(n + gDynFreeCap, 0, dr->textureCount - gDynFreeCap);
        gDynFree = n;
        gDynFreeCap = dr->textureCount;
    }
}

static void dynReleasePageTexture(D3D9Renderer* dr, uint32_t i) {
    if (i >= dr->textureCount || !dr->textures[i]) return;
    if (dr->currentTextureIndex == (int32_t)i) {
        flushBatch(dr);
        dr->currentTextureIndex = -1;
        Dev(dr)->SetTexture(0, NULL);
    }
    ((IDirect3DTexture9*)dr->textures[i])->Release();
    dr->textures[i] = NULL;
    dr->textureWidths[i] = 0;
    dr->textureHeights[i] = 0;
    if (dr->textureLastUsedFrame) dr->textureLastUsedFrame[i] = 0;
}

static int32_t dynAcquirePage(D3D9Renderer* dr, int32_t rw, int32_t rh) {
    dynEnsureCap(dr);
    uint32_t first = dr->originalTexturePageCount;
    int32_t slot = -1;

    for (uint32_t i = first; i < dr->textureCount && i < gDynFreeCap; i++) {
        if (gDynFree[i] && dr->textures[i] && dr->textureWidths[i] == rw && dr->textureHeights[i] == rh) {
            gDynFree[i] = 0;
            return (int32_t)i;
        }
    }
    for (uint32_t i = first; i < dr->textureCount; i++) {
        bool pooled = (i < gDynFreeCap) && gDynFree[i];
        if (pooled || !dr->textures[i]) {
            dynReleasePageTexture(dr, i);
            if (i < gDynFreeCap) gDynFree[i] = 0;
            slot = (int32_t)i;
            break;
        }
    }
    if (slot < 0) {
        uint32_t n = dr->textureCount + 1;
        void** t = (void**)realloc(dr->textures, n * sizeof(void*));
        if (!t) return -1;
        dr->textures = t;
        int32_t* tw = (int32_t*)realloc(dr->textureWidths, n * sizeof(int32_t));
        if (!tw) return -1;
        dr->textureWidths = tw;
        int32_t* th = (int32_t*)realloc(dr->textureHeights, n * sizeof(int32_t));
        if (!th) return -1;
        dr->textureHeights = th;
        uint32_t* tl = (uint32_t*)realloc(dr->textureLastUsedFrame, n * sizeof(uint32_t));
        if (!tl) return -1;
        dr->textureLastUsedFrame = tl;
        slot = (int32_t)dr->textureCount;
        dr->textures[slot] = NULL;
        dr->textureWidths[slot] = 0;
        dr->textureHeights[slot] = 0;
        dr->textureLastUsedFrame[slot] = 0;
        dr->textureCount = n;
        dynEnsureCap(dr);
    }

    IDirect3DTexture9* tex = NULL;
    HRESULT hr = Dev(dr)->CreateTexture((UINT)rw, (UINT)rh, 1, 0, D3DFMT_A8R8G8B8,
                                        D3DPOOL_DEFAULT, &tex, NULL);
    if (FAILED(hr) || !tex) {
        Butterscotch_xdkDiagTrace("D3D9: dynamic page CreateTexture failed %dx%d hr=0x%08X", rw, rh, (unsigned)hr);
        return -1;
    }
    dr->textures[slot] = tex;
    dr->textureWidths[slot] = rw;
    dr->textureHeights[slot] = rh;
    dr->textureLastUsedFrame[slot] = dr->frameCounter;
    return slot;
}

static uint32_t dynFindOrAllocTpagSlot(DataWin* dw, uint32_t originalTpagCount) {
    for (uint32_t i = originalTpagCount; i < dw->tpag.count; i++) {
        if (dw->tpag.items[i].texturePageId == -1) return i;
    }
    uint32_t newIndex = dw->tpag.count;
    TexturePageItem* items = (TexturePageItem*)realloc(dw->tpag.items, (newIndex + 1) * sizeof(TexturePageItem));
    if (!items) return UINT_MAX;
    dw->tpag.items = items;
    dw->tpag.count = newIndex + 1;
    memset(&dw->tpag.items[newIndex], 0, sizeof(TexturePageItem));
    dw->tpag.items[newIndex].texturePageId = -1;
    return newIndex;
}

static int32_t d3d9CreateSpriteFromSurface(Renderer* renderer, int32_t surfaceID, int32_t x, int32_t y,
                                            int32_t w, int32_t h, bool removeback,
                                            bool smooth, int32_t xorig, int32_t yorig) {
    (void)removeback; (void)smooth;
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    DataWin* dw = renderer->dataWin;
    static int logged = 0;
    if (surfaceID != APPLICATION_SURFACE_ID || !dr->appSurfaceLevel || !dw || w <= 0 || h <= 0 || x < 0 || y < 0) {
        d3d9DiagLimited(&logged, 64, "D3D9: sprite_create_from_surface unsupported surface=%d rect=%d,%d %dx%d",
                        surfaceID, x, y, w, h);
        return -1;
    }
    if (x + w > dr->appSurfaceW) w = dr->appSurfaceW - x;
    if (y + h > dr->appSurfaceH) h = dr->appSurfaceH - y;
    if (w <= 0 || h <= 0) return -1;

    int32_t rw = (x + w + 31) & ~31;
    int32_t rh = (y + h + 31) & ~31;
    if (rw > dr->appSurfaceAllocW) rw = dr->appSurfaceAllocW;
    if (rh > dr->appSurfaceAllocH) rh = dr->appSurfaceAllocH;

    flushBatch(dr);

    int32_t page = dynAcquirePage(dr, rw, rh);
    if (page < 0) return -1;

    D3DRECT rect;
    rect.x1 = 0;
    rect.y1 = 0;
    rect.x2 = rw;
    rect.y2 = rh;
    Dev(dr)->Resolve(D3DRESOLVE_RENDERTARGET0, &rect, (IDirect3DBaseTexture9*)dr->textures[page],
                     NULL, 0, 0, NULL, 1.0f, 0, NULL);

    uint32_t tpagIndex = dynFindOrAllocTpagSlot(dw, dr->originalTpagCount);
    if (tpagIndex == UINT_MAX) {
        dynEnsureCap(dr);
        if ((uint32_t)page < gDynFreeCap) gDynFree[page] = 1;
        return -1;
    }
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    tpag->sourceX = (uint16_t)x;
    tpag->sourceY = (uint16_t)y;
    tpag->sourceWidth = (uint16_t)w;
    tpag->sourceHeight = (uint16_t)h;
    tpag->targetX = 0;
    tpag->targetY = 0;
    tpag->targetWidth = (uint16_t)w;
    tpag->targetHeight = (uint16_t)h;
    tpag->boundingWidth = (uint16_t)w;
    tpag->boundingHeight = (uint16_t)h;
    tpag->texturePageId = (int16_t)page;

    uint32_t spriteIndex = DataWin_allocSpriteSlot(dw, dr->originalSpriteCount);
    Sprite* sprite = &dw->sprt.sprites[spriteIndex];
    sprite->width = (uint32_t)w;
    sprite->height = (uint32_t)h;
    sprite->originX = xorig;
    sprite->originY = yorig;
    sprite->textureCount = 1;
    sprite->tpagIndices = (int32_t*)malloc(sizeof(int32_t));
    sprite->tpagIndices[0] = (int32_t)tpagIndex;
    sprite->maskCount = 0;
    sprite->masks = NULL;

    static int createdLog = 0;
    d3d9DiagLimited(&createdLog, 64, "D3D9: dynamic sprite %u from app surface rect=%d,%d %dx%d page=%d tex=%dx%d",
                    spriteIndex, x, y, w, h, page, rw, rh);
    return (int32_t)spriteIndex;
}

static void d3d9DeleteSprite(Renderer* renderer, int32_t spriteIndex) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    DataWin* dw = renderer->dataWin;
    if (!dw || spriteIndex < 0 || (uint32_t)spriteIndex >= dw->sprt.count) return;
    if ((uint32_t)spriteIndex < dr->originalSpriteCount) return;

    Sprite* sprite = &dw->sprt.sprites[spriteIndex];
    if (sprite->textureCount == 0) return;

    dynEnsureCap(dr);
    for (uint32_t i = 0; i < sprite->textureCount; i++) {
        int32_t tpagIdx = sprite->tpagIndices[i];
        if (tpagIdx >= 0 && (uint32_t)tpagIdx >= dr->originalTpagCount && (uint32_t)tpagIdx < dw->tpag.count) {
            TexturePageItem* tpag = &dw->tpag.items[tpagIdx];
            int32_t pageId = tpag->texturePageId;
            if (pageId >= (int32_t)dr->originalTexturePageCount && (uint32_t)pageId < dr->textureCount
                && (uint32_t)pageId < gDynFreeCap) {
                gDynFree[pageId] = 1;
            }
            tpag->texturePageId = -1;
        }
    }
    free(sprite->tpagIndices);
    const char* keepName = sprite->name;
    memset(sprite, 0, sizeof(Sprite));
    sprite->name = keepName;
}

static int32_t d3d9CreateSpriteFromSurfaceStub(Renderer* renderer, int32_t surfaceID, int32_t x, int32_t y,
                                            int32_t w, int32_t h, bool removeback,
                                            bool smooth, int32_t xorig, int32_t yorig) {
    (void)renderer;
    static int logged = 0;
    d3d9DiagLimited(&logged, 64,
                    "D3D9: sprite_create_from_surface stub surface=%d rect=%d,%d %dx%d removeback=%d smooth=%d origin=%d,%d",
                    surfaceID, x, y, w, h, removeback ? 1 : 0, smooth ? 1 : 0, xorig, yorig);
    // TODO: implement surface capture
    return -1;
}

static void d3d9DeleteSpriteStub(Renderer* renderer, int32_t spriteIndex) {
    (void)renderer; (void)spriteIndex;
    // TODO: implement dynamic sprite deletion
}

static void d3d9GpuSetBlendMode(Renderer* renderer, int32_t mode) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    IDirect3DDevice9* dev = Dev(dr);
    flushBatch(dr);
    switch (mode) {
        case bm_normal:
            d3d9SetNormalBlend(dev);
            break;
        case bm_add:
            dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
            dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
            dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
            dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
            break;
        case bm_subtract:
            dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
            dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_REVSUBTRACT);
            dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
            dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
            break;
        case bm_reverse_subtract:
            dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
            dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_SUBTRACT);
            dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
            dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
            break;
        case bm_max:
            dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
            dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_MAX);
            dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
            dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
            break;
        case bm_min:
            dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
            dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_MIN);
            dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
            dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
            break;
        default:
            d3d9SetNormalBlend(dev);
            break;
    }
}

static void d3d9GpuSetBlendModeExt(Renderer* renderer, int32_t sfactor, int32_t dfactor) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    IDirect3DDevice9* dev = Dev(dr);
    flushBatch(dr);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    dev->SetRenderState(D3DRS_SRCBLEND, gmlBlendFactorToD3D(sfactor));
    dev->SetRenderState(D3DRS_DESTBLEND, gmlBlendFactorToD3D(dfactor));
}

static void d3d9GpuSetBlendEnable(Renderer* renderer, bool enable) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    flushBatch(dr);
    Dev(dr)->SetRenderState(D3DRS_ALPHABLENDENABLE, enable ? TRUE : FALSE);
}

static void d3d9GpuSetAlphaTestEnable(Renderer* renderer, bool enable) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    flushBatch(dr);
    Dev(dr)->SetRenderState(D3DRS_ALPHATESTENABLE, enable ? TRUE : FALSE);
    Dev(dr)->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
}

static void d3d9GpuSetAlphaTestRef(Renderer* renderer, uint8_t ref) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    flushBatch(dr);
    Dev(dr)->SetRenderState(D3DRS_ALPHAREF, (DWORD)ref);
}

static void d3d9GpuSetColorWriteEnable(Renderer* renderer, bool red, bool green, bool blue, bool alpha) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    DWORD mask = 0;
    flushBatch(dr);
    if (red) mask |= D3DCOLORWRITEENABLE_RED;
    if (green) mask |= D3DCOLORWRITEENABLE_GREEN;
    if (blue) mask |= D3DCOLORWRITEENABLE_BLUE;
    if (alpha) mask |= D3DCOLORWRITEENABLE_ALPHA;
    Dev(dr)->SetRenderState(D3DRS_COLORWRITEENABLE, mask);
}
static void d3d9GpuGetColorWriteEnable(Renderer* renderer, bool* red, bool* green, bool* blue, bool* alpha) {
    (void)renderer;
    if (red) *red = true;
    if (green) *green = true;
    if (blue) *blue = true;
    if (alpha) *alpha = true;
}
static bool d3d9GpuGetBlendEnable(Renderer* renderer) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    DWORD enabled = TRUE;
    Dev(dr)->GetRenderState(D3DRS_ALPHABLENDENABLE, &enabled);
    return enabled != FALSE;
}
static void d3d9GpuSetFog(Renderer* renderer, bool enable, uint32_t color) { (void)renderer; (void)enable; (void)color; }
static int32_t d3d9CreateSurface(Renderer* renderer, int32_t width, int32_t height) {
    (void)renderer;
    static int logged = 0;
    d3d9DiagLimited(&logged, 64, "D3D9: surface_create stub size=%dx%d", width, height);
    return -1;
}

static bool d3d9SurfaceExists(Renderer* renderer, int32_t surfaceID) {
    (void)renderer;
    static int logged = 0;
    if (surfaceID != APPLICATION_SURFACE_ID)
        d3d9DiagLimited(&logged, 64, "D3D9: surface_exists stub id=%d", surfaceID);
    return surfaceID == APPLICATION_SURFACE_ID;
}

static bool d3d9SetRenderTarget(Renderer* renderer, int32_t surfaceID) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    IDirect3DDevice9* dev = Dev(dr);
    static int logged = 0;
    if (surfaceID == APPLICATION_SURFACE_ID) {
        if (!dr->appSurfaceLevel) return false;
        flushBatch(dr);
        HRESULT hr = dev->SetRenderTarget(0, (IDirect3DSurface9*)dr->appSurfaceLevel);
        if (FAILED(hr)) {
            d3d9DiagLimited(&logged, 64, "D3D9: SetRenderTarget(app_surface) failed hr=0x%08X", (unsigned)hr);
            return false;
        }
        D3DVIEWPORT9 vp;
        vp.X = 0;
        vp.Y = 0;
        vp.Width = (DWORD)dr->appSurfaceW;
        vp.Height = (DWORD)dr->appSurfaceH;
        vp.MinZ = 0.0f;
        vp.MaxZ = 1.0f;
        dev->SetViewport(&vp);
        dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        dr->renderingToApplicationSurface = true;
        dr->appSurfaceResolved = false;
        setApplicationSurfaceTransform(dr);
        return true;
    }
    d3d9DiagLimited(&logged, 64, "D3D9: surface_set_target unsupported id=%d", surfaceID);
    return false;
}

static int32_t d3d9EnsureApplicationSurface(Renderer* renderer, int32_t width, int32_t height) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    IDirect3DDevice9* dev = Dev(dr);
    int32_t allocW = (width + 7) & ~7;
    int32_t allocH = (height + 7) & ~7;
    if (dr->appSurfaceTexture && dr->appSurfaceW == width && dr->appSurfaceH == height &&
        dr->appSurfaceAllocW == allocW && dr->appSurfaceAllocH == allocH) {
        return APPLICATION_SURFACE_ID;
    }

    releaseApplicationSurface(dr);

    IDirect3DTexture9* sampleTex = NULL;
    IDirect3DSurface9* surface = NULL;

    HRESULT hr = dev->CreateTexture((UINT)allocW, (UINT)allocH, 1, 0, D3DFMT_A8R8G8B8,
                                    D3DPOOL_DEFAULT, &sampleTex, NULL);
    if (FAILED(hr) || !sampleTex) {
        Butterscotch_xdkDiagTrace("D3D9: CreateTexture(app sample) failed %dx%d hr=0x%08X", allocW, allocH, (unsigned)hr);
        return APPLICATION_SURFACE_ID;
    }

    hr = dev->CreateRenderTarget((UINT)allocW, (UINT)allocH, D3DFMT_A8R8G8B8,
                                 D3DMULTISAMPLE_NONE, 0, FALSE, &surface, NULL);
    if (FAILED(hr) || !surface) {
        Butterscotch_xdkDiagTrace("D3D9: CreateRenderTarget(app rt) failed %dx%d hr=0x%08X", allocW, allocH, (unsigned)hr);
        sampleTex->Release();
        return APPLICATION_SURFACE_ID;
    }

    dr->appSurfaceTexture = sampleTex;
    dr->appRenderTexture = NULL;
    dr->appSurfaceLevel = surface;
    dr->appSurfaceW = width;
    dr->appSurfaceH = height;
    dr->appSurfaceAllocW = allocW;
    dr->appSurfaceAllocH = allocH;
    dr->appSurfaceResolved = false;
    Butterscotch_xdkDiagTrace("D3D9: application_surface created %dx%d alloc=%dx%d", width, height, allocW, allocH);
    return APPLICATION_SURFACE_ID;
}
static float d3d9GetSurfaceWidth(Renderer* renderer, int32_t surfaceID) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    if (surfaceID == APPLICATION_SURFACE_ID) return (float)dr->appSurfaceW;
    return 0.0f;
}
static float d3d9GetSurfaceHeight(Renderer* renderer, int32_t surfaceID) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    if (surfaceID == APPLICATION_SURFACE_ID) return (float)dr->appSurfaceH;
    return 0.0f;
}
static void d3d9DrawSurface(Renderer* renderer, int32_t surfaceID, int32_t srcLeft, int32_t srcTop, int32_t srcWidth, int32_t srcHeight, float x, float y, float xscale, float yscale, float angleDeg, uint32_t color, float alpha) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    if (surfaceID != APPLICATION_SURFACE_ID || !dr->appSurfaceTexture) {
        static int logged = 0;
        d3d9DiagLimited(&logged, 128, "D3D9: draw_surface unsupported id=%d src=%d,%d %dx%d", surfaceID, srcLeft, srcTop, srcWidth, srcHeight);
        return;
    }

    if (dr->renderingToApplicationSurface) {
        static int switchLogged = 0;
        flushBatch(dr);
        resolveApplicationSurface(dr);
        bindBackbuffer(dr);
        resetFullBackbufferState(dr);
        dr->renderingToApplicationSurface = false;
        setWindowSurfaceTransform(dr);
        d3d9DiagLimited(&switchLogged, 64,
                        "D3D9: manual application_surface present room=%d src=%d,%d %dx%d dst=%.2f,%.2f scale=%.2f,%.2f app=%dx%d screen=%dx%d transform=%.3f,%.3f",
                        renderer->runner ? renderer->runner->currentRoomIndex : -1,
                        srcLeft, srcTop, srcWidth, srcHeight,
                        x, y, xscale, yscale,
                        dr->appSurfaceW, dr->appSurfaceH, dr->screenW, dr->screenH,
                        dr->portScaleX, dr->portScaleY);
        Dev(dr)->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
        applyPointSampling(Dev(dr));
    }

    resolveApplicationSurface(dr);
    if (srcWidth < 0 || srcHeight < 0) {
        srcLeft = 0;
        srcTop = 0;
        srcWidth = dr->appSurfaceW;
        srcHeight = dr->appSurfaceH;
    }
    if (srcWidth <= 0 || srcHeight <= 0) return;

    flushBatch(dr);
    IDirect3DDevice9* dev = Dev(dr);
    dev->SetTexture(0, (IDirect3DBaseTexture9*)dr->appSurfaceTexture);
    dr->currentTextureIndex = -2;

    float texW = (float)dr->appSurfaceAllocW;
    float texH = (float)dr->appSurfaceAllocH;
    float u0 = (float)srcLeft / texW;
    float v0 = (float)srcTop / texH;
    float u1 = (float)(srcLeft + srcWidth) / texW;
    float v1 = (float)(srcTop + srcHeight) / texH;

    float drawW = (float)srcWidth * xscale;
    float drawH = (float)srcHeight * yscale;
    float qx[4] = { x, x + drawW, x + drawW, x };
    float qy[4] = { y, y, y + drawH, y + drawH };
    float cx[4], cy[4];
    if (angleDeg != 0.0f) {
        float rad = -angleDeg * (3.14159265f / 180.0f);
        float cosA = cosf(rad);
        float sinA = sinf(rad);
        for (int i = 0; i < 4; i++) {
            float dx = qx[i] - x;
            float dy = qy[i] - y;
            cx[i] = cosA * dx - sinA * dy + x;
            cy[i] = sinA * dx + cosA * dy + y;
        }
    } else {
        for (int i = 0; i < 4; i++) {
            cx[i] = qx[i];
            cy[i] = qy[i];
        }
    }

    float cr, cg, cb, ca;
    bgrToFloatColor(color, alpha, &cr, &cg, &cb, &ca);

    bool manualPostAppSurface =
        renderer->runner &&
        !renderer->runner->appSurfaceKeepWindowSize &&
        !renderer->runner->appSurfaceAutoDraw &&
        renderer->drawPhase == RENDER_PHASE_POST;
    float savedOffsetX = dr->offsetX;
    float savedOffsetY = dr->offsetY;
    float savedPortScaleX = dr->portScaleX;
    float savedPortScaleY = dr->portScaleY;
    float savedPortOffsetX = dr->portOffsetX;
    float savedPortOffsetY = dr->portOffsetY;
    if (manualPostAppSurface) {
        float insetScaleX = dr->screenW > 0 && dr->appSurfaceW > 0
            ? (float)dr->screenW / ((float)dr->appSurfaceW * 1.5f)
            : dr->renderScale;
        float insetScaleY = dr->screenH > 0 && dr->appSurfaceH > 0
            ? (float)dr->screenH / ((float)dr->appSurfaceH * 1.125f)
            : dr->renderScale;
        float insetScale = insetScaleX < insetScaleY ? insetScaleX : insetScaleY;
        float insetOffsetX = ((float)dr->screenW - (float)dr->appSurfaceW * insetScale) * 0.5f;
        float insetOffsetY = ((float)dr->screenH - (float)dr->appSurfaceH * insetScale) * 0.5f;

        static int logged = 0;
        d3d9DiagLimited(&logged, 64,
                        "D3D9: Draw Post application_surface using inset viewport room=%d dst=%.2f,%.2f scale=%.2f,%.2f app=%dx%d insetScale=%.3f offs=%.1f,%.1f",
                        renderer->runner->currentRoomIndex,
                        x, y, xscale, yscale,
                        dr->appSurfaceW, dr->appSurfaceH,
                        insetScale, insetOffsetX, insetOffsetY);
        dr->offsetX = 0.0f;
        dr->offsetY = 0.0f;
        dr->portScaleX = insetScale;
        dr->portScaleY = insetScale;
        dr->portOffsetX = insetOffsetX;
        dr->portOffsetY = insetOffsetY;
    }

    SpriteVertex v[4];
    float sx, sy;
    transformPoint(dr, cx[0], cy[0], &sx, &sy); setVertex(&v[0], sx, sy, u0, v0, cr, cg, cb, ca);
    transformPoint(dr, cx[1], cy[1], &sx, &sy); setVertex(&v[1], sx, sy, u1, v0, cr, cg, cb, ca);
    transformPoint(dr, cx[2], cy[2], &sx, &sy); setVertex(&v[2], sx, sy, u1, v1, cr, cg, cb, ca);
    transformPoint(dr, cx[3], cy[3], &sx, &sy); setVertex(&v[3], sx, sy, u0, v1, cr, cg, cb, ca);
    dev->DrawPrimitiveUP(D3DPT_QUADLIST, 1, v, sizeof(SpriteVertex));

    if (manualPostAppSurface) {
        dr->offsetX = savedOffsetX;
        dr->offsetY = savedOffsetY;
        dr->portScaleX = savedPortScaleX;
        dr->portScaleY = savedPortScaleY;
        dr->portOffsetX = savedPortOffsetX;
        dr->portOffsetY = savedPortOffsetY;
    }
}
static void d3d9SurfaceResize(Renderer* renderer, int32_t surfaceID, int32_t width, int32_t height) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    if (surfaceID == APPLICATION_SURFACE_ID || (renderer->runner && surfaceID == renderer->runner->applicationSurfaceId)) {
        Butterscotch_xdkDiagTrace("D3D9: application_surface resize requested %dx%d old=%dx%d room=%d",
                                  width, height, dr->appSurfaceW, dr->appSurfaceH,
                                  renderer->runner ? renderer->runner->currentRoomIndex : -1);
        if (width > 0 && height > 0 && (width != dr->appSurfaceW || height != dr->appSurfaceH)) {
            releaseApplicationSurface(dr);
        }
    }
}
static void d3d9SurfaceFree(Renderer* renderer, int32_t surfaceID) { (void)renderer; (void)surfaceID; }
static void d3d9SurfaceCopy(Renderer* renderer, int32_t destSurfaceID, int32_t destX, int32_t destY, int32_t srcSurfaceID, int32_t srcX, int32_t srcY, int32_t srcW, int32_t srcH, bool part) { (void)renderer; (void)destSurfaceID; (void)destX; (void)destY; (void)srcSurfaceID; (void)srcX; (void)srcY; (void)srcW; (void)srcH; (void)part; }
static bool d3d9SurfaceGetPixels(Renderer* renderer, int32_t surfaceID, uint8_t* outRGBA) { (void)renderer; (void)surfaceID; (void)outRGBA; return false; }
static void d3d9DrawTiledPart(Renderer* renderer, int32_t tpagIndex, int32_t srcX, int32_t srcY, int32_t srcW, int32_t srcH, float dstX, float dstY, float dstW, float dstH, uint32_t color, float alpha) {
    if (srcW <= 0 || srcH <= 0 || dstW <= 0.0f || dstH <= 0.0f) return;

    if (renderer->drawPhase == RENDER_PHASE_POST) {
        static int postTileLog = 0;
        d3d9DiagLimited(&postTileLog, 96,
                        "D3D9POST: tiled tpag=%d src=%d,%d %dx%d dst=%.2f,%.2f %.2fx%.2f room=%d",
                        tpagIndex, srcX, srcY, srcW, srcH, dstX, dstY, dstW, dstH,
                        renderer->runner ? renderer->runner->currentRoomIndex : -1);
    }

    float y = dstY;
    float remainingH = dstH;
    while (remainingH > 0.0f) {
        int32_t drawH = remainingH < (float)srcH ? (int32_t)remainingH : srcH;
        if (drawH <= 0) drawH = 1;

        float x = dstX;
        float remainingW = dstW;
        while (remainingW > 0.0f) {
            int32_t drawW = remainingW < (float)srcW ? (int32_t)remainingW : srcW;
            if (drawW <= 0) drawW = 1;

            d3d9DrawSpritePart(renderer, tpagIndex, srcX, srcY, drawW, drawH,
                               x, y, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, color, alpha);
            x += (float)drawW;
            remainingW -= (float)drawW;
        }

        y += (float)drawH;
        remainingH -= (float)drawH;
    }
}
static void d3d9GpuSetShader(Renderer* renderer, int32_t shaderIndex) {
    static int logged = 0;
    d3d9DiagLimited(&logged, 64, "D3D9: shader_set stub shader=%d", shaderIndex);
    renderer->currentShader = shaderIndex;
}
static void d3d9GpuResetShader(Renderer* renderer) {
    static int logged = 0;
    d3d9DiagLimited(&logged, 64, "D3D9: shader_reset");
    renderer->currentShader = -1;
}
static int32_t d3d9ShaderGetUniform(Renderer* renderer, int32_t shaderIndex, char* uniform) {
    (void)renderer;
    static int logged = 0;
    d3d9DiagLimited(&logged, 64, "D3D9: shader_get_uniform stub shader=%d uniform=%s", shaderIndex, uniform ? uniform : "(null)");
    return -1;
}
static int32_t d3d9ShaderGetSamplerIndex(Renderer* renderer, int32_t shaderIndex, char* uniform) {
    (void)renderer;
    static int logged = 0;
    d3d9DiagLimited(&logged, 64, "D3D9: shader_get_sampler_index stub shader=%d uniform=%s", shaderIndex, uniform ? uniform : "(null)");
    return -1;
}
static void d3d9ShaderSetUniformF(Renderer* renderer, int32_t handle, int32_t count, float value1, float value2, float value3, float value4) { (void)renderer; (void)handle; (void)count; (void)value1; (void)value2; (void)value3; (void)value4; }
static void d3d9ShaderSetUniformI(Renderer* renderer, int32_t handle, int32_t count, int32_t value1, int32_t value2, int32_t value3, int32_t value4) { (void)renderer; (void)handle; (void)count; (void)value1; (void)value2; (void)value3; (void)value4; }
static uint32_t d3d9SpriteGetTexture(Renderer* renderer, int32_t tpagIndex) { (void)renderer; return (uint32_t)(tpagIndex + 1); }
static float d3d9TextureGetTexelWidth(Renderer* renderer, uint32_t texID) { (void)renderer; (void)texID; return 1.0f; }
static float d3d9TextureGetTexelHeight(Renderer* renderer, uint32_t texID) { (void)renderer; (void)texID; return 1.0f; }
static bool d3d9TextureGetUVs(Renderer* renderer, uint32_t texID, float* outUVs) { (void)renderer; (void)texID; if (outUVs) { outUVs[0] = 0.0f; outUVs[1] = 0.0f; outUVs[2] = 1.0f; outUVs[3] = 1.0f; } return false; }
static void d3d9TextureSetStage(Renderer* renderer, int32_t slot, uint32_t texID) {
    (void)renderer;
    static int logged = 0;
    d3d9DiagLimited(&logged, 64, "D3D9: texture_set_stage stub slot=%d tex=%u", slot, (unsigned)texID);
}
static bool d3d9ShaderIsCompiled(Renderer* renderer, int32_t shader) { (void)renderer; (void)shader; return false; }
static bool d3d9ShadersSupported(Renderer* renderer) { (void)renderer; return false; }

// ===[ Vtable ]===

static RendererVtable d3d9RendererVtable = {};
#if 0
    d3d9Init,
    d3d9Destroy,
    d3d9BeginFrame,
    d3d9EndFrame,
    d3d9BeginView,
    d3d9EndView,
    d3d9DrawSprite,
    d3d9DrawSpritePart,
    d3d9DrawRectangle,
    d3d9DrawLine,
    d3d9DrawLineColor,
    d3d9DrawText,
    d3d9Flush,
    d3d9CreateSpriteFromSurface,
    d3d9DeleteSprite,
    NULL, // drawTile — use default path
};

#endif

// ===[ Public API ]===

Renderer* D3D9Renderer_create(void* pd3dDevice) {
    D3D9Renderer* dr = (D3D9Renderer*)calloc(1, sizeof(D3D9Renderer));
    d3d9RendererVtable.init = d3d9Init;
    d3d9RendererVtable.destroy = d3d9Destroy;
    d3d9RendererVtable.beginFrame = d3d9BeginFrame;
    d3d9RendererVtable.endFrameInit = d3d9EndFrameInit;
    d3d9RendererVtable.endFrameEnd = d3d9EndFrameEnd;
    d3d9RendererVtable.beginView = d3d9BeginView;
    d3d9RendererVtable.endView = d3d9EndView;
    d3d9RendererVtable.applyProjection = d3d9ApplyProjection;
    d3d9RendererVtable.beginGUI = d3d9BeginGUI;
    d3d9RendererVtable.endGUI = d3d9EndGUI;
    d3d9RendererVtable.drawSprite = d3d9DrawSprite;
    d3d9RendererVtable.drawSpritePart = d3d9DrawSpritePart;
    d3d9RendererVtable.drawSpritePos = d3d9DrawSpritePos;
    d3d9RendererVtable.drawRectangle = d3d9DrawRectangle;
    d3d9RendererVtable.drawRectangleColor = d3d9DrawRectangleColor;
    d3d9RendererVtable.drawLine = d3d9DrawLine;
    d3d9RendererVtable.drawTriangle = d3d9DrawTriangle;
    d3d9RendererVtable.drawLineColor = d3d9DrawLineColor;
    d3d9RendererVtable.drawText = d3d9DrawText;
    d3d9RendererVtable.drawTextColor = d3d9DrawTextColor;
    d3d9RendererVtable.flush = d3d9Flush;
    d3d9RendererVtable.clearScreen = d3d9ClearScreen;
    d3d9RendererVtable.createSpriteFromSurface = d3d9CreateSpriteFromSurface;
    d3d9RendererVtable.deleteSprite = d3d9DeleteSprite;
    d3d9RendererVtable.gpuSetBlendMode = d3d9GpuSetBlendMode;
    d3d9RendererVtable.gpuSetBlendModeExt = d3d9GpuSetBlendModeExt;
    d3d9RendererVtable.gpuSetBlendEnable = d3d9GpuSetBlendEnable;
    d3d9RendererVtable.gpuSetAlphaTestEnable = d3d9GpuSetAlphaTestEnable;
    d3d9RendererVtable.gpuSetAlphaTestRef = d3d9GpuSetAlphaTestRef;
    d3d9RendererVtable.gpuSetColorWriteEnable = d3d9GpuSetColorWriteEnable;
    d3d9RendererVtable.gpuGetColorWriteEnable = d3d9GpuGetColorWriteEnable;
    d3d9RendererVtable.gpuGetBlendEnable = d3d9GpuGetBlendEnable;
    d3d9RendererVtable.gpuSetFog = d3d9GpuSetFog;
    d3d9RendererVtable.drawTile = NULL;
    d3d9RendererVtable.drawTiled = NULL;
    d3d9RendererVtable.createSurface = d3d9CreateSurface;
    d3d9RendererVtable.surfaceExists = d3d9SurfaceExists;
    d3d9RendererVtable.setRenderTarget = d3d9SetRenderTarget;
    d3d9RendererVtable.ensureApplicationSurface = d3d9EnsureApplicationSurface;
    d3d9RendererVtable.getSurfaceWidth = d3d9GetSurfaceWidth;
    d3d9RendererVtable.getSurfaceHeight = d3d9GetSurfaceHeight;
    d3d9RendererVtable.drawSurface = d3d9DrawSurface;
    d3d9RendererVtable.surfaceResize = d3d9SurfaceResize;
    d3d9RendererVtable.surfaceFree = d3d9SurfaceFree;
    d3d9RendererVtable.surfaceCopy = d3d9SurfaceCopy;
    d3d9RendererVtable.surfaceGetPixels = d3d9SurfaceGetPixels;
    d3d9RendererVtable.drawTiledPart = d3d9DrawTiledPart;
    d3d9RendererVtable.gpuSetShader = d3d9GpuSetShader;
    d3d9RendererVtable.gpuResetShader = d3d9GpuResetShader;
    d3d9RendererVtable.shaderGetUniform = d3d9ShaderGetUniform;
    d3d9RendererVtable.shaderGetSamplerIndex = d3d9ShaderGetSamplerIndex;
    d3d9RendererVtable.shaderSetUniformF = d3d9ShaderSetUniformF;
    d3d9RendererVtable.shaderSetUniformI = d3d9ShaderSetUniformI;
    d3d9RendererVtable.spriteGetTexture = d3d9SpriteGetTexture;
    d3d9RendererVtable.textureGetTexelWidth = d3d9TextureGetTexelWidth;
    d3d9RendererVtable.textureGetTexelHeight = d3d9TextureGetTexelHeight;
    d3d9RendererVtable.textureGetUVs = d3d9TextureGetUVs;
    d3d9RendererVtable.textureSetStage = d3d9TextureSetStage;
    d3d9RendererVtable.shaderIsCompiled = d3d9ShaderIsCompiled;
    d3d9RendererVtable.shadersSupported = d3d9ShadersSupported;
    dr->base.vtable = &d3d9RendererVtable;
    dr->base.drawColor = 0xFFFFFF;
    dr->base.drawAlpha = 1.0f;
    dr->base.drawFont = -1;
    dr->base.circlePrecision = 24;
    dr->base.currentShader = -1;
    dr->base.drawPhase = RENDER_PHASE_NONE;
    dr->pd3dDevice = pd3dDevice;
    dr->currentTextureIndex = -1;
    return (Renderer*)dr;
}
