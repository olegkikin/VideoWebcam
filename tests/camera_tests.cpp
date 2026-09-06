#include <streams.h>
#include <wrl/client.h>
#include <ks.h>
#include <ksmedia.h>
#include <atomic>
#include <iostream>
#include "frame_bus.h"
using Microsoft::WRL::ComPtr;
const CLSID CameraId = {0xd9c360e1,0x24e1,0x4cd7,{0xba,0xe5,0x38,0x5b,0xa2,0x48,0xeb,0x72}};
static void Require(bool valid, const char* message) { if (!valid) { std::cerr << "FAIL: " << message << '\n'; ExitProcess(1); } }
static void Check(HRESULT hr, const char* message) {
    if (FAILED(hr)) { std::cerr << "HRESULT 0x" << std::hex << hr << " "; Require(false,message); }
}
class InspectRenderer final : public CBaseRenderer {
public:
    InspectRenderer(HRESULT* hr, bool yuy, vcam::OutputFormat output) : CBaseRenderer(CLSID_NULL,NAME("Inspect"),nullptr,hr), yuy_(yuy), output_(output) {}
    HRESULT CheckMediaType(const CMediaType* mt) override {
        return mt->subtype == (yuy_ ? MEDIASUBTYPE_YUY2 : MEDIASUBTYPE_RGB24) ? S_OK : VFW_E_TYPE_NOT_ACCEPTED;
    }
    HRESULT DoRenderSample(IMediaSample* sample) override {
        BYTE* data = nullptr; sample->GetPointer(&data);
        const LONG size = output_.width * output_.height * (yuy_ ? 2 : 3);
        if (sample->GetActualDataLength() != size) ++errors;
        REFERENCE_TIME start, end;
        if (FAILED(sample->GetTime(&start,&end)) || end-start != output_.Interval() || (count && start <= last)) ++errors;
        last = start;
        if (yuy_) {
            // The top row is red in top-down YUY2, bottom row is blue.
            if (data[0] == 82 && data[1] == 90 && data[2] == 82 && data[3] == 240 && data[size-4] == 41) ++colored;
            if (data[0] == 16 && data[1] == 128 && data[2] == 16 && data[3] == 128) ++black;
        } else {
            // RGB DIB rows must be bottom-up: blue first, red last.
            if (data[0] == 255 && data[1] == 0 && data[2] == 0 && data[size-3] == 0 && data[size-1] == 255) ++colored;
            if (data[0] == 0 && data[1] == 0 && data[2] == 0 && data[size-1] == 0) ++black;
        }
        ++count; return S_OK;
    }
    std::atomic<int> count{0}, errors{0}, colored{0}, black{0};
    REFERENCE_TIME last = -1;
private: bool yuy_; vcam::OutputFormat output_;
};
int wmain(int argc, wchar_t** argv) {
    Require(argc == 2,"Pass the camera DLL path");
    Check(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"COM startup");
    vcam::FrameBus writer, reader;
    Require(writer.Open(true),"Create writer mapping");
    std::vector<unsigned char> pixels(vcam::FrameBytes), received(vcam::FrameBytes,77);
    for (int y=0;y<vcam::Height;++y)
        for (int x=0;x<vcam::Width;++x) pixels[(size_t(y)*vcam::Width+x)*3+(y<vcam::Height/2 ? 2 : 0)] = 255;
    Require(writer.Publish(pixels.data(),true),"Publish source");
    Require(reader.Read(received.data()) && pixels == received,"Shared frame round trip");
    writer.Publish(nullptr,false);
    Require(!reader.Read(received.data()) && received == std::vector<unsigned char>(vcam::FrameBytes),"Stopped camera is black");
    writer.Publish(pixels.data(),true);
    HMODULE dll = LoadLibraryW(argv[1]); Require(dll != nullptr,"Load camera DLL");
    auto getFactory = reinterpret_cast<HRESULT (STDAPICALLTYPE*)(REFCLSID,REFIID,void**)>(GetProcAddress(dll,"DllGetClassObject"));
    auto canUnload = reinterpret_cast<HRESULT (STDAPICALLTYPE*)()>(GetProcAddress(dll,"DllCanUnloadNow"));
    Require(getFactory && canUnload,"COM exports");
    {
        ComPtr<IClassFactory> factory;
        Check(getFactory(CameraId,IID_PPV_ARGS(&factory)),"Get class factory");
        for (const auto resolution : vcam::Resolutions) for (const int fps : vcam::FrameRates) for (int format=0;format<2;++format) {
            const vcam::OutputFormat selected{int(resolution.cx),int(resolution.cy),fps};
            std::vector<unsigned char> source(selected.Bytes());
            for (int y=0;y<selected.height;++y)
                for (int x=0;x<selected.width;++x) source[(size_t(y)*selected.width+x)*3+(y<selected.height/2 ? 2 : 0)] = 255;
            writer.Publish(source.data(),true,selected);
            ComPtr<IBaseFilter> camera;
            Check(factory->CreateInstance(nullptr,IID_PPV_ARGS(&camera)),"Create camera");
            ComPtr<IEnumPins> enumeration; Check(camera->EnumPins(&enumeration),"Enumerate capture pin");
            ComPtr<IPin> output; Require(enumeration->Next(1,&output,nullptr)==S_OK,"One capture pin");
            ComPtr<IAMStreamConfig> config; Check(output.As(&config),"Stream config");
            int count=0,size=0; Check(config->GetNumberOfCapabilities(&count,&size),"Capabilities");
            Require(count==2 && size==sizeof(VIDEO_STREAM_CONFIG_CAPS),"RGB24 and YUY2 capabilities");
            ComPtr<IKsPropertySet> properties; Check(output.As(&properties),"Pin property set");
            GUID category{}; DWORD returned=0;
            Check(properties->Get(AMPROPSETID_Pin,AMPROPERTY_PIN_CATEGORY,nullptr,0,&category,sizeof(category),&returned),"Capture category");
            Require(category==PIN_CATEGORY_CAPTURE,"Capture pin category");
            AM_MEDIA_TYPE* mt=nullptr; VIDEO_STREAM_CONFIG_CAPS caps{};
            Check(config->GetStreamCaps(format,&mt,reinterpret_cast<BYTE*>(&caps)),"Read format");
            auto* vi=reinterpret_cast<VIDEOINFOHEADER*>(mt->pbFormat);
            Require(vi->bmiHeader.biWidth == selected.width && vi->bmiHeader.biHeight == selected.height &&
                    vi->AvgTimePerFrame == selected.Interval(),"Camera advertises selected resolution and FPS");
            Require(caps.MinFrameInterval == selected.Interval() && caps.MinOutputSize.cx == selected.width,"Capabilities match media type");
            vi->bmiHeader.biWidth=123;
            Require(FAILED(config->SetFormat(mt)),"Reject unsupported dimensions");
            vi->bmiHeader.biWidth=selected.width;
            vi->AvgTimePerFrame = 1;
            Require(FAILED(config->SetFormat(mt)),"Reject unsupported FPS");
            vi->AvgTimePerFrame = selected.Interval();
            Check(config->SetFormat(mt),"Set format");
            ComPtr<IGraphBuilder> graph; Check(CoCreateInstance(CLSID_FilterGraph,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&graph)),"Create graph");
            Check(graph->AddFilter(camera.Get(),L"VideoWebcam"),"Add source");
            HRESULT hr=S_OK;
            auto* sink=new InspectRenderer(&hr,format==1,selected); Check(hr,"Create test renderer");
            ComPtr<IBaseFilter> sinkFilter;
            Check(sink->NonDelegatingQueryInterface(IID_PPV_ARGS(&sinkFilter)),"Renderer interface");
            Check(graph->AddFilter(sinkFilter.Get(),L"Inspect"),"Add renderer");
            Check(graph->ConnectDirect(output.Get(),sink->GetPin(0),mt),"Connect capture graph");
            DeleteMediaType(mt);
            ComPtr<IMediaControl> control; Check(graph.As(&control),"Media control");
            writer.Publish(source.data(),true,selected);
            Check(control->Run(),"Run capture graph");
            for(int i=0;i<100 && sink->colored<5;++i) Sleep(20);
            Require(sink->colored>=5,"Real capture samples have correct source pixels and orientation");
            writer.Publish(nullptr,false,selected);
            for(int i=0;i<100 && sink->black<3;++i) Sleep(20);
            Require(sink->black>=3,"Live graph switches to black when producer stops");
            writer.Publish(source.data(),true,selected);
            const int before=sink->colored;
            for(int i=0;i<100 && sink->colored<before+3;++i) Sleep(20);
            Require(sink->colored>=before+3,"Live graph reconnects to active producer");
            // A producer format change cannot alter an already allocated consumer buffer.
            writer.Publish(pixels.data(),true,{});
            const int oldCount = sink->count;
            for(int i=0;i<100 && sink->count<oldCount+3;++i) Sleep(20);
            Require(sink->count>=oldCount+3,"Existing capture graph survives a producer format change");
            Check(control->Stop(),"Stop graph without hanging");
            Require(sink->errors==0,"Sample sizes and timestamps are valid");
            sink->count=0; sink->last=-1;
            Check(control->Run(),"Restart graph");
            for(int i=0;i<100 && sink->count<3;++i) Sleep(20);
            Check(control->Stop(),"Stop restarted graph");
            Require(sink->count>=3 && sink->errors==0,"Restart resets timestamps");
            std::cout << selected.width << "x" << selected.height << " @ " << fps << " " << (format ? "YUY2" : "RGB24") << " graph passed\n";
        }
    }
    Require(canUnload()==S_OK,"No leaked camera COM instances");
    FreeLibrary(dll); CoUninitialize();
    std::cout << "Camera pipeline tests passed.\n";
}
