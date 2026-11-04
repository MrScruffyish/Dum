// Dum - ENet / loopback transport implementation.
#include "net/net.h"

#include <enet/enet.h>

static bool gNetInited = false;

bool netInit() {
    if (gNetInited) return true;
    if (enet_initialize() != 0) return false;
    gNetInited = true;
    return true;
}

void netShutdown() {
    if (gNetInited) enet_deinitialize();
    gNetInited = false;
}

// Channel 0: reliable, channel 1: unreliable (snapshots/input).
static ENetPacket* makePacket(const std::vector<uint8_t>& d, bool reliable) {
    return enet_packet_create(d.data(), d.size(), reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
}

// ---------------------------------------------------------------- server ----

ServerNet::ServerNet() { peers_.push_back(nullptr); }

ServerNet::~ServerNet() {
    if (host_) {
        for (auto* p : peers_)
            if (p) enet_peer_disconnect_now(p, 0);
        enet_host_destroy(host_);
    }
}

bool ServerNet::listen(uint16_t port, int maxClients, std::string& err) {
    if (!netInit()) { err = "ENet init failed"; return false; }
    ENetAddress addr;
    addr.host = ENET_HOST_ANY;
    addr.port = port;
    host_ = enet_host_create(&addr, (size_t)maxClients, 2, 0, 0);
    if (!host_) { err = "could not open UDP port " + std::to_string(port) + " (already in use?)"; return false; }
    return true;
}

void ServerNet::poll(std::vector<NetEvent>& out) {
    if (loop_) {
        if (loop_->clientConnected && !loop_->serverSawConnect) {
            loop_->serverSawConnect = true;
            out.push_back({NetEvent::Connect, 0, {}});
        }
        while (!loop_->toServer.empty()) {
            out.push_back({NetEvent::Data, 0, std::move(loop_->toServer.front())});
            loop_->toServer.pop_front();
        }
        if (loop_->closed && loop_->serverSawConnect) {
            out.push_back({NetEvent::Disconnect, 0, {}});
            loop_.reset();
        }
    }
    if (!host_) return;
    ENetEvent ev;
    while (enet_host_service(host_, &ev, 0) > 0) {
        switch (ev.type) {
            case ENET_EVENT_TYPE_CONNECT: {
                int id = -1;
                for (size_t i = 1; i < peers_.size(); i++)
                    if (!peers_[i]) { id = (int)i; break; }
                if (id < 0) { id = (int)peers_.size(); peers_.push_back(nullptr); }
                peers_[id] = ev.peer;
                ev.peer->data = (void*)(intptr_t)id;
                out.push_back({NetEvent::Connect, id, {}});
                break;
            }
            case ENET_EVENT_TYPE_RECEIVE: {
                int id = (int)(intptr_t)ev.peer->data;
                NetEvent ne{NetEvent::Data, id, {}};
                ne.data.assign(ev.packet->data, ev.packet->data + ev.packet->dataLength);
                out.push_back(std::move(ne));
                enet_packet_destroy(ev.packet);
                break;
            }
            case ENET_EVENT_TYPE_DISCONNECT: {
                int id = (int)(intptr_t)ev.peer->data;
                if (id > 0 && id < (int)peers_.size()) peers_[id] = nullptr;
                out.push_back({NetEvent::Disconnect, id, {}});
                break;
            }
            default: break;
        }
    }
}

void ServerNet::send(int peer, const std::vector<uint8_t>& data, bool reliable) {
    if (peer == 0) {
        if (loop_) loop_->toClient.push_back(data);
        return;
    }
    if (peer < 0 || peer >= (int)peers_.size() || !peers_[peer]) return;
    enet_peer_send(peers_[peer], reliable ? 0 : 1, makePacket(data, reliable));
}

void ServerNet::disconnect(int peer) {
    if (peer > 0 && peer < (int)peers_.size() && peers_[peer]) enet_peer_disconnect_later(peers_[peer], 0);
}

void ServerNet::flush() {
    if (host_) enet_host_flush(host_);
}

// ---------------------------------------------------------------- client ----

ClientNet::ClientNet() {}

ClientNet::~ClientNet() {
    disconnect();
    if (host_) enet_host_destroy(host_);
}

void ClientNet::useLoopback(std::shared_ptr<LoopbackPipe> pipe) {
    loop_ = pipe;
    loop_->clientConnected = true;
}

bool ClientNet::connect(const std::string& hostName, uint16_t port, std::string& err) {
    if (!netInit()) { err = "ENet init failed"; return false; }
    host_ = enet_host_create(nullptr, 1, 2, 0, 0);
    if (!host_) { err = "could not create client socket"; return false; }
    ENetAddress addr;
    if (enet_address_set_host(&addr, hostName.c_str()) != 0) { err = "unknown host " + hostName; return false; }
    addr.port = port;
    peer_ = enet_host_connect(host_, &addr, 2, 0);
    if (!peer_) { err = "connect failed"; return false; }
    return true;
}

void ClientNet::poll(std::vector<NetEvent>& out) {
    if (loop_) {
        if (!connected_) { connected_ = true; out.push_back({NetEvent::Connect, 0, {}}); }
        while (!loop_->toClient.empty()) {
            out.push_back({NetEvent::Data, 0, std::move(loop_->toClient.front())});
            loop_->toClient.pop_front();
        }
        return;
    }
    if (!host_) return;
    ENetEvent ev;
    while (enet_host_service(host_, &ev, 0) > 0) {
        if (ev.type == ENET_EVENT_TYPE_CONNECT) {
            connected_ = true;
            out.push_back({NetEvent::Connect, 0, {}});
        } else if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
            NetEvent ne{NetEvent::Data, 0, {}};
            ne.data.assign(ev.packet->data, ev.packet->data + ev.packet->dataLength);
            out.push_back(std::move(ne));
            enet_packet_destroy(ev.packet);
        } else if (ev.type == ENET_EVENT_TYPE_DISCONNECT) {
            connected_ = false;
            peer_ = nullptr;
            out.push_back({NetEvent::Disconnect, 0, {}});
        }
    }
}

void ClientNet::send(const std::vector<uint8_t>& data, bool reliable) {
    if (loop_) { loop_->toServer.push_back(data); return; }
    if (peer_ && connected_) {
        enet_peer_send(peer_, reliable ? 0 : 1, makePacket(data, reliable));
        enet_host_flush(host_);
    }
}

void ClientNet::disconnect() {
    if (loop_) { loop_->closed = true; loop_.reset(); connected_ = false; return; }
    if (peer_ && host_) {
        enet_peer_disconnect(peer_, 0);
        ENetEvent ev;
        // Give the server a moment to receive the disconnect.
        while (enet_host_service(host_, &ev, 100) > 0)
            if (ev.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(ev.packet);
            else if (ev.type == ENET_EVENT_TYPE_DISCONNECT) break;
        enet_peer_reset(peer_);
        peer_ = nullptr;
    }
    connected_ = false;
}
