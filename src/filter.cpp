#include <streams.h>
#include <ks.h>
#include <ksmedia.h>
#include <algorithm>
#include <chrono>
#include "frame_bus.h"

// {D9C360E1-24E1-4CD7-BAE5-385BA248EB72}
const CLSID CLSID_VideoWebcam = {0xd9c360e1,0x24e1,0x4cd7,{0xba,0xe5,0x38,0x5b,0xa2,0x48,0xeb,0x72}};
constexpr wchar_t CameraClsid[] = L"{D9C360E1-24E1-4CD7-BAE5-385BA248EB72}";

class CameraStream final : public CSourceStream, public IAMStreamConfig, public IKsPropertySet {
public:
    CameraStream(HRESULT* hr, CSource* source) :
        CSourceStream(NAME("VideoWebcam Output"), hr, source, L"Capture") {
        output_ = bus_.PreferredFormat();
        pixels_.resize(output_.Bytes());
        stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!stop_) *hr = HRESULT_FROM_WIN32(GetLastError());
        GetMediaType(0, &m_mt);
    }
    ~CameraStream() override { if (stop_) CloseHandle(stop_); }
    DECLARE_IUNKNOWN;
    STDMETHODIMP NonDelegatingQueryInterface(REFIID iid, void** out) override {
        if (iid == IID_IAMStreamConfig) return GetInterface(static_cast<IAMStreamConfig*>(this), out);
        if (iid == IID_IKsPropertySet) return GetInterface(static_cast<IKsPropertySet*>(this), out);
        return CSourceStream::NonDelegatingQueryInterface(iid, out);
    }
    void Cancel() { SetEvent(stop_); }
    HRESULT OnThreadCreate() override { ResetEvent(stop_); return S_OK; }
    HRESULT OnThreadStartPlay() override {
        ResetEvent(stop_); frames_ = 0; start_ = std::chrono::steady_clock::now(); return S_OK;
    }
    HRESULT GetMediaType(int pos, CMediaType* mt) override {
        if (formatSet_) {
            if (!mt) return E_POINTER;
            if (pos < 0) return E_INVALIDARG;
            if (pos > 0) return VFW_S_NO_MORE_ITEMS;
            *mt = m_mt; return S_OK;
        }
        return MakeMediaType(pos, mt);
    }
    HRESULT MakeMediaType(int pos, CMediaType* mt) {
        if (!mt) return E_POINTER;
        if (pos < 0) return E_INVALIDARG;
        if (pos > 1) return VFW_S_NO_MORE_ITEMS;
        mt->InitMediaType();
        auto* vi = reinterpret_cast<VIDEOINFOHEADER*>(mt->AllocFormatBuffer(sizeof(VIDEOINFOHEADER)));
        if (!vi) return E_OUTOFMEMORY;
        ZeroMemory(vi, sizeof(*vi));
        vi->AvgTimePerFrame = output_.Interval();
        vi->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        vi->bmiHeader.biWidth = output_.width;
        vi->bmiHeader.biHeight = output_.height;
        vi->bmiHeader.biPlanes = 1;
        vi->bmiHeader.biBitCount = pos == 0 ? 24 : 16;
        vi->bmiHeader.biCompression = pos == 0 ? BI_RGB : MAKEFOURCC('Y','U','Y','2');
        vi->bmiHeader.biSizeImage = output_.width * output_.height * (pos == 0 ? 3 : 2);
        vi->dwBitRate = vi->bmiHeader.biSizeImage * 8 * output_.fps;
        mt->SetType(&MEDIATYPE_Video);
        mt->SetSubtype(pos == 0 ? &MEDIASUBTYPE_RGB24 : &MEDIASUBTYPE_YUY2);
        mt->SetFormatType(&FORMAT_VideoInfo);
        mt->SetTemporalCompression(FALSE);
        mt->SetSampleSize(vi->bmiHeader.biSizeImage);
        return S_OK;
    }
    HRESULT CheckMediaType(const CMediaType* mt) override {
        if (!mt || mt->majortype != MEDIATYPE_Video || mt->formattype != FORMAT_VideoInfo ||
            mt->cbFormat < sizeof(VIDEOINFOHEADER) || !mt->pbFormat) return VFW_E_TYPE_NOT_ACCEPTED;
        const auto* vi = reinterpret_cast<const VIDEOINFOHEADER*>(mt->pbFormat);
        const bool rgb = mt->subtype == MEDIASUBTYPE_RGB24;
        if (formatSet_ && mt->subtype != m_mt.subtype) return VFW_E_TYPE_NOT_ACCEPTED;
        if (!rgb && mt->subtype != MEDIASUBTYPE_YUY2) return VFW_E_TYPE_NOT_ACCEPTED;
        const auto& bmi = vi->bmiHeader;
        if (bmi.biWidth != output_.width || bmi.biHeight != output_.height || bmi.biPlanes != 1 ||
            bmi.biBitCount != (rgb ? 24 : 16) ||
            bmi.biCompression != (rgb ? BI_RGB : MAKEFOURCC('Y','U','Y','2')) ||
            vi->AvgTimePerFrame != output_.Interval()) return VFW_E_TYPE_NOT_ACCEPTED;
        return S_OK;
    }
    HRESULT DecideBufferSize(IMemAllocator* allocator, ALLOCATOR_PROPERTIES* props) override {
        if (!allocator || !props) return E_POINTER;
        CAutoLock lock(m_pFilter->pStateLock());
        props->cBuffers = 2;
        props->cbBuffer = output_.width * output_.height * (m_mt.subtype == MEDIASUBTYPE_RGB24 ? 3 : 2);
        ALLOCATOR_PROPERTIES actual{};
        HRESULT hr = allocator->SetProperties(props, &actual);
        if (FAILED(hr)) return hr;
        return actual.cbBuffer >= props->cbBuffer ? S_OK : E_FAIL;
    }
    HRESULT FillBuffer(IMediaSample* sample) override {
        // Pace on a monotonic clock. Bound catch-up after a consumer stalls.
        auto now = std::chrono::steady_clock::now();
        auto due = start_ + std::chrono::nanoseconds(frames_ * output_.Interval() * 100);
        if (now - due > std::chrono::milliseconds(250)) {
            start_ += now - due; due = now;
        }
        auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(due - now).count();
        if (WaitForSingleObject(stop_, delay > 0 ? DWORD(delay) : 0) == WAIT_OBJECT_0) return S_FALSE;
        BYTE* out = nullptr;
        HRESULT hr = sample->GetPointer(&out);
        if (FAILED(hr)) return hr;
        const bool rgb = m_mt.subtype == MEDIASUBTYPE_RGB24;
        const LONG size = output_.width * output_.height * (rgb ? 3 : 2);
        if (sample->GetSize() < size) return E_FAIL;
        bus_.Read(pixels_.data(), output_);
        if (rgb) {
            for (int y = 0; y < output_.height; ++y)
                memcpy(out + size_t(output_.height - 1 - y) * output_.width * 3,
                       pixels_.data() + size_t(y) * output_.width * 3, output_.width * 3);
        } else {
            auto clamp = [](int n) { return BYTE(std::clamp(n, 0, 255)); };
            for (size_t i = 0, j = 0; i < pixels_.size(); i += 6, j += 4) {
                int b = pixels_[i], g = pixels_[i+1], r = pixels_[i+2];
                int b2 = pixels_[i+3], g2 = pixels_[i+4], r2 = pixels_[i+5];
                out[j] = clamp(((66*r + 129*g + 25*b + 128) >> 8) + 16);
                out[j+2] = clamp(((66*r2 + 129*g2 + 25*b2 + 128) >> 8) + 16);
                r = (r+r2)/2; g = (g+g2)/2; b = (b+b2)/2;
                out[j+1] = clamp(((-38*r - 74*g + 112*b + 128) >> 8) + 128);
                out[j+3] = clamp(((112*r - 94*g - 18*b + 128) >> 8) + 128);
            }
        }
        REFERENCE_TIME begin = frames_ * output_.Interval(), end = begin + output_.Interval();
        sample->SetTime(&begin, &end);
        sample->SetActualDataLength(size);
        sample->SetSyncPoint(TRUE);
        sample->SetDiscontinuity(frames_ == 0);
        ++frames_;
        return S_OK;
    }
    STDMETHODIMP SetFormat(AM_MEDIA_TYPE* mt) override {
        if (!mt) return E_POINTER;
        CAutoLock lock(m_pFilter->pStateLock());
        if (IsConnected()) return VFW_E_ALREADY_CONNECTED;
        CMediaType candidate(*mt);
        const bool wasSet = formatSet_;
        formatSet_ = false;
        HRESULT hr = CheckMediaType(&candidate);
        formatSet_ = wasSet;
        if (SUCCEEDED(hr)) { m_mt = candidate; formatSet_ = true; }
        return hr;
    }
    STDMETHODIMP GetFormat(AM_MEDIA_TYPE** mt) override {
        if (!mt) return E_POINTER;
        CAutoLock lock(m_pFilter->pStateLock());
        *mt = CreateMediaType(&m_mt); return *mt ? S_OK : E_OUTOFMEMORY;
    }
    STDMETHODIMP GetNumberOfCapabilities(int* count, int* size) override {
        if (!count || !size) return E_POINTER;
        *count = 2; *size = sizeof(VIDEO_STREAM_CONFIG_CAPS); return S_OK;
    }
    STDMETHODIMP GetStreamCaps(int index, AM_MEDIA_TYPE** mt, BYTE* caps) override {
        if (!mt || !caps) return E_POINTER;
        *mt = nullptr;
        CMediaType type;
        HRESULT hr = MakeMediaType(index, &type);
        if (hr != S_OK) return index < 0 ? E_INVALIDARG : S_FALSE;
        *mt = CreateMediaType(&type);
        if (!*mt) return E_OUTOFMEMORY;
        auto* c = reinterpret_cast<VIDEO_STREAM_CONFIG_CAPS*>(caps);
        ZeroMemory(c, sizeof(*c));
        c->guid = FORMAT_VideoInfo;
        c->InputSize = c->MinCroppingSize = c->MaxCroppingSize = c->MinOutputSize = c->MaxOutputSize = {output_.width, output_.height};
        c->CropGranularityX = c->CropGranularityY = c->OutputGranularityX = c->OutputGranularityY = 1;
        c->MinFrameInterval = c->MaxFrameInterval = output_.Interval();
        c->MinBitsPerSecond = c->MaxBitsPerSecond = type.GetSampleSize() * 8 * output_.fps;
        return S_OK;
    }
    STDMETHODIMP Set(REFGUID, DWORD, LPVOID, DWORD, LPVOID, DWORD) override { return E_NOTIMPL; }
    STDMETHODIMP Get(REFGUID set, DWORD id, LPVOID, DWORD, LPVOID data, DWORD size, DWORD* returned) override {
        if (set != AMPROPSETID_Pin) return E_PROP_SET_UNSUPPORTED;
        if (id != AMPROPERTY_PIN_CATEGORY) return E_PROP_ID_UNSUPPORTED;
        if (returned) *returned = sizeof(GUID);
        if (!data) return returned ? S_OK : E_POINTER;
        if (size < sizeof(GUID)) return E_UNEXPECTED;
        *static_cast<GUID*>(data) = PIN_CATEGORY_CAPTURE; return S_OK;
    }
    STDMETHODIMP QuerySupported(REFGUID set, DWORD id, DWORD* support) override {
        if (!support) return E_POINTER;
        *support = 0;
        if (set != AMPROPSETID_Pin) return E_PROP_SET_UNSUPPORTED;
        if (id != AMPROPERTY_PIN_CATEGORY) return E_PROP_ID_UNSUPPORTED;
        *support = KSPROPERTY_SUPPORT_GET; return S_OK;
    }
