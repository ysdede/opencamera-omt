package net.sourceforge.opencamera;

import android.graphics.ImageFormat;
import android.media.Image;
import android.media.ImageReader;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;
import android.content.Context;
import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import android.net.wifi.WifiManager;
import android.os.PowerManager;
import android.view.Surface;

import java.nio.ByteBuffer;

/**
 * OMT Streaming Manager - Manages the OMT network streaming lifecycle.
 * 
 * This class handles:
 * - mDNS service announcement (phone immediately discoverable)
 * - Creating and managing an ImageReader for frame capture
 * - Initializing the OMT sender with proper settings
 * - Converting camera frames to the format needed by OMT
 * - Thread-safe frame processing
 * 
 * Usage:
 * 1. Create instance with callback
 * 2. Call startAnnouncing() to make device discoverable via mDNS
 * 3. Call startStreaming() when user wants to start video streaming
 * 4. Add getStreamingSurface() to camera capture session surfaces
 * 5. Call stopStreaming() when done
 */
public class OMTStreamingManager {
    private static final String TAG = "OMTStreamingManager";

    // Callback interface for streaming events
    public interface StreamingCallback {
        void onAnnouncingStarted();

        void onStreamingStarted();

        void onStreamingStopped();

        void onStreamingError(String error);

        void onConnectionCountChanged(int count);
        
        /**
         * Called when frames are being dropped due to network congestion.
         * @param droppedCount Number of frames dropped in the last reporting period
         * @param totalDropped Total frames dropped since streaming started
         * @param totalSent Total frames sent since streaming started
         */
        void onFramesDropped(long droppedCount, long totalDropped, long totalSent);
    }

    private final Context context;
    private final StreamingCallback callback;
    private OMTSender omtSender;
    private ImageReader imageReader;
    private HandlerThread backgroundThread;
    private Handler backgroundHandler;

    private ByteBuffer frameBuffer;
    private boolean isAnnouncing = false;  // mDNS is active, device is discoverable
    private boolean isStreaming = false;   // Camera frames are being sent

    // Streaming parameters
    private int streamWidth;
    private int streamHeight;
    private int streamFps;
    private int streamQuality;
    private String streamName;

    // mDNS and Locks
    private NsdManager nsdManager;
    private NsdManager.RegistrationListener registrationListener;
    private WifiManager.MulticastLock multicastLock;
    private WifiManager.WifiLock wifiLock;
    private PowerManager.WakeLock wakeLock;
    private static final String SERVICE_TYPE = "_omt._tcp";

    public OMTStreamingManager(Context context, StreamingCallback callback) {
        this.context = context;
        this.callback = callback;
    }

    /**
     * Start mDNS announcement only - makes device discoverable without streaming.
     * This allows viewers to see this device in their sender list immediately.
     * Call startStreaming() later to actually start sending video frames.
     * 
     * @param width   Video width (for announcement)
     * @param height  Video height (for announcement)
     * @param fps     Frame rate (for announcement)
     * @param quality Quality setting (1=Low, 50=Medium, 100=High)
     * @param name    Stream name visible in discovery
     * @return true if announcement started successfully
     */
    public synchronized boolean startAnnouncing(int width, int height, int fps, int quality, String name) {
        if (isAnnouncing) {
            Log.d(TAG, "Already announcing");
            return true;
        }

        this.streamWidth = width;
        this.streamHeight = height;
        this.streamFps = fps;
        this.streamQuality = quality;
        this.streamName = name;

        Log.i(TAG, "Starting OMT announcement: " + name + " (" + width + "x" + height + "@" + fps + "fps)");

        try {
            // Start background thread
            startBackgroundThread();

            // Initialize OMT sender (creates TCP server + mDNS announcement)
            omtSender = new OMTSender();
            if (!omtSender.init(name, width, height, fps, quality)) {
                Log.e(TAG, "Failed to initialize OMT sender for announcement");
                cleanupAnnouncement();
                if (callback != null) {
                    callback.onStreamingError("Failed to initialize OMT sender");
                }
                return false;
            }

            // Register mDNS service (with frequent announcements)
            registerService(6400, name);
            
            isAnnouncing = true;
            Log.i(TAG, "OMT announcement started - device is now discoverable");

            // Start connection monitoring
            startConnectionMonitor();

            if (callback != null) {
                callback.onAnnouncingStarted();
            }

            return true;
        } catch (Exception e) {
            Log.e(TAG, "Failed to start announcement: " + e.getMessage());
            cleanupAnnouncement();
            if (callback != null) {
                callback.onStreamingError("Failed to start announcement: " + e.getMessage());
            }
            return false;
        }
    }

