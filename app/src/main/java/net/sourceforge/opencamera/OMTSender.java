package net.sourceforge.opencamera;

import android.util.Log;
import java.nio.ByteBuffer;

/**
 * OMTSender - Java wrapper for the Open Media Transport sender.
 * 
 * This class provides a high-level interface to stream video frames
 * from the Android camera to OMT receivers on the local network.
 * 
 * Usage:
 * 1. Create an instance of OMTSender
 * 2. Call init() with your desired configuration
 * 3. Call sendFrame() for each camera frame
 * 4. Call cleanup() when done streaming
 */
public class OMTSender {

    private static final String TAG = "OMTSender";

    /**
     * Quality/Compression constants for OMT video codec (VMX 4:2:2)
     * 
     * Bandwidth reference at 1080p30:
     * - LOW: ~43 Mbps (Best for Wi-Fi stability)
     * - MEDIUM: ~100 Mbps (Requires good Wi-Fi 5/6)
     * - HIGH: ~130 Mbps (Requires Wi-Fi 6 or wired connection)
     */
    public static final int QUALITY_DEFAULT = 0; // Auto
    public static final int QUALITY_LOW = 1; // ~43 Mbps @ 1080p30 (Recommended for Wi-Fi)
    public static final int QUALITY_MEDIUM = 50; // ~100 Mbps @ 1080p30
    public static final int QUALITY_HIGH = 100; // ~130 Mbps @ 1080p30 (Wired/Wi-Fi 6 only)

    private boolean isInitialized = false;

    // Static block to load native libraries
    static {
        try {
            // Load the VMX codec library first (dependency)
            System.loadLibrary("vmx");
            Log.i(TAG, "Loaded libvmx.so");

            // Load the OMT library
            System.loadLibrary("omt");
            Log.i(TAG, "Loaded libomt.so");

            // Load our OMT bridge library with the JNI functions
            System.loadLibrary("omtbridge");
            Log.i(TAG, "Loaded libomtbridge.so");
        } catch (UnsatisfiedLinkError e) {
            Log.e(TAG, "Failed to load native libraries: " + e.getMessage());
            throw e;
        }
    }

    /**
     * Initialize the OMT sender.
     * 
     * @param name      The name of the sender (visible in discovery)
     * @param width     Video width in pixels
     * @param height    Video height in pixels
     * @param frameRate Frame rate
     * @param quality   Video quality (0=Default, 1=Low, 50=Medium, 100=High)
     * @return true if initialization succeeded
     */
    public boolean init(String name, int width, int height, int frameRate, int quality) {
        if (isInitialized) {
            Log.w(TAG, "OMTSender already initialized, cleaning up first");
            cleanup();
        }

        isInitialized = nativeInit(name, width, height, frameRate, 1, quality);

        if (isInitialized) {
            Log.i(TAG, "OMT Sender initialized: " + name + " (" + width + "x" + height + " @ " + frameRate
                    + "fps, Quality: " + quality + ")");
        } else {
            Log.e(TAG, "Failed to initialize OMT Sender");
        }

        return isInitialized;
    }

    /**
     * Initialize with default quality (MEDIUM).
     */
    public boolean init(String name, int width, int height, int frameRate) {
        return init(name, width, height, frameRate, QUALITY_MEDIUM);
    }

    /**
     * Send a video frame to connected receivers.
     * 
     * The buffer should contain NV12 formatted pixel data (Y plane followed by UV
     * plane).
     * This is the native format from Android's Camera2 API with
     * ImageFormat.YUV_420_888.
     * 
     * @param buffer   Direct ByteBuffer containing NV12 pixel data
     * @param width    Frame width
     * @param height   Frame height
     * @param yStride  Stride of the Y plane (may include row padding)
     * @param uvStride Stride of the UV plane
     * @return true if frame was sent successfully
     */
    public boolean sendFrame(ByteBuffer buffer, int width, int height, int yStride, int uvStride) {
        if (!isInitialized) {
            Log.e(TAG, "Cannot send frame: OMTSender not initialized");
            return false;
        }

        if (!buffer.isDirect()) {
            Log.e(TAG, "Buffer must be a direct ByteBuffer");
            return false;
        }

        return nativeSendFrame(buffer, width, height, yStride, uvStride);
    }

    /**
     * Get the number of currently connected receivers.
     */
    public int getConnectionCount() {
        return isInitialized ? nativeGetConnectionCount() : 0;
    }

    /**
     * Get the discovery address for this sender.
     * Format: "HOSTNAME (Name)"
     */
    public String getAddress() {
        return isInitialized ? nativeGetAddress() : null;
    }

    /**
     * Check if OMT sender is currently initialized and ready.
     */
    public boolean isReady() {
        return isInitialized;
    }

    /**
     * Cleanup and release resources.
     * Call this when streaming is stopped.
     */
    public void cleanup() {
        if (isInitialized) {
            nativeCleanup();
            isInitialized = false;
            Log.i(TAG, "OMT Sender cleaned up");
        }
    }

    // =============================================================
    // Statistics API (for monitoring and UI feedback)
    // =============================================================

    /**
     * Get total frames sent since initialization.
     */
    public long getFramesSent() {
        return isInitialized ? nativeGetFramesSent() : 0;
    }

    /**
     * Get total frames dropped since initialization.
     * Frames are dropped when the receiver can't keep up (network congestion).
     */
    public long getFramesDropped() {
        return isInitialized ? nativeGetFramesDropped() : 0;
    }

    /**
     * Get recent frame drops and reset the counter.
     * Use this for periodic UI updates - call every second to get drops/second.
     * 
     * @return Number of frames dropped since last call
     */
    public long getRecentDropsAndReset() {
        return isInitialized ? nativeGetRecentDropsAndReset() : 0;
    }

    /**
     * Get total bytes sent since initialization.
     */
    public long getBytesSent() {
        return isInitialized ? nativeGetBytesSent() : 0;
    }

    /**
     * Get drop rate as percentage (0-100).
     * @return Drop rate percentage, or 0 if no frames sent
     */
    public float getDropRatePercent() {
        long sent = getFramesSent();
        long dropped = getFramesDropped();
        if (sent == 0) return 0f;
        return (dropped * 100f) / sent;
    }

    // Native method declarations
    private native boolean nativeInit(String name, int width, int height, int frameRateN, int frameRateD, int quality);

    private native boolean nativeSendFrame(ByteBuffer buffer, int width, int height, int yStride, int uvStride);

    private native int nativeGetConnectionCount();

    private native String nativeGetAddress();

    private native void nativeCleanup();

    // Statistics native methods
    private native long nativeGetFramesSent();
    private native long nativeGetFramesDropped();
    private native long nativeGetRecentDropsAndReset();
    private native long nativeGetBytesSent();
}