private:
    HANDLE stop_ = nullptr;
    vcam::FrameBus bus_;
    vcam::OutputFormat output_;
    bool formatSet_ = false;
    std::vector<unsigned char> pixels_;
    long long frames_ = 0;
    std::chrono::steady_clock::time_point start_;
};
class Camera final : public CSource {
public:
    Camera(LPUNKNOWN outer, HRESULT* hr) : CSource(NAME("VideoWebcam"), outer, CLSID_VideoWebcam) {
        stream_ = new(std::nothrow) CameraStream(hr, this);
        if (!stream_) *hr = E_OUTOFMEMORY;
    }
    static CUnknown* WINAPI Create(LPUNKNOWN outer, HRESULT* hr) {
        auto* camera = new(std::nothrow) Camera(outer, hr);
        if (!camera) *hr = E_OUTOFMEMORY;
        return camera;
    }
    STDMETHODIMP Stop() override { if (stream_) stream_->Cancel(); return CSource::Stop(); }
private:
    CameraStream* stream_ = nullptr; // CSource owns its streams.
};
CFactoryTemplate g_Templates[] = {{L"VideoWebcam", &CLSID_VideoWebcam, Camera::Create, nullptr, nullptr}};
int g_cTemplates = sizeof(g_Templates) / sizeof(g_Templates[0]);
extern "C" BOOL WINAPI DllEntryPoint(HINSTANCE, ULONG, LPVOID);
BOOL APIENTRY DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    return DllEntryPoint(instance, reason, reserved);
}