    /**
     * Check if device is currently announcing (discoverable).
     */
    public boolean isAnnouncing() {
        return isAnnouncing;
    }

    /**
     * Start OMT video streaming.
     * If already announcing, this adds ImageReader to start sending camera frames.
     * If not announcing, this starts both announcement and streaming.
     * 
     * @param width   Video width
     * @param height  Video height
     * @param fps     Frame rate
     * @param quality Quality setting (1=Low, 50=Medium, 100=High)
     * @param name    Stream name visible in discovery
     * @return true if streaming started successfully
     */
    public synchronized boolean startStreaming(int width, int height, int fps, int quality, String name) {
        if (isStreaming) {
            Log.w(TAG, "Already streaming");
            return true;
        }

        Log.i(TAG, "Starting OMT streaming: " + width + "x" + height + "@" + fps + "fps, quality=" + quality);

        // If not already announcing, start announcement first
        if (!isAnnouncing) {
            if (!startAnnouncing(width, height, fps, quality, name)) {
                return false;
            }
        }

        try {
            // Create ImageReader for frame capture
            imageReader = ImageReader.newInstance(
                    width, height,
                    ImageFormat.YUV_420_888,
                    3 // Max images in the queue
            );
            imageReader.setOnImageAvailableListener(imageListener, backgroundHandler);

            isStreaming = true;
            Log.i(TAG, "OMT streaming started - camera frames will be sent");

            if (callback != null) {
                callback.onStreamingStarted();
            }

            return true;

        } catch (Exception e) {
            Log.e(TAG, "Error starting streaming: " + e.getMessage(), e);
            cleanupStreaming();
            if (callback != null) {
                callback.onStreamingError("Error: " + e.getMessage());
            }
            return false;
        }
    }

    /**
     * Stop video streaming only (keep mDNS announcement active).
     */
    public synchronized void stopStreaming() {
        if (!isStreaming) {
            return;
        }

        Log.i(TAG, "Stopping OMT streaming (keeping announcement active)");
        isStreaming = false;
        cleanupStreaming();

        if (callback != null) {
            callback.onStreamingStopped();
        }
    }

    /**
     * Stop both streaming and announcement (full shutdown).
     */
    public synchronized void stopAll() {
        Log.i(TAG, "Stopping all OMT services");
        isStreaming = false;
        isAnnouncing = false;
        unregisterService();
        cleanup();

        if (callback != null) {
            callback.onStreamingStopped();
        }
    }

    /**
     * Get the Surface for the ImageReader.
     * This surface should be added to the camera capture session.
     * 
     * @return Surface for frame capture, or null if not streaming
     */
    public Surface getStreamingSurface() {
        return imageReader != null ? imageReader.getSurface() : null;
    }

    /**
     * Check if currently streaming.
     */
    public boolean isStreaming() {
        return isStreaming;
    }

    /**
     * Get the current number of connected receivers.
     */
    public int getConnectionCount() {
        return omtSender != null ? omtSender.getConnectionCount() : 0;
    }

    /**
     * Get the OMT address for discovery.
     */
    public String getAddress() {
        return omtSender != null ? omtSender.getAddress() : null;
    }

    // ImageReader listener - processes each available frame
    private int frameCount = 0;
    private final ImageReader.OnImageAvailableListener imageListener = new ImageReader.OnImageAvailableListener() {
        @Override
        public void onImageAvailable(ImageReader reader) {
            frameCount++;
            if (frameCount % 30 == 1) {
                Log.d(TAG, "onImageAvailable called, frame #" + frameCount + ", isStreaming=" + isStreaming);
            }
            
            if (!isStreaming || omtSender == null) {
                // Drain the queue to prevent backpressure
                Image image = reader.acquireLatestImage();
                if (image != null) {
                    image.close();
                }
                return;
            }

            Image image = reader.acquireLatestImage();
            if (image == null) {
                Log.w(TAG, "acquireLatestImage returned null");
                return;
            }

            try {
                sendFrame(image);
            } finally {
                image.close();
            }
        }
    };

