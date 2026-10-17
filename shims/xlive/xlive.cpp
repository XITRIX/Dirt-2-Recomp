// Games for Windows Live, offline. One local profile ("Player") is signed in,
// online services report failure, enumerations are empty, protected data is
// stored in the clear. Asynchronous calls complete immediately: XOVERLAPPED
// gets its result, its event is signalled, and ERROR_IO_PENDING is returned,
// which is what the game expects from real GFWL.
//
// Argument counts were taken from the `ret N` of each export in the xliveless
// emulator shipped with the game, and from the documented winsock signatures
// for the X* socket forwards.
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "../shim.h"

using namespace shim;

namespace {

constexpr uint32_t S_OK = 0, E_FAIL = 0x80004005, E_NOTIMPL = 0x80004001;
constexpr uint32_t ERROR_NO_SUCH_USER = 0x525, ERROR_NOT_LOGGED_ON = 0x4E5, ERROR_FUNCTION_FAILED = 0x65B;
constexpr uint32_t XN_SYS_SIGNINCHANGED = 0x0000000A;
constexpr uint64_t kXuid = 0xE000007300000001ull;  // offline XUID
const char* const kUserName = "Player";

std::atomic<uint32_t> g_next_handle{0x0F000010};
std::atomic<bool> g_signin_notified{false};

uint32_t new_handle() { return g_next_handle += 4; }

// Completes an XOVERLAPPED: InternalLow (+0) result, InternalHigh (+4)
// bytes/items, hEvent (+12), dwExtendedError (+24).
uint32_t complete(uint32_t ov, uint32_t result, uint32_t info = 0) {
    if (!ov) return result;
    wr32(ov + 0, result);
    wr32(ov + 4, info);
    wr32(ov + 24, result);
    if (const uint32_t ev = rd32(ov + 12)) set_event(ev);
    return ERROR_IO_PENDING;
}

// PBuffers: [size][data...]
std::mutex g_pb_mu;

}  // namespace

// ---- lifecycle ----
STDCALL_ORD(xlive, 5297, 2) { return S_OK; }   // XLiveInitializeEx
STDCALL_ORD(xlive, 5003, 0) { return S_OK; }   // XLiveUninitialize
STDCALL_ORD(xlive, 5001, 1) { return 1; }      // XLiveInput: not handled
STDCALL_ORD(xlive, 5002, 0) { return S_OK; }   // XLiveRender
STDCALL_ORD(xlive, 5005, 2) { return S_OK; }   // XLiveOnCreateDevice
STDCALL_ORD(xlive, 5006, 0) { return S_OK; }   // XLiveOnDestroyDevice
STDCALL_ORD(xlive, 5007, 1) { return S_OK; }   // XLiveOnResetDevice
STDCALL_ORD(xlive, 5030, 1) { return 0; }      // XLivePreTranslateMessage
STDCALL_ORD(xlive, 5022, 1) { return 0x80070490; }  // XLiveGetUpdateInformation: no update
STDCALL_ORD(xlive, 5024, 1) { return S_OK; }   // XLiveUpdateSystem
STDCALL_ORD(xlive, 5008, 3) { return E_FAIL; } // XHVCreateEngine: no voice chat
STDCALL_ORD(xlive, 5310, 0) { return S_OK; }   // XOnlineStartup
STDCALL_ORD(xlive, 5311, 0) { return S_OK; }   // XOnlineCleanup
STDCALL_ORD(xlive, 476, 5) { return 0; }       // XCustomGetLastActionPressEx

// ---- handles and overlapped ----
STDCALL_ORD(xlive, 5251, 1) {                  // XCloseHandle: also closes kernel handles
    handle_close(a[0]);
    return 1;
}
STDCALL_ORD(xlive, 5254, 1) { return 0; }      // XCancelOverlapped
STDCALL_ORD(xlive, 1082, 1) { return a[0] ? rd32(a[0] + 24) : 0; }  // XGetOverlappedExtendedError
STDCALL_ORD(xlive, 1083, 3) {  // XGetOverlappedResult(ov, *result, wait)
    if (a[1]) wr32(a[1], rd32(a[0] + 4));
    return rd32(a[0]);
}

// ---- notifications ----
STDCALL_ORD(xlive, 5270, 2) { return new_handle(); }  // XNotifyCreateListener
STDCALL_ORD(xlive, 651, 4) {  // XNotifyGetNext(h, filter, *id, *param)
    if (!g_signin_notified.exchange(true)) {
        if (a[2]) wr32(a[2], XN_SYS_SIGNINCHANGED);
        if (a[3]) wr32(a[3], 1);  // user 0 signed in
        return 1;
    }
    return 0;
}

