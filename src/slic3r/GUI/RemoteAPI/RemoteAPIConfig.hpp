// src/slic3r/GUI/RemoteAPI/RemoteAPIConfig.hpp
#ifndef slic3r_RemoteAPIConfig_hpp_
#define slic3r_RemoteAPIConfig_hpp_

#include <string>

namespace Slic3r { namespace GUI { namespace RemoteAPI {

// Persisted in AppConfig (main-thread only: load()/save() must run on the GUI thread).
struct Config
{
    bool        enabled  { false };
    // Stock-Orca remote-api build: 13132 (CeramicaSlicer uses 13131, upstream
    // remote-api 13130), so all three can run side by side without fighting for a port.
    int         port     { 13132 };
    bool        bind_lan { false }; // loopback (127.0.0.1) only unless the user opts in
    bool        notify   { true };  // toast in-app when the API changes state
    std::string token;

    bool operator==(const Config &o) const
    {
        return enabled == o.enabled && port == o.port && bind_lan == o.bind_lan && token == o.token;
    }
    bool operator!=(const Config &o) const { return !(*this == o); }

    static Config      load();
    void               save() const;
    // 32 hex chars from std::random_device. Not for cryptographic secrets in
    // general, but fine for a LAN API token.
    static std::string generate_token();
};

}}} // namespace

#endif
