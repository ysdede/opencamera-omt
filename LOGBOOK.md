# OMT Integration Logbook

## 1. Project Overview
Integration of Open Media Transport (OMT) streaming capabilities into Open Camera. This allows the Open Camera app to stream the camera feed over the network to compatible receivers using VMX compression and mDNS discovery.

## 2. Porting Process
We ported core functionality from the reference `omtandroid` project into the `opencamera-code` codebase.
*   **Native Integration**: Integrated `libomt`, `libomtbridge`, and `libvmx` native libraries via `OMTSender`.
*   **Logic Migration**: Ported streaming lifecycle management into a dedicated `OMTStreamingManager` class.
*   **Discovery**: Ported `NsdManager` (mDNS) implementation, ensuring compliance with Android's background execution rules (WakeLocks, WifiLocks).
*   **Workflow**: Adopted PowerShell workflow scripts (`deploy.ps1`, `logcat.ps1`) for efficient debugging.

## 3. Implementation Details & Workarounds
*   **Camera Surface**: We chose to Attach the OMT `ImageReader` surface directly to the Camera2 Capture Session.
    *   *Challenge*: Camera2 API requires all surfaces to be known at session creation.
    *   *Solution*: We implemented a session restart mechanism. When streaming starts, `Preview.java` reopens the camera to inject the OMT surface.
*   **Default Mode**: Open Camera defaults to Photo mode. We modified `MyApplicationInterface` to default to Video mode (`true`) to suit the streaming use case.
*   **UI Visibility**: The OMT button was initially hidden due to complex layout logic in `MainUI`. We manually registered the button in `MainUI`'s permanent button list to ensure it renders correctly in the control bar.
*   **Status Overlay**: Implemented a custom text overlay in `MainActivity` to display real-time stream stats (VMX Quality, Resolution, FPS) as the original Toast notifications were insufficient.

## 4. Modified Files (Touch List)
**Core Logic:**
*   `app/src/main/java/net/sourceforge/opencamera/OMTStreamingManager.java` (New: Application logic & mDNS)
*   `app/src/main/java/net/sourceforge/opencamera/OMTSender.java` (New: JNI Bridge)

**Camera & UI Integration:**
*   `app/src/main/java/net/sourceforge/opencamera/MainActivity.java` (Glue code, UI updates, preference loading)
*   `app/src/main/java/net/sourceforge/opencamera/preview/Preview.java` (Surface management, session restart trigger)
*   `app/src/main/java/net/sourceforge/opencamera/cameracontroller/CameraController.java` (Base interface update)
*   `app/src/main/java/net/sourceforge/opencamera/cameracontroller/CameraController2.java` (Session configuration logic)
*   `app/src/main/java/net/sourceforge/opencamera/ui/MainUI.java` (Layout & visibility logic)
*   `app/src/main/java/net/sourceforge/opencamera/MyApplicationInterface.java` (Default settings modification)

**Resources & Config:**
*   `app/src/main/res/layout/activity_main.xml` (Added Button & Status TextView)
*   `app/src/main/res/xml/preferences_sub_video.xml` (Added OMT settings)
*   `app/src/main/AndroidManifest.xml` (Added Network, WiFi, WakeLock permissions)

---

## 5. Session: January 11, 2026 - Black Screen & mDNS Fixes

### Issue 1: Black Screen in Viewer (No Video Output)
**Symptom**: Device was discoverable via mDNS, viewer connected, but displayed a black screen.

**Root Cause**: Open Camera was using **Camera1 API** by default (`preference_camera_api_old`), but OMT streaming requires **Camera2 API** for ImageReader functionality.

**Investigation**:
- Logs showed `CameraController1` being used instead of `CameraController2`
- ImageReader's `onImageAvailable` callback was never being triggered
- Camera1 API doesn't support multi-surface output required for simultaneous preview + streaming

**Fix**: Added automatic Camera2 API switch in `startOmtStreaming()`:
```java
if (!preview.usingCamera2API()) {
    if (supportsCamera2()) {
        SharedPreferences.Editor editor = sharedPreferences.edit();
        editor.putString(PreferenceKeys.CameraAPIPreferenceKey, "preference_camera_api_camera2");
        editor.apply();
        recreate();  // Restart activity to apply
    }
}
```

### Issue 2: VMX Codec Producing Black Frames
**Symptom**: After fixing Camera2 switch, frames were being captured but viewer still showed black.

