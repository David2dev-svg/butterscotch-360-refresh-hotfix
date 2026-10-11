#include <xtl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Core headers — compiled as C++ alongside the .c files (via /TP flag)
#include "utils.h"
#include "xdk_file_system.h"

//--------
extern "C" void Butterscotch_xdkDiagTrace(const char* fmt, ...);
//---------
// ===[ Vtable Implementations ]===

// Root (first game, the launcher) base path, used as a read-only fallback for shared "mus/" files.
static char gXdkRootBase[512] = "";

static bool xdkPathExists(const char* path) {
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

static bool xdkIsSharedAsset(const char* rel) {
    return (_strnicmp(rel, "mus/", 4) == 0) || (_strnicmp(rel, "mus\\", 4) == 0);
}

// collapses "." and ".." segments of a backslash path, in place (the XDK does not resolve ".." itself)
static void xdkCollapseDots(char* p) {
    char tmp[512];
    char* parts[64];
    char* keepParts[64];
    int n = 0;
    int keep = 0;
    size_t len0 = strlen(p);
    bool trailing = len0 > 0 && p[len0 - 1] == '\\';
    if (len0 >= sizeof(tmp)) return;
    strcpy(tmp, p);
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
    if (trailing && keep > 0) strcat(p, "\\");
}

static char* xdkResolvePath(FileSystem* fs, const char* relativePath) {
    XdkFileSystem* xfs = (XdkFileSystem*)fs;
    if (!gXdkRootBase[0]) {
        strncpy(gXdkRootBase, xfs->basePath, sizeof(gXdkRootBase) - 1);
        gXdkRootBase[sizeof(gXdkRootBase) - 1] = 0;
    }
    // Se o caminho ja traz uma unidade (ex.: "game:\\" ou "game:/"), usa como esta.
    const char* colon = strchr(relativePath, ':');
    const char* firstSep = strpbrk(relativePath, "/\\");
    bool hasDrive = (colon != NULL) && (firstSep == NULL || colon < firstSep);

    size_t baseLen = hasDrive ? 0 : strlen(xfs->basePath);
    size_t relLen = strlen(relativePath);
    char* result = (char*)malloc(baseLen + relLen + 1);
    if (!result) return NULL;
    if (baseLen) memcpy(result, xfs->basePath, baseLen);
    memcpy(result + baseLen, relativePath, relLen + 1);

    // Convert forward slashes to backslashes for Xbox 360
    for (char* p = result + baseLen; *p; p++) {
        if (*p == '/') *p = '\\';
    }
    xdkCollapseDots(result);
    if (!hasDrive && gXdkRootBase[0] && xdkIsSharedAsset(relativePath)
        && strcmp(gXdkRootBase, xfs->basePath) != 0 && !xdkPathExists(result)) {
        size_t rl = strlen(gXdkRootBase);
        char* alt = (char*)malloc(rl + relLen + 1);
        if (alt) {
            memcpy(alt, gXdkRootBase, rl);
            memcpy(alt + rl, relativePath, relLen + 1);
            for (char* q = alt + rl; *q; q++) {
                if (*q == '/') *q = '\\';
            }
            if (xdkPathExists(alt)) {
                free(result);
                return alt;
            }
            free(alt);
        }
    }
    return result;
}

static bool xdkFileExists(FileSystem* fs, const char* relativePath) {
    char* fullPath = xdkResolvePath(fs, relativePath);
    if (!fullPath) return false;

    Butterscotch_xdkDiagTrace("FS: open '%s'", fullPath);
    HANDLE hFile = CreateFileA(fullPath, GENERIC_READ, FILE_SHARE_READ,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    free(fullPath);

    // em xdkFileExists, logo antes dos dois returns:
    Butterscotch_xdkDiagTrace("FS: exists -> %d", hFile != INVALID_HANDLE_VALUE);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    CloseHandle(hFile);
    return true;
}

static char* xdkReadFileText(FileSystem* fs, const char* relativePath) {
    char* fullPath = xdkResolvePath(fs, relativePath);
    if (!fullPath) return NULL;

    Butterscotch_xdkDiagTrace("FS: open '%s'", fullPath);
    HANDLE hFile = CreateFileA(fullPath, GENERIC_READ, FILE_SHARE_READ,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    free(fullPath);

    if (hFile == INVALID_HANDLE_VALUE) return NULL;

    DWORD fileSize = GetFileSize(hFile, NULL);
    if (fileSize == INVALID_FILE_SIZE) {
        CloseHandle(hFile);
        return NULL;
    }

    char* buffer = (char*)malloc(fileSize + 1);
    if (!buffer) {
        CloseHandle(hFile);
        return NULL;
    }

    DWORD bytesRead;

    Butterscotch_xdkDiagTrace("FS: size=%lu", (unsigned long)fileSize);
    ReadFile(hFile, buffer, fileSize, &bytesRead, NULL);
    Butterscotch_xdkDiagTrace("FS: read ok bytes=%lu", (unsigned long)bytesRead);

    CloseHandle(hFile);

    buffer[bytesRead] = '\0';
    return buffer;
}

static bool xdkWriteFileText(FileSystem* fs, const char* relativePath, const char* contents) {
    char* fullPath = xdkResolvePath(fs, relativePath);
    if (!fullPath) return false;

    Butterscotch_xdkDiagTrace("FS: open '%s'", fullPath);
    HANDLE hFile = CreateFileA(fullPath, GENERIC_WRITE, 0,
                               NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    free(fullPath);

    if (hFile == INVALID_HANDLE_VALUE) return false;

    DWORD len = (DWORD)strlen(contents);
    DWORD written;
    BOOL ok = WriteFile(hFile, contents, len, &written, NULL);
    CloseHandle(hFile);
    return ok && written == len;
}

static bool xdkDeleteFile(FileSystem* fs, const char* relativePath) {
    char* fullPath = xdkResolvePath(fs, relativePath);
    if (!fullPath) return false;

    BOOL ok = DeleteFileA(fullPath);
    free(fullPath);
    return ok != FALSE;
}


static bool xdkReadFileBinary(FileSystem* fs, const char* relativePath, uint8_t** outData, int32_t* outSize) {
    char* fullPath = xdkResolvePath(fs, relativePath);
    if (!fullPath) return false;
    Butterscotch_xdkDiagTrace("FS: open bin '%s'", fullPath);
    HANDLE h = CreateFileA(fullPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    free(fullPath);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE) { CloseHandle(h); return false; }
    uint8_t* data = (uint8_t*)malloc(size ? size : 1);
    DWORD got = 0;
    BOOL ok = data ? ReadFile(h, data, size, &got, NULL) : FALSE;
    CloseHandle(h);
    if (!ok || got != size) { free(data); return false; }
    *outData = data;
    *outSize = (int32_t)size;
    return true;
}

static bool xdkWriteFileBinary(FileSystem* fs, const char* relativePath, const uint8_t* data, int32_t size) {
    char* fullPath = xdkResolvePath(fs, relativePath);
    if (!fullPath) return false;
    HANDLE h = CreateFileA(fullPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    free(fullPath);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(h, data, (DWORD)size, &written, NULL);
    CloseHandle(h);
    return ok && written == (DWORD)size;
}

static void* xdkBinaryOpen(FileSystem* fs, const char* relativePath, int32_t mode) {
    char* fullPath = xdkResolvePath(fs, relativePath);
    if (!fullPath) return NULL;
    DWORD access = GENERIC_READ, disp = OPEN_EXISTING;
    if (mode == GML_FILE_BIN_WRITE) { access = GENERIC_WRITE; disp = CREATE_ALWAYS; }
    else if (mode == GML_FILE_BIN_READWRITE) { access = GENERIC_READ | GENERIC_WRITE; disp = OPEN_ALWAYS; }
    HANDLE h = CreateFileA(fullPath, access, FILE_SHARE_READ, NULL, disp, FILE_ATTRIBUTE_NORMAL, NULL);
    free(fullPath);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    return (void*)h;
}

static void xdkBinaryClose(FileSystem* fs, void* handle) {
    (void)fs;
    if (handle) CloseHandle((HANDLE)handle);
}

static int32_t xdkBinaryRead(FileSystem* fs, void* handle, void* dst, int32_t n) {
    (void)fs;
    DWORD got = 0;
    if (!handle || !ReadFile((HANDLE)handle, dst, (DWORD)n, &got, NULL)) return 0;
    return (int32_t)got;
}

static int32_t xdkBinaryWrite(FileSystem* fs, void* handle, const void* src, int32_t n) {
    (void)fs;
    DWORD put = 0;
    if (!handle || !WriteFile((HANDLE)handle, src, (DWORD)n, &put, NULL)) return 0;
    return (int32_t)put;
}

static int32_t xdkBinaryTell(FileSystem* fs, void* handle) {
    (void)fs;
    if (!handle) return 0;
    return (int32_t)SetFilePointer((HANDLE)handle, 0, NULL, FILE_CURRENT);
}

static bool xdkBinarySeek(FileSystem* fs, void* handle, int32_t pos) {
    (void)fs;
    if (!handle) return false;
    return SetFilePointer((HANDLE)handle, pos, NULL, FILE_BEGIN) != INVALID_SET_FILE_POINTER;
}

static int32_t xdkBinarySize(FileSystem* fs, void* handle) {
    (void)fs;
    if (!handle) return 0;
    DWORD sz = GetFileSize((HANDLE)handle, NULL);
    return sz == INVALID_FILE_SIZE ? 0 : (int32_t)sz;
}

static void xdkBinaryRewrite(FileSystem* fs, void* handle) {
    (void)fs;
    if (!handle) return;
    SetFilePointer((HANDLE)handle, 0, NULL, FILE_BEGIN);
    SetEndOfFile((HANDLE)handle);
}

static bool xdkDirectoryExists(FileSystem* fs, const char* relativePath) {
    char* fullPath = xdkResolvePath(fs, relativePath);
    if (!fullPath) return false;
    DWORD attr = GetFileAttributesA(fullPath);
    free(fullPath);
    return attr != 0xFFFFFFFF && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

static bool xdkCreateDirectory(FileSystem* fs, const char* relativePath) {
    char* fullPath = xdkResolvePath(fs, relativePath);
    if (!fullPath) return false;
    BOOL ok = CreateDirectoryA(fullPath, NULL);
    free(fullPath);
    return ok != FALSE;
}

static bool xdkDeleteDirectory(FileSystem* fs, const char* relativePath) {
    char* fullPath = xdkResolvePath(fs, relativePath);
    if (!fullPath) return false;
    BOOL ok = RemoveDirectoryA(fullPath);
    free(fullPath);
    return ok != FALSE;
}

static FileSystemDirEntry* xdkListDirectory(FileSystem* fs, const char* relativeDirPath) {
    (void)fs; (void)relativeDirPath;
    return NULL;
}

// ===[ Vtable ]===

static FileSystemVtable xdkFileSystemVtable = {
    xdkResolvePath,
    xdkFileExists,
    xdkReadFileText,
    xdkWriteFileText,
    xdkDeleteFile,
    xdkReadFileBinary,
    xdkWriteFileBinary,
    xdkBinaryOpen,
    xdkBinaryClose,
    xdkBinaryRead,
    xdkBinaryWrite,
    xdkBinaryTell,
    xdkBinarySeek,
    xdkBinarySize,
    xdkBinaryRewrite,
    xdkDirectoryExists,
    xdkCreateDirectory,
    xdkDeleteDirectory,
    xdkListDirectory,
};

// ===[ Public API ]===

XdkFileSystem* XdkFileSystem_create(const char* dataWinPath) {
    XdkFileSystem* xfs = (XdkFileSystem*)calloc(1, sizeof(XdkFileSystem));
    xfs->base.vtable = &xdkFileSystemVtable;

    // Extract directory from data.win path
    const char* lastSlash = strrchr(dataWinPath, '\\');
    if (!lastSlash) lastSlash = strrchr(dataWinPath, '/');

    if (lastSlash) {
        size_t dirLen = (size_t)(lastSlash - dataWinPath + 1);
        if (dirLen >= sizeof(xfs->basePath)) dirLen = sizeof(xfs->basePath) - 1;
        memcpy(xfs->basePath, dataWinPath, dirLen);
        xfs->basePath[dirLen] = '\0';
    } else {
        strcpy(xfs->basePath, "game:\\");
    }

    return xfs;
}
