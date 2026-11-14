# Sidespeaker

Use your phone as a PC speaker over a USB cable. A small program on the PC captures whatever it is playing and streams it through the cable to an app on the phone. No Wi-Fi, no account, no cost.

| | iPhone | Android |
|---|---|---|
| Windows PC | supported | supported |
| Linux PC | planned | planned |
| macOS PC | planned | planned |

## How it works

- **PC sender (`sender/`):** captures system audio (WASAPI loopback on Windows) and finds the phone, then sends raw 16-bit PCM. A short header (sample rate and channel count) comes first.
  - **iPhone:** through the Apple usbmuxd service that ships with iTunes and the Apple Devices app.
  - **Android:** through `adb forward`, which tunnels a port on the PC to a port on the phone.
- **iPhone app (`ios/`):** a SwiftUI app that listens on a TCP port and plays the stream with AVAudioEngine. Built on GitHub Actions, so no Mac is needed.
- **Android app (`android/`):** a Kotlin app with a foreground service that plays the stream with AudioTrack, so it keeps playing with the screen off. Built on GitHub Actions.

The sender keeps the stream continuous by filling silent moments with silence, and reconnects by itself when the cable is unplugged and replugged.

## Repository layout

```
sender/    PC sender (C++, CMake)
installer/ Windows installer script (Inno Setup)
ios/       iPhone app (Swift, built with XcodeGen)
android/   Android app (Kotlin, Gradle)
.github/   CI: one workflow per target
```

## Get the builds

Every push builds what changed. Open the **Actions** tab, pick the latest run of the workflow you need, and download the artifact at the bottom of the page:

| Workflow | Artifact | Contents |
|---|---|---|
| sender | Sidespeaker-installer | `Sidespeaker-Setup.exe` |
| sender | Sidespeaker-windows | `sidespeaker.exe` and `config.ini` (portable) |
| ios | Sidespeaker-ipa | `Sidespeaker.ipa` |
| android | Sidespeaker-apk | `Sidespeaker.apk` |

Artifacts download as a zip. You can also start any workflow by hand with **Run workflow**.

## Install on Windows

Run `Sidespeaker-Setup.exe`. It installs for your user only (no admin needed) and adds a Start menu entry with an **Edit config** shortcut. It can also add a desktop shortcut or start Sidespeaker when you sign in. Upgrading keeps your `config.ini`. Uninstall from Settings > Apps.

The installer is not code-signed, so Windows SmartScreen may warn. Choose **More info**, then **Run anyway**. It does not install Apple Devices or `adb`; see the phone setup below.

The portable `sidespeaker.exe` reads the `config.ini` that sits next to it.

## Build the PC sender yourself

Requires Visual Studio with the C++ workload (it includes CMake). From "x64 Native Tools Command Prompt for VS":

```
cmake -S . -B build
cmake --build build --config Release
```

The exe and a copy of `config.ini` end up in `build\bin\Release`. Add `-DSIDESPEAKER_CONSOLE=ON` to the first command to build a console version that prints the status in a window instead of using the tray, which is handy for debugging.

## Build the installer yourself