**Root Cause**: Missing `-DARM64` compile flag in `CMakeLists.txt` for the `omt` library, which prevented NEON SIMD optimizations in VMX codec.

**Reference**: This was documented in `omtandroid/OMT_DEVELOPMENT_LOGBOOK.md` Issue #12.

**Fix**: Added ARM64 flag to CMake:
```cmake
target_compile_options(omt PRIVATE -fms-extensions -fdeclspec -DARM64)
```

### Issue 3: mDNS Service Name Format
**Symptom**: Device not detected by some OMT clients/viewers.

**Root Cause**: OMT client libraries expect service name in format `MachineName (FriendlyName)`.

**Reference**: `omtandroid/OMT_DEVELOPMENT_LOGBOOK.md` Issue #16.

**Fix**: Changed default stream name from "Open Camera" to "Android (Camera)" in `MyApplicationInterface.java`:
```java
public String getOmtStreamingName() {
    return sharedPreferences.getString(PreferenceKeys.OmtStreamingNameKey, "Android (Camera)");
}
```

### Issue 4: Auto-Start Streaming vs Auto-Announce
**User Request**: Device should be immediately discoverable when app opens, but video streaming should only start when user clicks the button.

**Previous Behavior**: App would auto-start full streaming (mDNS + video) when camera opened.

**New Implementation**: Separated announcement from streaming:
1. `startAnnouncing()` - Creates OMT sender, registers mDNS, acquires locks (device discoverable)
2. `startStreaming()` - Creates ImageReader, starts sending camera frames

**Changes**:
- `OMTStreamingManager.java`: Added `startAnnouncing()` method, `isAnnouncing` state
- `MainActivity.java`: Added `autoStartOmtAnnouncement()` called from `cameraSetup()`
- Streaming callback now includes `onAnnouncingStarted()` event

### Issue 5: Preference Default Mismatch
**Symptom**: OMT button invisible on first launch despite XML default being `true`.

**Root Cause**: Java code used `false` as fallback in `getBoolean()`:
```java
// Wrong:
sharedPreferences.getBoolean(PreferenceKeys.OmtStreamingEnabledKey, false)
// Correct:
sharedPreferences.getBoolean(PreferenceKeys.OmtStreamingEnabledKey, true)
```

**Fix**: Updated all occurrences in `MainUI.java` and `MyApplicationInterface.java` to use `true`.

---

## 6. Performance Metrics (Tested Resolutions)

### 4K (3840x2160 @ 30fps)
- **Bitrate**: ~118-121 Mbps average
- **Frame Size**: ~500-700 KB per frame
- **Total YUV Size**: 12.4 MB per frame (YUV_420_888)
- **Profile**: 166 (VMX_PROFILE_OMT_SQ - Standard Quality)
- **WiFi Requirement**: WiFi 6 recommended

### 1080p (1920x1080 @ 30fps)
- **Bitrate**: ~96-110 Mbps average
- **Frame Size**: ~400-450 KB per frame
- **Total YUV Size**: 3.1 MB per frame
- **Profile**: 166 (VMX_PROFILE_OMT_SQ)
- **WiFi Requirement**: WiFi 5/6

### Encoding Notes
- VMX codec creates codec instance on first frame received
- Encoder runs on background thread (`OMTStreamingThread`)
- Frame counter logs every 30 frames for monitoring

### VMX Quality/Profile System
The VMX codec uses **profile-based** compression, NOT numeric quality values:

| Java Constant | Value | VMX Profile | Profile ID | Bandwidth (1080p30) |
|---------------|-------|-------------|------------|---------------------|
| `QUALITY_DEFAULT` | 0 | `VMX_PROFILE_OMT_SQ` | 166 | ~100 Mbps |
| `QUALITY_LOW` | 1 | `VMX_PROFILE_OMT_LQ` | 133 | ~43 Mbps |
| `QUALITY_MEDIUM` | 50 | `VMX_PROFILE_OMT_SQ` | 166 | ~100 Mbps |
| `QUALITY_HIGH` | 100 | `VMX_PROFILE_OMT_HQ` | 199 | ~130 Mbps |

**Important**: The VMX codec handles bitrate targeting internally via these profiles. The original `omtandroid` project discovered that calling `VMX_SetQuality()` directly breaks the internal rate controller. Our implementation correctly uses profile-based initialization without manual quality overrides.

