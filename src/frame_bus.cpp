#include "frame_bus.h"
#include <sddl.h>
#include <cstring>

namespace vcam {
std::wstring ObjectName(const wchar_t* suffix) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> storage(size);
    if (!GetTokenInformation(token, TokenUser, storage.data(), size, &size)) {
        CloseHandle(token); return {};
    }
    LPWSTR sid = nullptr;
    const BOOL ok = ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid, &sid);
    CloseHandle(token);
    if (!ok) return {};
    std::wstring name = L"Local\\VideoWebcam.v2.";
    name += sid;
    name += L".";
    name += suffix;
    LocalFree(sid);
    return name;
}
FrameBus::~FrameBus() { Close(); }
void FrameBus::Close() {
    if (frame_) UnmapViewOfFile(frame_);
    if (mapping_) CloseHandle(mapping_);
    if (mutex_) CloseHandle(mutex_);
    frame_ = nullptr; mapping_ = mutex_ = nullptr;
}
bool FrameBus::Open(bool writer) {
    Close(); writer_ = writer;
    auto mapName = ObjectName(L"frames"), lockName = ObjectName(L"lock");
    if (mapName.empty() || lockName.empty()) return false;
    mutex_ = writer ? CreateMutexW(nullptr, FALSE, lockName.c_str())
                    : OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, lockName.c_str());
    if (!mutex_) return false;
    mapping_ = writer ? CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                          DWORD(sizeof(SharedFrame)), mapName.c_str())
                      : OpenFileMappingW(FILE_MAP_READ, FALSE, mapName.c_str());
    if (!mapping_) { Close(); return false; }
    frame_ = static_cast<SharedFrame*>(MapViewOfFile(mapping_, writer ? FILE_MAP_WRITE : FILE_MAP_READ,
                                                    0, 0, sizeof(SharedFrame)));
    if (!frame_) { Close(); return false; }
    if (writer) Publish(nullptr, false);
    return true;
}
bool FrameBus::Publish(const unsigned char* pixels, bool active, OutputFormat format) {
    if (!writer_ || !frame_ || !format.Valid()) return false;
    DWORD wait = WaitForSingleObject(mutex_, 20);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) return false;
    frame_->magic = Magic; frame_->version = 2;
    frame_->width = format.width; frame_->height = format.height; frame_->fps = format.fps;
    frame_->updated = GetTickCount64(); frame_->active = active && pixels;
    if (frame_->active) memcpy(frame_->pixels, pixels, format.Bytes());
    ReleaseMutex(mutex_);
    return true;
}
OutputFormat FrameBus::PreferredFormat() {
    if (!frame_ && !Open(false)) return {};
    DWORD wait = WaitForSingleObject(mutex_, 20);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) return {};
    OutputFormat format{frame_->width, frame_->height, frame_->fps};
    bool valid = wait != WAIT_ABANDONED && frame_->magic == Magic && frame_->version == 2 && format.Valid();
    ReleaseMutex(mutex_);
    return valid ? format : OutputFormat{};
}
bool FrameBus::Read(unsigned char* pixels, OutputFormat target) {
    if (!pixels || !target.Valid()) return false;
    if (!frame_ && !Open(false)) { memset(pixels, 0, target.Bytes()); return false; }
    DWORD wait = WaitForSingleObject(mutex_, 15);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) { memset(pixels, 0, target.Bytes()); return false; }
    OutputFormat source{frame_->width, frame_->height, frame_->fps};
    bool valid = wait != WAIT_ABANDONED && frame_->magic == Magic && frame_->version == 2 &&
                 source.Valid() && frame_->active && GetTickCount64() - frame_->updated < 2000;
    if (valid && source.width == target.width && source.height == target.height)
        memcpy(pixels, frame_->pixels, target.Bytes());
    else {
        memset(pixels, 0, target.Bytes());
        if (valid) {
            // Existing capture graphs retain their negotiated size until reopened.
            // Fit the new source into that size without overrunning their buffers.
            int width = target.width, height = source.height * width / source.width;
            if (height > target.height) { height = target.height; width = source.width * height / source.height; }
            int left = (target.width-width)/2, top = (target.height-height)/2;
            for (int y=0; y<height; ++y) {
                const auto* row = frame_->pixels + size_t(y*source.height/height)*source.width*3;
                auto* out = pixels + (size_t(y+top)*target.width+left)*3;
                for (int x=0; x<width; ++x) memcpy(out+x*3, row+(x*source.width/width)*3, 3);
            }
        }
    }
    ReleaseMutex(mutex_);
    // Reopen stale mappings so a restarted producer can be discovered.
    if (!valid) Close();
    return valid;
}
}
