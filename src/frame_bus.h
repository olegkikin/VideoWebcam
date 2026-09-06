#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace vcam {
inline constexpr int Width = 1280, Height = 720, Fps = 30;
inline constexpr size_t FrameBytes = size_t(Width) * Height * 3;
struct OutputFormat {
    int width = Width, height = Height, fps = Fps;
    size_t Bytes() const { return size_t(width) * height * 3; }
    int64_t Interval() const { return 10000000 / fps; }
    bool Valid() const {
        return ((width == 640 && height == 480) || (width == 1280 && height == 720) ||
                (width == 1920 && height == 1080)) &&
               (fps == 15 || fps == 24 || fps == 25 || fps == 30 || fps == 60);
    }
};
inline constexpr int FrameRates[] = {15, 24, 25, 30, 60};
inline constexpr SIZE Resolutions[] = {{640,480}, {1280,720}, {1920,1080}};
inline constexpr size_t MaxFrameBytes = size_t(1920) * 1080 * 3;
inline constexpr DWORD Magic = 0x56435732;
// Fixed-width layout shared by 32-bit and 64-bit processes. Pixels are top-down BGR24.
struct SharedFrame {
    DWORD magic;
    DWORD version;
    uint64_t updated;
    DWORD active;
    int32_t width, height, fps;
    unsigned char pixels[MaxFrameBytes];
};
static_assert(offsetof(SharedFrame, pixels) == 32);
std::wstring ObjectName(const wchar_t* suffix);
class FrameBus {
public:
    FrameBus() = default;
    ~FrameBus();
    FrameBus(const FrameBus&) = delete;
    FrameBus& operator=(const FrameBus&) = delete;
    bool Open(bool writer);
    void Close();
    bool Publish(const unsigned char* pixels, bool active, OutputFormat format = {});
    bool Read(unsigned char* pixels, OutputFormat target = {});
    OutputFormat PreferredFormat();
private:
    HANDLE mapping_ = nullptr, mutex_ = nullptr;
    SharedFrame* frame_ = nullptr;
    bool writer_ = false;
};
}