// ---- users ----
STDCALL_ORD(xlive, 5262, 1) { return a[0] == 0 ? 1 : 0; }  // XUserGetSigninState: signed in locally
STDCALL_ORD(xlive, 5263, 3) {  // XUserGetName(user, buf, len)
    if (a[0] != 0) return ERROR_NO_SUCH_USER;
    copy_out(a[1], a[2], kUserName);
    return 0;
}
STDCALL_ORD(xlive, 5267, 3) {  // XUserGetSigninInfo(user, flags, *info)
    if (a[0] != 0) return ERROR_NO_SUCH_USER;
    const uint32_t i = a[2];
    std::memset(rt::host_ptr(i), 0, 40);
    wr64(i + 0, kXuid);
    wr32(i + 12, 1);  // eXUserSigninState_SignedInLocally
    copy_out(i + 24, 16, kUserName);
    return 0;
}
STDCALL_ORD(xlive, 5265, 4) {  // XUserCheckPrivilege(user, priv, *result)
    if (a[2]) wr32(a[2], 0);
    return a[0] == 0 ? 0 : ERROR_NO_SUCH_USER;
}
STDCALL_ORD(xlive, 5264, 5) { return ERROR_NOT_LOGGED_ON; }  // XUserAreUsersFriends
STDCALL_ORD(xlive, 5292, 4) { return complete(a[3], 0); }    // XUserSetContextEx(user, id, value, ov)
STDCALL_ORD(xlive, 5293, 5) { return complete(a[4], 0); }    // XUserSetPropertyEx(user, id, size, data, ov)
STDCALL_ORD(xlive, 5278, 3) { return complete(a[2], 0); }    // XUserWriteAchievements(count, ach, ov)
STDCALL_ORD(xlive, 5274, 4) { return complete(a[3], 0); }    // XUserAwardGamerPicture
STDCALL_ORD(xlive, 5314, 4) {                                // XUserMuteListQuery(user, xuid, *muted)
    if (a[3]) wr32(a[3], 0);
    return 0;
}

// ---- enumerators: always empty ----
STDCALL_ORD(xlive, 5280, 9) {  // XUserCreateAchievementEnumerator(title, user, xuid, flags, start, count, *cb, *h)
    if (a[7]) wr32(a[7], 0);
    if (a[8]) wr32(a[8], new_handle());
    return 0;
}
STDCALL_ORD(xlive, 5284, 7) {  // XUserCreateStatsEnumeratorByRank(title, rank, rows, nspecs, specs, *cb, *h)
    if (a[5]) wr32(a[5], 0);
    if (a[6]) wr32(a[6], new_handle());
    return 0;
}
STDCALL_ORD(xlive, 5286, 8) {  // XUserCreateStatsEnumeratorByXuid(title, xuid(8), rows, nspecs, specs, *cb, *h)
    if (a[6]) wr32(a[6], 0);
    if (a[7]) wr32(a[7], new_handle());
    return 0;
}
STDCALL_ORD(xlive, 5281, 8) { return complete(a[7], ERROR_NOT_LOGGED_ON); }  // XUserReadStats
STDCALL_ORD(xlive, 5312, 5) {  // XFriendsCreateEnumerator(user, start, count, *cb, *h)
    if (a[3]) wr32(a[3], 0);
    if (a[4]) wr32(a[4], new_handle());
    return 0;
}
STDCALL_ORD(xlive, 5360, 4) {  // XContentCreateEnumerator
    rt::log("XContentCreateEnumerator: no downloadable content");
    return ERROR_NO_MORE_FILES;
}
STDCALL_ORD(xlive, 5256, 5) {  // XEnumerate(h, buf, cb, *items, ov)
    if (a[3]) wr32(a[3], 0);
    if (a[4]) return complete(a[4], ERROR_NO_MORE_FILES);
    return ERROR_NO_MORE_FILES;
}
STDCALL_ORD(xlive, 5335, 4) { return ERROR_FUNCTION_FAILED; }  // XTitleServerCreateEnumerator
STDCALL_ORD(xlive, 5287, 3) { return complete(a[2], 0); }      // XUserResetStatsView
STDCALL_ORD(xlive, 5291, 2) { return complete(a[1], 0); }      // XUserResetStatsViewAllUsers

