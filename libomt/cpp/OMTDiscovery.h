/**
 * OMTDiscovery.h - mDNS/DNS-SD Discovery for OMT
 * Port of libomtnet/src/OMTDiscovery.cs
 * 
 * On Android, this uses Android NSD (Network Service Discovery) API
 * via JNI. The Java side handles the actual NsdManager interaction.
 * 
 * Service Type: _omt._tcp
 */

#ifndef OMT_DISCOVERY_H
#define OMT_DISCOVERY_H

#include <string>
#include <vector>
#include <mutex>
#include <functional>
#include <atomic>
#include <memory>

#include "OMTConstants.h"

namespace omt {

/**
 * Discovered address entry
 */
struct DiscoveryEntry {
    std::string name;           // Service name (e.g., "MyCamera")
    std::string fullName;       // Full name (e.g., "MyCamera._omt._tcp.local")
    std::string hostAddress;    // IP address
    int port = 0;
    bool isLocal = false;       // True if registered by this instance
    int64_t discoveredAt = 0;   // Timestamp when discovered
    int64_t expiresAt = 0;      // Expiry time (0 = never)
};

/**
 * Discovery event callback interface
 */
class IDiscoveryListener {
public:
    virtual ~IDiscoveryListener() = default;
    virtual void onServiceFound(const DiscoveryEntry& entry) = 0;
    virtual void onServiceLost(const std::string& name) = 0;
    virtual void onRegistrationSuccess(const std::string& name) = 0;
    virtual void onRegistrationFailed(const std::string& name, int errorCode) = 0;
};

/**
 * Discovery status
 */
enum class DiscoveryStatus {
    Stopped,
    Starting,
    Running,
    Failed
};

/**
 * Discovery - mDNS/DNS-SD discovery for OMT
 * 
 * Design Pattern: Singleton (shared across all senders/receivers)
 * 
 * On Android, this class is a thin wrapper that coordinates with
 * Java NsdManager via JNI. The actual mDNS operations happen in Java.
 */
class Discovery {
public:
    // Singleton access
    static Discovery& getInstance();
    
    // Non-copyable
    Discovery(const Discovery&) = delete;
    Discovery& operator=(const Discovery&) = delete;
    
    /**
     * Start discovery (browsing for services)
     * @return true if started successfully
     */
    bool start();
    
    /**
     * Stop discovery
     */
    void stop();
    
    /**
     * Register a service
     * @param name Service name (e.g., "MyCamera")
     * @param port TCP port
     * @return true if registration started (async completion via callback)
     */
    bool registerService(const std::string& name, int port);
    
    /**
     * Unregister a service
     */
    bool unregisterService(const std::string& name);
    
    /**
     * Get list of discovered addresses
     * @return Vector of "omt://name:port" strings
     */
    std::vector<std::string> getAddresses();
    
    /**
     * Get all discovery entries
     */
    std::vector<DiscoveryEntry> getEntries() const;
    
    /**
     * Find entry by name
     */
    const DiscoveryEntry* findEntry(const std::string& name) const;
    
    /**
     * Get discovery status
     */
    DiscoveryStatus getStatus() const { return status_.load(); }
    
    /**
     * Set listener for discovery events
     */
    void setListener(IDiscoveryListener* listener) { listener_ = listener; }
    
    // Called from JNI/Java layer
    void onServiceDiscovered(const std::string& name, const std::string& host, int port);
    void onServiceLost(const std::string& name);
    void onRegistered(const std::string& name);
    void onRegistrationFailed(const std::string& name, int errorCode);

private:
    Discovery();
    ~Discovery();
    
    std::vector<DiscoveryEntry> entries_;
    mutable std::mutex mutex_;
    std::atomic<DiscoveryStatus> status_{DiscoveryStatus::Stopped};
    IDiscoveryListener* listener_ = nullptr;
    
    // Currently registered services (by this instance)
    std::vector<std::string> registeredServices_;
    
    void removeExpiredEntries();
    void addOrUpdateEntry(const DiscoveryEntry& entry);
    void removeEntry(const std::string& name);
};

/**
 * Service type constant for OMT
 */
namespace ServiceType {
    constexpr const char* OMT = "_omt._tcp";
    constexpr const char* LOCAL_DOMAIN = "local";
}

} // namespace omt

#endif // OMT_DISCOVERY_H