// Per-user COM and device enumeration registration. Each regsvr32 process selects its registry view.
static LONG SetString(const std::wstring& key, const wchar_t* name, const wchar_t* value) {
    HKEY h = nullptr;
    LONG result = RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &h, nullptr);
    if (result != ERROR_SUCCESS) return result;
    result = RegSetValueExW(h, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value), DWORD((wcslen(value)+1)*sizeof(wchar_t)));
    RegCloseKey(h); return result;
}
static std::wstring ClassKey() { return std::wstring(L"Software\\Classes\\CLSID\\") + CameraClsid; }
static std::wstring DeviceKey() {
    return std::wstring(L"Software\\Classes\\CLSID\\{860BB310-5D01-11D0-BD3B-00A0C911CE86}\\Instance\\") + CameraClsid;
}
STDAPI DllUnregisterServer() {
    LONG a = RegDeleteTreeW(HKEY_CURRENT_USER, DeviceKey().c_str());
    LONG b = RegDeleteTreeW(HKEY_CURRENT_USER, ClassKey().c_str());
    if (a != ERROR_SUCCESS && a != ERROR_FILE_NOT_FOUND) return HRESULT_FROM_WIN32(a);
    return b == ERROR_FILE_NOT_FOUND ? S_OK : HRESULT_FROM_WIN32(b);
}
STDAPI DllRegisterServer() {
    wchar_t path[32768];
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&DllRegisterServer), &module)) return HRESULT_FROM_WIN32(GetLastError());
    DWORD length = GetModuleFileNameW(module, path, ARRAYSIZE(path));
    if (!length || length >= ARRAYSIZE(path)) return E_FAIL;
    LONG result = SetString(ClassKey(), nullptr, L"VideoWebcam");
    if (!result) result = SetString(ClassKey() + L"\\InprocServer32", nullptr, path);
    if (!result) result = SetString(ClassKey() + L"\\InprocServer32", L"ThreadingModel", L"Both");
    if (!result) result = SetString(DeviceKey(), L"CLSID", CameraClsid);
    if (!result) result = SetString(DeviceKey(), L"FriendlyName", L"VideoWebcam");
    return HRESULT_FROM_WIN32(result);
}
