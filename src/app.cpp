#include "decoder.h"
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <objbase.h>
#include <algorithm>
#include <filesystem>
#include <memory>
#include <cstdio>

namespace {
constexpr COLORREF Background = RGB(17,21,28), Panel = RGB(27,33,43), Text = RGB(234,240,246),
                   Muted = RGB(152,166,183), Accent = RGB(83,224,178);
enum { Browse = 101, Live, Pause, Restart, Loop, Fit, Mirror, Install, Resolution, FrameRate };
struct App {
    HWND window{};
    HFONT titleFont{}, font{}, smallFont{};
    HBRUSH background = CreateSolidBrush(Background);
    UINT dpi = 96;
    std::wstring directory, file, status = L"Choose a video or image to begin.";
    std::mutex mutex;
    std::vector<unsigned char> preview;
    vcam::PlaybackState state = vcam::PlaybackState::Ended;
    vcam::PlaybackOptions options;
    bool paused = false;
    std::atomic<bool> live{false};
    vcam::FrameBus bus;
    vcam::Decoder decoder;
    ~App() {
        decoder.Stop(); bus.Publish(nullptr, false, options.output);
        DeleteObject(titleFont); DeleteObject(font); DeleteObject(smallFont); DeleteObject(background);
    }
    int S(int n) const { return MulDiv(n, int(dpi), 96); }
    void Fonts() {
        DeleteObject(titleFont); DeleteObject(font); DeleteObject(smallFont);
        auto make = [&](int size, int weight) {
            return CreateFontW(-S(size), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        };
        titleFont = make(30, FW_SEMIBOLD); font = make(15, FW_NORMAL); smallFont = make(13, FW_NORMAL);
        for (int id = Browse; id <= FrameRate; ++id) SendDlgItemMessageW(window, id, WM_SETFONT, WPARAM(font), TRUE);
    }
    void Label(HDC dc, const std::wstring& text, RECT r, COLORREF color, HFONT face, UINT flags = DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS) {
        SelectObject(dc, face); SetTextColor(dc, color); SetBkMode(dc, TRANSPARENT);
        DrawTextW(dc, text.c_str(), -1, &r, flags);
    }
    RECT PreviewRect() {
        RECT client; GetClientRect(window, &client);
        int width = client.right - S(64), available = client.bottom - S(352);
        int height = width * options.output.height / options.output.width;
        if (height > available) { height = available; width = height * options.output.width / options.output.height; }
        int x = (client.right - width) / 2;
        return {x, S(106), x+width, S(106)+height};
    }
    void Layout() {
        RECT c; GetClientRect(window, &c);
        int y = c.bottom - S(123);
        auto place = [&](int id, int x, int top, int w) { MoveWindow(GetDlgItem(window,id), S(x), top, S(w), S(38), TRUE); };
        place(Browse,32,y,145); place(Live,189,y,180); place(Pause,381,y,96); place(Restart,489,y,96);
        place(Loop,32,y+S(51),145); place(Fit,189,y+S(51),180); place(Mirror,381,y+S(51),96);
        MoveWindow(GetDlgItem(window,Install),c.right-S(191),y+S(51),S(159),S(38),TRUE);
        MoveWindow(GetDlgItem(window,Resolution),S(115),y-S(56),S(180),S(160),TRUE);
        MoveWindow(GetDlgItem(window,FrameRate),S(349),y-S(56),S(104),S(200),TRUE);
        InvalidateRect(window,nullptr,FALSE);
    }
    void Buttons() {
        SetDlgItemTextW(window, Live, live ? L"Stop camera" : L"Start camera");
        SetDlgItemTextW(window, Pause, paused ? L"Resume" : L"Pause");
        SetDlgItemTextW(window, Loop, options.loop ? L"Loop: on" : L"Loop: off");
        SetDlgItemTextW(window, Fit, options.crop ? L"Framing: fill / crop" : L"Framing: fit");
        SetDlgItemTextW(window, Mirror, options.mirror ? L"Mirror: on" : L"Mirror: off");
        bool ready;
        { std::lock_guard<std::mutex> lock(mutex); ready = !preview.empty() && state != vcam::PlaybackState::Error; }
        EnableWindow(GetDlgItem(window,Live), ready || live);
        EnableWindow(GetDlgItem(window,Pause), ready);
        EnableWindow(GetDlgItem(window,Restart), !file.empty());
    }
    void ChangeOutput() {
        const int resolution = int(SendDlgItemMessageW(window,Resolution,CB_GETCURSEL,0,0));
        const int fps = int(SendDlgItemMessageW(window,FrameRate,CB_GETCURSEL,0,0));
        if (resolution < 0 || resolution >= int(ARRAYSIZE(vcam::Resolutions)) ||
            fps < 0 || fps >= int(ARRAYSIZE(vcam::FrameRates))) return;
        decoder.Stop();
        options.output = {int(vcam::Resolutions[resolution].cx),int(vcam::Resolutions[resolution].cy),vcam::FrameRates[fps]};
        { std::lock_guard<std::mutex> lock(mutex); preview.clear(); }
        bus.Publish(nullptr,false,options.output);
        const auto settings = directory + L"\\settings.ini";
        WritePrivateProfileStringW(L"Output",L"Width",std::to_wstring(options.output.width).c_str(),settings.c_str());
        WritePrivateProfileStringW(L"Output",L"Height",std::to_wstring(options.output.height).c_str(),settings.c_str());
        WritePrivateProfileStringW(L"Output",L"FPS",std::to_wstring(options.output.fps).c_str(),settings.c_str());
        if (!file.empty()) Load(file);
        Layout();
    }
    void Load(const std::wstring& path) {
        if (!std::filesystem::is_regular_file(path)) {
            MessageBoxW(window,L"Choose a local video or image file.",L"VideoWebcam",MB_OK | MB_ICONERROR); return;
        }
        decoder.Stop();
        file = std::filesystem::absolute(path).wstring(); paused = false;
        { std::lock_guard<std::mutex> lock(mutex); preview.clear(); state = vcam::PlaybackState::Loading; status = L"Opening media..."; }
        bus.Publish(nullptr,false,options.output);
        const auto output = options.output;
        decoder.Start(directory+L"\\ffmpeg.exe",file,directory+L"\\decoder.log",options,
            [this,output](const auto& pixels) {
                std::lock_guard<std::mutex> lock(mutex);
                preview = pixels;
                bus.Publish(pixels.data(),live.load(),output);
            },
            [this](auto value,const auto& message) {
                std::lock_guard<std::mutex> lock(mutex);
                state = value; status = message;
                if (value == vcam::PlaybackState::Error) { live = false; preview.clear(); bus.Publish(nullptr,false,options.output); }
            });
        Buttons(); InvalidateRect(window,nullptr,FALSE);
    }
    void Choose() {
        wchar_t path[32768]{};
        OPENFILENAMEW dialog{sizeof(dialog)};
        dialog.hwndOwner = window;
        dialog.lpstrFilter = L"Videos and images\0*.mp4;*.mkv;*.mov;*.avi;*.webm;*.wmv;*.m4v;*.mpg;*.mpeg;*.ts;*.jpg;*.jpeg;*.png;*.webp;*.bmp;*.gif\0All files\0*.*\0";
        dialog.lpstrFile = path; dialog.nMaxFile = ARRAYSIZE(path);
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
        if (GetOpenFileNameW(&dialog)) Load(path);
    }
    void Paint() {
        PAINTSTRUCT ps{}; HDC target = BeginPaint(window,&ps);
        RECT c; GetClientRect(window,&c);
        HDC dc = CreateCompatibleDC(target); HBITMAP bitmap = CreateCompatibleBitmap(target,c.right,c.bottom);
        auto old = SelectObject(dc,bitmap); FillRect(dc,&c,background);
        Label(dc,L"VideoWebcam",{S(32),S(22),c.right-S(200),S(66)},Text,titleFont);
        Label(dc,L"Your media. Your camera.",{S(33),S(67),c.right-S(200),S(89)},Muted,smallFont);
        Label(dc,live ? L"●  CAMERA LIVE" : L"●  CAMERA OFF",{c.right-S(192),S(30),c.right-S(32),S(60)},live ? Accent : Muted,font,DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
        RECT previewRect = PreviewRect();
        FillRect(dc,&previewRect,static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        std::wstring currentStatus;
        {
            std::lock_guard<std::mutex> lock(mutex);
            currentStatus = status;
            if (!preview.empty()) {
                BITMAPINFO info{};
                info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                info.bmiHeader.biWidth = options.output.width; info.bmiHeader.biHeight = -options.output.height;
                info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 24; info.bmiHeader.biCompression = BI_RGB;
                SetStretchBltMode(dc,HALFTONE); SetBrushOrgEx(dc,0,0,nullptr);
                StretchDIBits(dc,previewRect.left,previewRect.top,previewRect.right-previewRect.left,previewRect.bottom-previewRect.top,
                             0,0,options.output.width,options.output.height,preview.data(),&info,DIB_RGB_COLORS,SRCCOPY);
            } else {
                RECT center = previewRect; center.top += (center.bottom-center.top)/2-S(24); center.bottom = center.top+S(28);
                Label(dc,L"Drop a video or image here",center,Text,font,DT_CENTER | DT_SINGLELINE | DT_VCENTER);
                center.top += S(30); center.bottom += S(30);
                Label(dc,L"JPG  ·  PNG  ·  WebP  ·  MP4  ·  MOV  ·  MKV  ·  and more",center,Muted,smallFont,DT_CENTER | DT_SINGLELINE | DT_VCENTER);
            }
        }
        int y = c.bottom-S(237);
        std::wstring filename = file.empty() ? L"No source selected" : std::filesystem::path(file).filename().wstring();
        Label(dc,filename,{S(32),y,c.right-S(215),y+S(23)},Text,font);
        const auto formatLabel = std::to_wstring(options.output.width)+L" × "+std::to_wstring(options.output.height)+L" / "+std::to_wstring(options.output.fps)+L" fps";
        Label(dc,formatLabel,{c.right-S(210),y,c.right-S(32),y+S(23)},Muted,smallFont,DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
        Label(dc,currentStatus,{S(32),y+S(25),c.right-S(32),y+S(46)},Muted,smallFont);
        Label(dc,L"Resolution",{S(32),y+S(58),S(113),y+S(87)},Muted,smallFont);
        Label(dc,L"FPS",{S(312),y+S(58),S(347),y+S(87)},Muted,smallFont);
        Label(dc,L"Reopen the webcam in your meeting app after changing output.",
              {S(32),y+S(88),c.right-S(32),y+S(109)},Muted,smallFont);
        Label(dc,L"Select “VideoWebcam” in your meeting app. Keep this window open while broadcasting.",
              {S(32),c.bottom-S(28),c.right-S(32),c.bottom-S(6)},Muted,smallFont);
        BitBlt(target,0,0,c.right,c.bottom,dc,0,0,SRCCOPY);
        SelectObject(dc,old); DeleteObject(bitmap); DeleteDC(dc); EndPaint(window,&ps);
    }
};

bool RegisterCamera(const std::wstring& directory, bool remove) {
    // The registration exports write only this camera's keys under HKCU.
    HMODULE module = LoadLibraryW((directory+L"\\VideoWebcamFilter64.dll").c_str());
    if (!module) { fwprintf(stderr,L"Cannot load camera DLL: Windows error %lu\n",GetLastError()); return false; }
    auto function = reinterpret_cast<HRESULT (STDAPICALLTYPE*)()>(GetProcAddress(module,remove ? "DllUnregisterServer" : "DllRegisterServer"));
    HRESULT result = function ? function() : E_NOINTERFACE;
    FreeLibrary(module);
    if (FAILED(result)) { fwprintf(stderr,L"Camera registration export failed: HRESULT 0x%08lx\n",result); return false; }
    auto dll32 = directory+L"\\VideoWebcamFilter32.dll";
    if (std::filesystem::exists(dll32)) {
        wchar_t system[MAX_PATH + 1]{};
        UINT systemLength = GetSystemWow64DirectoryW(system,ARRAYSIZE(system));
        if (!systemLength || systemLength >= ARRAYSIZE(system)) {
            fwprintf(stderr,L"Cannot locate 32-bit Windows system directory: Windows error %lu\n",GetLastError()); return false;
        }
        auto executable = std::wstring(system)+L"\\regsvr32.exe";
        auto command = vcam::QuoteArgument(executable)+(remove ? L" /u /s " : L" /s ")+vcam::QuoteArgument(dll32);
        STARTUPINFOW si{sizeof(si)}; PROCESS_INFORMATION pi{};
        if (!CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi)) {
            fwprintf(stderr,L"Cannot start 32-bit registration (%ls): Windows error %lu\n",executable.c_str(),GetLastError()); return false;
        }
        DWORD wait = WaitForSingleObject(pi.hProcess,10000), code = 1;
        if (wait == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess,&code);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        if (code != 0) { fwprintf(stderr,L"32-bit registration failed: exit code %lu\n",code); return false; }
    }
    return true;
}
LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        app->window = window; SetWindowLongPtrW(window,GWLP_USERDATA,LONG_PTR(app));
    }
    if (!app) return DefWindowProcW(window,message,wp,lp);
    switch (message) {
    case WM_CREATE: {
        app->dpi = GetDpiForWindow(window);
        const wchar_t* names[] = {L"Open media...",L"Start camera",L"Pause",L"Restart",L"Loop: on",L"Framing: fit",L"Mirror: off",L"Install camera"};
        for (int id = Browse; id <= Install; ++id)
            CreateWindowExW(0,L"BUTTON",names[id-Browse],WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                            0,0,0,0,window,HMENU(INT_PTR(id)),GetModuleHandleW(nullptr),nullptr);
        for (int id : {Resolution,FrameRate})
            CreateWindowExW(0,L"COMBOBOX",L"",WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                            0,0,0,0,window,HMENU(INT_PTR(id)),GetModuleHandleW(nullptr),nullptr);
        for (int i=0; i<int(ARRAYSIZE(vcam::Resolutions)); ++i) {
            const auto size = vcam::Resolutions[i];
            const auto label = std::to_wstring(size.cx)+L" × "+std::to_wstring(size.cy);
            SendDlgItemMessageW(window,Resolution,CB_ADDSTRING,0,LPARAM(label.c_str()));
            if (size.cx == app->options.output.width) SendDlgItemMessageW(window,Resolution,CB_SETCURSEL,i,0);
        }
        for (int i=0; i<int(ARRAYSIZE(vcam::FrameRates)); ++i) {
            const auto label = std::to_wstring(vcam::FrameRates[i]);
            SendDlgItemMessageW(window,FrameRate,CB_ADDSTRING,0,LPARAM(label.c_str()));
            if (vcam::FrameRates[i] == app->options.output.fps) SendDlgItemMessageW(window,FrameRate,CB_SETCURSEL,i,0);
        }
        app->Fonts(); app->Layout(); app->Buttons(); DragAcceptFiles(window,TRUE); SetTimer(window,1,66,nullptr); return 0;
    }
    case WM_GETMINMAXINFO:
        reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {app->S(800),app->S(640)}; return 0;
    case WM_DPICHANGED: {
        app->dpi = HIWORD(wp); app->Fonts(); auto* r = reinterpret_cast<RECT*>(lp);
        SetWindowPos(window,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER); return 0;
    }
    case WM_SIZE: app->Layout(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: app->Paint(); return 0;
    case WM_TIMER: app->Buttons(); InvalidateRect(window,nullptr,FALSE); return 0;
    case WM_DRAWITEM: {
        auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        bool primary = item->CtlID == Live, disabled = (item->itemState & ODS_DISABLED) != 0;
        HBRUSH brush = CreateSolidBrush(primary && !disabled ? Accent : Panel);
        FillRect(item->hDC,&item->rcItem,brush); DeleteObject(brush);
        wchar_t text[128]; GetWindowTextW(item->hwndItem,text,ARRAYSIZE(text));
        app->Label(item->hDC,text,item->rcItem,disabled ? RGB(85,98,115) : primary ? Background : Text,app->font,DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        if (item->itemState & ODS_FOCUS) { RECT r = item->rcItem; InflateRect(&r,-4,-4); DrawFocusRect(item->hDC,&r); }
        return TRUE;
    }
    case WM_DROPFILES: {
        HDROP drop = reinterpret_cast<HDROP>(wp); wchar_t path[32768]{};
        DragQueryFileW(drop,0,path,ARRAYSIZE(path)); DragFinish(drop); app->Load(path); return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case Resolution: case FrameRate:
            if (HIWORD(wp) == CBN_SELCHANGE) app->ChangeOutput();
            break;
        case Browse: app->Choose(); break;
        case Live:
            app->live = !app->live;
            if (!app->live) { std::lock_guard<std::mutex> lock(app->mutex); app->bus.Publish(nullptr,false,app->options.output); }
            break;
        case Pause: app->paused = !app->paused; app->decoder.Pause(app->paused); break;
        case Restart: if (!app->file.empty()) app->Load(app->file); break;
        case Loop: app->options.loop = !app->options.loop; if (!app->file.empty()) app->Load(app->file); break;
        case Fit: app->options.crop = !app->options.crop; if (!app->file.empty()) app->Load(app->file); break;
        case Mirror: app->options.mirror = !app->options.mirror; if (!app->file.empty()) app->Load(app->file); break;
        case Install:
            if (RegisterCamera(app->directory,false))
                MessageBoxW(window,L"VideoWebcam is installed for your Windows user. Restart your meeting app, then select VideoWebcam in its camera settings.\n\nKeep this folder in its current location. Use uninstall.cmd to remove the camera.",L"Camera installed",MB_OK);
            else MessageBoxW(window,L"Camera registration failed. Check that both filter DLLs are in this folder and the folder is writable.",L"Installation failed",MB_OK | MB_ICONERROR);
            break;
        }
        app->Buttons(); InvalidateRect(window,nullptr,FALSE); return 0;
    case WM_DESTROY:
        KillTimer(window,1); app->decoder.Stop(); app->bus.Publish(nullptr,false,app->options.output); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(window,message,wp,lp);
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int show) {
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    wchar_t executable[32768]{}; GetModuleFileNameW(nullptr,executable,ARRAYSIZE(executable));
    auto directory = std::filesystem::path(executable).parent_path().wstring();
    int argc = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(),&argc);
    std::wstring argument = argc > 1 ? argv[1] : L""; LocalFree(argv);
    if (argument == L"--register" || argument == L"--unregister")
        return RegisterCamera(directory,argument == L"--unregister") ? 0 : 1;
    auto name = vcam::ObjectName(L"producer");
    if (name.empty()) return 1;
    HANDLE instanceLock = CreateMutexW(nullptr,FALSE,name.c_str());
    if (!instanceLock || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (instanceLock) CloseHandle(instanceLock);
        MessageBoxW(nullptr,L"VideoWebcam is already running in this Windows session.",L"VideoWebcam",MB_OK); return 1;
    }
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES}; InitCommonControlsEx(&controls);
    int result = 0;
    {
        App app; app.directory = directory;
        const auto settings = directory + L"\\settings.ini";
        vcam::OutputFormat saved{int(GetPrivateProfileIntW(L"Output",L"Width",1280,settings.c_str())),
            int(GetPrivateProfileIntW(L"Output",L"Height",720,settings.c_str())),
            int(GetPrivateProfileIntW(L"Output",L"FPS",30,settings.c_str()))};
        if (saved.Valid()) app.options.output = saved;
        if (!app.bus.Open(true)) {
            MessageBoxW(nullptr,L"Could not create the camera frame buffer.",L"VideoWebcam",MB_OK | MB_ICONERROR); result = 1;
        } else {
            app.bus.Publish(nullptr,false,app.options.output);
            WNDCLASSW cls{}; cls.lpfnWndProc = WindowProc; cls.hInstance = instance;
            cls.lpszClassName = L"VideoWebcam.Window"; cls.hCursor = LoadCursorW(nullptr,IDC_ARROW);
            cls.hIcon = LoadIconW(nullptr,IDI_APPLICATION); RegisterClassW(&cls);
            HWND window = CreateWindowExW(WS_EX_ACCEPTFILES,cls.lpszClassName,L"VideoWebcam",WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                           CW_USEDEFAULT,CW_USEDEFAULT,1000,850,nullptr,nullptr,instance,&app);
            if (!window) result = 1;
            else {
                ShowWindow(window,show); UpdateWindow(window);
                if (!argument.empty()) app.Load(argument);
                MSG message{};
                while (GetMessageW(&message,nullptr,0,0) > 0)
                    if (!IsDialogMessageW(window,&message)) { TranslateMessage(&message); DispatchMessageW(&message); }
            }
        }
    }
    CoUninitialize(); CloseHandle(instanceLock); return result;
}
