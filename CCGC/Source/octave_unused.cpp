// Parts of Octave the game never uses, stood in for so that the linker
// doesn't take them from libEngine.a (an archive member is only linked for
// a symbol nothing else defines), and their memory stays free.
//
// The network: Octave's GameCube backend (Network_Dolphin.cpp), every call
// doing nothing. With it went lwIP (libbba): 450 KB of memory it sets
// aside, and a try at the Broadband Adapter every boot. The game has no
// online play on the GameCube.
//
// Vorbis encoding (vorbisenc.o, 485 KB of code and tables): only
// AUD_EncodeVorbis calls it, which the editor uses to import sounds.
#include <cstdint>
#include <cstdio>

#include "Network/Network.h"

struct vorbis_info;

extern "C" int vorbis_encode_init_vbr(vorbis_info*, long, long, float) {
    return -130;  // OV_EIMPL: not implemented
}

void NET_Initialize() {}
void NET_Shutdown() {}
void NET_Update() {}

bool NET_IsActive() {
    return false;
}

SocketHandle NET_SocketCreate() {
    return -1;
}

void NET_SocketBind(SocketHandle, uint32_t, uint16_t) {}

int32_t NET_SocketRecv(SocketHandle, char*, uint32_t) {
    return -1;
}

int32_t NET_SocketRecvFrom(SocketHandle, char*, uint32_t, uint32_t&, uint16_t&) {
    return -1;
}

int32_t NET_SocketSendTo(SocketHandle, const char*, uint32_t, uint32_t, uint16_t) {
    return -1;
}

void NET_SocketClose(SocketHandle) {}
void NET_SocketSetBlocking(SocketHandle, bool) {}
void NET_SocketSetBroadcast(SocketHandle, bool) {}

void NET_SocketGetIpAndPort(SocketHandle, uint32_t& ipAddr, uint16_t& port) {
    ipAddr = 0;
    port = 0;
}

uint32_t NET_IpStringToUint32(const char* ipString) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (std::sscanf(ipString, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return 0;
    return (a & 255) << 24 | (b & 255) << 16 | (c & 255) << 8 | (d & 255);
}

void NET_IpUint32ToString(uint32_t ip, char* outIpString) {
    std::sprintf(outIpString, "%u.%u.%u.%u", unsigned(ip >> 24), unsigned(ip >> 16 & 255), unsigned(ip >> 8 & 255),
                 unsigned(ip & 255));
}

uint32_t NET_GetIpAddress() {
    return 0;
}

uint32_t NET_GetSubnetMask() {
    return 0;
}
