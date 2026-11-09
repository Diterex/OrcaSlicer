// src/slic3r/GUI/RemoteAPI/RemoteAPIConfig.cpp
#include "RemoteAPIConfig.hpp"

#include "libslic3r/AppConfig.hpp"
#include "slic3r/GUI/GUI_App.hpp"

#include <boost/log/trivial.hpp>
#include <openssl/rand.h>

namespace Slic3r { namespace GUI { namespace RemoteAPI {

Config Config::load()
{
    AppConfig *app_config = wxGetApp().app_config;
    Config     cfg;
    cfg.enabled  = app_config->get_bool("remote_api_enabled");
    cfg.bind_lan = app_config->get_bool("remote_api_bind_lan");
    // Security defaults (CeramicaSlicer): every opt-in below reads as false when the
    // key is absent (AppConfig::get_bool("") == false), so a fresh install has the API
    // off, loopback-only, and both guard toggles - remote_api_allow_file_changes
    // ("Allow deleting and replacing project files") and remote_api_allow_sensitive
    // ("Allow script, G-code and connection edits") - off. Nothing here writes them.
    if (app_config->has("remote_api_notify")) {
        cfg.notify = app_config->get_bool("remote_api_notify");
    } else {
        cfg.notify = true;
        app_config->set_bool("remote_api_notify", true); // persist default so the Preferences checkbox reflects it
    }
    if (app_config->has("remote_api_port")) {
        try { cfg.port = std::stoi(app_config->get("remote_api_port")); }
        catch (...) { cfg.port = Config{}.port; }
        if (cfg.port < 1 || cfg.port > 65535) cfg.port = Config{}.port; // hand-edited config
    }
    cfg.token = app_config->get("remote_api_token");
    if (cfg.token.empty()) {
        cfg.token = generate_token();
        app_config->set("remote_api_token", cfg.token);
        // No save() here: AppConfig is dirty-flagged and the idle handler persists it.
    }
    return cfg;
}

void Config::save() const
{
    AppConfig *app_config = wxGetApp().app_config;
    app_config->set_bool("remote_api_enabled", enabled);
    app_config->set_bool("remote_api_bind_lan", bind_lan);
    app_config->set_bool("remote_api_notify", notify);
    app_config->set("remote_api_port", std::to_string(port));
    app_config->set("remote_api_token", token);
    app_config->save();
}

std::string Config::generate_token()
{
    // std::random_device is not required by the standard to be a CSPRNG - on MinGW
    // it is a fixed deterministic sequence - and this token is the only thing
    // guarding the API once the user opts into LAN binding. Use OpenSSL's CSPRNG,
    // as Utils/OrcaCloudServiceAgent.cpp already does for PKCE secrets.
    static const char hex[] = "0123456789abcdef";
    unsigned char     bytes[16];
    if (RAND_bytes(bytes, sizeof(bytes)) != 1) {
        // Fail closed: an empty token makes check_token() reject every request,
        // which is the safe outcome. Never fall back to a weaker generator.
        BOOST_LOG_TRIVIAL(error) << "RemoteAPI: CSPRNG unavailable, refusing to generate a guessable token";
        return {};
    }
    std::string tok(2 * sizeof(bytes), '0');
    for (size_t i = 0; i < sizeof(bytes); ++i) {
        tok[2 * i]     = hex[bytes[i] >> 4];
        tok[2 * i + 1] = hex[bytes[i] & 0x0F];
    }
    return tok;
}

}}} // namespace