// ---- UI: nothing to show ----
STDCALL_ORD(xlive, 5210, 3) { return 0; }  // XShowFriendRequestUI
STDCALL_ORD(xlive, 5214, 3) { return 0; }  // XShowPlayerReviewUI
STDCALL_ORD(xlive, 5216, 8) { return complete(a[7], ERROR_FUNCTION_FAILED); }  // XShowKeyboardUI
STDCALL_ORD(xlive, 5250, 1) { return 0; }  // XShowAchievementsUI
STDCALL_ORD(xlive, 5252, 3) { return 0; }  // XShowGamerCardUI
STDCALL_ORD(xlive, 5260, 2) { return 0; }  // XShowSigninUI
STDCALL_ORD(xlive, 5266, 7) { return complete(a[6], 0); }  // XShowMessageBoxUI
STDCALL_ORD(xlive, 5275, 1) { return 0; }  // XShowFriendsUI
STDCALL_ORD(xlive, 5365, 5) { return 0; }  // XShowMarketplaceUI

// ---- sessions and online: not available offline ----
STDCALL_ORD(xlive, 5300, 8) { return complete(a[6], ERROR_FUNCTION_FAILED); }   // XSessionCreate
STDCALL_ORD(xlive, 5317, 6) { return complete(a[5], 0); }                       // XSessionWriteStats
STDCALL_ORD(xlive, 5318, 3) { return complete(a[2], 0); }                       // XSessionStart
STDCALL_ORD(xlive, 5319, 11) { return complete(a[10], ERROR_FUNCTION_FAILED); } // XSessionSearchEx
STDCALL_ORD(xlive, 5320, 6) { return complete(a[5], ERROR_FUNCTION_FAILED); }   // XSessionSearchByID
STDCALL_ORD(xlive, 5321, 10) { return complete(a[9], ERROR_FUNCTION_FAILED); }  // XSessionSearch
STDCALL_ORD(xlive, 5322, 5) { return complete(a[4], 0); }                       // XSessionModify
STDCALL_ORD(xlive, 5323, 4) { return complete(a[3], ERROR_FUNCTION_FAILED); }   // XSessionMigrateHost
STDCALL_ORD(xlive, 5325, 4) { return complete(a[3], 0); }                       // XSessionLeaveLocal
STDCALL_ORD(xlive, 5326, 5) { return complete(a[4], 0); }                       // XSessionJoinRemote
STDCALL_ORD(xlive, 5327, 5) { return complete(a[4], 0); }                       // XSessionJoinLocal
STDCALL_ORD(xlive, 5328, 4) { return complete(a[3], ERROR_FUNCTION_FAILED); }   // XSessionGetDetails
STDCALL_ORD(xlive, 5329, 2) { return complete(a[1], 0); }                       // XSessionFlushStats
STDCALL_ORD(xlive, 5330, 2) { return complete(a[1], 0); }                       // XSessionDelete
STDCALL_ORD(xlive, 5332, 2) { return complete(a[1], 0); }                       // XSessionEnd
STDCALL_ORD(xlive, 5333, 7) { return complete(a[6], ERROR_FUNCTION_FAILED); }   // XSessionArbitrationRegister
STDCALL_ORD(xlive, 5336, 4) { return complete(a[3], 0); }                       // XSessionLeaveRemote
STDCALL_ORD(xlive, 5342, 4) { return complete(a[3], ERROR_FUNCTION_FAILED); }   // XSessionModifySkill
STDCALL_ORD(xlive, 5343, 5) { return complete(a[4], ERROR_FUNCTION_FAILED); }   // XSessionCalculateSkill
STDCALL_ORD(xlive, 5315, 2) { return ERROR_FUNCTION_FAILED; }                   // XInviteGetAcceptedInfo
STDCALL_ORD(xlive, 5305, 5) { return complete(a[4], ERROR_FUNCTION_FAILED); }   // XStorageUploadFromMemory
STDCALL_ORD(xlive, 5309, 8) { return ERROR_FUNCTION_FAILED; }                   // XStorageBuildServerPathByXuid
STDCALL_ORD(xlive, 5344, 9) { return ERROR_FUNCTION_FAILED; }                   // XStorageBuildServerPath
STDCALL_ORD(xlive, 5345, 7) { return complete(a[6], ERROR_FUNCTION_FAILED); }   // XStorageDownloadToMemory

