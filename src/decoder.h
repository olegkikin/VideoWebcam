#pragma once
#include "frame_bus.h"
#include <atomic>
#include <functional>
#include <mutex>
#include <thread>

namespace vcam {
struct PlaybackOptions { bool loop = true; bool mirror = false; bool crop = false; OutputFormat output; };
enum class PlaybackState { Loading, Playing, Paused, Ended, Error };
std::wstring QuoteArgument(const std::wstring& argument);
class Decoder {
public:
    using FrameCallback = std::function<void(const std::vector<unsigned char>&)>;
    using StateCallback = std::function<void(PlaybackState, const std::wstring&)>;
    Decoder();
    ~Decoder();
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;
    void Start(const std::wstring& ffmpeg, const std::wstring& file, const std::wstring& log,
               PlaybackOptions options, FrameCallback frame, StateCallback state);
    void Stop();
    void Pause(bool value) { paused_ = value; }
private:
    void Run(std::wstring ffmpeg, std::wstring file, std::wstring log, PlaybackOptions options,
             FrameCallback frame, StateCallback state);
    HANDLE stop_ = nullptr;
    HANDLE job_ = nullptr;
    std::mutex processMutex_;
    std::thread worker_;
    std::atomic<bool> paused_{false};
};
}
