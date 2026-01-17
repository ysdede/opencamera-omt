/**
 * OMTConstants.h - OMT Constants and Enums
 * Port of libomtnet/src/OMTConstants.cs
 */

#ifndef OMT_CONSTANTS_H
#define OMT_CONSTANTS_H

#include <cstdint>

namespace omt {

// =============================================================
// Constants (from OMTConstants.cs)
// =============================================================
namespace Constants {
    constexpr int NETWORK_SEND_BUFFER = 65536;
    constexpr int NETWORK_RECEIVE_BUFFER = 8388608;  // 8MB
    constexpr int NETWORK_ASYNC_COUNT = 128;         // Increased for Audio+Video
    constexpr int NETWORK_ASYNC_BUFFER_AV = 65536;   // 64KB initial (grows as needed)
    constexpr int NETWORK_ASYNC_COUNT_META_ONLY = 64;
    constexpr int NETWORK_ASYNC_BUFFER_META_ONLY = 1024;
    
    constexpr int VIDEO_FRAME_POOL_COUNT = 4;
    constexpr int VIDEO_MIN_SIZE = 65536;
    constexpr int VIDEO_MAX_SIZE = 10485760;         // 10MB max frame
    
    constexpr int AUDIO_FRAME_POOL_COUNT = 10;
    constexpr int AUDIO_MIN_SIZE = 65536;
    constexpr int AUDIO_MAX_SIZE = 1048576;
    constexpr int AUDIO_SAMPLE_SIZE = 4;
    
    constexpr int NETWORK_PORT_START = 6400;
    constexpr int NETWORK_PORT_END = 6600;
    constexpr int DISCOVERY_SERVER_PORT = 6399;
    
    constexpr int METADATA_MAX_COUNT = 60;
    constexpr int METADATA_FRAME_SIZE = 65536;
    
    constexpr const char* URL_PREFIX = "omt://";
}

// =============================================================
// Metadata Constants (from OMTMetadata.cs)
// =============================================================
namespace MetadataConstants {
    // Subscription commands
    constexpr const char* CHANNEL_SUBSCRIBE_VIDEO = "<OMTSubscribe Video=\"true\" />";
    constexpr const char* CHANNEL_SUBSCRIBE_AUDIO = "<OMTSubscribe Audio=\"true\" />";
    constexpr const char* CHANNEL_SUBSCRIBE_METADATA = "<OMTSubscribe Metadata=\"true\" />";
    
    // Preview mode commands
    constexpr const char* CHANNEL_PREVIEW_VIDEO_ON = "<OMTSettings Preview=\"true\" />";
    constexpr const char* CHANNEL_PREVIEW_VIDEO_OFF = "<OMTSettings Preview=\"false\" />";
    
    // Tally commands
    constexpr const char* TALLY_PREVIEW = "<OMTTally Preview=\"true\" Program==\"false\" />";
    constexpr const char* TALLY_PROGRAM = "<OMTTally Preview=\"false\" Program==\"true\" />";
    constexpr const char* TALLY_PREVIEWPROGRAM = "<OMTTally Preview=\"true\" Program==\"true\" />";
    constexpr const char* TALLY_NONE = "<OMTTally Preview=\"false\" Program==\"false\" />";
    
    // Quality suggestion prefix
    constexpr const char* SUGGESTED_QUALITY_PREFIX = "<OMTSettings Quality=";
    
    // Sender info
    constexpr const char* SENDER_INFO_PREFIX = "<OMTInfo";
    constexpr const char* REDIRECT_PREFIX = "<OMTRedirect";
}

// =============================================================
// Enums
// =============================================================

// Subscription flags (matching OMTFrameType)
enum class Subscription : uint8_t {
    None = 0,
    Video = 1,
    Audio = 2,
    Metadata = 4
};

// Bitwise OR operator for Subscription
inline Subscription operator|(Subscription a, Subscription b) {
    return static_cast<Subscription>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

inline Subscription& operator|=(Subscription& a, Subscription b) {
    a = a | b;
    return a;
}

inline bool hasFlag(Subscription sub, Subscription flag) {
    return (static_cast<uint8_t>(sub) & static_cast<uint8_t>(flag)) != 0;
}

// Quality levels
enum class Quality {
    Default = 0,
    Low = 1,
    Medium = 2,
    High = 3
};

// Frame types
enum class FrameType : uint8_t {
    Metadata = 1,
    Video = 2,
    Audio = 3
};

// Event types (for callbacks)
enum class EventType {
    None,
    Connected,
    Disconnected,
    TallyChanged,
    RedirectChanged,
    QualityChanged
};

} // namespace omt

#endif // OMT_CONSTANTS_H
