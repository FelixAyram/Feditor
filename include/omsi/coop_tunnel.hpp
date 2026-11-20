#pragma once

// Internet for co-op rooms behind any router (CGNAT included), without a server of our own -
// the technique of openOMSI (crates/omsi-net/src/tunnel.rs + ws.rs):
//
// * Host: a free Cloudflare quick tunnel. `cloudflared tunnel --url http://127.0.0.1:<ws port>` gives
//   an https://<words>.trycloudflare.com address reachable from anywhere (no account, no port
//   forwarding, no VPN). Behind it a tiny WebSocket server unwraps each connection into a plain TCP
//   connection to the room's port, so the room protocol is unchanged.
// * Guest: WinHTTP opens wss://<words>.trycloudflare.com and a local TCP port bridges to it; the
//   guest joins 127.0.0.1:<that port> as if it were on the LAN.
// * cloudflared is looked for next to the editor, in tools\, on the PATH and in
//   %APPDATA%\OMSI_F4\bin; when missing it is fetched once from Cloudflare's GitHub releases (a
//   pinned release, checked against its published SHA-256 before it is ever run).

#include <cstdint>
#include <memory>
#include <string>

namespace omsi::coop {

class HostTunnel {
public:
    HostTunnel();
    ~HostTunnel();
    // Starts the WebSocket bridge on 127.0.0.1:ws_port -> 127.0.0.1:room_port and cloudflared in front
    // of it. Blocking (download + start): call it on a worker thread.
    bool start(std::uint16_t ws_port, std::uint16_t room_port);
    void stop();
    [[nodiscard]] std::string url();    // https://<words>.trycloudflare.com once known
    [[nodiscard]] std::string status(); // for the UI
private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

class ClientBridge {
public:
    ClientBridge();
    ~ClientBridge();
    // Connects wss://<host>/ and listens on a local port; returns that port (0 on failure, err set).
    std::uint16_t start(const std::string& host, std::string& err);
    void stop();
private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

} // namespace omsi::coop
