#include "decoder.h"
#include <filesystem>
#include <iostream>
#include <condition_variable>
static void Require(bool valid,const char* message) { if (!valid) { std::cerr << "FAIL: " << message << '\n'; ExitProcess(1); } }
int wmain(int argc,wchar_t** argv) {
    Require(argc>=3,"Pass ffmpeg.exe and media fixture paths");
    Require(vcam::QuoteArgument(L"a\\\"b\\") == L"\"a\\\\\\\"b\\\\\"","Windows argument quoting");
    for(int index=2;index<argc;++index) for (const auto resolution : vcam::Resolutions) {
        std::mutex mutex; std::condition_variable changed;
        int frames=0; bool failed=false; std::vector<unsigned char> latest;
        vcam::Decoder decoder;
        auto file=std::filesystem::absolute(argv[index]);
        vcam::PlaybackOptions options; options.loop=false;
        options.output = {int(resolution.cx),int(resolution.cy),resolution.cx == 640 ? 15 : resolution.cx == 1920 ? 60 : 30};
        decoder.Start(std::filesystem::absolute(argv[1]).wstring(),file.wstring(),
                      (file.parent_path()/L"decoder-test.log").wstring(),options,
            [&](const auto& pixels) { std::lock_guard<std::mutex> lock(mutex); latest=pixels; ++frames; changed.notify_all(); },
            [&](auto state,const auto& message) {
                if(state==vcam::PlaybackState::Error) { std::lock_guard<std::mutex> lock(mutex); failed=true; std::wcerr << message << '\n'; changed.notify_all(); }
            });
        {
            std::unique_lock<std::mutex> lock(mutex);
            changed.wait_for(lock,std::chrono::seconds(10),[&]{return frames>=5 || failed;});
            Require(!failed && frames>=5,"Media decodes and maintains frame cadence");
            Require(latest.size()==options.output.Bytes(),"Decoded output dimensions");
            const size_t center=(size_t(options.output.height/2)*options.output.width+options.output.width/2)*3;
            Require(latest[center+2]>220 && latest[center]<30 && latest[center+1]<30,"Decoded fixture is red in BGR order");
        }
        decoder.Pause(true); Sleep(100);
        std::vector<unsigned char> paused;
        int before=0;
        { std::lock_guard<std::mutex> lock(mutex); paused=latest; before=frames; }
        Sleep(150);
        { std::lock_guard<std::mutex> lock(mutex); Require(latest==paused && frames>before,"Pause freezes pixels while maintaining heartbeat"); }
        decoder.Pause(false);
        auto start=std::chrono::steady_clock::now(); decoder.Stop();
        Require(std::chrono::steady_clock::now()-start<std::chrono::seconds(3),"Decoder stops promptly");
        std::wcout << L"Decoded " << file.filename().wstring() << L" at " << options.output.width << L"x" << options.output.height << L" / " << options.output.fps << L" fps\n";
    }
    // A nonexistent input must surface an error instead of streaming uninitialized memory.
    std::atomic<bool> error{false}; vcam::Decoder invalid;
    invalid.Start(argv[1],L"Z:\\VideoWebcam-nonexistent-file.mp4",L"decoder-invalid.log",{},
                  [](const auto&){ Require(false,"Invalid input must not publish frames"); },
                  [&](auto state,const auto&){ if(state==vcam::PlaybackState::Error) error=true; });
    for(int i=0;i<100 && !error;++i) Sleep(50);
    invalid.Stop(); Require(error,"Invalid input error reporting");
    std::cout << "Decoder tests passed.\n";
}
