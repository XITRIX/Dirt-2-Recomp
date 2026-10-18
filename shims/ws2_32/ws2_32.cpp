// Winsock (ordinals). The game only uses sockets for the online (Quazal) and
// update services, which no longer exist, so the network reports itself down.
// Byte-order helpers and address parsing are real.
#include <arpa/inet.h>
#include <sys/socket.h>

#include "../shim.h"

using namespace shim;

namespace {
constexpr uint32_t SOCKET_ERROR = 0xFFFFFFFF, WSAENETDOWN = 10050;
thread_local uint32_t t_wsa_error = 0;
uint32_t net_down() {
    t_wsa_error = WSAENETDOWN;
    return SOCKET_ERROR;
}
}  // namespace

STDCALL_ORD(ws2_32, 115, 2) {  // WSAStartup(version, *WSADATA)
    if (a[1]) {
        std::memset(rt::host_ptr(a[1]), 0, 400);
        wr16(a[1], 0x0202);
        wr16(a[1] + 2, 0x0202);
        copy_out(a[1] + 4, 257, "WinSock 2.0");
    }
    return 0;
}
STDCALL_ORD(ws2_32, 116, 0) { return 0; }                 // WSACleanup
STDCALL_ORD(ws2_32, 111, 0) { return t_wsa_error; }       // WSAGetLastError
STDCALL_ORD(ws2_32, 112, 1) {                             // WSASetLastError
    t_wsa_error = a[0];
    return 0;
}
STDCALL_ORD(ws2_32, 23, 3) { return net_down(); }         // socket
STDCALL_ORD(ws2_32, 3, 1) { return 0; }                   // closesocket
STDCALL_ORD(ws2_32, 1, 3) { return net_down(); }          // accept
STDCALL_ORD(ws2_32, 2, 3) { return net_down(); }          // bind
STDCALL_ORD(ws2_32, 4, 3) { return net_down(); }          // connect
STDCALL_ORD(ws2_32, 5, 3) { return net_down(); }          // getpeername
STDCALL_ORD(ws2_32, 6, 3) { return net_down(); }          // getsockname
STDCALL_ORD(ws2_32, 7, 5) { return net_down(); }          // getsockopt
STDCALL_ORD(ws2_32, 10, 3) { return net_down(); }         // ioctlsocket
STDCALL_ORD(ws2_32, 13, 2) { return net_down(); }         // listen
STDCALL_ORD(ws2_32, 16, 4) { return net_down(); }         // recv
STDCALL_ORD(ws2_32, 17, 6) { return net_down(); }         // recvfrom
STDCALL_ORD(ws2_32, 18, 5) { return 0; }                  // select: nothing ready
STDCALL_ORD(ws2_32, 19, 4) { return net_down(); }         // send
STDCALL_ORD(ws2_32, 20, 6) { return net_down(); }         // sendto
STDCALL_ORD(ws2_32, 21, 5) { return net_down(); }         // setsockopt
STDCALL_ORD(ws2_32, 22, 2) { return net_down(); }         // shutdown
STDCALL_ORD(ws2_32, 151, 2) { return 0; }                 // __WSAFDIsSet
STDCALL_ORD(ws2_32, 8, 1) { return __builtin_bswap32(a[0]); }                    // htonl
STDCALL_ORD(ws2_32, 14, 1) { return __builtin_bswap32(a[0]); }                   // ntohl
STDCALL_ORD(ws2_32, 9, 1) { return __builtin_bswap16(uint16_t(a[0])); }          // htons
STDCALL_ORD(ws2_32, 15, 1) { return __builtin_bswap16(uint16_t(a[0])); }         // ntohs
STDCALL_ORD(ws2_32, 11, 1) {                                                      // inet_addr
    in_addr x{};
    return a[0] && inet_pton(AF_INET, a.str(0), &x) == 1 ? x.s_addr : 0xFFFFFFFF;
}
STDCALL_ORD(ws2_32, 12, 1) {  // inet_ntoa (returns a per-thread static buffer)
    static thread_local uint32_t buf = 0;
    if (!buf) buf = rt::heap_alloc(16);
    in_addr x{};
    x.s_addr = a[0];
    char s[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &x, s, sizeof s);
    copy_out(buf, 16, s);
    return buf;
}
STDCALL_ORD(ws2_32, 51, 3) {  // gethostbyaddr
    t_wsa_error = 11001;
    return 0;
}
STDCALL_ORD(ws2_32, 52, 1) {  // gethostbyname
    t_wsa_error = 11001;  // WSAHOST_NOT_FOUND
    return 0;
}
STDCALL_ORD(ws2_32, 57, 2) {  // gethostname
    copy_out(a[0], a[1], "dirt2pc");
    return 0;
}