Install [Inno Setup 6](https://jrsoftware.org/isinfo.php), build the sender as above, then:

```
cmake --install build --config Release --prefix dist
iscc installer\sidespeaker.iss
```

The result is `installer-out\Sidespeaker-Setup.exe`. Pass `/DAppVersion=1.2.3` to `iscc` to set the version.

## Set up the phone

### iPhone

1. Install Apple Devices (or iTunes) on the PC.
2. Sideload `Sidespeaker.ipa` with Sideloadly or AltStore. With a free Apple ID it expires after 7 days; re-sideload to refresh.
3. Plug in the iPhone, unlock it, and tap **Trust** the first time.

### Android

1. Install `Sidespeaker.apk` (copy it to the phone, or `adb install Sidespeaker.apk`). Allow installs from unknown sources if asked.
2. On the phone, enable Developer options, then **USB debugging**.
3. Install Android platform-tools on the PC and make sure `adb` is on your PATH, or set `adb_path` in `config.ini`.
4. Plug in the phone and accept the "Allow USB debugging" prompt.

## Use it

1. Open Sidespeaker on the phone.
2. Start Sidespeaker on the PC. It runs in the system tray (it may be under the ^ overflow arrow). The icon is grey while waiting, green while streaming, and red on an error. Hover over it to see the status.
3. Play audio on the PC. It comes out of the phone.

Right-click the tray icon for **Edit config**, **Reload config**, and **Quit**.

Your PC's own speakers keep playing too, so mute them or switch outputs if you only want the phone. If both an iPhone and an Android phone are plugged in, the iPhone is used. Set `transport` to choose.

## Configuration

### PC sender (`config.ini`)

| Key | Default | Meaning |
|---|---|---|
| `transport` | auto | `auto`, `ios`, or `android`. Auto tries the iPhone first, then Android. |
| `port` | 5000 | Phone port to connect to. Must match the app. |
| `poll_ms` | 5 | How often the capture buffer is checked. |
| `retry_ms` | 1000 | Delay between connection attempts. |
| `fill_ms` | 10 | Smallest chunk of silence sent to keep the stream continuous. |
| `usb_only` | 1 | iPhone only: ignore Wi-Fi connections. Set to 0 to allow them. |
| `adb_path` | adb | Android only: path to the `adb` program. |
| `adb_serial` | (empty) | Android only: pick a device when several are plugged in. |
| `startup_notice` | 1 | Show a one-time "running in the tray" notice at startup. Set to 0 to hide it. |

### iPhone (`ios/project.yml`, under `info.properties`)

| Key | Default | Meaning |
|---|---|---|
| `SidePort` | 5000 | Port the app listens on. |
| `SidePrebufferMs` | 60 | Audio queued before playback starts (screen on). |
| `SideMaxBufferMs` | 200 | Maximum queue before audio is dropped (screen on). |
| `SideBgPrebufferMs` | 300 | Same as the prebuffer, while in the background or locked. |
| `SideBgMaxBufferMs` | 450 | Same as the maximum queue, while in the background or locked. |
| `SideTimeoutMs` | 3000 | Disconnect if no data arrives for this long. |

### Android (`android/app/src/main/res/values/config.xml`)

| Key | Default | Meaning |
|---|---|---|
| `port` | 5000 | Port the app listens on. |
| `prebuffer_ms` | 80 | Audio queued before playback starts. |
| `max_buffer_ms` | 250 | Maximum queue before audio is dropped. |
| `timeout_ms` | 3000 | Disconnect if no data arrives for this long. |
| `track_buffer_mult` | 2 | Size of Android's own output buffer, as a multiple of the minimum. |

Phone settings take effect after a rebuild and reinstall.

## Tuning

- **Crackling or cuts:** raise the prebuffer values. Both apps show the current buffer level and an underrun count on screen.
- **Too much delay:** lower the prebuffer and maximum values. Smaller buffers are less forgiving.
- **iPhone, locked screen:** iOS throttles background apps, so the background values are deeper on purpose. Turn off Low Power Mode if it still stutters.

## Troubleshooting

- **Tray status keeps saying it is waiting for a phone:** check the cable. For an iPhone, unlock it, tap Trust, and install Apple Devices. For Android, enable USB debugging and accept the prompt. If your phone only shows up over Wi-Fi, `usb_only=1` ignores it.
- **Status says adb was not found:** install platform-tools or set `adb_path`.
- **Status says it is waiting for the app:** open Sidespeaker on the phone and check that the ports match.
- **iPhone app stops launching after a week:** the free Apple ID signing expired. Re-sideload.
- **Two copies of the iPhone app:** an older build used a different bundle ID. Delete the old one.

## Limitations

- The PC sender only captures audio on Windows so far.
- One phone at a time. Audio is raw PCM, sized for a USB cable, not Wi-Fi.
- Expect roughly the prebuffer length in delay, which is noticeable with video.
