# Open Camera OMT

A fork of [Open Camera](https://opencamera.org.uk/) with **Open Media Transport (OMT)** network streaming support. Stream your Android camera feed in real-time over your local network to applications like vMix, OBS (via OMT plugin), or the OMT Viewer.

## Features

In addition to all standard Open Camera features:

- Network streaming via OMT protocol over WiFi
- Automatic device discovery using mDNS/Bonjour
- VMX video codec with low-latency compression
- Quality presets: Low (~43 Mbps), Medium (~100 Mbps), High (~130 Mbps) at 1080p30
- Support for 720p, 1080p, and 4K resolutions
- Real-time frame drop monitoring

## Requirements

- Android device with ARM64 (arm64-v8a) processor
- Android 5.0 or later (API 21+)
- WiFi network connection

## Building

```bash
git clone https://github.com/ysdede/opencamera-omt.git
cd opencamera-omt
./gradlew assembleDebug
```

### Release Build

For signed release builds, copy `local.properties.example` to `local.properties` and configure your signing credentials:

```bash
cp local.properties.example local.properties
# Edit local.properties with your keystore details
./gradlew assembleRelease
```

The APK will be output to `app/build/outputs/apk/release/OpenCameraOMT.apk`
## Usage

1. Open the app and navigate to **Settings > Video Settings**
2. Enable **OMT Streaming**
3. Select your preferred quality and resolution
4. Return to the camera view
5. Tap the OMT button to start streaming
6. Connect using vMix, OBS with OMT plugin, or the OMT Viewer app

## Native Libraries

This project includes the following libraries from the [Open Media Transport](https://openmediatransport.org/) project:

| Library | Description | License |
|---------|-------------|---------|
| libomt | OMT protocol implementation | MIT |
| libvmx | VMX video codec | MIT |

## License

This project is licensed under the **GNU General Public License v3.0** - see [gpl-3.0.txt](gpl-3.0.txt) for details.

### Attribution

**Original Open Camera**
- Copyright 2013-2025 Mark Harman
- Website: [opencamera.org.uk](https://opencamera.org.uk/)
- Source: [SourceForge](https://sourceforge.net/p/opencamera/code/)

**OMT Integration**
- Copyright 2026 ysdede
- OMT streaming implementation and native library integration

## Links

- Open Camera: [opencamera.org.uk](https://opencamera.org.uk/)
- Open Media Transport: [openmediatransport.org](https://openmediatransport.org/)
- VMX Codec Documentation: [openmediatransport.org/docs](https://www.openmediatransport.org/docs/)