    /**
     * Send a frame to the OMT sender.
     * Converts YUV_420_888 format to the NV12 format expected by OMT.
     */
    private int sentFrameCount = 0;
    private void sendFrame(Image image) {
        if (omtSender == null || !isStreaming) {
            return;
        }

        // Get Y and U planes (U plane for NV12 interleaved UV)
        Image.Plane yPlane = image.getPlanes()[0];
        Image.Plane uPlane = image.getPlanes()[1];

        int yStride = yPlane.getRowStride();
        int uvStride = uPlane.getRowStride();
        int ySize = yStride * image.getHeight();
        int uvSize = uvStride * (image.getHeight() / 2);
        int totalSize = ySize + uvSize;
        
        sentFrameCount++;
        if (sentFrameCount % 30 == 1) {
            Log.d(TAG, "sendFrame #" + sentFrameCount + ": " + image.getWidth() + "x" + image.getHeight() + 
                       ", yStride=" + yStride + ", uvStride=" + uvStride + ", totalSize=" + totalSize);
        }

        // Allocate or resize frame buffer if needed
        if (frameBuffer == null || frameBuffer.capacity() < totalSize) {
            frameBuffer = ByteBuffer.allocateDirect(totalSize);
            Log.d(TAG, "Allocated frame buffer: " + totalSize + " bytes");
        }

        // Copy Y and UV planes into the frame buffer
        frameBuffer.clear();

        ByteBuffer yBuffer = yPlane.getBuffer();
        yBuffer.rewind();
        frameBuffer.put(yBuffer);

        ByteBuffer uBuffer = uPlane.getBuffer();
        uBuffer.rewind();
        frameBuffer.put(uBuffer);

        frameBuffer.flip();

        // Send to OMT
        omtSender.sendFrame(frameBuffer, image.getWidth(), image.getHeight(), yStride, uvStride);
    }

    private void startBackgroundThread() {
        if (backgroundThread != null) {
            return;
        }
        backgroundThread = new HandlerThread("OMTStreamingThread");
        backgroundThread.start();
        backgroundHandler = new Handler(backgroundThread.getLooper());
    }

    private void stopBackgroundThread() {
        if (backgroundThread == null) {
            return;
        }
        backgroundThread.quitSafely();
        try {
            backgroundThread.join();
        } catch (InterruptedException e) {
            Log.e(TAG, "Interrupted while stopping background thread");
        }
        backgroundThread = null;
        backgroundHandler = null;
    }

    private void startConnectionMonitor() {
        if (backgroundHandler == null) {
            return;
        }

        backgroundHandler.post(new Runnable() {
            private int lastCount = -1;

            @Override
            public void run() {
                // Monitor connections while announcing (even if not streaming)
                if (!isAnnouncing || omtSender == null) {
                    return;
                }

                // Check connection count
                int count = omtSender.getConnectionCount();
                if (count != lastCount) {
                    lastCount = count;
                    if (callback != null) {
                        callback.onConnectionCountChanged(count);
                    }
                }
                
                // Check for frame drops (only while streaming)
                if (isStreaming) {
                    long recentDrops = omtSender.getRecentDropsAndReset();
                    if (recentDrops > 0) {
                        long totalDropped = omtSender.getFramesDropped();
                        long totalSent = omtSender.getFramesSent();
                        Log.w(TAG, "Frame drops detected: " + recentDrops + " in last second (total: " + 
                              totalDropped + "/" + totalSent + ", " + 
                              String.format("%.1f%%", omtSender.getDropRatePercent()) + " drop rate)");
                        if (callback != null) {
                            callback.onFramesDropped(recentDrops, totalDropped, totalSent);
                        }
                    }
                }

                // Check again in 1 second
                if (backgroundHandler != null && isAnnouncing) {
                    backgroundHandler.postDelayed(this, 1000);
                }
            }
        });
    }

    /**
     * Cleanup streaming only (ImageReader), keep OMT sender and mDNS active.
     */
    private void cleanupStreaming() {
        // Release ImageReader only
        if (imageReader != null) {
            imageReader.close();
            imageReader = null;
        }
        // Clear frame buffer
        frameBuffer = null;
    }

