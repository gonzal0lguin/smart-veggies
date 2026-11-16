#ifdef ARDUINO
#include "fs_storage.h"

#include <LittleFS.h>

static const char* DIR = "/log";

static void pathOf(uint32_t segment, char* out, size_t n) { snprintf(out, n, "%s/%08lx.bin", DIR, (unsigned long)segment); }

bool FsStorage::begin() {
    if (!LittleFS.begin(true)) return false;
    if (!LittleFS.exists(DIR)) LittleFS.mkdir(DIR);
    return LittleFS.exists(DIR);
}

size_t FsStorage::usedBytes() { return LittleFS.usedBytes(); }
size_t FsStorage::totalBytes() { return LittleFS.totalBytes(); }

size_t FsStorage::list(uint32_t* segments, size_t max) {
    size_t n = 0;
    File dir = LittleFS.open(DIR);
    if (!dir) return 0;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        const char* name = f.name();
        if (const char* slash = strrchr(name, '/')) name = slash + 1;
        char* end = nullptr;
        unsigned long seg = strtoul(name, &end, 16);
        if (end == name + 8 && !strcmp(end, ".bin") && n < max) segments[n++] = (uint32_t)seg;
    }
    return n;
}

size_t FsStorage::size(uint32_t segment) {
    char path[24];
    pathOf(segment, path, sizeof(path));
    if (!LittleFS.exists(path)) return 0;
    File f = LittleFS.open(path, FILE_READ);
    return f ? f.size() : 0;
}

bool FsStorage::append(uint32_t segment, const uint8_t* data, size_t n) {
    char path[24];
    pathOf(segment, path, sizeof(path));
    File f = LittleFS.open(path, FILE_APPEND);
    if (!f) return false;
    size_t written = f.write(data, n);
    f.close();
    return written == n;
}

bool FsStorage::read(uint32_t segment, size_t offset, uint8_t* data, size_t n) {
    char path[24];
    pathOf(segment, path, sizeof(path));
    if (!LittleFS.exists(path)) return false;
    File f = LittleFS.open(path, FILE_READ);
    return f && f.seek(offset) && f.read(data, n) == n;
}

void FsStorage::remove(uint32_t segment) {
    char path[24];
    pathOf(segment, path, sizeof(path));
    if (LittleFS.exists(path)) LittleFS.remove(path);
}
#endif
