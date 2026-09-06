#include "decoder.h"
#include <algorithm>
#include <chrono>
#include <cwctype>
#include <filesystem>

namespace vcam {
namespace {
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    void Reset() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); value = nullptr; }
};
std::wstring WinError(const wchar_t* context) {
    return std::wstring(context) + L" (Windows error " + std::to_wstring(GetLastError()) + L").";
}
}
std::wstring QuoteArgument(const std::wstring& argument) {
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : argument) {
        if (c == L'\\') { ++slashes; continue; }
        if (c == L'\"') out.append(slashes * 2 + 1, L'\\');
        else out.append(slashes, L'\\');
        slashes = 0; out += c;
    }
    out.append(slashes * 2, L'\\');
    return out + L"\"";
}
Decoder::Decoder() { stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr); }
Decoder::~Decoder() { Stop(); if (stop_) CloseHandle(stop_); }
void Decoder::Stop() {
    if (stop_) SetEvent(stop_);
    {
        std::lock_guard<std::mutex> lock(processMutex_);
        if (job_) TerminateJobObject(job_, 0);
    }
    if (worker_.joinable()) worker_.join();
}
void Decoder::Start(const std::wstring& ffmpeg, const std::wstring& file, const std::wstring& log,
                    PlaybackOptions options, FrameCallback frame, StateCallback state) {
    Stop();
    if (!stop_) { state(PlaybackState::Error, L"Could not create playback event."); return; }
    paused_ = false; ResetEvent(stop_);
    worker_ = std::thread([=] {
        try { Run(ffmpeg, file, log, options, frame, state); }
        catch (...) { state(PlaybackState::Error, L"Playback failed unexpectedly."); }
    });
}
void Decoder::Run(std::wstring ffmpeg, std::wstring file, std::wstring log, PlaybackOptions options,
                  FrameCallback frame, StateCallback state) {
    state(PlaybackState::Loading, L"Opening media...");
    if (!options.output.Valid()) { state(PlaybackState::Error, L"Unsupported output resolution or frame rate."); return; }
    const auto frameBytes = options.output.Bytes();
    auto extension = std::filesystem::path(file).extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t c) { return wchar_t(towlower(c)); });
    const bool still = extension == L".jpg" || extension == L".jpeg" || extension == L".png" ||
                       extension == L".webp" || extension == L".bmp";
    const auto dimensions = std::to_wstring(options.output.width) + L":" + std::to_wstring(options.output.height);
    std::wstring filters = options.crop ?
        L"scale=" + dimensions + L":force_original_aspect_ratio=increase,crop=" + dimensions :
        L"scale=" + dimensions + L":force_original_aspect_ratio=decrease,pad=" + dimensions + L":(ow-iw)/2:(oh-ih)/2:color=black";
    filters += L",setsar=1,fps=" + std::to_wstring(options.output.fps);
    if (options.mirror) filters += L",hflip";
    std::wstring command = QuoteArgument(ffmpeg) + L" -hide_banner -loglevel error -nostdin ";
    if (!still && options.loop) command += L"-stream_loop -1 ";
    command += L"-i " + QuoteArgument(file) + L" -map 0:v:0 -an -sn -dn -vf " + QuoteArgument(filters);
    if (still) command += L" -frames:v 1";
    command += L" -threads 2 -pix_fmt bgr24 -f rawvideo pipe:1";

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    Handle read, write, errors, input, process, processThread, job;
    if (!CreatePipe(&read.value, &write.value, &sa, 0) ||
        !SetHandleInformation(read.value, HANDLE_FLAG_INHERIT, 0)) {
        state(PlaybackState::Error, WinError(L"Could not create decoder pipe")); return;
    }
    errors.value = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (errors.value == INVALID_HANDLE_VALUE) {
        state(PlaybackState::Error, WinError(L"Could not open decoder log")); return;
    }
    input.value = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    if (input.value == INVALID_HANDLE_VALUE) {
        state(PlaybackState::Error, WinError(L"Could not open decoder input")); return;
    }
    job.value = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job.value || !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        state(PlaybackState::Error, WinError(L"Could not isolate decoder process")); return;
    }
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<unsigned char> attrs(attrSize);
    auto attrList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrs.data());
    if (!InitializeProcThreadAttributeList(attrList, 1, 0, &attrSize)) {
        state(PlaybackState::Error, WinError(L"Could not initialize decoder")); return;
    }
    HANDLE inherit[] = {write.value, errors.value, input.value};
    bool updated = UpdateProcThreadAttribute(attrList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                             inherit, sizeof(inherit), nullptr, nullptr) != FALSE;
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si); si.lpAttributeList = attrList;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdOutput = write.value; si.StartupInfo.hStdError = errors.value; si.StartupInfo.hStdInput = input.value;
    PROCESS_INFORMATION pi{};
    BOOL created = updated && CreateProcessW(ffmpeg.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &si.StartupInfo, &pi);
    DWORD createError = GetLastError();
    DeleteProcThreadAttributeList(attrList);
    if (!created) {
        SetLastError(createError);
        state(PlaybackState::Error, WinError(L"Could not start ffmpeg.exe; place it next to VideoWebcam.exe")); return;
    }
    process.value = pi.hProcess; processThread.value = pi.hThread;
    if (!AssignProcessToJobObject(job.value, process.value)) {
        TerminateProcess(process.value, 1);
        state(PlaybackState::Error, WinError(L"Could not attach decoder to job")); return;
    }
    {
        std::lock_guard<std::mutex> lock(processMutex_);
        if (WaitForSingleObject(stop_, 0) == WAIT_OBJECT_0) return;
        job_ = job.value;
        ResumeThread(processThread.value);
    }
    // Reset job_ before the local owning handle is destroyed, including exceptional exits.
    struct ResetJob {
        Decoder* self;
        HANDLE process;
        ~ResetJob() {
            std::lock_guard<std::mutex> lock(self->processMutex_);
            TerminateJobObject(self->job_, 0);
            // Termination is asynchronous; wait for inherited pipe/log handles to close.
            WaitForSingleObject(process, 5000);
            self->job_ = nullptr;
        }
    } reset{this, process.value};
    write.Reset(); errors.Reset(); input.Reset(); processThread.Reset();
    std::vector<unsigned char> pixels(frameBytes);
    bool haveFrame = false, ended = false, wasPaused = false;
    auto next = std::chrono::steady_clock::now();
    while (WaitForSingleObject(stop_, 0) != WAIT_OBJECT_0) {
        const bool paused = paused_.load();
        if (haveFrame && paused != wasPaused && !ended) {
            state(paused ? PlaybackState::Paused : PlaybackState::Playing, paused ? L"Paused" : L"Playing");
            wasPaused = paused;
        }
        if (!haveFrame || (!paused && !ended && !still)) {
            size_t filled = 0;
            while (filled < frameBytes) {
                DWORD count = 0;
                if (!ReadFile(read.value, pixels.data() + filled, DWORD(frameBytes - filled), &count, nullptr) || !count) break;
                filled += count;
            }
            if (WaitForSingleObject(stop_, 0) == WAIT_OBJECT_0) break;
            if (filled != frameBytes) {
                WaitForSingleObject(process.value, 2000);
                DWORD code = 1; GetExitCodeProcess(process.value, &code);
                if (!haveFrame || filled != 0 || code != 0) {
                    state(PlaybackState::Error, L"Cannot decode this file. See decoder.log for details."); return;
                }
                ended = true;
                state(PlaybackState::Ended, L"Finished - holding the last frame");
            } else if (!haveFrame) {
                haveFrame = true;
                next = std::chrono::steady_clock::now();
                state(PlaybackState::Playing, still ? L"Still image" : L"Playing");
            }
        }
        auto now = std::chrono::steady_clock::now();
        if (now - next > std::chrono::milliseconds(250)) next = now;
        auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(next - now).count();
        if (WaitForSingleObject(stop_, delay > 0 ? DWORD(delay) : 0) == WAIT_OBJECT_0) break;
        frame(pixels);
        next += std::chrono::nanoseconds(1000000000 / options.output.fps);
    }
}
}