// ---- content and protected files ----
STDCALL_ORD(xlive, 5350, 5) { return E_FAIL; }  // XLiveContentCreateAccessHandle
STDCALL_ORD(xlive, 5355, 4) { return E_FAIL; }  // XLiveContentGetPath
STDCALL_ORD(xlive, 5356, 4) { return E_FAIL; }  // XLiveContentGetDisplayName
STDCALL_ORD(xlive, 5354, 2) { return S_OK; }    // XLiveContentVerifyInstalledPackage
STDCALL_ORD(xlive, 5349, 3) {                   // XLiveProtectedVerifyFile(content, buf, path)
    return S_OK;
}
STDCALL_ORD(xlive, 5348, 9) {  // XLiveProtectedCreateFile(content, reserved, wpath, access, share, sec, disp, flags, *h)
    const uint32_t h = open_file(from_utf16(a[2]), a[3], a[6]);
    if (a[8]) wr32(a[8], h);
    return h == kInvalidHandle ? E_FAIL : S_OK;
}

// ---- protected data: stored as-is ----
STDCALL_ORD(xlive, 5036, 2) {  // XLiveCreateProtectedDataContext(info, *ctx)
    if (a[1]) wr32(a[1], new_handle());
    return S_OK;
}
STDCALL_ORD(xlive, 5037, 2) { return S_OK; }  // XLiveQueryProtectedDataInformation
STDCALL_ORD(xlive, 5038, 1) { return S_OK; }  // XLiveCloseProtectedDataContext
static uint32_t copy_protected(const Args& a) {  // (in, in_size, out, *out_size, ctx)
    const uint32_t need = a[1];
    const uint32_t have = rd32(a[3]);
    wr32(a[3], need);
    if (!a[2] || have < need) return 0x8007007A;  // insufficient buffer
    std::memmove(rt::host_ptr(a[2]), rt::host_ptr(a[0]), need);
    return S_OK;
}
STDCALL_ORD(xlive, 5034, 5) { return copy_protected(a); }  // XLiveProtectData
STDCALL_ORD(xlive, 5035, 5) { return copy_protected(a); }  // XLiveUnprotectData

// ---- PBuffers ----
STDCALL_ORD(xlive, 5016, 2) {  // XLivePBufferAllocate(size, *buf)
    const uint32_t p = rt::heap_alloc(a[0] + 4, true);
    wr32(p, a[0]);
    wr32(a[1], p);
    return S_OK;
}
STDCALL_ORD(xlive, 5017, 1) {  // XLivePBufferFree
    rt::heap_free(a[0]);
    return S_OK;
}
STDCALL_ORD(xlive, 5294, 4) {  // XLivePBufferGetByteArray(buf, offset, dst, size)
    std::lock_guard lk(g_pb_mu);
    if (!a[0] || a[1] + a[3] > rd32(a[0])) return E_FAIL;
    std::memcpy(rt::host_ptr(a[2]), rt::host_ptr(a[0] + 4 + a[1]), a[3]);
    return S_OK;
}
STDCALL_ORD(xlive, 5295, 4) {  // XLivePBufferSetByteArray(buf, offset, src, size)
    std::lock_guard lk(g_pb_mu);
    if (!a[0] || a[1] + a[3] > rd32(a[0])) return E_FAIL;
    std::memcpy(rt::host_ptr(a[0] + 4 + a[1]), rt::host_ptr(a[2]), a[3]);
    return S_OK;
}

