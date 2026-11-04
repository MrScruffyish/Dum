// Dum - network transport. ENet for remote players, an in-memory loopback for the local player.
#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

struct _ENetHost;
struct _ENetPeer;

struct NetEvent {
    enum Type { Connect, Disconnect, Data } type;
    int peer = -1;
    std::vector<uint8_t> data;
};

// In-process pipe between a local client and the server (single player / host).
struct LoopbackPipe {
    std::deque<std::vector<uint8_t>> toServer, toClient;
    bool clientConnected = false;   // client has "connected"
    bool serverSawConnect = false;
    bool closed = false;
};

class ServerNet {
public:
    ServerNet();
    ~ServerNet();
    // Start listening for remote players. Returns false on failure (port in use...).
    bool listen(uint16_t port, int maxClients, std::string& err);
    // Attach the local loopback client (always peer id 0).
    void attachLoopback(std::shared_ptr<LoopbackPipe> pipe) { loop_ = pipe; }
    void poll(std::vector<NetEvent>& out);
    void send(int peer, const std::vector<uint8_t>& data, bool reliable);
    void disconnect(int peer);
    void flush();
    bool listening() const { return host_ != nullptr; }

private:
    struct _ENetHost* host_ = nullptr;
    std::vector<struct _ENetPeer*> peers_;   // index = peer id (0 reserved for loopback)
    std::shared_ptr<LoopbackPipe> loop_;
};

class ClientNet {
public:
    ClientNet();
    ~ClientNet();
    void useLoopback(std::shared_ptr<LoopbackPipe> pipe);
    bool connect(const std::string& host, uint16_t port, std::string& err);
    void poll(std::vector<NetEvent>& out);
    void send(const std::vector<uint8_t>& data, bool reliable);
    void disconnect();
    bool connected() const { return connected_; }

private:
    struct _ENetHost* host_ = nullptr;
    struct _ENetPeer* peer_ = nullptr;
    std::shared_ptr<LoopbackPipe> loop_;
    bool connected_ = false;
};

bool netInit();
void netShutdown();