Reference: `omtandroid/OMT_DEVELOPMENT_LOGBOOK.md` - Phase 7 (Native Compression Quality Fix)

---

## 7. Current Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                        Open Camera App                          │
├─────────────────────────────────────────────────────────────────┤
│  MainActivity                                                   │
│  ├── autoStartOmtAnnouncement()  ← Called from cameraSetup()   │
│  ├── startOmtStreaming()         ← Called when user clicks btn │
│  └── stopOmtStreaming()          ← Stops video, keeps announce │
├─────────────────────────────────────────────────────────────────┤
│  OMTStreamingManager                                            │
│  ├── startAnnouncing()  → Creates OMTSender + mDNS             │
│  ├── startStreaming()   → Creates ImageReader + sends frames   │
│  ├── stopStreaming()    → Stops ImageReader, keeps mDNS        │
│  └── stopAll()          → Full cleanup                         │
├─────────────────────────────────────────────────────────────────┤
│  OMTSender (JNI Bridge)                                         │
│  ├── nativeInit()       → Creates native sender                │
│  ├── nativeSendFrame()  → Encodes and sends frame              │
│  └── nativeCleanup()    → Destroys native sender               │
├─────────────────────────────────────────────────────────────────┤
│  Native Libraries (C++)                                         │
│  ├── libomtbridge.so   → JNI interface                         │
│  ├── libomt.so         → OMT protocol + TCP server             │
│  └── libvmx.so         → VMX video codec (NEON optimized)      │
└─────────────────────────────────────────────────────────────────┘
```

---

## 8. Session: January 12, 2026 - Connection Stability & Frame Drop Monitoring

### Issue 6: Stream Disconnections Under Load
**Symptom**: Viewers would disconnect after a few minutes, requiring manual reconnection. This occurred with both the sample viewer app and vMix.

**Root Cause Analysis**:
1. Non-blocking TCP socket returned `EAGAIN` when send buffer was full
2. Original implementation had retry loop with 10ms sleeps (up to 500ms timeout)
3. Large send buffer (4MB) caused bandwidth spikes and increased latency
4. Partial writes were causing unnecessary disconnections

**Investigation**: Referenced `omtandroid/OMT_DEVELOPMENT_LOGBOOK.md` Issue #17 - Similar issues caused by Android Power Management and bandwidth saturation.

**Fix - Immediate Frame Drop (No Retries)**:
```cpp
bool SendAll(const void* data, size_t length) {
    // ...
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // Buffer full - drop frame immediately, no retries
        return false;  // Don't disconnect, just skip frame
    }
    // ...
}
```

### Issue 7: High Bandwidth / Buffer Sizing
**Symptom**: Bandwidth nearly doubled after increasing send buffer from 1MB to 4MB.

**Root Cause**: Large TCP send buffer allowed more frames to queue up, causing burst sends and bandwidth spikes.

**Solution - Adaptive Buffer Sizing**:
Implemented dynamic buffer calculation based on resolution and quality profile:

```cpp
static int calculateOptimalBuffer(int w, int h, VMX_PROFILE prof) {
    int rawSize = w * h * 3 / 2;  // YUV420
    
    // Compression ratio by profile
    float compressionRatio;
    switch (prof) {
        case VMX_PROFILE_OMT_HQ: compressionRatio = 0.25f; break;
        case VMX_PROFILE_OMT_SQ: compressionRatio = 0.18f; break;
        case VMX_PROFILE_OMT_LQ: compressionRatio = 0.12f; break;
        default: compressionRatio = 0.20f; break;
    }
    
    int estimatedFrameSize = (int)(rawSize * compressionRatio);
    int bufferSize = (int)(estimatedFrameSize * 1.5f) + 4096;  // 1.5x frame + overhead
    
    // Clamp: 256KB - 2MB
    return std::clamp(bufferSize, 256*1024, 2*1024*1024);
}
```

**Calculated Buffer Sizes**:
| Resolution | Profile | Est. Frame | Buffer |
|------------|---------|------------|--------|
| 4K | HQ | ~620KB | ~935KB |
| 4K | SQ | ~450KB | ~680KB |
| 1080p | HQ | ~155KB | ~235KB |
| 1080p | SQ | ~112KB | ~170KB |

### Feature: Frame Drop Monitoring & UI Warnings

Added comprehensive frame drop tracking with UI feedback:

**C++ Layer** (`libomt_android.cpp`):
- Atomic counters: `totalFramesSent`, `totalFramesDropped`, `recentDrops`
- API functions: `omt_send_get_frames_sent/dropped/recent_drops_and_reset`

**JNI Bridge** (`OMTBridge.cpp`):
- `nativeGetFramesSent()`, `nativeGetFramesDropped()`, `nativeGetRecentDropsAndReset()`

**Java Layer** (`OMTSender.java`):
- `getFramesSent()`, `getFramesDropped()`, `getDropRatePercent()`

**Streaming Manager** (`OMTStreamingManager.java`):
- New callback: `onFramesDropped(droppedCount, totalDropped, totalSent)`
- Polls drop count every second during streaming

**UI** (`MainActivity.java`):
- Toast warning: "⚠ X frames dropped (Y% total)"
- Status text overlay for severe drops (>5 frames): "⚠ X frames dropped - network congestion"
- Auto-hides warning after 3 seconds

**String Resources**:
```xml
<string name="omt_frames_dropped">⚠ %1$d frames dropped (%2$s%% total)</string>
```

---

## 9. Performance Metrics (Updated)

### 4K (3840x2160 @ 30fps) - Latest Test
- **Bitrate**: ~143-144 Mbps average (Profile 166 - SQ)
- **Frame Size**: ~600-670 KB per frame (avg 603KB)
- **Total Frames**: 2760+ frames sent successfully
- **Frame Drops**: 0 (with adaptive buffer)
- **Stability**: No disconnections during extended testing
- **Buffer Size**: ~935KB (calculated: 4K × SQ profile)

### Key Improvements
| Metric | Before | After |
|--------|--------|-------|
| Disconnections | Frequent (every 2-5 min) | None |
| Frame Drops | Hidden/unknown | Tracked & displayed |
| Buffer Strategy | Fixed 4MB | Adaptive (256KB-2MB) |
| Retry on EAGAIN | 50× retry loop | Immediate drop |
| User Feedback | None | Toast + status overlay |

---

### Feature: Unique Device Naming

**Problem**: Hardcoded "Android (Camera)" name doesn't work when multiple phones are on the same network. Even using Build.MODEL fails with identical device models.

**Solution**: Use ANDROID_ID to generate unique device identifiers.

**Format**: `Model_XXXX (Camera)`
- **Model**: Device model (e.g., "Pixel_6", "SM-G991B", "6165H")
- **XXXX**: First 4 characters of ANDROID_ID (unique per device + app signing key)

**Examples**:
| Device | Broadcast Name |
|--------|----------------|
| TCL 6165H | `6165H_2F3E (Camera)` |
| Pixel 6 #1 | `Pixel_6_A1B2 (Camera)` |
| Pixel 6 #2 | `Pixel_6_E5F6 (Camera)` |

**Implementation** (`MyApplicationInterface.java`):
```java
private String getUniqueDeviceName() {
    String model = android.os.Build.MODEL;
    String androidId = Settings.Secure.getString(
            context.getContentResolver(),
            Settings.Secure.ANDROID_ID
    );
    
    String uniqueSuffix = "_" + androidId.substring(0, 4).toUpperCase();
    String cleanModel = model.replaceAll("[^a-zA-Z0-9\\-]", "_");
    
    return cleanModel + uniqueSuffix;
}
```

**Properties**:
- ✅ Unique per device (ANDROID_ID is different on each phone)
- ✅ Persistent (survives app reinstall)
- ✅ No permissions required
- ✅ Privacy compliant (scoped to app's signing key)
- ✅ User can override with custom name in Settings

---

## 10. Current Status
*   **Functional**: Streaming works at 720p, 1080p, and 4K resolutions
*   **mDNS**: Auto-announces when app starts (device immediately discoverable)
*   **Device Naming**: Unique per-device identifiers using ANDROID_ID
*   **Camera2**: Auto-switches from Camera1 to Camera2 API when needed
*   **UI**: Dedicated button toggles streaming, frame drop warnings displayed
*   **Stability**: No disconnections with adaptive buffer sizing
*   **Performance**: 4K @ 30fps achieving ~144 Mbps throughput
*   **Monitoring**: Real-time frame drop tracking with UI feedback