// ---- XNet: loopback only ----
STDCALL_ORD(xlive, 51, 1) { return 0; }   // XNetStartup
STDCALL_ORD(xlive, 52, 0) { return 0; }   // XNetCleanup
STDCALL_ORD(xlive, 54, 2) { return 0; }   // XNetCreateKey
STDCALL_ORD(xlive, 55, 2) { return 0; }   // XNetRegisterKey
STDCALL_ORD(xlive, 56, 1) { return 0; }   // XNetUnregisterKey
STDCALL_ORD(xlive, 57, 3) {               // XNetXnAddrToInAddr(xnaddr, xnkid, *in_addr)
    if (a[2]) wr32(a[2], 0x0100007F);
    return 0;
}
STDCALL_ORD(xlive, 58, 3) { return 1; }   // XNetServerToInAddr
STDCALL_ORD(xlive, 60, 3) { return 1; }   // XNetInAddrToXnAddr
STDCALL_ORD(xlive, 63, 1) { return 0; }   // XNetUnregisterInAddr
STDCALL_ORD(xlive, 64, 2) { return 1; }   // XNetXnAddrToMachineId
STDCALL_ORD(xlive, 65, 1) { return 0; }   // XNetConnect
STDCALL_ORD(xlive, 66, 1) { return 2; }   // XNetGetConnectStatus: XNET_CONNECT_STATUS_CONNECTED
STDCALL_ORD(xlive, 67, 3) { return 11001; }  // XNetDnsLookup: WSAHOST_NOT_FOUND
STDCALL_ORD(xlive, 68, 1) { return 0; }   // XNetDnsRelease
STDCALL_ORD(xlive, 69, 5) { return 1; }   // XNetQosListen
STDCALL_ORD(xlive, 70, 12) { return 1; }  // XNetQosLookup
STDCALL_ORD(xlive, 72, 1) { return 0; }   // XNetQosRelease
STDCALL_ORD(xlive, 73, 1) {               // XNetGetTitleXnAddr(*xnaddr)
    if (a[0]) {
        std::memset(rt::host_ptr(a[0]), 0, 36);
        wr32(a[0], 0x0100007F);
    }
    return 4;  // XNET_GET_XNADDR_STATIC
}
STDCALL_ORD(xlive, 75, 0) { return 0; }   // XNetGetEthernetLinkStatus: no link (offline)
STDCALL_ORD(xlive, 84, 1) { return 0; }   // XNetSetSystemLinkPort

// ---- XSockets: network down (system link disabled) ----
constexpr uint32_t SOCKET_ERROR = 0xFFFFFFFF, WSAENETDOWN = 10050;
STDCALL_ORD(xlive, 1, 2) { return 0; }                // XWSAStartup
STDCALL_ORD(xlive, 2, 0) { return 0; }                // XWSACleanup
STDCALL_ORD(xlive, 3, 3) { return SOCKET_ERROR; }     // XCreateSocket
STDCALL_ORD(xlive, 4, 1) { return 0; }                // XSocketClose
STDCALL_ORD(xlive, 6, 3) { return SOCKET_ERROR; }     // XSocketIOCTLSocket
STDCALL_ORD(xlive, 7, 5) { return SOCKET_ERROR; }     // XSocketSetSockOpt
STDCALL_ORD(xlive, 9, 3) { return SOCKET_ERROR; }     // XSocketGetSockName
STDCALL_ORD(xlive, 11, 3) { return SOCKET_ERROR; }    // XSocketBind
STDCALL_ORD(xlive, 12, 3) { return SOCKET_ERROR; }    // XSocketConnect
STDCALL_ORD(xlive, 13, 2) { return SOCKET_ERROR; }    // XSocketListen
STDCALL_ORD(xlive, 14, 3) { return SOCKET_ERROR; }    // XSocketAccept
STDCALL_ORD(xlive, 15, 5) { return SOCKET_ERROR; }    // XSocketSelect
STDCALL_ORD(xlive, 16, 5) { return 0; }               // XSocketWSAGetOverlappedResult
STDCALL_ORD(xlive, 18, 4) { return SOCKET_ERROR; }    // XSocketRecv
STDCALL_ORD(xlive, 20, 6) { return SOCKET_ERROR; }    // XSocketRecvFrom
STDCALL_ORD(xlive, 21, 9) { return SOCKET_ERROR; }    // XSocketWSARecvFrom
STDCALL_ORD(xlive, 22, 4) { return SOCKET_ERROR; }    // XSocketSend
STDCALL_ORD(xlive, 24, 6) { return SOCKET_ERROR; }    // XSocketSendTo
STDCALL_ORD(xlive, 25, 9) { return SOCKET_ERROR; }    // XSocketWSASendTo
STDCALL_ORD(xlive, 27, 0) { return WSAENETDOWN; }     // XSocketWSAGetLastError
STDCALL_ORD(xlive, 29, 0) { return new_handle(); }    // XSocketWSACreateEvent
STDCALL_ORD(xlive, 30, 1) { return 1; }               // XSocketWSACloseEvent
STDCALL_ORD(xlive, 32, 1) { return 1; }               // XSocketWSAResetEvent
STDCALL_ORD(xlive, 33, 5) { return 0x102; }           // XSocketWSAWaitForMultipleEvents: timeout
STDCALL_ORD(xlive, 38, 1) { return uint32_t(__builtin_bswap16(uint16_t(a[0]))); }  // XSocketNTOHS
STDCALL_ORD(xlive, 40, 1) { return uint32_t(__builtin_bswap16(uint16_t(a[0]))); }  // XSocketHTONS
