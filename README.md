# VideoWebcam

A native C++ Windows app that turns a local video or image into a virtual webcam. It includes a DirectShow camera component of its own; OBS and a physical webcam are not required.

## Run

1. Keep the entire `dist/VideoWebcam` folder together in a permanent, writable location.
2. Open `VideoWebcam.exe` and click **Install camera** once (or run `install.cmd`). Registration is for your current Windows user and does not require administrator rights.
3. Open or drag in a video, JPG, PNG, or WebP file.
4. Click **Start camera**. Restart your meeting application if it was open during installation, then choose **VideoWebcam** in its camera settings.
5. Keep VideoWebcam open. **Stop camera** sends black frames immediately; closing or crashing the app makes the camera go black within two seconds.

**Pause** holds the current frame. **Restart** starts the source again. **Loop** repeats videos; when disabled, the last frame is held at the end. **Framing: fit** adds black bars to preserve the whole image; **fill / crop** fills the frame. **Mirror** flips the actual camera output. Changing loop, framing, or mirror restarts playback. The preview continues while the camera is off, so you can prepare your source before broadcasting.

Use **Resolution** and **FPS** to choose 640 × 480, 1280 × 720, or 1920 × 1080 at 15, 24, 25, 30, or 60 frames per second. Choices are saved in `settings.ini` beside the app. Changing output restarts playback and updates the preview and camera source. Reopen the webcam in your meeting app (or restart that app) to use the new format. An already connected camera keeps its negotiated resolution/FPS and receives resized frames until reopened. Set the output before opening the webcam in other apps; a camera created without an available producer configuration defaults to 720p/30.

When upgrading from the original fixed-output build, close VideoWebcam and all camera consumers before replacing the executable and **both** filter DLLs. They must be updated together because the shared-frame format changed. Reinstall if the folder moved.

Run `uninstall.cmd` before moving or deleting the folder. Restart camera applications after installation or removal. You can also run `VideoWebcam.exe --register` or `--unregister` silently, or pass a media path to open it at startup.

## Compatibility and limits

- Windows 10/11 **x64**, with both 64-bit and 32-bit DirectShow camera components included. Native ARM64 is not provided.
- Camera output is user-selectable up to **1920 × 1080 at 60 fps**, RGB24 or YUY2. Arbitrary source dimensions are scaled into that output. Higher resolutions and frame rates require more CPU; increasing FPS duplicates source frames when needed and does not create additional motion detail.
- Designed for desktop software that enumerates DirectShow capture devices. Browser and meeting-app behavior varies by version and policy; this is not a Media Foundation virtual camera, and Media Foundation-only applications such as some Windows Camera configurations may not show it. An in-process DirectShow capture graph is covered by automated tests; individual meeting apps still need a live compatibility check.
- FFmpeg decodes local video and images. MP4, MOV, MKV, AVI, WebM, JPG, PNG and WebP are supported when their codecs are included in the supplied FFmpeg build. Encrypted/DRM media and corrupt files are not supported. Animated WebP is treated as a still image, subject to the decoder's support; the app makes no universal codec guarantee.
- Video only: audio is neither played nor exposed as a microphone. Transparent images are converted to opaque camera frames.
- One producer per Windows user/session. Multiple camera consumers can read the same source. Run the app and consumers as the same Windows user in the same session.
- Installation stores the DLL's absolute path. Keep the folder in place, and close camera consumers before replacing DLLs with a newer build.

## Build from source

Install **Visual Studio 2022 Build Tools** with **Desktop development with C++**, the Windows 10/11 SDK, and CMake tools. Git and PowerShell are needed for the initial dependency download.

```powershell
powershell -ExecutionPolicy Bypass -File scripts/bootstrap.ps1
powershell -ExecutionPolicy Bypass -File scripts/build.ps1
powershell -ExecutionPolicy Bypass -File scripts/test-media.ps1
```

`bootstrap.ps1` copies Microsoft's DirectShow base classes at a pinned commit and downloads the Windows FFmpeg essentials build from Gyan (a binary provider linked by FFmpeg). The C++ runtime is linked statically. Builds and camera graph tests run for both architectures. The runnable folder is `dist/VideoWebcam`.

No registry changes are made by the build or media test scripts. Install through the app when ready. Camera graph tests load the filter DLL directly and verify COM lifetime, capture-pin discovery, format negotiation, RGB/YUY2 pixels, image orientation, stop/resume, black output, timestamps, and graph restart. Media tests generate actual JPG/PNG/WebP/video fixtures and decode them through the same FFmpeg process code used by the UI.

Optional integration checks: `scripts/test-registration.ps1` temporarily registers the camera, captures both formats with an independent FFmpeg process, and removes the registration in a `finally` block. It refuses to overwrite an existing installation. `python scripts/test-ui.py` opens a hidden app window, exercises playback/broadcast controls, and closes the app. Run media tests first to generate its fixture. This checks control behavior, not visual rendering.

## Implementation

- `src/app.cpp`: Win32 desktop UI, preview, drag-and-drop, per-user registration.
- `src/decoder.cpp`: isolated FFmpeg subprocess, exact frame reads, bounded playback, pause/loop, error reporting, and child cleanup through a Windows job object.
- `src/frame_bus.cpp`: versioned shared memory guarded by a named mutex. Names contain the current user SID and use the local session namespace. Consumers reject stale frames.
- `src/filter.cpp`: DirectShow source filter with `IAMStreamConfig` and capture pin properties. No kernel driver, driver signing, or test-signing mode is needed.

If a file will not load, open `decoder.log` beside the executable. If the camera is absent, reinstall and restart the consumer. If the camera is black, check **Start camera**, the selected file, and that both processes run under the same Windows user/session.

API references: [DirectShow filter categories](https://learn.microsoft.com/en-us/windows/win32/directshow/filter-categories), [Microsoft DirectShow base classes](https://github.com/microsoft/Windows-classic-samples/tree/434f6002bdf9cf9829406c3ff2b33387982d6168/Samples/Win7Samples/multimedia/directshow/baseclasses), [FFmpeg downloads](https://ffmpeg.org/download.html).