    /**
     * Cleanup announcement (OMT sender, mDNS, locks).
     */
    private void cleanupAnnouncement() {
        // Cleanup OMT sender
        if (omtSender != null) {
            omtSender.cleanup();
            omtSender = null;
        }
        // Stop background thread
        stopBackgroundThread();
    }

    /**
     * Full cleanup - both streaming and announcement.
     */
    private void cleanup() {
        cleanupStreaming();
        cleanupAnnouncement();
    }

    // ===== Service Discovery =====

    private void registerService(int port, String serviceName) {
        try {
            // 1. Acquire Multicast Lock
            WifiManager wifiManager = (WifiManager) context.getApplicationContext()
                    .getSystemService(Context.WIFI_SERVICE);
            if (wifiManager != null) {
                multicastLock = wifiManager.createMulticastLock("OMT_mDNS");
                multicastLock.setReferenceCounted(true);
                multicastLock.acquire();
                Log.i(TAG, "Multicast lock acquired");

                // 2. Acquire WiFi Lock
                if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.Q) {
                    wifiLock = wifiManager.createWifiLock(WifiManager.WIFI_MODE_FULL_LOW_LATENCY, "OMT_Streaming_Lock");
                } else {
                    wifiLock = wifiManager.createWifiLock(WifiManager.WIFI_MODE_FULL_HIGH_PERF, "OMT_Streaming_Lock");
                }
                wifiLock.setReferenceCounted(false);
                wifiLock.acquire();
                Log.i(TAG, "High-Perf WiFi lock acquired");
            }

            // 3. Acquire WakeLock
            PowerManager powerManager = (PowerManager) context.getSystemService(Context.POWER_SERVICE);
            if (powerManager != null) {
                wakeLock = powerManager.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "OMT:StreamingWakeLock");
                wakeLock.acquire();
                Log.i(TAG, "CPU WakeLock acquired");
            }

            // 4. Register Service
            nsdManager = (NsdManager) context.getSystemService(Context.NSD_SERVICE);
            NsdServiceInfo serviceInfo = new NsdServiceInfo();
            serviceInfo.setServiceName(serviceName);
            serviceInfo.setServiceType(SERVICE_TYPE);
            serviceInfo.setPort(port);

            registrationListener = new NsdManager.RegistrationListener() {
                @Override
                public void onServiceRegistered(NsdServiceInfo NsdServiceInfo) {
                    String mServiceName = NsdServiceInfo.getServiceName();
                    Log.i(TAG, "NSD Service registered: " + mServiceName);
                }

                @Override
                public void onRegistrationFailed(NsdServiceInfo serviceInfo, int errorCode) {
                    Log.e(TAG, "NSD Registration failed: " + errorCode);
                }

                @Override
                public void onServiceUnregistered(NsdServiceInfo arg0) {
                    Log.i(TAG, "NSD Service unregistered");
                }

                @Override
                public void onUnregistrationFailed(NsdServiceInfo serviceInfo, int errorCode) {
                    Log.e(TAG, "NSD Unregistration failed: " + errorCode);
                }
            };

            if (nsdManager != null) {
                nsdManager.registerService(serviceInfo, NsdManager.PROTOCOL_DNS_SD, registrationListener);
                Log.i(TAG, "NSD registration initiated");
            }
        } catch (Exception e) {
            Log.e(TAG, "Failed during service registration", e);
        }
    }

    private void unregisterService() {
        try {
            if (nsdManager != null && registrationListener != null) {
                nsdManager.unregisterService(registrationListener);
                registrationListener = null;
            }

            if (wifiLock != null) {
                if (wifiLock.isHeld())
                    wifiLock.release();
                wifiLock = null;
                Log.i(TAG, "WiFi lock released");
            }

            if (wakeLock != null) {
                if (wakeLock.isHeld())
                    wakeLock.release();
                wakeLock = null;
                Log.i(TAG, "WakeLock released");
            }

            if (multicastLock != null) {
                if (multicastLock.isHeld())
                    multicastLock.release();
                multicastLock = null;
                Log.i(TAG, "Multicast lock released");
            }
        } catch (Exception e) {
            Log.e(TAG, "Failed to unregister/release locks", e);
        }
    }
}
