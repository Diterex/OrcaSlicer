// src/slic3r/GUI/RemoteAPI/RemoteAPIConfig.hpp
#ifndef slic3r_RemoteAPIConfig_hpp_
#define slic3r_RemoteAPIConfig_hpp_

#include <string>

namespace Slic3r { namespace GUI { namespace RemoteAPI {

// Persisted in AppConfig (main-thread only: load()/save() must run on the GUI thread).
struct Config
{
    bool        enabled  { false };
    // CeramicaSlicer: 13131, one above upstream remote-api's 13130, so this fork
    // and a stock MaxEllis build can run side by side without fighting for a port.
    int         port     { 13131 };
    bool        bind_lan { false }; // loopback (127.0.0.1) only unless the user opts in
    bool        notify   { true };  // toast in-app when the API changes state
    std::string token;

    static Config      load();
    void               save() const;
    // 32 hex chars from std::random_device. Not for cryptographic secrets in
    // general, but fine for a LAN API token.
    static std::string generate_token();
};

}}} // namespace

#endif
