// sceNp: PlayStation Network. A non-goal (docs/phase0-gta5.md, D-024): every function reports "not initialized" or
// "offline" the way RPCS3 does when the user is not signed in, so single-player never waits on PSN.
// Reference: RPCS3 rpcs3/Emu/Cell/Modules/sceNp.cpp/.h, with the NP state of rpcs3/Emu/NP/np_handler.cpp,
// np_contexts.cpp, np_helpers.cpp and signaling_handler.cpp, the message dialogs of
// rpcs3/Emu/RSX/Overlays/Network/overlay_recvmessage_dialog.cpp, and the DRM check's rpcs3/Crypto/unedat.cpp,
// unself.cpp and rpcs3/Emu/system_utils.cpp.
//
// GTA V initializes NP with sceNp2Init (module sceNp2, sceNp2.cpp), L.A. Noire with sceNpInit; both go through np_init
// (np.hpp).
// Until then the manager functions return SCE_NP_ERROR_NOT_INITIALIZED, as RPCS3 does before init; afterwards they
// report a user who is signed out (offline). The callbacks and handlers registered are kept; the basic handler gets
// the one event RPCS3 sends offline too (sceNpBasicAddPlayersHistoryAsync's result), the others are never called.
//
// Offline is RPCS3's np_handler with PSN disabled and no network: get_psn_status() is SCE_NP_MANAGER_STATUS_OFFLINE,
// the user's NP ID is all zeros, no ticket has been received, no basic event is queued, no message can be sent or
// received, no signaling connection exists and the local and public addresses are 0. Each function makes RPCS3's
// checks in RPCS3's order, then takes that path.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <format>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "guest.hpp"
#include "np.hpp"
#include "prx.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/npdrm.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/lv2/syscalls.hpp"
#include "pspocket/recomp/nid.hpp"
#include "pspocket/recomp/npd.hpp"
#include "pspocket/recomp/npdrm.hpp"
#include "pspocket/recomp/self_decrypt.hpp"
#include "sysutil.hpp"
#include "sysutil_cxml.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kChan = "sceNp";

enum : u32 {
    SCE_NP_ERROR_NOT_INITIALIZED = 0x8002AA01,
    SCE_NP_ERROR_ALREADY_INITIALIZED = 0x8002AA02,
    SCE_NP_ERROR_INVALID_ARGUMENT = 0x8002AA03,
    SCE_NP_ERROR_ID_NOT_FOUND = 0x8002AA06,
    SCE_NP_ERROR_INVALID_STATE = 0x8002AA0A,
    SCE_NP_ERROR_OFFLINE = 0x8002AA0C,
    SCE_NP_ERROR_INSUFFICIENT_BUFFER = 0x8002AA0F,
    SCE_NP_AUTH_EINVALID_ARGUMENT = 0x8002A015,
    SCE_NP_BASIC_ERROR_NOT_INITIALIZED = 0x8002A662,
    SCE_NP_BASIC_ERROR_NOT_SUPPORTED = 0x8002A663,
    SCE_NP_BASIC_ERROR_INVALID_ARGUMENT = 0x8002A665,
    SCE_NP_BASIC_ERROR_NO_EVENT = 0x8002A66A,
    SCE_NP_BASIC_ERROR_EXCEEDS_MAX = 0x8002A66B,
    SCE_NP_BASIC_ERROR_NOT_REGISTERED = 0x8002A66D,
    SCE_NP_BASIC_ERROR_BUSY = 0x8002A66F,
    SCE_NP_BASIC_ERROR_CANCEL = 0x8002A671,
    SCE_NP_BASIC_ERROR_INVALID_DATA_ID = 0x8002A673,
    SCE_NP_BASIC_ERROR_NOT_CONNECTED = 0x8002A678,
    SCE_NP_COMMUNITY_ERROR_ALREADY_INITIALIZED = 0x8002A101,
    SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED = 0x8002A102,
    SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT = 0x8002A104,
    SCE_NP_COMMUNITY_ERROR_TOO_MANY_OBJECTS = 0x8002A107,
    SCE_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT = 0x8002A1A1,
    SCE_NP_COMMUNITY_ERROR_INVALID_ID = 0x8002A1A3,
    SCE_NP_COMMUNITY_ERROR_INVALID_ONLINE_ID = 0x8002A1A4,
    SCE_NP_COMMUNITY_ERROR_TOO_MANY_NPID = 0x8002A1AD,
    SCE_NP_COMMUNITY_ERROR_TOO_LARGE_RANGE = 0x8002A1AE,
    SCE_NP_SIGNALING_ERROR_NOT_INITIALIZED = 0x8002A801,
    SCE_NP_SIGNALING_ERROR_CTX_NOT_FOUND = 0x8002A805,
    SCE_NP_SIGNALING_ERROR_CONN_NOT_FOUND = 0x8002A80E,
    SCE_NP_SIGNALING_ERROR_PEER_UNREACHABLE = 0x8002A80F,
    SCE_NP_SIGNALING_ERROR_CTX_MAX = 0x8002A812,
    SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT = 0x8002A815,
    SCE_NP_SIGNALING_ERROR_OWN_NP_ID = 0x8002A816,
    SCE_NP_UTIL_ERROR_INVALID_ARGUMENT = 0x8002AB01,
    SCE_NP_UTIL_ERROR_INVALID_CHARACTER = 0x8002AB0B,
    SCE_NP_UTIL_ERROR_NOT_MATCH = 0x8002AB0F,
    SCE_NP_UTIL_ERROR_UNKNOWN_PLATFORM_TYPE = 0x8002AB10,
    SCE_NP_FRIENDLIST_ERROR_NOT_INITIALIZED = 0x8002AB21,
    SCE_NP_FRIENDLIST_ERROR_INVALID_ARGUMENT = 0x8002AB28,
    SCE_NP_CUSTOM_MENU_ERROR_NOT_INITIALIZED = 0x80023B02,
    SCE_NP_CUSTOM_MENU_ERROR_INVALID_ARGUMENT = 0x80023B09,
    SCE_NP_CUSTOM_MENU_ERROR_NOT_REGISTERED = 0x80023B0B,
    SCE_NP_CUSTOM_MENU_ERROR_EXCEEDS_MAX = 0x80023B0C,
    SCE_NP_MATCHING_ERROR_NOT_INITIALIZED = 0x8002A701,
    SCE_NP_MATCHING_ERROR_INVALID_ARG = 0x8002A703,
    SCE_NP_MATCHING_ERROR_CTX_NOT_FOUND = 0x8002A709,
    SCE_NP_MATCHING_ERROR_ROOM_NOT_FOUND = 0x8002A70B,
    SCE_NP_MATCHING_ERROR_INVALID_ATTR_ID = 0x8002A70F,
    SCE_NP_MATCHING_ERROR_CTX_STILL_RUNNING = 0x8002A713,
    SCE_NP_MATCHING_ERROR_CTX_MAX = 0x8002A716,
    SCE_NP_MATCHING_ERROR_INVALID_REQ_ID = 0x8002A717,
    SCE_NP_MATCHING_ERROR_RESULT_NOT_FOUND = 0x8002A718,
    SCE_NP_MATCHING_ERROR_BUSY = 0x8002A719,
    SCE_NP_MATCHING_ERROR_COND_MAX = 0x8002A71D,
    SCE_NP_MATCHING_ERROR_INVALID_COND = 0x8002A71E,
    SCE_NP_MATCHING_ERROR_INVALID_ATTR = 0x8002A71F,
    SCE_NP_MATCHING_ERROR_COMP_OP_INEQUALITY_MAX = 0x8002A720,
    SCE_NP_MATCHING_ERROR_CANCELED = 0x8002A723,
    SCE_NP_MATCHING_ERROR_INVALID_COMP_OP = 0x8002A725,
    SCE_NP_MATCHING_ERROR_INVALID_COMP_TYPE = 0x8002A726,
    SCE_NP_MATCHING_ERROR_ATTR_NOT_SPECIFIED = 0x8002A72A,
    SCE_NP_MATCHING_ERROR_INVALID_ATTR_VALUE = 0x8002A72D,
    SCE_NP_MATCHING_ERROR_DUPLICATE = 0x8002A72E,
    SCE_NP_MATCHING_ERROR_UTILITY_UNAVAILABLE = 0x8002A733,
    SCE_NP_COMMERCE_ERROR_UNSUPPORTED_VERSION = 0x80029404,
    SCE_NP_COMMERCE_ERROR_INSUFFICIENT_BUFFER = 0x8002940E,
    SCE_NP_COMMERCE_ERROR_INVALID_DATA_FLAG_NUM = 0x8002941A,
    SCE_NP_COMMERCE_ERROR_PARSE_PRODUCT_CATEGORY = 0x80029421,
    SCE_NP_COMMERCE_ERROR_CURRENCY_INFO_NOT_FOUND = 0x80029422,
    SCE_NP_COMMERCE_ERROR_CATEGORY_INFO_NOT_FOUND = 0x80029423,
    SCE_NP_COMMERCE_ERROR_CHILD_CATEGORY_COUNT_NOT_FOUND = 0x80029424,
    SCE_NP_COMMERCE_ERROR_CHILD_CATEGORY_INFO_NOT_FOUND = 0x80029425,
    SCE_NP_COMMERCE_ERROR_SKU_COUNT_NOT_FOUND = 0x80029426,
    SCE_NP_COMMERCE_ERROR_SKU_INFO_NOT_FOUND = 0x80029427,
    SCE_NP_COMMERCE_ERROR_INVALID_SKU_NUM = 0x80029429,
    SCE_NP_COMMERCE_ERROR_INVALID_GPC_PROTOCOL_VERSION = 0x8002942A,
    SCE_NP_COMMERCE_BROWSE_SERVER_ERROR_UNKNOWN = 0x80029600,
    SCE_NP_PROFILE_ERROR_NOT_INITIALIZED = 0x8002AB41,
    SCE_NP_PROFILE_ERROR_INVALID_ARGUMENT = 0x8002AB48,
    SCE_NP_DRM_ERROR_INVALID_PARAM = 0x80029502,
    SCE_NP_DRM_ERROR_NO_ENTITLEMENT = 0x80029513,
    SCE_NP_DRM_ERROR_BAD_FORMAT = 0x80029515,
    SCE_NP_DRM_ERROR_INTERNAL = 0x80029517,
    SCE_NP_DRM_ERROR_TIME_LIMIT = 0x8002951B,
    SCE_NP_DRM_ERROR_SERVICE_NOT_STARTED = 0x8002951E,
    SCE_NP_DRM_ERROR_LICENSE_NOT_FOUND = 0x80029521,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

constexpr s32 SCE_NP_MANAGER_STATUS_OFFLINE = -1;
constexpr u32 SCE_NP_MIN_POOL_SIZE = 128 * 1024;
constexpr u32 SCE_NP_COOKIE_MAX_SIZE = 1024;
constexpr s32 SCE_NP_TICKET_PARAM_SERIAL_ID = 0, SCE_NP_TICKET_PARAM_SUBJECT_DOB = 11;
constexpr u32 SCE_NP_ENTITLEMENT_ID_SIZE = 32;
constexpr u32 SCE_NP_BASIC_MAX_PRESENCE_SIZE = 128;
constexpr u32 SCE_NP_BASIC_PRESENCE_OPTIONS_SET_DATA = 1, SCE_NP_BASIC_PRESENCE_OPTIONS_SET_STATUS = 2,
              SCE_NP_BASIC_PRESENCE_OPTIONS_ALL_OPTIONS = 3;
constexpr u32 SCE_NP_BASIC_MESSAGE_INFO_TYPE_BOOTABLE_CUSTOM_DATA_MESSAGE = 7;
constexpr u32 SCE_NP_BASIC_SELECTED_INVITATION_DATA = 1, SCE_NP_BASIC_SELECTED_MESSAGE_DATA = 2;
constexpr u32 SCE_NP_BASIC_PLAYERS_HISTORY_OPTIONS_ALL = 1; // BY_NP_COMM_ID is 0
// Messages (sceNpBasicSendMessage, sceNpBasicSendMessageGui, sceNpBasicAddFriend, sceNpBasicRecvMessageCustom).
constexpr u32 SCE_NP_BASIC_MAX_MESSAGE_SIZE = 512, SCE_NP_BASIC_SEND_MESSAGE_MAX_RECIPIENTS = 12;
constexpr u32 SCE_NP_BASIC_SUBJECT_CHARACTER_MAX = 18, SCE_NP_BASIC_BODY_CHARACTER_MAX = 512;
constexpr u32 SCE_NP_BASIC_MAX_MESSAGE_ATTACHMENT_SIZE = 1048576, SCE_NP_BASIC_MAX_INVITATION_DATA_SIZE = 1024,
              SCE_NP_BASIC_MAX_URL_ATTACHMENT_SIZE = 2048;
// MULTI_RECEIPIENTS 1, BOOTABLE 2, ASSUME_SEND 4; ALL_FEATURES adds SCE_NP_BASIC_MESSAGE_FEATURES_EXP_MIN(0xffff).
constexpr u32 SCE_NP_BASIC_MESSAGE_FEATURES_BOOTABLE = 2, SCE_NP_BASIC_MESSAGE_FEATURES_ASSUME_SEND = 4,
              SCE_NP_BASIC_MESSAGE_FEATURES_ALL_FEATURES = 0xFFFF0007;
constexpr u16 SCE_NP_BASIC_MESSAGE_MAIN_TYPE_DATA_ATTACHMENT = 0, SCE_NP_BASIC_MESSAGE_MAIN_TYPE_GENERAL = 1,
              SCE_NP_BASIC_MESSAGE_MAIN_TYPE_ADD_FRIEND = 2, SCE_NP_BASIC_MESSAGE_MAIN_TYPE_INVITE = 3,
              SCE_NP_BASIC_MESSAGE_MAIN_TYPE_CUSTOM_DATA = 4, SCE_NP_BASIC_MESSAGE_MAIN_TYPE_URL_ATTACHMENT = 5;
// Every main type's sub types start at 0 (ACTION_USE, NONE, ACTION_ACCEPT_DENY); invitations also have ACTION_ACCEPT.
constexpr u16 SCE_NP_BASIC_MESSAGE_INVITE_SUBTYPE_ACTION_ACCEPT = 1;
constexpr u32 SCE_NP_BASIC_RECV_MESSAGE_OPTIONS_ALL_OPTIONS = 7; // PRESERVE, INCLUDE_BOOTABLE, ASSUME_LATEST
// The history of players met (sceNpBasicAddPlayersHistoryAsync).
constexpr u32 SCE_NP_BASIC_PLAYER_HISTORY_MAX_PLAYERS = 8, SCE_NP_BASIC_DESCRIPTION_CHARACTER_MAX = 341;
constexpr s32 SCE_NP_BASIC_EVENT_ADD_PLAYERS_HISTORY_RESULT = 18;
constexpr u32 kRequestIdMisc = 0x3333; // np_handler REQUEST_ID_HIGH::MISC: the high half of such a request's ID
// NPDRM content (sceNpDrmIsAvailable), RPCS3 Crypto/unedat.h: an EDAT or SDAT file starts with NPD_HEADER (0x80 bytes:
// magic, version, license, type, content_id[0x30], ...), then EDAT_HEADER (flags first).
constexpr u32 kNpdHeaderSize = 0x80, kEdatHeaderSize = 0x10, kNpdLicense = 8, kNpdContentId = 0x10,
              kNpdContentIdSize = 0x30;
constexpr u32 SDAT_FLAG = 0x01000000;
constexpr u32 kNpdLicenseFree = 3; // the klicensee alone opens it: no RAP licence needed
constexpr u32 kDrmPathMax = 0x100; // RPCS3 reads at most this many characters of the path
constexpr u64 SCE_NP_DRM_TIME_INFO_ENDLESS = 0x7FFFFFFFFFFFFFFFull;
// NPD_HEADER's validity window (RPCS3 Crypto/unedat.h): be s64 activate_time and expire_time at 0x70 and 0x78, in
// milliseconds since 1970 (0: no limit).
constexpr u32 kNpdActivateTime = 0x70, kNpdExpireTime = 0x78;
// A SELF's NPD header (RPCS3 Crypto/unself.cpp get_npdrm_self_header): the extended header at 0x20 gives the
// supplemental headers' offset and size (u64s at 0x58 and 0x60); each is be32 type, be32 size, be64 next, then its
// data, and type 3 holds the NPD header. A debug SELF (key revision 0x8000 or 0xC000) has none.
constexpr u32 kSelfKeyRevision = 0x08, kSelfSupplementalOffset = 0x58, kSelfSupplementalSize = 0x60;
constexpr u32 kSupplementalNpdrm = 3, kSupplementalHeader = 0x10;
constexpr u32 kSelfHeadMax = 0x10000; // the SELF's first bytes read for its headers
constexpr u32 kUpgradeContentIdMax = 0x2F; // npDrmVerifyUpgradeLicense reads at most this many characters
constexpr u32 SCE_NP_LOOKUP_MAX_CTX_NUM = 32;
constexpr u32 SCE_NP_SCORE_MAX_CTX_NUM = 32, SCE_NP_SCORE_CENSOR_COMMENT_MAXLEN = 255,
              SCE_NP_SCORE_MAX_RANGE_NUM_PER_TRANS = 100, SCE_NP_SCORE_MAX_NPID_NUM_PER_TRANS = 101,
              SCE_NP_SCORE_MAX_CLAN_NUM_PER_TRANS = 101;
constexpr u64 kScoreTimeoutDefault = 60'000'000, kCommunityTimeoutMin = 10'000'000; // microseconds
// The matching library (RPCS3 sceNp.h): attributes (SceNpMatchingAttr: next, be s32 type, be32 id, then the value:
// be32 num, or a pointer and be32 size; 20 bytes), search conditions (SceNpMatchingSearchCondition: next, be s32
// target_attr_type, be32 target_attr_id, be s32 comp_op, be s32 comp_type, then the compared attribute; 40 bytes),
// request ranges (SceNpMatchingReqRange: be32 start, max).
constexpr u32 kMatchingAttrSize = 20, kMatchingCondSize = 40;
constexpr s32 SCE_NP_MATCHING_ATTR_TYPE_BASIC_BIN = 1, SCE_NP_MATCHING_ATTR_TYPE_BASIC_NUM = 2,
              SCE_NP_MATCHING_ATTR_TYPE_GAME_BIN = 3, SCE_NP_MATCHING_ATTR_TYPE_GAME_NUM = 4;
constexpr u32 SCE_NP_MATCHING_ROOM_ATTR_ID_TOTAL_SLOT = 1, SCE_NP_MATCHING_ROOM_ATTR_ID_PRIVATE_SLOT = 2,
              SCE_NP_MATCHING_ROOM_ATTR_ID_CUR_TOTAL_NUM = 3, SCE_NP_MATCHING_ROOM_ATTR_ID_CUR_PRIVATE_NUM = 5,
              SCE_NP_MATCHING_ROOM_ATTR_ID_PRIVILEGE_TYPE = 6, SCE_NP_MATCHING_ROOM_ATTR_ID_ROOM_SEARCH_FLAG = 7;
constexpr s32 SCE_NP_MATCHING_CONDITION_SEARCH_EQ = 0, SCE_NP_MATCHING_CONDITION_SEARCH_NE = 1,
              SCE_NP_MATCHING_CONDITION_SEARCH_GE = 5, SCE_NP_MATCHING_CONDITION_TYPE_VALUE = 0;
constexpr s32 SCE_NP_MATCHING_ROOM_SLOT_TYPE_PUBLIC = 0, SCE_NP_MATCHING_ROOM_SLOT_TYPE_PRIVATE = 1;
constexpr s32 SCE_NP_MATCHING_ROOM_SEARCH_FLAG_OPEN = 0, SCE_NP_MATCHING_ROOM_SEARCH_FLAG_STEALTH = 1;
constexpr s32 SCE_NP_MATCHING_INVITATION_DESTINATION_MAX = 12;
constexpr s32 SCE_NP_MATCHING_GUI_EVENT_CREATE_ROOM = 1, SCE_NP_MATCHING_GUI_EVENT_JOIN_ROOM = 2,
              SCE_NP_MATCHING_GUI_EVENT_SEARCH_JOIN = 4, SCE_NP_MATCHING_GUI_EVENT_QUICK_MATCH = 5,
              SCE_NP_MATCHING_GUI_EVENT_COMMON_LOAD = 8, SCE_NP_MATCHING_GUI_EVENT_COMMON_UNLOAD = 9,
              SCE_NP_MATCHING_GUI_EVENT_GET_ROOM_LIST = 10, SCE_NP_MATCHING_GUI_EVENT_GET_ROOM_LIST_LIMIT = 11;
constexpr u32 kMatchingListMax = 256; // pspocket's bound on a guest list walk (a cycle would hang RPCS3)
constexpr u64 kRoomInfoInterval = 30'000'000; // the limited room info calls: one per 30 s (microseconds)
// Commerce (version 1): SceNpCommerceProductCategory {be32 version; data; be32 dataSize; be32 dval; reserved[16]};
// the category, SKU and currency infos {pc; data; reserved[8]}; SceNpCommercePrice {be32 integer, fractional}.
constexpr u32 SCE_NP_COMMERCE_VERSION = 1;
constexpr u32 kPcVersion = 0, kPcData = 4, kPcDataSize = 8, kPcDval = 12;
constexpr u32 kPriceFractional = 4;
constexpr u32 SCE_NP_SIGNALING_CTX_MAX = 8;
constexpr u32 SCE_NP_SIGNALING_CONN_STATUS_INACTIVE = 0;
constexpr u32 SCE_NP_SIGNALING_NETINFO_NAT_STATUS_TYPE2 = 2;
constexpr u32 SCE_NP_SIGNALING_NETINFO_UPNP_STATUS_INVALID = 1;
constexpr u32 SCE_NP_SIGNALING_NETINFO_NPPORT_STATUS_OPEN = 1;
constexpr u16 SCE_NP_PORT = 3658;

// Guest structures (RPCS3 sceNp.h).
// SceNpId (0x24 bytes): handle (SceNpOnlineId: data[16 + 1], dummy[3]), opt[8], reserved[8].
constexpr u32 kNpIdSize = 0x24, kOnlineIdLength = 16;
// SceNpBasicPresenceDetails2 (524 bytes): struct_size, state, title[128], status[192] at 136, comment[64], data[128],
// then the data's size at 520.
constexpr u32 kPresenceDetails2Size = 524, kPresence2Status = 136, kPresence2StatusMax = 192, kPresence2DataSize = 520;
// SceNpSignalingNetInfo (28 bytes): size, local_addr, mapped_addr, nat_status, upnp_status, npport_status, then the u16
// npport at 24; SceNpSignalingNetInfoDeprecated (24 bytes) stops before npport.
constexpr u32 kNetInfoSize = 28, kNetInfoDeprecatedSize = 24;
// SceNpBasicPresenceDetails (392 bytes): title[128], status[64] at 128, comment[64], data[128], then the data's size at
// 384 and the state.
constexpr u32 kPresenceDetailsSize = 392, kPresenceStatus = 128, kPresenceStatusMax = 64, kPresenceDataSize = 384;
// SceNpManagerCacheParam (240 bytes): be32 size; SceNpOnlineId (20), SceNpId (0x24), SceNpOnlineName (52) and
// SceNpAvatarUrl (128), all empty offline.
constexpr u32 kCacheParamSize = 240;
constexpr u32 kSigninIdSize = 64;  // RPCS3: "signInId seems to be a 64 byte thing"
constexpr u32 kNpIdOpt = 20;       // SceNpId's opt[8] (RPCS3 unk1[2]: be32 each); unk1[1], the platform, at 24
constexpr u32 kNpIdPlatform = 24;
constexpr u32 SCE_NP_PLATFORM_TYPE_NONE = 0, SCE_NP_PLATFORM_TYPE_PS3 = 1, SCE_NP_PLATFORM_TYPE_VITA = 2,
              SCE_NP_PLATFORM_TYPE_PS4 = 3;
constexpr u32 CELL_SYSUTIL_USERID_CURRENT = 0, kUserId = 1; // the one user, 00000001 (RPCS3 Emu.GetUsrId())
// The custom menu (RPCS3 sceNp.h): SceNpCustomMenu {be64 options; actions; be32 numActions} (16 bytes);
// SceNpCustomMenuAction {be32 options; name; be32 mask} (12); SceNpCustomMenuIndexArray (16: four be32 words);
// SceNpCustomMenuActionExceptions {be32 options; SceNpId; SceNpCustomMenuIndexArray; reserved[4]} (60).
constexpr u32 kCustomMenuSize = 16, kCustomMenuActions = 8, kCustomMenuNumActions = 12;
constexpr u32 kCustomMenuActionSize = 12, kCustomMenuActionName = 4, kCustomMenuActionMask = 8;
constexpr u32 kCustomMenuIndexArraySize = 16, kCustomMenuExceptionSize = 60;
constexpr u32 SCE_NP_CUSTOM_MENU_ACTION_CHARACTER_MAX = 21, SCE_NP_CUSTOM_MENU_ACTION_ITEMS_TOTAL_MAX = 16,
              SCE_NP_CUSTOM_MENU_EXCEPTION_ITEMS_MAX = 256;

// The IDs of one kind of NP context, handed out as RPCS3's idm does (Emu/IdManager.cpp idm::allocate_id): Base,
// Base + 1, ... in order until each has been used once, then the lowest free one; 0, idm's invalid ID, when all are in
// use. Removing an ID or clearing the table never rewinds the order (idm keeps its highest index).
template <class T, u32 Base, u32 Count>
class IdTable {
public:
    u32 make(T value)
    {
        u32 i = next_;
        if (next_ < Count) next_++;
        else i = static_cast<u32>(std::find(slots_.begin(), slots_.end(), std::nullopt) - slots_.begin());
        if (i == Count) return 0;
        slots_[i] = std::move(value);
        return Base + i;
    }

    T* find(u32 id)
    {
        const u32 i = id - Base; // an ID below Base wraps past Count, as in idm's get_index
        return i < Count && slots_[i] ? &*slots_[i] : nullptr;
    }

    bool remove(u32 id)
    {
        if (!find(id)) return false;
        slots_[id - Base].reset();
        return true;
    }

    void clear()
    {
        for (std::optional<T>& s : slots_) s.reset();
    }

private:
    std::array<std::optional<T>, Count> slots_{};
    u32 next_ = 0; // idm's highest_index
};

// RPCS3 np_contexts.h score_ctx: a score title context's communication ID ("NPWR01234_00"), and the player character
// ID and timeout its transactions would start with.
struct ScoreCtx {
    std::string communication_id;
    s32 pc_id = 0;
    u64 timeout = kScoreTimeoutDefault;
};

// RPCS3 np_contexts.h matching_ctx: the creator's NP ID, its request handler, and the GUI call running on it (busy) with
// that call's handler.
struct MatchingCtx {
    std::array<u8, kNpIdSize> npid{};
    u32 handler = 0, arg = 0;
    bool busy = false;
    u32 gui_handler = 0, gui_arg = 0;
};

// RPCS3 np_contexts.h signaling_ctx: the creator's NP ID and its connection event handlers. Offline no connection
// exists, so the handlers are never called.
struct SignalingCtx {
    std::array<u8, kNpIdSize> npid{};
    u32 handler = 0, arg = 0;
    u32 ext_handler = 0, ext_arg = 0; // sceNpSignalingAddExtendedHandler
};

struct NpState {
    std::mutex mutex;                     // everything below but `initialized`, which changes only with it held
    std::atomic<bool> initialized{false}; // sceNpInit's state (RPCS3 is_NP_init); sceNp2Init's is in sceNp2.cpp
    u32 pool_size = 0;                     // the pool's size while NP is up (np_memory)
    u32 pool_ptr = 0;                      // and its address
    u32 client_heap = 0, client_buffer = 0; // libsysutil_np's client heap and receive buffer on it (np_client)
    bool matching2_running = false;         // sce_np_matching_set_matching2_running's flag (kept over NP's term)
    u32 manager_cb = 0, manager_cb_arg = 0;
    bool basic_handler_registered = false;
    u32 basic_handler = 0, basic_handler_arg = 0;
    // The low half of the next request ID (np_handler match2_low_reqid_cnt: from 1, every kind of NP request takes
    // one, never rewound).
    u16 next_request = 1;
    // sceNpLookupInit's state (is_NP_Lookup_init) and the lookup title contexts (idm lookup_title_ctx, IDs 0x3001 on;
    // each keeps its communication ID). RPCS3's NP term leaves both ("TODO: Other contexts"). There is no table of
    // lookup transaction contexts: offline sceNpLookupCreateTransactionCtx never makes one.
    bool lookup_initialized = false;
    IdTable<std::string, 0x3001, SCE_NP_LOOKUP_MAX_CTX_NUM> lookup_title_ctxs;
    // sceNpScoreInit's state (is_NP_Score_init) and the score title contexts (idm score_ctx, IDs 0x2001 on). Neither
    // NP's term nor the score library's clears the contexts, as in RPCS3. There is no table of score transaction
    // contexts: offline sceNpScoreCreateTransactionCtx never makes one, so every transaction ID is unknown.
    bool score_initialized = false;
    IdTable<ScoreCtx, 0x2001, SCE_NP_SCORE_MAX_CTX_NUM> score_ctxs;
    // The signaling contexts (idm signaling_ctx, IDs 0x6001 on), cleared by NP term as in RPCS3 sceNpTerm.
    IdTable<SignalingCtx, 0x6001, SCE_NP_SIGNALING_CTX_MAX> signaling_ctxs;
    // The matching library's context (idm matching_ctx: one, ID 0x9001), the one the GUI calls report on (np_handler
    // gui_notifications.current_gui_ctx_id: set by its creation, cleared by its destruction), and when each limited
    // room info call was last let through (RPCS3's function statics). Offline no request reaches a server, so no
    // result or notification is ever kept. NP term leaves them, as in RPCS3.
    IdTable<MatchingCtx, 0x9001, 1> matching_ctxs;
    u32 matching_gui_ctx = 0;
    u64 last_get_room_info = 0, last_set_room_info = 0;
    // The custom menu (np_handler custom_menu_*), for the system's friend list and profile dialogs, which never open
    // offline: kept as RPCS3 keeps it, never shown. NP term leaves it, as in RPCS3.
    bool custom_menu_registered = false;
    u32 custom_menu_handler = 0, custom_menu_arg = 0;
    std::vector<std::pair<u32, std::string>> custom_menu_actions; // each action's mask and name
    std::array<u8, kCustomMenuIndexArraySize> custom_menu_activation{};
    u32 custom_menu_exceptions = 0;
};

NpState& np()
{
    HleProcess* p = HleProcess::current();
    if (!p) PS_FATAL(kChan, "sceNp called without an HLE process");
    return p->state<NpState>();
}

bool basic_registered(NpState& s)
{
    std::lock_guard lock(s.mutex);
    return s.basic_handler_registered;
}

// The first checks of most basic functions: NP up, the basic handler registered.
s32 basic_handler_check(NpState& s)
{
    if (!s.initialized) return error(SCE_NP_BASIC_ERROR_NOT_INITIALIZED);
    if (!basic_registered(s)) return error(SCE_NP_BASIC_ERROR_NOT_REGISTERED);
    return lv2::CELL_OK;
}

bool lookup_initialized(NpState& s)
{
    std::lock_guard lock(s.mutex);
    return s.lookup_initialized;
}

// A NUL-terminated guest string of at most `max` characters, for logs (cut where guest memory ends).
std::string guest_string(lv2::PpuThread& t, u32 addr, u32 max)
{
    std::string out;
    for (u32 i = 0; i < max && guest_readable(t, addr + i, 1); i++) {
        const char c = static_cast<char>(ps_read8(t.kernel.base(), addr + i));
        if (!c) break;
        out += c;
    }
    return out;
}

// The length of the NUL-terminated guest string at `addr` if it is under `max`, else `max`; nullopt when it runs into
// memory that isn't guest memory first.
std::optional<u32> guest_strnlen(lv2::PpuThread& t, u32 addr, u32 max)
{
    for (u32 i = 0; i < max; i++) {
        if (!guest_readable(t, addr + i, 1)) return std::nullopt;
        if (!ps_read8(t.kernel.base(), addr + i)) return i;
    }
    return max;
}

// A SceNpCommunicationId (data[9 + 1], num, dummy) written as RPCS3 does ("NPWR01234_00"), if it is a valid one
// (np::validate_communication_id: nine characters of 0-9 and A-Z, num at most 99).
std::optional<std::string> communication_id(u8* base, u32 addr)
{
    std::string id;
    for (u32 i = 0; i < 9; i++) {
        const char c = static_cast<char>(ps_read8(base, addr + i));
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z'))) return std::nullopt;
        id += c;
    }
    const u8 num = ps_read8(base, addr + 10);
    if (num > 99) return std::nullopt;
    return std::format("{}_{:02}", id, num);
}

// ------------------------------------------------------------------------------------------------- manager

s32 sceNpManagerGetStatus(PPUContext& ctx, u32 status)
{
    if (!np().initialized) {
        PS_LOG_DEBUG(kChan, "sceNpManagerGetStatus: NP not initialized");
        return error(SCE_NP_ERROR_NOT_INITIALIZED);
    }
    if (!status) return error(SCE_NP_ERROR_INVALID_ARGUMENT);
    ps_write32(guest_base(ctx), status, static_cast<u32>(SCE_NP_MANAGER_STATUS_OFFLINE));
    return lv2::CELL_OK;
}

// RPCS3 sceNpManagerRegisterCallback / sceNpManagerUnregisterCallback: the callback is kept for sign-in status
// changes; RPCS3 calls it from its NP handler when the user signs in or out, which never happens offline.
s32 sceNpManagerRegisterCallback(PPUContext&, u32 callback, u32 arg)
{
    NpState& s = np();
    if (!s.initialized) return error(SCE_NP_ERROR_NOT_INITIALIZED);
    if (!callback) return error(SCE_NP_ERROR_INVALID_ARGUMENT);
    std::lock_guard lock(s.mutex);
    s.manager_cb = callback;
    s.manager_cb_arg = arg;
    PS_LOG_INFO(kChan, "sceNpManagerRegisterCallback({:#x}, arg {:#x}): kept; offline, it is never called", callback, arg);
    return lv2::CELL_OK;
}

s32 sceNpManagerUnregisterCallback(PPUContext&)
{
    NpState& s = np();
    if (!s.initialized) return error(SCE_NP_ERROR_NOT_INITIALIZED);
    std::lock_guard lock(s.mutex);
    s.manager_cb = 0; // RPCS3 leaves the argument
    return lv2::CELL_OK;
}

// RPCS3 sceNpManagerGetNpId, GetAccountRegion, GetAccountAge, GetContentRatingFlag, GetChatRestrictionFlag and
// GetNetworkTime ask about the signed-in user: NP must be initialized and every output pointer given; offline the
// answer is SCE_NP_ERROR_OFFLINE and nothing is written.
s32 manager_query_offline(std::string_view name, std::initializer_list<u32> outputs)
{
    if (!np().initialized) return error(SCE_NP_ERROR_NOT_INITIALIZED);
    for (const u32 out : outputs)
        if (!out) return error(SCE_NP_ERROR_INVALID_ARGUMENT);
    PS_LOG_DEBUG(kChan, "{}: offline", name);
    return error(SCE_NP_ERROR_OFFLINE);
}

s32 sceNpManagerGetNpId(PPUContext&, u32 np_id) { return manager_query_offline("sceNpManagerGetNpId", {np_id}); }

s32 sceNpManagerGetAccountRegion(PPUContext&, u32 country_code, u32 language)
{
    return manager_query_offline("sceNpManagerGetAccountRegion", {country_code, language});
}

s32 sceNpManagerGetAccountAge(PPUContext&, u32 age)
{
    return manager_query_offline("sceNpManagerGetAccountAge", {age});
}

s32 sceNpManagerGetContentRatingFlag(PPUContext&, u32 is_restricted, u32 age)
{
    return manager_query_offline("sceNpManagerGetContentRatingFlag", {is_restricted, age});
}

s32 sceNpManagerGetChatRestrictionFlag(PPUContext&, u32 is_restricted)
{
    return manager_query_offline("sceNpManagerGetChatRestrictionFlag", {is_restricted});
}

s32 sceNpManagerGetNetworkTime(PPUContext&, u32 tick)
{
    return manager_query_offline("sceNpManagerGetNetworkTime", {tick});
}

// RPCS3 sceNpManagerRequestTicket2: asks PSN's authentication server for a ticket; offline it is refused first.
s32 sceNpManagerRequestTicket2(PPUContext& ctx, u32 np_id, u32 version, u32 service_id, u32 cookie, u32 cookie_size,
    u32 entitlement_id, u32 consumed_count)
{
    if (!np().initialized) return error(SCE_NP_ERROR_NOT_INITIALIZED);
    if (!np_id || !service_id || cookie_size > SCE_NP_COOKIE_MAX_SIZE) return error(SCE_NP_AUTH_EINVALID_ARGUMENT);
    PS_LOG_INFO(kChan, "sceNpManagerRequestTicket2(npId {:#x}, version {:#x}, service \"{}\", cookie {:#x} ({} bytes), "
                       "entitlement {:#x}, consumed {}): offline, no ticket", np_id, version,
        guest_string(thread_of(ctx), service_id, 64), cookie, cookie_size, entitlement_id, consumed_count);
    return error(SCE_NP_ERROR_OFFLINE);
}

// RPCS3 sceNpManagerGetTicket: copies the ticket the last ticket request received. Offline there is none, so its size
// is 0: without a buffer that size is reported, with one nothing is copied and the size read is 0.
s32 sceNpManagerGetTicket(PPUContext& ctx, u32 buffer, u32 buffer_size)
{
    if (!np().initialized) return error(SCE_NP_ERROR_NOT_INITIALIZED);
    if (!buffer_size) return error(SCE_NP_ERROR_INVALID_ARGUMENT);
    PS_LOG_DEBUG(kChan, "sceNpManagerGetTicket(buffer {:#x}): offline, no ticket", buffer);
    ps_write32(guest_base(ctx), buffer_size, 0);
    return lv2::CELL_OK;
}

// RPCS3 sceNpManagerGetTicketParam: one field of that ticket; offline there is none (SCE_NP_ERROR_INVALID_STATE).
s32 sceNpManagerGetTicketParam(PPUContext&, s32 param_id, u32 param)
{
    if (!np().initialized) return error(SCE_NP_ERROR_NOT_INITIALIZED);
    if (!param || param_id < SCE_NP_TICKET_PARAM_SERIAL_ID || param_id > SCE_NP_TICKET_PARAM_SUBJECT_DOB) {
        return error(SCE_NP_ERROR_INVALID_ARGUMENT);
    }
    return error(SCE_NP_ERROR_INVALID_STATE);
}

// RPCS3 sceNpManagerGetEntitlementIdList: a TODO that reports no entitlements (the count, 0) and writes nothing.
s32 sceNpManagerGetEntitlementIdList(PPUContext&, u32 list, u32 list_num)
{
    if (!np().initialized) return error(SCE_NP_ERROR_NOT_INITIALIZED);
    PS_LOG_DEBUG(kChan, "sceNpManagerGetEntitlementIdList(list {:#x}, {} entries): none", list, list_num);
    return 0;
}

// RPCS3 sceNpManagerGetEntitlementById: a TODO that finds no entitlement.
s32 sceNpManagerGetEntitlementById(PPUContext& ctx, u32 ent_id, u32 ent)
{
    if (!np().initialized) return error(SCE_NP_ERROR_NOT_INITIALIZED);
    if (!ent_id) return error(SCE_NP_ERROR_INVALID_ARGUMENT);
    PS_LOG_INFO(kChan, "sceNpManagerGetEntitlementById(\"{}\", ent {:#x}): not found",
        guest_string(thread_of(ctx), ent_id, SCE_NP_ENTITLEMENT_ID_SIZE), ent);
    return error(SCE_NP_ERROR_ID_NOT_FOUND);
}

// RPCS3 sceNpManagerGetOnlineId, GetPsHandle (the same), GetOnlineName, GetAvatarUrl and GetMyLanguages: the signed-in
// user's; offline SCE_NP_ERROR_OFFLINE (returned as not an error), as manager_query_offline.
s32 sceNpManagerGetOnlineId(PPUContext&, u32 online_id)
{
    return manager_query_offline("sceNpManagerGetOnlineId", {online_id});
}

s32 sceNpManagerGetPsHandle(PPUContext&, u32 online_id)
{
    return manager_query_offline("sceNpManagerGetPsHandle", {online_id});
}

s32 sceNpManagerGetOnlineName(PPUContext&, u32 online_name)
{
    return manager_query_offline("sceNpManagerGetOnlineName", {online_name});
}

s32 sceNpManagerGetAvatarUrl(PPUContext&, u32 avatar_url)
{
    return manager_query_offline("sceNpManagerGetAvatarUrl", {avatar_url});
}

s32 sceNpManagerGetMyLanguages(PPUContext&, u32 languages)
{
    return manager_query_offline("sceNpManagerGetMyLanguages", {languages});
}

// RPCS3 sceNpManagerRequestTicket: as sceNpManagerRequestTicket2 (ticket version 2.1), refused offline first.
s32 sceNpManagerRequestTicket(PPUContext& ctx, u32 np_id, u32 service_id, u32 cookie, u32 cookie_size,
    u32 entitlement_id, u32 consumed_count)
{
    if (!np().initialized) return error(SCE_NP_ERROR_NOT_INITIALIZED);
    if (!np_id || !service_id || cookie_size > SCE_NP_COOKIE_MAX_SIZE) return error(SCE_NP_AUTH_EINVALID_ARGUMENT);
    PS_LOG_INFO(kChan, "sceNpManagerRequestTicket(npId {:#x}, service \"{}\", cookie {:#x} ({} bytes), entitlement "
                       "{:#x}, consumed {}): offline, no ticket", np_id, guest_string(thread_of(ctx), service_id, 64),
        cookie, cookie_size, entitlement_id, consumed_count);
    return error(SCE_NP_ERROR_OFFLINE);
}

// RPCS3 sceNpManagerGetCachedInfo: what the system keeps of the user's PSN account (SceNpManagerCacheParam: its size,
// then the online ID, NP ID, online name and avatar URL), for the current user or the user's ID (1). Offline, with
// PSN disabled, RPCS3's np_handler has them all empty: the struct is written with its size and zeros. Any other user is
// CELL_ENOENT. RPCS3 writes the struct unchecked: one that isn't guest memory is refused here with a log.
s32 sceNpManagerGetCachedInfo(PPUContext& ctx, u32 user_id, u32 param)
{
    if (!np().initialized) return error(SCE_NP_ERROR_NOT_INITIALIZED);
    if (!param) return error(SCE_NP_ERROR_INVALID_ARGUMENT);
    if (user_id != CELL_SYSUTIL_USERID_CURRENT && user_id != kUserId) return lv2::CELL_ENOENT;
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    if (!guest_readable(t, param, kCacheParamSize)) {
        log_call_site(ctx, base, std::format("sceNpManagerGetCachedInfo: the parameter {:#x} is not guest memory", param));
        return error(SCE_NP_ERROR_INVALID_ARGUMENT);
    }
    std::memset(base + param, 0, kCacheParamSize);
    ps_write32(base, param, kCacheParamSize);
    PS_LOG_DEBUG(kChan, "sceNpManagerGetCachedInfo(user {}): offline, every field empty", user_id);
    return lv2::CELL_OK;
}

// RPCS3 sceNpManagerGetSigninId: 64 zero bytes ("signInId seems to be a 64 byte thing"), signed in or not. RPCS3
// writes them unchecked: memory that isn't guest memory is refused here with a log.
s32 sceNpManagerGetSigninId(PPUContext& ctx, u32 signin_id)
{
    if (!np().initialized) return error(SCE_NP_ERROR_NOT_INITIALIZED);
    if (!signin_id) return error(SCE_NP_ERROR_INVALID_ARGUMENT);
    lv2::PpuThread& t = thread_of(ctx);
    if (!guest_readable(t, signin_id, kSigninIdSize)) {
        log_call_site(ctx, t.kernel.base(),
            std::format("sceNpManagerGetSigninId: the sign-in ID {:#x} is not guest memory", signin_id));
        return error(SCE_NP_ERROR_INVALID_ARGUMENT);
    }
    std::memset(t.kernel.base() + signin_id, 0, kSigninIdSize);
    return lv2::CELL_OK;
}

// RPCS3 sceNpManagerSubSignin, sceNpManagerSubSigninAbortGui and sceNpManagerSubSignout: TODOs that need NP and do
// nothing else (no second user signs in; the callback isn't called).
s32 sceNpManagerSubSignin(PPUContext&, u32 user_id, u32 cb_func, u32 cb_arg, s32 flag)
{
    if (!np().initialized) return error(SCE_NP_ERROR_NOT_INITIALIZED);
    PS_LOG_WARN(kChan, "sceNpManagerSubSignin(user {}, callback {:#x}, arg {:#x}, flag {}): a TODO in RPCS3; nobody "
                       "signs in and the callback isn't called", user_id, cb_func, cb_arg, flag);
    return lv2::CELL_OK;
}

s32 sceNpManagerSubSigninAbortGui(PPUContext&)
{
    if (!np().initialized) return error(SCE_NP_ERROR_NOT_INITIALIZED);
    return lv2::CELL_OK;
}

s32 sceNpManagerSubSignout(PPUContext&, u32 np_id)
{
    if (!np().initialized) return error(SCE_NP_ERROR_NOT_INITIALIZED);
    PS_LOG_DEBUG(kChan, "sceNpManagerSubSignout({:#x}): nobody signed in", np_id);
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------------- friend list

// RPCS3 sceNpFriendlist / sceNpFriendlistCustom: the system's friend list dialog (a TODO: no dialog, the handler isn't
// called). After the checks RPCS3 asks for the user's NP ID (sceNpManagerGetNpId), which offline answers
// SCE_NP_ERROR_OFFLINE, and returns that.
s32 friendlist_offline(std::string_view name, u32 handler)
{
    if (!np().initialized) return error(SCE_NP_FRIENDLIST_ERROR_NOT_INITIALIZED);
    if (!handler) return error(SCE_NP_FRIENDLIST_ERROR_INVALID_ARGUMENT); // RPCS3: "TODO: SCE_NP_FRIENDLIST_ERROR_BUSY"
    PS_LOG_INFO(kChan, "{}(handler {:#x}): offline, the user's NP ID isn't available", name, handler);
    return error(SCE_NP_ERROR_OFFLINE);
}

s32 sceNpFriendlist(PPUContext&, u32 handler, u32 user_arg, u32 container_id)
{
    PS_LOG_DEBUG(kChan, "sceNpFriendlist: arg {:#x}, container {:#x}", user_arg, container_id);
    return friendlist_offline("sceNpFriendlist", handler);
}

s32 sceNpFriendlistCustom(PPUContext&, u32 options, u32 handler, u32 user_arg, u32 container_id)
{
    PS_LOG_DEBUG(kChan, "sceNpFriendlistCustom: options {:#x}, arg {:#x}, container {:#x}", options, user_arg,
        container_id);
    return friendlist_offline("sceNpFriendlistCustom", handler);
}

s32 sceNpFriendlistAbortGui(PPUContext&)
{
    if (!np().initialized) return error(SCE_NP_FRIENDLIST_ERROR_NOT_INITIALIZED);
    return lv2::CELL_OK; // RPCS3: "TODO: abort friendlist GUI interaction"; none is open
}

// ------------------------------------------------------------------------------------------------- custom menu

// RPCS3 sceNpCustomMenuRegisterActions: the game's actions for the custom menu of the friend list and profile
// dialogs (at most 16, each with a name of at most 20 characters), the handler for the action picked, and a fresh
// activation and exception list. The options aren't checked (RPCS3: "TODO: Is there any error if options is not 0 ?").
// RPCS3 reads the menu, the actions and their names unchecked: ones that aren't guest memory are refused here with a
// log.
s32 sceNpCustomMenuRegisterActions(PPUContext& ctx, u32 menu, u32 handler, u32 user_arg, u64 options)
{
    NpState& s = np();
    if (!s.initialized) return error(SCE_NP_CUSTOM_MENU_ERROR_NOT_INITIALIZED);
    if (!menu || !handler) return error(SCE_NP_CUSTOM_MENU_ERROR_INVALID_ARGUMENT);
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    const auto refuse = [&](std::string what) {
        log_call_site(ctx, base, std::format("sceNpCustomMenuRegisterActions: {} is not guest memory", what));
        return error(SCE_NP_CUSTOM_MENU_ERROR_INVALID_ARGUMENT);
    };
    if (!guest_readable(t, menu, kCustomMenuSize)) return refuse(std::format("the menu {:#x}", menu));
    const u32 actions = ps_read32(base, menu + kCustomMenuActions);
    const u32 count = ps_read32(base, menu + kCustomMenuNumActions);
    if (count > SCE_NP_CUSTOM_MENU_ACTION_ITEMS_TOTAL_MAX) return error(SCE_NP_CUSTOM_MENU_ERROR_EXCEEDS_MAX);
    std::vector<std::pair<u32, std::string>> kept;
    for (u32 i = 0; i < count; i++) {
        if (!actions) return error(SCE_NP_CUSTOM_MENU_ERROR_INVALID_ARGUMENT);
        const u32 action = actions + i * kCustomMenuActionSize;
        if (!guest_readable(t, action, kCustomMenuActionSize)) return refuse(std::format("action {} ({:#x})", i, action));
        const u32 name = ps_read32(base, action + kCustomMenuActionName);
        if (!name) return error(SCE_NP_CUSTOM_MENU_ERROR_INVALID_ARGUMENT);
        const std::optional<u32> length = guest_strnlen(t, name, SCE_NP_CUSTOM_MENU_ACTION_CHARACTER_MAX);
        if (!length) return refuse(std::format("action {}'s name ({:#x})", i, name));
        if (*length == SCE_NP_CUSTOM_MENU_ACTION_CHARACTER_MAX) return error(SCE_NP_CUSTOM_MENU_ERROR_EXCEEDS_MAX);
        kept.emplace_back(ps_read32(base, action + kCustomMenuActionMask),
            std::string(reinterpret_cast<const char*>(base + name), *length));
    }
    std::string names;
    for (const auto& [mask, name] : kept) names += std::format("{}\"{}\" (mask {:#x})", names.empty() ? "" : ", ", name, mask);
    std::lock_guard lock(s.mutex);
    s.custom_menu_handler = handler;
    s.custom_menu_arg = user_arg;
    s.custom_menu_actions = std::move(kept);
    s.custom_menu_registered = true;
    s.custom_menu_activation = {};
    s.custom_menu_exceptions = 0;
    PS_LOG_INFO(kChan, "sceNpCustomMenuRegisterActions(handler {:#x}, arg {:#x}, options {:#x}): [{}]; kept, though no "
                       "dialog shows them offline", handler, user_arg, options, names);
    return lv2::CELL_OK;
}

// RPCS3 sceNpCustomMenuActionSetActivation: which actions are active (a SceNpCustomMenuIndexArray), once a menu is
// registered. RPCS3 reads the array unchecked: one that isn't guest memory is refused here with a log.
s32 sceNpCustomMenuActionSetActivation(PPUContext& ctx, u32 array, u64 options)
{
    NpState& s = np();
    if (!s.initialized) return error(SCE_NP_CUSTOM_MENU_ERROR_NOT_INITIALIZED);
    std::lock_guard lock(s.mutex);
    if (!s.custom_menu_registered) return error(SCE_NP_CUSTOM_MENU_ERROR_NOT_REGISTERED);
    if (!array) return error(SCE_NP_CUSTOM_MENU_ERROR_INVALID_ARGUMENT);
    lv2::PpuThread& t = thread_of(ctx);
    if (!guest_readable(t, array, kCustomMenuIndexArraySize)) {
        log_call_site(ctx, t.kernel.base(),
            std::format("sceNpCustomMenuActionSetActivation: the array {:#x} is not guest memory", array));
        return error(SCE_NP_CUSTOM_MENU_ERROR_INVALID_ARGUMENT);
    }
    std::memcpy(s.custom_menu_activation.data(), t.kernel.base() + array, kCustomMenuIndexArraySize);
    PS_LOG_DEBUG(kChan, "sceNpCustomMenuActionSetActivation(options {:#x})", options);
    return lv2::CELL_OK;
}

// RPCS3 sceNpCustomMenuRegisterExceptionList: the users for whom some actions are off (at most 256
// SceNpCustomMenuActionExceptions), replacing the list. Kept here as a count: no dialog shows the menu.
s32 sceNpCustomMenuRegisterExceptionList(PPUContext& ctx, u32 items, u32 count, u64 options)
{
    NpState& s = np();
    if (!s.initialized) return error(SCE_NP_CUSTOM_MENU_ERROR_NOT_INITIALIZED);
    std::lock_guard lock(s.mutex);
    if (!s.custom_menu_registered) return error(SCE_NP_CUSTOM_MENU_ERROR_NOT_REGISTERED);
    if (count > SCE_NP_CUSTOM_MENU_EXCEPTION_ITEMS_MAX) return error(SCE_NP_CUSTOM_MENU_ERROR_EXCEEDS_MAX);
    if (!items) return error(SCE_NP_CUSTOM_MENU_ERROR_INVALID_ARGUMENT);
    lv2::PpuThread& t = thread_of(ctx);
    if (count && !guest_readable(t, items, count * kCustomMenuExceptionSize)) {
        log_call_site(ctx, t.kernel.base(), std::format("sceNpCustomMenuRegisterExceptionList: the {} exceptions at "
                                                        "{:#x} are not guest memory", count, items));
        return error(SCE_NP_CUSTOM_MENU_ERROR_INVALID_ARGUMENT);
    }
    s.custom_menu_exceptions = count;
    PS_LOG_DEBUG(kChan, "sceNpCustomMenuRegisterExceptionList({} exceptions, options {:#x})", count, options);
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------------- basic

// RPCS3 sceNpBasicRegisterHandler, sceNpBasicRegisterContextSensitiveHandler (np_handler::register_basic_handler,
// context sensitive or not) and sceNpBasicUnregisterHandler: one basic event handler, kept for presence and messages
// from other users; offline none ever come, so it is never called. The communication ID RPCS3 keeps with it is used
// only online (the players' history and presence).
s32 register_basic_handler(std::string_view name, u32 context, u32 handler, u32 arg)
{
    NpState& s = np();
    if (!s.initialized) return error(SCE_NP_BASIC_ERROR_NOT_INITIALIZED);
    std::lock_guard lock(s.mutex);
    if (s.basic_handler_registered) return error(SCE_NP_BASIC_ERROR_EXCEEDS_MAX);
    if (!context || !handler) return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
    s.basic_handler_registered = true;
    s.basic_handler = handler;
    s.basic_handler_arg = arg;
    PS_LOG_INFO(kChan, "{}(context {:#x}, handler {:#x}, arg {:#x}): kept; offline, it is never called", name, context,
        handler, arg);
    return lv2::CELL_OK;
}

s32 sceNpBasicRegisterHandler(PPUContext&, u32 context, u32 handler, u32 arg)
{
    return register_basic_handler("sceNpBasicRegisterHandler", context, handler, arg);
}

s32 sceNpBasicRegisterContextSensitiveHandler(PPUContext&, u32 context, u32 handler, u32 arg)
{
    return register_basic_handler("sceNpBasicRegisterContextSensitiveHandler", context, handler, arg);
}

s32 sceNpBasicUnregisterHandler(PPUContext&)
{
    NpState& s = np();
    if (!s.initialized) return error(SCE_NP_BASIC_ERROR_NOT_INITIALIZED);
    std::lock_guard lock(s.mutex);
    if (!s.basic_handler_registered) return error(SCE_NP_BASIC_ERROR_NOT_REGISTERED);
    s.basic_handler_registered = false;
    return lv2::CELL_OK;
}

// RPCS3 sceNpBasicGetEvent: the next queued basic event. Offline nothing is ever queued (presence and messages come
// from RPCN), so there is none.
s32 sceNpBasicGetEvent(PPUContext&, u32 event, u32 from, u32 data, u32 size)
{
    NpState& s = np();
    if (!s.initialized) return error(SCE_NP_BASIC_ERROR_NOT_INITIALIZED);
    if (!basic_registered(s)) return error(SCE_NP_BASIC_ERROR_NOT_REGISTERED);
    if (!event || !from || !data || !size) return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
    return error(SCE_NP_BASIC_ERROR_NO_EVENT);
}

// RPCS3 sceNpBasicSetPresence: the game's presence data, which RPCS3 keeps and sends to other users once online;
// offline it goes nowhere. RPCS3 ensure()s the data is readable ("Not checked by API"): refused here with a log.
s32 sceNpBasicSetPresence(PPUContext& ctx, u32 data, u32 size)
{
    NpState& s = np();
    if (!s.initialized) return error(SCE_NP_BASIC_ERROR_NOT_INITIALIZED);
    if (!basic_registered(s)) return error(SCE_NP_BASIC_ERROR_NOT_REGISTERED);
    if (size > SCE_NP_BASIC_MAX_PRESENCE_SIZE) return error(SCE_NP_BASIC_ERROR_EXCEEDS_MAX);
    if (!guest_readable(thread_of(ctx), data, size)) {
        log_call_site(ctx, guest_base(ctx),
            std::format("sceNpBasicSetPresence: the data ({:#x}, {} bytes) is not guest memory", data, size));
        return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
    }
    PS_LOG_DEBUG(kChan, "sceNpBasicSetPresence({:#x}, {} bytes): offline, not sent", data, size);
    return lv2::CELL_OK;
}

// RPCS3 sceNpBasicSetPresenceDetails2: presence status and data (SceNpBasicPresenceDetails2), kept and sent like
// sceNpBasicSetPresence's; offline they go nowhere. RPCS3 reads the struct unchecked: one that isn't guest memory is
// refused here with a log.
s32 sceNpBasicSetPresenceDetails2(PPUContext& ctx, u32 pres, u32 options)
{
    NpState& s = np();
    if (!s.initialized) return error(SCE_NP_BASIC_ERROR_NOT_INITIALIZED);
    if (!basic_registered(s)) return error(SCE_NP_BASIC_ERROR_NOT_REGISTERED);
    if (options < SCE_NP_BASIC_PRESENCE_OPTIONS_SET_DATA || options > SCE_NP_BASIC_PRESENCE_OPTIONS_ALL_OPTIONS) {
        return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
    }
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    if (!guest_readable(t, pres, kPresenceDetails2Size)) {
        log_call_site(ctx, base,
            std::format("sceNpBasicSetPresenceDetails2: the details {:#x} are not guest memory", pres));
        return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
    }
    const u32 struct_size = ps_read32(base, pres);
    if (struct_size != kPresenceDetails2Size) return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
    const u32 size = ps_read32(base, pres + kPresence2DataSize);
    if (size > SCE_NP_BASIC_MAX_PRESENCE_SIZE) return error(SCE_NP_BASIC_ERROR_EXCEEDS_MAX);
    PS_LOG_DEBUG(kChan, "sceNpBasicSetPresenceDetails2(options {}): status \"{}\", {} bytes of data; offline, not sent",
        options, (options & SCE_NP_BASIC_PRESENCE_OPTIONS_SET_STATUS) ? guest_string(t, pres + kPresence2Status,
                     kPresence2StatusMax) : "(unchanged)", size);
    return lv2::CELL_OK;
}

// RPCS3 sceNpBasicGetBlockListEntryCount / sceNpBasicGetBlockListEntry: the block list is the PSN account's; offline
// RPCS3 answers SCE_NP_ERROR_ID_NOT_FOUND ("TODO: Find the correct test") and writes nothing.
s32 sceNpBasicGetBlockListEntryCount(PPUContext&, u32 count)
{
    if (!np().initialized) return error(SCE_NP_BASIC_ERROR_NOT_INITIALIZED);
    if (!count) return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
    return error(SCE_NP_ERROR_ID_NOT_FOUND);
}

s32 sceNpBasicGetBlockListEntry(PPUContext&, u32 index, u32 np_id)
{
    if (!np().initialized) return error(SCE_NP_BASIC_ERROR_NOT_INITIALIZED);
    if (!np_id) return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT); // RPCS3: "TODO: check index"
    PS_LOG_DEBUG(kChan, "sceNpBasicGetBlockListEntry({}): offline, no block list", index);
    return error(SCE_NP_ERROR_ID_NOT_FOUND);
}

// RPCS3 sceNpBasicGetMessageEntryCount: the type is checked before anything else ("TODO: verify this check and its
// location"); offline, SCE_NP_ERROR_ID_NOT_FOUND as for the block list.
s32 sceNpBasicGetMessageEntryCount(PPUContext&, u32 type, u32 count)
{
    if (type > SCE_NP_BASIC_MESSAGE_INFO_TYPE_BOOTABLE_CUSTOM_DATA_MESSAGE) {
        return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
    }
    if (!np().initialized) return error(SCE_NP_BASIC_ERROR_NOT_INITIALIZED);
    if (!count) return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
    return error(SCE_NP_ERROR_ID_NOT_FOUND);
}

// RPCS3 sceNpBasicRecvMessageAttachmentLoad: the attachment of the invitation or message the user picked in the
// system's message screens. Offline nothing can be picked (np_handler::get_message_selected finds none), so the data
// ID is invalid there too.
s32 sceNpBasicRecvMessageAttachmentLoad(PPUContext&, u32 id, u32 buffer, u32 size)
{
    NpState& s = np();
    if (!s.initialized) return error(SCE_NP_BASIC_ERROR_NOT_INITIALIZED);
    if (!basic_registered(s)) return error(SCE_NP_BASIC_ERROR_NOT_REGISTERED);
    if (!buffer || !size) return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
    if (id != SCE_NP_BASIC_SELECTED_INVITATION_DATA && id != SCE_NP_BASIC_SELECTED_MESSAGE_DATA) {
        return error(SCE_NP_BASIC_ERROR_INVALID_DATA_ID);
    }
    PS_LOG_DEBUG(kChan, "sceNpBasicRecvMessageAttachmentLoad({}): offline, nothing selected", id);
    return error(SCE_NP_BASIC_ERROR_INVALID_DATA_ID);
}

// RPCS3 sceNpBasicSetPresenceDetails: as sceNpBasicSetPresenceDetails2, for the older SceNpBasicPresenceDetails (no
// struct size); offline nothing is sent. RPCS3 ensure()s the struct is readable ("Not checked by API"): refused here
// with a log.
s32 sceNpBasicSetPresenceDetails(PPUContext& ctx, u32 pres, u32 options)
{
    if (const s32 r = basic_handler_check(np())) return r;
    if (options < SCE_NP_BASIC_PRESENCE_OPTIONS_SET_DATA || options > SCE_NP_BASIC_PRESENCE_OPTIONS_ALL_OPTIONS) {
        return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
    }
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    if (!guest_readable(t, pres, kPresenceDetailsSize)) {
        log_call_site(ctx, base, std::format("sceNpBasicSetPresenceDetails: the details {:#x} are not guest memory", pres));
        return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
    }
    const u32 size = ps_read32(base, pres + kPresenceDataSize);
    if (size > SCE_NP_BASIC_MAX_PRESENCE_SIZE) return error(SCE_NP_BASIC_ERROR_EXCEEDS_MAX);
    PS_LOG_DEBUG(kChan, "sceNpBasicSetPresenceDetails(options {}): status \"{}\", {} bytes of data; offline, not sent",
        options, (options & SCE_NP_BASIC_PRESENCE_OPTIONS_SET_STATUS) ? guest_string(t, pres + kPresenceStatus,
                     kPresenceStatusMax) : "(unchanged)", size);
    return lv2::CELL_OK;
}

// RPCS3 sceNpBasicAbortGui: ends the basic dialog on screen (np_state's abort flag). None is ever open here.
s32 sceNpBasicAbortGui(PPUContext&)
{
    if (const s32 r = basic_handler_check(np())) return r;
    PS_LOG_DEBUG(kChan, "sceNpBasicAbortGui(): no dialog to abort");
    return lv2::CELL_OK;
}

// RPCS3 sceNpBasicMarkMessageAsUsed: a TODO that only checks NP and the handler (no message ID is checked).
s32 sceNpBasicMarkMessageAsUsed(PPUContext&, u64 msg_id)
{
    if (const s32 r = basic_handler_check(np())) return r;
    PS_LOG_DEBUG(kChan, "sceNpBasicMarkMessageAsUsed({:#x}): no messages offline", msg_id);
    return lv2::CELL_OK;
}

// What RPCS3 answers for the lists that come from PSN once their checks pass (NP, the basic handler when `need_handler`,
// the output pointer): offline, SCE_NP_ERROR_ID_NOT_FOUND (RPCS3: "TODO: Find the correct test which returns
// SCE_NP_ERROR_ID_NOT_FOUND"), and nothing written. For the entries and counts of the clan messages, custom and
// matching invitations, message attachments, messages, friends and the players' history.
s32 basic_list_offline(std::string_view name, bool need_handler, u32 out)
{
    NpState& s = np();
    if (!s.initialized) return error(SCE_NP_BASIC_ERROR_NOT_INITIALIZED);
    if (need_handler && !basic_registered(s)) return error(SCE_NP_BASIC_ERROR_NOT_REGISTERED);
    if (!out) return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT); // RPCS3: "TODO: check index"
    PS_LOG_DEBUG(kChan, "{}: offline, not found", name);
    return error(SCE_NP_ERROR_ID_NOT_FOUND);
}

s32 sceNpBasicGetClanMessageEntryCount(PPUContext&, u32 count)
{
    return basic_list_offline("sceNpBasicGetClanMessageEntryCount", true, count);
}

s32 sceNpBasicGetClanMessageEntry(PPUContext&, u32 index, u32 from)
{
    return basic_list_offline(std::format("sceNpBasicGetClanMessageEntry({})", index), true, from);
}

s32 sceNpBasicGetCustomInvitationEntryCount(PPUContext&, u32 count)
{
    return basic_list_offline("sceNpBasicGetCustomInvitationEntryCount", true, count);
}

s32 sceNpBasicGetCustomInvitationEntry(PPUContext&, u32 index, u32 from)
{
    return basic_list_offline(std::format("sceNpBasicGetCustomInvitationEntry({})", index), true, from);
}

s32 sceNpBasicGetMatchingInvitationEntryCount(PPUContext&, u32 count)
{
    return basic_list_offline("sceNpBasicGetMatchingInvitationEntryCount", true, count);
}

s32 sceNpBasicGetMatchingInvitationEntry(PPUContext&, u32 index, u32 from)
{
    return basic_list_offline(std::format("sceNpBasicGetMatchingInvitationEntry({})", index), true, from);
}

s32 sceNpBasicGetMessageAttachmentEntryCount(PPUContext&, u32 count)
{
    return basic_list_offline("sceNpBasicGetMessageAttachmentEntryCount", true, count);
}

s32 sceNpBasicGetMessageAttachmentEntry(PPUContext&, u32 index, u32 from)
{
    return basic_list_offline(std::format("sceNpBasicGetMessageAttachmentEntry({})", index), true, from);
}

// The type is checked first, as in sceNpBasicGetMessageEntryCount; the handler isn't asked for.
s32 sceNpBasicGetMessageEntry(PPUContext&, u32 type, u32 index, u32 from)
{
    if (type > SCE_NP_BASIC_MESSAGE_INFO_TYPE_BOOTABLE_CUSTOM_DATA_MESSAGE) {
        return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
    }
    return basic_list_offline(std::format("sceNpBasicGetMessageEntry(type {}, {})", type, index), false, from);
}

// The friend list is the PSN account's (RPCS3 asks RPCN). The count needs no handler.
s32 sceNpBasicGetFriendListEntryCount(PPUContext&, u32 count)
{
    return basic_list_offline("sceNpBasicGetFriendListEntryCount", false, count);
}

// Unlike the count, an entry is refused offline with SCE_NP_BASIC_ERROR_BUSY (RPCS3).
s32 sceNpBasicGetFriendListEntry(PPUContext&, u32 index, u32 np_id)
{
    if (!np().initialized) return error(SCE_NP_BASIC_ERROR_NOT_INITIALIZED);
    if (!np_id) return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT); // RPCS3: "TODO: check index"
    PS_LOG_DEBUG(kChan, "sceNpBasicGetFriendListEntry({}): offline, busy", index);
    return error(SCE_NP_BASIC_ERROR_BUSY);
}

// RPCS3 sceNpBasicGetFriendPresenceByIndex(2) and ByNpId(2) (np_handler::get_friend_presence_by_index / by_npid): a
// friend's presence, which comes from RPCN; without it, SCE_NP_BASIC_ERROR_NOT_CONNECTED. The details pointer is
// checked before NP; the options aren't (RPCS3: "TODO ... depending on fw").
s32 friend_presence_offline(std::string_view name, u32 who, u32 pres)
{
    if (!pres) return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
    if (!np().initialized) return error(SCE_NP_BASIC_ERROR_NOT_INITIALIZED);
    if (!who) return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
    PS_LOG_DEBUG(kChan, "{}: offline, not connected", name);
    return error(SCE_NP_BASIC_ERROR_NOT_CONNECTED);
}

s32 sceNpBasicGetFriendPresenceByIndex(PPUContext&, u32 index, u32 user, u32 pres, u32 options)
{
    return friend_presence_offline(std::format("sceNpBasicGetFriendPresenceByIndex({}, options {:#x})", index, options),
        user, pres);
}

s32 sceNpBasicGetFriendPresenceByIndex2(PPUContext&, u32 index, u32 user, u32 pres, u32 options)
{
    return friend_presence_offline(std::format("sceNpBasicGetFriendPresenceByIndex2({}, options {:#x})", index, options),
        user, pres);
}

s32 sceNpBasicGetFriendPresenceByNpId(PPUContext&, u32 np_id, u32 pres, u32 options)
{
    return friend_presence_offline(std::format("sceNpBasicGetFriendPresenceByNpId(options {:#x})", options), np_id, pres);
}

s32 sceNpBasicGetFriendPresenceByNpId2(PPUContext&, u32 np_id, u32 pres, u32 options)
{
    return friend_presence_offline(std::format("sceNpBasicGetFriendPresenceByNpId2(options {:#x})", options), np_id,
        pres);
}

// Whether the SceNpId at `np_id` (non-null) has an online ID, as RPCS3 tests it (handle.data[0]); nullopt when it isn't
// guest memory (logged: RPCS3 would fault).
std::optional<bool> has_online_id(PPUContext& ctx, std::string_view name, u32 np_id)
{
    lv2::PpuThread& t = thread_of(ctx);
    if (!guest_readable(t, np_id, 1)) {
        log_call_site(ctx, t.kernel.base(), std::format("{}: the NP ID {:#x} is not guest memory", name, np_id));
        return std::nullopt;
    }
    return ps_read8(t.kernel.base(), np_id) != 0;
}

// RPCS3 sceNpBasicAddBlockListEntry: the block list is the PSN account's; offline, after the checks,
// SCE_NP_BASIC_ERROR_NOT_CONNECTED (returned as not an error).
s32 sceNpBasicAddBlockListEntry(PPUContext& ctx, u32 np_id)
{
    if (const s32 r = basic_handler_check(np())) return r;
    if (!np_id) return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
    const std::optional<bool> named = has_online_id(ctx, "sceNpBasicAddBlockListEntry", np_id);
    if (!named || !*named) return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
    PS_LOG_INFO(kChan, "sceNpBasicAddBlockListEntry(\"{}\"): offline, not connected",
        guest_string(thread_of(ctx), np_id, kOnlineIdLength));
    return error(SCE_NP_BASIC_ERROR_NOT_CONNECTED);
}

// RPCS3 sceNpBasicGetPlayersHistoryEntryCount / sceNpBasicGetPlayersHistoryEntry: the players met, which RPCS3 lists
// only online; offline SCE_NP_ERROR_ID_NOT_FOUND after the checks (the options first: BY_NP_COMM_ID or ALL).
s32 sceNpBasicGetPlayersHistoryEntryCount(PPUContext&, u32 options, u32 count)
{
    if (options > SCE_NP_BASIC_PLAYERS_HISTORY_OPTIONS_ALL) return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
    return basic_list_offline(std::format("sceNpBasicGetPlayersHistoryEntryCount(options {})", options), true, count);
}

s32 sceNpBasicGetPlayersHistoryEntry(PPUContext&, u32 options, u32 index, u32 np_id)
{
    if (options > SCE_NP_BASIC_PLAYERS_HISTORY_OPTIONS_ALL) return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
    return basic_list_offline(std::format("sceNpBasicGetPlayersHistoryEntry(options {}, {})", options, index), true,
        np_id);
}

// ------------------------------------------------------------------------------------------------- basic: messages

// RPCS3 sceNpBasicSendMessage (_sceNpBasicSendMessage without the rate limit): a message to one user, which RPCS3 sends
// through RPCN. Offline it is refused once its arguments pass, with SCE_NP_BASIC_ERROR_NOT_CONNECTED (RPCS3 returns it
// as not an error); the data isn't read. The recipient's NP ID is read unchecked by RPCS3: one that isn't guest memory
// is refused here with a log.
s32 sceNpBasicSendMessage(PPUContext& ctx, u32 to, u32 data, u32 size)
{
    if (const s32 r = basic_handler_check(np())) return r;
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    if (to && !guest_readable(t, to, 1)) {
        log_call_site(ctx, base, std::format("sceNpBasicSendMessage: the recipient {:#x} is not guest memory", to));
        return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
    }
    if (!to || !ps_read8(base, to) || !data || !size) return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
    if (size > SCE_NP_BASIC_MAX_MESSAGE_SIZE) return error(SCE_NP_BASIC_ERROR_EXCEEDS_MAX);
    PS_LOG_INFO(kChan, "sceNpBasicSendMessage(to \"{}\", {} bytes at {:#x}): offline, not sent",
        guest_string(t, to, kOnlineIdLength), size, data);
    return error(SCE_NP_BASIC_ERROR_NOT_CONNECTED);
}

// SceNpBasicMessageDetails (40 bytes): msgId (u64), mainType and subType (u16), msgFeatures, then npids (count
// SceNpIds), count, subject and body (strings), data and its size.
struct MessageDetails {
    u64 msg_id = 0;
    u16 main_type = 0, sub_type = 0;
    u32 features = 0, npids = 0, count = 0, subject = 0, body = 0, data = 0, size = 0;
};
constexpr u32 kMessageDetailsSize = 40;

MessageDetails read_message_details(u8* base, u32 addr)
{
    MessageDetails m;
    m.msg_id = ps_read64(base, addr);
    m.main_type = ps_read16(base, addr + 8);
    m.sub_type = ps_read16(base, addr + 10);
    m.features = ps_read32(base, addr + 12);
    m.npids = ps_read32(base, addr + 16);
    m.count = ps_read32(base, addr + 20);
    m.subject = ps_read32(base, addr + 24);
    m.body = ps_read32(base, addr + 28);
    m.data = ps_read32(base, addr + 32);
    m.size = ps_read32(base, addr + 36);
    return m;
}

// RPCS3 check_text (libnp's FUN_000245e0 as RPCS3 decompiled it), for a message's subject and body: UTF-8 whose
// characters RPCS3 checks by their lead byte (C2..DF, E0..EF and F0..F7 with one, two and three continuation bytes).
// The result is the number of characters, or SCE_NP_UTIL_ERROR_INVALID_CHARACTER; nullopt when the text runs into
// memory that isn't guest memory (RPCS3 faults). As RPCS3's conditions, a byte is read only while the ones before it
// continue the character, so nothing past the terminator is read.
std::optional<s32> check_text(lv2::PpuThread& t, u32 text)
{
    u8* base = t.kernel.base();
    // Whether the byte at `addr` continues a character (10xxxxxx); nullopt if it isn't guest memory.
    const auto continues = [&](u32 addr) -> std::optional<bool> {
        if (!guest_readable(t, addr, 1)) return std::nullopt;
        return (ps_read8(base, addr) & 0xC0) == 0x80;
    };
    s32 count = 0;
    for (u32 p = text;; count++) {
        if (!guest_readable(t, p, 1)) return std::nullopt;
        const u8 c = ps_read8(base, p);
        if (!c) return count;
        if (c < 0x80) {
            p++;
            continue;
        }
        if ((c & 0xE0) == 0xC0 && c > 0xC1) {
            const std::optional<bool> c1 = continues(p + 1);
            if (!c1) return std::nullopt;
            if (*c1) {
                p += 2;
                continue;
            }
        }
        if ((c & 0xF0) == 0xE0) {
            const std::optional<bool> c1 = continues(p + 1);
            if (!c1) return std::nullopt;
            const std::optional<bool> c2 = *c1 ? continues(p + 2) : std::optional<bool>(false);
            if (!c2) return std::nullopt;
            if (*c2) {
                p += 3;
                continue;
            }
        }
        if ((c & 0xF8) != 0xF0) return error(SCE_NP_UTIL_ERROR_INVALID_CHARACTER);
        for (u32 i = 1; i <= 3; i++) {
            const std::optional<bool> ci = continues(p + i);
            if (!ci) return std::nullopt;
            if (!*ci) return error(SCE_NP_UTIL_ERROR_INVALID_CHARACTER);
        }
        p += 4;
    }
}

// RPCS3 sceNpBasicSendMessageGui after its first checks (NP up, the basic handler registered, a message given): the
// message's checks in RPCS3's order. Online RPCS3 then opens the send dialog and reports its result to the basic
// handler; offline it stops before the dialog with SCE_NP_BASIC_ERROR_NOT_CONNECTED (returned as not an error), and
// no event is sent. RPCS3 reads the recipients and the texts unchecked: ones that aren't guest memory are refused
// here with a log.
s32 send_message_offline(PPUContext& ctx, std::string_view name, const MessageDetails& m, u32 container_id)
{
    if (m.count > SCE_NP_BASIC_SEND_MESSAGE_MAX_RECIPIENTS || (m.features & ~SCE_NP_BASIC_MESSAGE_FEATURES_ALL_FEATURES) ||
        m.main_type > SCE_NP_BASIC_MESSAGE_MAIN_TYPE_URL_ATTACHMENT || m.msg_id != 0) {
        return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
    }
    if (!(m.features & SCE_NP_BASIC_MESSAGE_FEATURES_BOOTABLE)) {
        switch (m.main_type) {
        case SCE_NP_BASIC_MESSAGE_MAIN_TYPE_DATA_ATTACHMENT: // sub type ACTION_USE only
        case SCE_NP_BASIC_MESSAGE_MAIN_TYPE_CUSTOM_DATA:
        case SCE_NP_BASIC_MESSAGE_MAIN_TYPE_URL_ATTACHMENT:
            if (m.sub_type != 0) return error(SCE_NP_BASIC_ERROR_NOT_SUPPORTED);
            break;
        case SCE_NP_BASIC_MESSAGE_MAIN_TYPE_GENERAL: // sub type NONE only, and no data
        case SCE_NP_BASIC_MESSAGE_MAIN_TYPE_ADD_FRIEND:
            if (m.sub_type != 0) return error(SCE_NP_BASIC_ERROR_NOT_SUPPORTED);
            if (m.data) return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
            break;
        case SCE_NP_BASIC_MESSAGE_MAIN_TYPE_INVITE:
            if (m.sub_type > SCE_NP_BASIC_MESSAGE_INVITE_SUBTYPE_ACTION_ACCEPT) {
                return error(SCE_NP_BASIC_ERROR_NOT_SUPPORTED);
            }
            break;
        default: return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
        }
    } else if (m.main_type == SCE_NP_BASIC_MESSAGE_MAIN_TYPE_INVITE) { // bootable: an accepted invitation
        if (m.sub_type != SCE_NP_BASIC_MESSAGE_INVITE_SUBTYPE_ACTION_ACCEPT) {
            return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
        }
    } else if (m.main_type == SCE_NP_BASIC_MESSAGE_MAIN_TYPE_CUSTOM_DATA) { // or custom data to use
        if (m.sub_type != 0) return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
    } else {
        return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
    }
    if (!m.data && m.main_type != SCE_NP_BASIC_MESSAGE_MAIN_TYPE_ADD_FRIEND) {
        return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
    }
    if (!m.size) return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT); // sceNpBasicAddFriend's too: its size is 0
    if (m.main_type == SCE_NP_BASIC_MESSAGE_MAIN_TYPE_ADD_FRIEND && m.count != 1) {
        return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
    }
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    if (m.npids) {
        if (!guest_readable(t, m.npids, m.count * kNpIdSize)) {
            log_call_site(ctx, base, std::format("{}: the recipients ({} at {:#x}) are not guest memory", name,
                m.count, m.npids));
            return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
        }
        for (u32 i = 0; i < m.count; i++) {
            if (!ps_read8(base, m.npids + i * kNpIdSize)) return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
        }
    }
    // A text's length in characters, or the error that ends the call.
    const auto text_length = [&](u32 text, u32 max, std::string_view what) -> std::pair<u32, s32> {
        const std::optional<s32> n = check_text(t, text);
        if (!n) {
            log_call_site(ctx, base, std::format("{}: the {} at {:#x} runs out of guest memory", name, what, text));
            return {0, error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT)};
        }
        if (*n < 0) return {0, *n};
        if (static_cast<u32>(*n) > max) return {0, error(SCE_NP_BASIC_ERROR_EXCEEDS_MAX)};
        return {static_cast<u32>(*n), lv2::CELL_OK};
    };
    u32 subject_length = 0, body_length = 0;
    if (m.subject) {
        if (m.main_type == SCE_NP_BASIC_MESSAGE_MAIN_TYPE_ADD_FRIEND) return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
        const auto [n, r] = text_length(m.subject, SCE_NP_BASIC_SUBJECT_CHARACTER_MAX, "subject");
        if (r != lv2::CELL_OK) return r;
        subject_length = n;
    }
    if (m.body) {
        const auto [n, r] = text_length(m.body, SCE_NP_BASIC_BODY_CHARACTER_MAX, "body");
        if (r != lv2::CELL_OK) return r;
        body_length = n;
    }
    switch (m.main_type) {
    case SCE_NP_BASIC_MESSAGE_MAIN_TYPE_DATA_ATTACHMENT:
    case SCE_NP_BASIC_MESSAGE_MAIN_TYPE_CUSTOM_DATA:
        if (m.size > SCE_NP_BASIC_MAX_MESSAGE_ATTACHMENT_SIZE) return error(SCE_NP_BASIC_ERROR_EXCEEDS_MAX);
        break;
    case SCE_NP_BASIC_MESSAGE_MAIN_TYPE_INVITE:
        if (m.size > SCE_NP_BASIC_MAX_INVITATION_DATA_SIZE) return error(SCE_NP_BASIC_ERROR_EXCEEDS_MAX);
        break;
    case SCE_NP_BASIC_MESSAGE_MAIN_TYPE_URL_ATTACHMENT:
        if (m.size > SCE_NP_BASIC_MAX_URL_ATTACHMENT_SIZE) return error(SCE_NP_BASIC_ERROR_EXCEEDS_MAX);
        break;
    default: break;
    }
    if (m.features & SCE_NP_BASIC_MESSAGE_FEATURES_ASSUME_SEND) {
        if (m.main_type > SCE_NP_BASIC_MESSAGE_MAIN_TYPE_CUSTOM_DATA || !m.count || !body_length ||
            (m.main_type != SCE_NP_BASIC_MESSAGE_MAIN_TYPE_ADD_FRIEND && !subject_length)) {
            return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
        }
    }
    PS_LOG_INFO(kChan, "{}(main type {}, sub type {}, features {:#x}, {} recipients, subject \"{}\", {} bytes of data, "
                       "container {:#x}): offline, no dialog", name, m.main_type, m.sub_type, m.features, m.count,
        m.subject ? guest_string(t, m.subject, 4 * SCE_NP_BASIC_SUBJECT_CHARACTER_MAX) : std::string(), m.size,
        container_id);
    return error(SCE_NP_BASIC_ERROR_NOT_CONNECTED);
}

s32 sceNpBasicSendMessageGui(PPUContext& ctx, u32 msg, u32 container_id)
{
    if (const s32 r = basic_handler_check(np())) return r;
    if (!msg) return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
    lv2::PpuThread& t = thread_of(ctx);
    if (!guest_readable(t, msg, kMessageDetailsSize)) {
        log_call_site(ctx, t.kernel.base(),
            std::format("sceNpBasicSendMessageGui: the message {:#x} is not guest memory", msg));
        return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
    }
    return send_message_offline(ctx, "sceNpBasicSendMessageGui", read_message_details(t.kernel.base(), msg),
        container_id);
}

// RPCS3 sceNpBasicAddFriend: a friend request (main type ADD_FRIEND to the one contact, with the body and no data)
// through sceNpBasicSendMessageGui. Its checks want a data size, which a friend request doesn't have, so in RPCS3 it
// ends there with SCE_NP_BASIC_ERROR_INVALID_ARGUMENT, before the offline check.
s32 sceNpBasicAddFriend(PPUContext& ctx, u32 contact, u32 body, u32 container_id)
{
    if (const s32 r = basic_handler_check(np())) return r;
    MessageDetails m;
    m.main_type = SCE_NP_BASIC_MESSAGE_MAIN_TYPE_ADD_FRIEND;
    m.npids = contact;
    m.count = 1;
    m.body = body;
    return send_message_offline(ctx, "sceNpBasicAddFriend", m, container_id);
}

// RPCS3 sceNpBasicRecvMessageCustom (recv_message_gui): opens the system's list of invitations or custom data
// messages, waits for the user to pick one or cancel, and hands the pick to the basic handler. Offline the list is
// empty (overlay_recvmessage_dialog: only "cancel" is offered), so the result is SCE_NP_BASIC_ERROR_CANCEL (returned
// as not an error) and no event. No dialog is shown here: that cancel is the answer at once.
s32 sceNpBasicRecvMessageCustom(PPUContext&, u16 main_type, u32 recv_options, u32 container_id)
{
    if (const s32 r = basic_handler_check(np())) return r;
    if (main_type != SCE_NP_BASIC_MESSAGE_MAIN_TYPE_INVITE && main_type != SCE_NP_BASIC_MESSAGE_MAIN_TYPE_CUSTOM_DATA) {
        return error(SCE_NP_BASIC_ERROR_NOT_SUPPORTED);
    }
    if (recv_options & ~SCE_NP_BASIC_RECV_MESSAGE_OPTIONS_ALL_OPTIONS) return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
    PS_LOG_INFO(kChan, "sceNpBasicRecvMessageCustom(main type {}, options {:#x}, container {:#x}): offline there is no "
                       "message to list; cancelled", main_type, recv_options, container_id);
    return error(SCE_NP_BASIC_ERROR_CANCEL);
}

// RPCS3 sceNpBasicRecvMessageAttachment: recv_message_gui for data attachments, without options. As for
// sceNpBasicRecvMessageCustom, the list is empty offline: cancelled at once, no event.
s32 sceNpBasicRecvMessageAttachment(PPUContext&, u32 container_id)
{
    if (const s32 r = basic_handler_check(np())) return r;
    PS_LOG_INFO(kChan, "sceNpBasicRecvMessageAttachment(container {:#x}): offline there is no message to list; "
                       "cancelled", container_id);
    return error(SCE_NP_BASIC_ERROR_CANCEL);
}

// RPCS3 sceNpBasicSendMessageAttachment: a data attachment (main type DATA_ATTACHMENT, sub type ACTION_USE, no
// features) to one user, through sceNpBasicSendMessageGui: offline its checks, then SCE_NP_BASIC_ERROR_NOT_CONNECTED.
s32 sceNpBasicSendMessageAttachment(PPUContext& ctx, u32 to, u32 subject, u32 body, u32 data, u32 size,
    u32 container_id)
{
    if (const s32 r = basic_handler_check(np())) return r;
    MessageDetails m;
    m.main_type = SCE_NP_BASIC_MESSAGE_MAIN_TYPE_DATA_ATTACHMENT;
    m.npids = to;
    m.count = 1;
    m.subject = subject;
    m.body = body;
    m.data = data;
    m.size = size;
    return send_message_offline(ctx, "sceNpBasicSendMessageAttachment", m, container_id);
}

// ------------------------------------------------------------------------------------------------- basic: players

// RPCS3 sceNpBasicAddPlayersHistory (np_handler::add_player_to_history): one player for the history of players met,
// without a request or an event; it needs no basic handler. As for sceNpBasicAddPlayersHistoryAsync, the player isn't
// kept: the history is read only online. RPCS3 reads the NP ID and the description unchecked: memory that isn't guest
// memory is refused here, with a log.
s32 sceNpBasicAddPlayersHistory(PPUContext& ctx, u32 np_id, u32 description)
{
    if (!np().initialized) return error(SCE_NP_BASIC_ERROR_NOT_INITIALIZED);
    if (!np_id) return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
    const std::optional<bool> named = has_online_id(ctx, "sceNpBasicAddPlayersHistory", np_id);
    if (!named || !*named) return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
    lv2::PpuThread& t = thread_of(ctx);
    if (description) {
        const std::optional<u32> length = guest_strnlen(t, description, SCE_NP_BASIC_DESCRIPTION_CHARACTER_MAX + 1);
        if (!length) {
            log_call_site(ctx, t.kernel.base(), std::format("sceNpBasicAddPlayersHistory: the description at {:#x} runs "
                                                            "out of guest memory", description));
            return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
        }
        if (*length > SCE_NP_BASIC_DESCRIPTION_CHARACTER_MAX) return error(SCE_NP_BASIC_ERROR_EXCEEDS_MAX);
    }
    PS_LOG_INFO(kChan, "sceNpBasicAddPlayersHistory(\"{}\", description \"{}\"): not kept (read only online)",
        guest_string(t, np_id, kOnlineIdLength),
        description ? guest_string(t, description, SCE_NP_BASIC_DESCRIPTION_CHARACTER_MAX) : std::string());
    return lv2::CELL_OK;
}

// RPCS3 sceNpBasicAddPlayersHistoryAsync (np_handler::add_players_to_history), which works offline too: the players
// go into the console's history of players met, and the result goes to the basic handler, if one is registered, as
// SCE_NP_BASIC_EVENT_ADD_PLAYERS_HISTORY_RESULT with result 0 and the request ID this returns (np_handler::
// send_basic_event: at the game's next cellSysutilCheckCallback). The request ID is REQUEST_ID_HIGH::MISC << 16 | the
// next request number. The players aren't kept: RPCS3 writes them to its players_history.yml, which only
// sceNpBasicGetPlayersHistoryEntryCount and sceNpBasicGetPlayersHistoryEntry read, and those answer
// SCE_NP_ERROR_ID_NOT_FOUND offline. RPCS3
// reads the NP IDs and the description unchecked, and writes the request ID after the event is sent: memory that
// isn't guest memory is refused here first, with a log.
s32 sceNpBasicAddPlayersHistoryAsync(PPUContext& ctx, u32 npids, u32 count, u32 description, u32 req_id)
{
    NpState& s = np();
    if (!s.initialized) return error(SCE_NP_BASIC_ERROR_NOT_INITIALIZED);
    if (!count) return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
    if (count > SCE_NP_BASIC_PLAYER_HISTORY_MAX_PLAYERS) return error(SCE_NP_BASIC_ERROR_EXCEEDS_MAX);
    if (!npids) return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    if (!guest_readable(t, npids, count * kNpIdSize)) {
        log_call_site(ctx, base, std::format("sceNpBasicAddPlayersHistoryAsync: the NP IDs ({} at {:#x}) are not guest "
                                             "memory", count, npids));
        return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
    }
    for (u32 i = 0; i < count; i++) {
        if (!ps_read8(base, npids + i * kNpIdSize)) return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
    }
    if (description) {
        const std::optional<u32> length = guest_strnlen(t, description, SCE_NP_BASIC_DESCRIPTION_CHARACTER_MAX + 1);
        if (!length) {
            log_call_site(ctx, base, std::format("sceNpBasicAddPlayersHistoryAsync: the description at {:#x} runs out of "
                                                 "guest memory", description));
            return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
        }
        if (*length > SCE_NP_BASIC_DESCRIPTION_CHARACTER_MAX) return error(SCE_NP_BASIC_ERROR_EXCEEDS_MAX);
    }
    if (req_id && !guest_readable(t, req_id, 4)) {
        log_call_site(ctx, base, std::format("sceNpBasicAddPlayersHistoryAsync: the request ID's address {:#x} is not "
                                             "guest memory", req_id));
        return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
    }
    std::string players;
    for (u32 i = 0; i < count; i++) {
        players += std::format("{}\"{}\"", i ? ", " : "", guest_string(t, npids + i * kNpIdSize, kOnlineIdLength));
    }
    u32 request = 0, handler = 0, handler_arg = 0;
    bool send_event = false;
    {
        std::lock_guard lock(s.mutex);
        request = (kRequestIdMisc << 16) | s.next_request++;
        send_event = s.basic_handler_registered;
        handler = s.basic_handler;
        handler_arg = s.basic_handler_arg;
    }
    if (send_event) {
        sysutil_queue_call([=](PPUContext& cb_ctx) -> s32 {
            // SceNpBasicEventHandler(event, retCode, reqId, arg); RPCS3 ignores its result.
            call_guest(cb_ctx, handler, {SCE_NP_BASIC_EVENT_ADD_PLAYERS_HISTORY_RESULT, 0, request, handler_arg});
            return lv2::CELL_OK;
        });
    }
    if (req_id) ps_write32(base, req_id, request);
    PS_LOG_INFO(kChan, "sceNpBasicAddPlayersHistoryAsync({}, description \"{}\"): request {:#x}, {}", players,
        description ? guest_string(t, description, SCE_NP_BASIC_DESCRIPTION_CHARACTER_MAX) : std::string(), request,
        send_event ? "its result goes to the basic handler" : "no basic handler for its result");
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------------- profile

// RPCS3 sceNpProfileCallGui (a TODO): would show a user's profile and report to the handler when it closes. After its
// checks it asks for the user's NP ID (sceNpManagerGetNpId on a stack variable), which offline answers
// SCE_NP_ERROR_OFFLINE, and returns that. The handler isn't called; the NP ID isn't checked.
s32 sceNpProfileCallGui(PPUContext&, u32 np_id, u32 handler, u32 user_arg, u64 options)
{
    if (!np().initialized) return error(SCE_NP_PROFILE_ERROR_NOT_INITIALIZED);
    if (!handler) return error(SCE_NP_PROFILE_ERROR_INVALID_ARGUMENT);
    PS_LOG_WARN(kChan, "sceNpProfileCallGui(npId {:#x}, handler {:#x}, arg {:#x}, options {:#x}): a TODO in RPCS3; "
                       "offline the user's NP ID isn't available", np_id, handler, user_arg, options);
    return error(SCE_NP_ERROR_OFFLINE);
}

// RPCS3 sceNpProfileAbortGui: no profile dialog is ever open. Its NP check answers the friend list's error ("Not
// SCE_NP_PROFILE_ERROR_NOT_INITIALIZED !").
s32 sceNpProfileAbortGui(PPUContext&)
{
    if (!np().initialized) return error(SCE_NP_FRIENDLIST_ERROR_NOT_INITIALIZED);
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------------- DRM

// A guest file read through lv2's file system (a host directory or the disc image, as mounted): whether it opened,
// sys_fs_open's result or else the reads' (CELL_OK, or the error that ended them), and up to the bytes asked for from
// the file's start.
struct GuestFile {
    bool opened = false;
    s32 result = lv2::CELL_OK;
    std::vector<u8> bytes;
};

// Opens `vpath` read-only and reads up to `limit` bytes, with the syscalls' arguments on the caller's guest stack (as
// sceNpTrophy reads its packs).
GuestFile read_guest_file(PPUContext& ctx, std::string_view vpath, u64 limit)
{
    constexpr u32 kChunk = 0x1000;
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    const u32 path_space = (static_cast<u32>(vpath.size()) + 16) & ~15u; // zeroed: the path ends there
    const StackVar var(ctx, base, path_space + 16 + kChunk);
    const u32 path = var.addr(), fd = path + path_space, nread = fd + 8, buf = nread + 8;
    std::memcpy(base + path, vpath.data(), vpath.size());
    GuestFile out;
    out.result = lv2::sys_fs_open(t, path, 0 /* CELL_FS_O_RDONLY */, fd, 0, 0, 0);
    if (out.result != lv2::CELL_OK) return out;
    out.opened = true;
    const u32 file = ps_read32(base, fd);
    while (out.bytes.size() < limit) {
        const u64 want = std::min<u64>(kChunk, limit - out.bytes.size());
        out.result = lv2::sys_fs_read(t, file, buf, want, nread);
        const u64 n = ps_read64(base, nread);
        if (out.result != lv2::CELL_OK || !n) break;
        out.bytes.insert(out.bytes.end(), base + buf, base + buf + n);
    }
    lv2::sys_fs_close(t, file);
    return out;
}

// The game's licence keys (D-581): where sceNpDrmIsAvailable installs the key of the content it found usable.
void drm_install_key(const recomp::Key128& key)
{
    if (NpdrmKeyRing* ring = npdrm_key_ring()) ring->install(key);
}

// The name of the file at a guest path (RPCS3 extract_file_name: after the last '/' or '\'), for an EDAT's title hash.
std::string_view drm_file_name(std::string_view path)
{
    const usize slash = path.find_last_of("/\\");
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

// RPCS3 npDrmIsAvailable, from the file's first bytes on: whether the content at `path` can be used with the
// klicensee given (none: zeros, or for a SELF NP_KLIC_FREE, as RPCS3), and if so the key that opens it goes to the
// game's licence keys, where lv2's file system finds it when the game opens the content (D-581). A file that is
// neither a SELF ("SCE\0") nor an EDAT/SDAT ("NPD\0") is taken as unencrypted (disc files): available.
// - A SELF is available when it decrypts (recomp::decrypt_self, as the loader does, where RPCS3 opens its metadata
//   with its key vault); a PSN executable with the klicensee or the player's RAP (D-582). Its klicensee is installed.
// - An SDAT is refused as RPCS3's EDAT check refuses it (NO_ENTITLEMENT). An EDAT must pass RPCS3's header checks
//   (VerifyEDATHeaderWithKLicense: the title hash over its content ID and the name in the path, and for free content
//   the dev hash keyed with the klicensee; recomp::npd_hashes_valid), else NO_ENTITLEMENT. Free content (licence 3)
//   then installs the klicensee; any other needs the player's licence, /dev_hdd0/home/00000001/exdata/<content
//   ID>.rap (LICENSE_NOT_FOUND without it or with an invalid content ID, BAD_FORMAT when it is under 16 bytes), and
//   installs its RIF key (RPCS3 GetEdatRifKeyFromRapFile).
// - A build without the NPDRM keys cannot check an EDAT's hashes or make its key: the licence is still looked for (and
//   its absence answered as above), and content that would need the keys is NO_ENTITLEMENT, logged.
// - RPCS3 searches every user's exdata for a RAP licence; pspocket has the one user, 00000001.
s32 drm_check_content(PPUContext& ctx, const std::string& path, const GuestFile& head,
    const std::optional<recomp::Key128>& klicensee)
{
    const std::vector<u8>& b = head.bytes;
    // The first four bytes are `magic` and its terminator ("SCE\0", "NPD\0").
    const auto magic_is = [&](const char* magic) { return b.size() >= 4 && std::memcmp(b.data(), magic, 4) == 0; };
    if (head.result != lv2::CELL_OK) {
        PS_LOG_WARN(kChan, "sceNpDrmIsAvailable(\"{}\"): reading failed after {} bytes ({}); checked as read", path,
            b.size(), lv2::error_name(head.result));
    }
    const recomp::NpdrmKeys& keys = recomp::builtin_npdrm_keys();
    if (magic_is("SCE")) {
        constexpr u64 kSelfMax = 256ull << 20; // far above any PS3 executable; a bigger file isn't read whole
        const GuestFile whole = read_guest_file(ctx, path, kSelfMax + 1);
        if (whole.result != lv2::CELL_OK || whole.bytes.size() > kSelfMax) {
            PS_LOG_ERROR(kChan, "sceNpDrmIsAvailable(\"{}\"): a SELF that can't be read ({}, {} bytes): no entitlement",
                path, lv2::error_name(whole.result), whole.bytes.size());
            return error(SCE_NP_DRM_ERROR_NO_ENTITLEMENT);
        }
        // RPCS3 verify_npdrm_self_headers: a PSN executable's licence is the game's klicensee (else the default one)
        // or the player's RAP for its content ID (D-582).
        const recomp::Key128 klic = klicensee.value_or(keys.klic_free);
        const recomp::NpdrmLicence licence = process_licence(thread_of(ctx).kernel, klic);
        const recomp::Result<std::vector<u8>> elf =
            recomp::decrypt_self(whole.bytes, recomp::builtin_self_keys(), &licence);
        if (!elf) {
            PS_LOG_ERROR(kChan, "sceNpDrmIsAvailable(\"{}\"): a SELF that doesn't open ({}): no entitlement", path,
                elf.error().message);
            return error(SCE_NP_DRM_ERROR_NO_ENTITLEMENT);
        }
        // RPCS3 installs the klicensee it checked with.
        drm_install_key(klic);
        PS_LOG_INFO(kChan, "sceNpDrmIsAvailable(\"{}\"): a SELF that opens: available", path);
        return lv2::CELL_OK;
    }
    if (!magic_is("NPD")) {
        PS_LOG_INFO(kChan, "sceNpDrmIsAvailable(\"{}\"): not encrypted ({} bytes read): available", path, b.size());
        return lv2::CELL_OK;
    }
    // RPCS3 read_npd_edat_header: the headers, zeros past the end of a short file.
    std::array<u8, kNpdHeaderSize + kEdatHeaderSize> h{};
    std::memcpy(h.data(), b.data(), std::min(b.size(), h.size()));
    const u32 license = ps_read32(h.data(), kNpdLicense);
    const u32 flags = ps_read32(h.data(), kNpdHeaderSize);
    const std::string_view id_field(reinterpret_cast<const char*>(h.data() + kNpdContentId), kNpdContentIdSize);
    const std::string content_id(id_field.substr(0, id_field.find('\0')));
    if ((flags & SDAT_FLAG) == SDAT_FLAG) { // VerifyEDATHeaderWithKLicense: "SDATA file given to edat function"
        PS_LOG_INFO(kChan, "sceNpDrmIsAvailable(\"{}\"): an SDAT ({}), which RPCS3's EDAT check refuses: no entitlement",
            path, content_id);
        return error(SCE_NP_DRM_ERROR_NO_ENTITLEMENT);
    }
    const recomp::Key128 klic = klicensee.value_or(recomp::Key128{});
    if (!keys.present) {
        PS_LOG_ERROR(kChan, "sceNpDrmIsAvailable(\"{}\"): an EDAT ({}), and this build has no NPDRM keys to check its "
                            "header", path, content_id);
    } else if (!recomp::npd_hashes_valid(h, drm_file_name(path), klic, keys)) {
        PS_LOG_ERROR(kChan, "sceNpDrmIsAvailable(\"{}\"): an EDAT ({}, licence type {}) that fails RPCS3's header checks "
                            "(its title hash for the name \"{}\"{}): no entitlement", path, content_id, license,
            drm_file_name(path), (license & 3) == 3 ? ", or its dev hash for the klicensee" : "");
        return error(SCE_NP_DRM_ERROR_NO_ENTITLEMENT);
    }
    if (license == kNpdLicenseFree) {
        if (!keys.present) {
            PS_LOG_ERROR(kChan, "sceNpDrmIsAvailable(\"{}\"): a free EDAT ({}) pspocket can't verify: no entitlement",
                path, content_id);
            return error(SCE_NP_DRM_ERROR_NO_ENTITLEMENT);
        }
        drm_install_key(klic);
        PS_LOG_INFO(kChan, "sceNpDrmIsAvailable(\"{}\"): a free EDAT ({}): available with the klicensee", path,
            content_id);
        return lv2::CELL_OK;
    }
    // A licence is needed: the user's RAP file for the content (none for a content ID RPCS3 finds invalid).
    const std::string rap = recomp::rap_guest_path(content_id);
    if (rap.empty()) {
        PS_LOG_INFO(kChan, "sceNpDrmIsAvailable(\"{}\"): an EDAT (licence type {}) whose content ID \"{}\" isn't valid, "
                           "so no licence: licence not found", path, license, content_id);
        return error(SCE_NP_DRM_ERROR_LICENSE_NOT_FOUND);
    }
    const GuestFile key = read_guest_file(ctx, rap, 16);
    if (!key.opened) {
        PS_LOG_INFO(kChan, "sceNpDrmIsAvailable(\"{}\"): an EDAT ({}, licence type {}) without its licence {}: licence "
                           "not found", path, content_id, license, rap);
        return error(SCE_NP_DRM_ERROR_LICENSE_NOT_FOUND);
    }
    if (key.bytes.size() < 16) {
        PS_LOG_ERROR(kChan, "sceNpDrmIsAvailable(\"{}\"): the licence {} is too small ({} bytes): bad format", path, rap,
            key.bytes.size());
        return error(SCE_NP_DRM_ERROR_BAD_FORMAT);
    }
    if (!keys.present) {
        PS_LOG_ERROR(kChan, "sceNpDrmIsAvailable(\"{}\"): an EDAT ({}) with its licence {}, and this build has no NPDRM "
                            "keys to turn it into the content key: no entitlement", path, content_id, rap);
        return error(SCE_NP_DRM_ERROR_NO_ENTITLEMENT);
    }
    recomp::Key128 rap_bytes{};
    std::copy(key.bytes.begin(), key.bytes.begin() + 16, rap_bytes.begin());
    drm_install_key(recomp::rap_to_rif(rap_bytes, keys));
    PS_LOG_INFO(kChan, "sceNpDrmIsAvailable(\"{}\"): an EDAT ({}, licence type {}) with the player's licence {}: "
                       "available", path, content_id, license, rap);
    return lv2::CELL_OK;
}

// The path (at most `max` characters) of the C string at `addr`, which `name` reads; nullopt when it runs out of guest
// memory first (logged: RPCS3 reads it unchecked and aborts, "Secret access violation").
std::optional<std::string> drm_string(PPUContext& ctx, std::string_view name, std::string_view what, u32 addr, u32 max)
{
    lv2::PpuThread& t = thread_of(ctx);
    const std::optional<u32> length = guest_strnlen(t, addr, max);
    if (!length) {
        log_call_site(ctx, t.kernel.base(), std::format("{}: the {} at {:#x} runs out of guest memory", name, what, addr));
        return std::nullopt;
    }
    return std::string(reinterpret_cast<const char*>(t.kernel.base() + addr), *length);
}

// RPCS3 npDrmIsAvailable: whether the NPDRM content at drm_path can be used, given the game's klicensee (16 bytes,
// optional). NP need not be up. The file is opened as RPCS3's lv2_file::open does (an error there is the result:
// CELL_ENOENT for a missing file) and checked by its first bytes (drm_check_content). The klicensee is a key: it is
// never logged. RPCS3 reads the klicensee and the path (at most 0x100 characters) unchecked: ones that aren't guest
// memory are refused here with a log. RPCS3's key for PSP remasters (category "PE") doesn't apply: pspocket runs PS3
// games.
s32 drm_is_available(PPUContext& ctx, std::string_view name, u32 k_licensee, u32 drm_path)
{
    lv2::PpuThread& t = thread_of(ctx);
    if (k_licensee && !guest_readable(t, k_licensee, 16)) {
        log_call_site(ctx, t.kernel.base(), std::format("{}: the klicensee {:#x} is not guest memory", name, k_licensee));
        return error(SCE_NP_DRM_ERROR_INVALID_PARAM);
    }
    const std::optional<std::string> path = drm_string(ctx, name, "path", drm_path, kDrmPathMax);
    if (!path) return error(SCE_NP_DRM_ERROR_INVALID_PARAM);
    PS_LOG_INFO(kChan, "{}(\"{}\", {}): checking", name, *path, k_licensee ? "with a klicensee" : "no klicensee");
    std::optional<recomp::Key128> klicensee;
    if (k_licensee) {
        klicensee.emplace();
        std::memcpy(klicensee->data(), t.kernel.base() + k_licensee, klicensee->size());
    }
    const GuestFile head = read_guest_file(ctx, *path, kNpdHeaderSize + kEdatHeaderSize);
    if (!head.opened) {
        PS_LOG_INFO(kChan, "{}(\"{}\"): can't be opened: {}", name, *path, lv2::error_name(head.result));
        return head.result;
    }
    return drm_check_content(ctx, *path, head, klicensee);
}

// RPCS3 sceNpDrmIsAvailable: the check, then the thread sleeps 5 ms.
s32 sceNpDrmIsAvailable(PPUContext& ctx, u32 k_licensee, u32 drm_path)
{
    if (!drm_path) return error(SCE_NP_DRM_ERROR_INVALID_PARAM);
    const s32 result = drm_is_available(ctx, "sceNpDrmIsAvailable", k_licensee, drm_path);
    lv2::sys_timer_usleep(thread_of(ctx), 5'000);
    return result;
}

// sceNpDrmIsAvailable2: RPCS3 makes the same check without the first one's wait (its sleep is a TODO there): Sniper:
// Ghost Warrior calls it at boot (phone run of 2026-10-06).
s32 sceNpDrmIsAvailable2(PPUContext& ctx, u32 k_licensee, u32 drm_path)
{
    if (!drm_path) return error(SCE_NP_DRM_ERROR_INVALID_PARAM);
    return drm_is_available(ctx, "sceNpDrmIsAvailable2", k_licensee, drm_path);
}

// RPCS3 sceNpDrmProcessExitSpawn / sceNpDrmProcessExitSpawn2: the content check (npDrmIsAvailable, no wait), then
// sys_game_process_exitspawn (or ...2) with the same arguments, which ends the game (starting the next executable is
// not implemented, D-031) and doesn't return. A failed check is the result.
s32 sceNpDrmProcessExitSpawn(PPUContext& ctx, u32 k_licensee, u32 path, u32 argv, u32 envp, u32 data, u32 data_size,
    s32 prio, u64 flags)
{
    if (!path) return error(SCE_NP_DRM_ERROR_INVALID_PARAM);
    if (const s32 r = drm_is_available(ctx, "sceNpDrmProcessExitSpawn", k_licensee, path)) return r;
    game_process_exitspawn(ctx, path, argv, envp, data, data_size, prio, flags);
    return lv2::CELL_OK;
}

s32 sceNpDrmProcessExitSpawn2(PPUContext& ctx, u32 k_licensee, u32 path, u32 argv, u32 envp, u32 data, u32 data_size,
    s32 prio, u64 flags)
{
    if (!path) return error(SCE_NP_DRM_ERROR_INVALID_PARAM);
    if (const s32 r = drm_is_available(ctx, "sceNpDrmProcessExitSpawn2", k_licensee, path)) return r;
    game_process_exitspawn2(ctx, path, argv, envp, data, data_size, prio, flags);
    return lv2::CELL_OK;
}

// The NPD header of a SELF's NPDRM supplemental header (RPCS3 get_npdrm_self_header), from the file's first bytes;
// nullopt for a debug SELF, headers past those bytes, or no NPDRM header.
std::optional<std::vector<u8>> self_npd_header(const std::vector<u8>& b)
{
    if (b.size() < kSelfSupplementalSize + 8) return std::nullopt;
    const u16 key_revision = ps_ld16(b.data() + kSelfKeyRevision);
    if (key_revision == 0x8000 || key_revision == 0xC000) return std::nullopt;
    const u64 start = ps_ld64(b.data() + kSelfSupplementalOffset), size = ps_ld64(b.data() + kSelfSupplementalSize);
    for (u64 at = start; at - start < size;) {
        if (at > b.size() || b.size() - at < kSupplementalHeader) return std::nullopt;
        const u32 type = ps_ld32(b.data() + at), length = ps_ld32(b.data() + at + 4);
        if (type == kSupplementalNpdrm) {
            if (b.size() - at < kSupplementalHeader + kNpdHeaderSize) return std::nullopt;
            const auto npd = b.begin() + static_cast<std::ptrdiff_t>(at + kSupplementalHeader);
            return std::vector<u8>(npd, npd + kNpdHeaderSize);
        }
        if (!length) return std::nullopt; // RPCS3 would read the same header forever
        at += length;
    }
    return std::nullopt;
}

// RPCS3 sceNpDrmGetTimelimit: how long the content at `path` may still be used, from its NPD header's validity window
// (an EDAT's, or a SELF's NPDRM supplemental header's) and the time now: SCE_NP_DRM_ERROR_SERVICE_NOT_STARTED before
// it starts, SCE_NP_DRM_TIME_INFO_ENDLESS without an end, SCE_NP_DRM_ERROR_TIME_LIMIT after it ends, else the
// microseconds left. Any other file is SCE_NP_DRM_ERROR_BAD_FORMAT; a file that can't be opened, the open's error.
// RPCS3 reads the path (at most 0x100 characters) unchecked: one that isn't guest memory is refused here with a log, as
// is a result pointer that isn't.
s32 sceNpDrmGetTimelimit(PPUContext& ctx, u32 drm_path, u32 time_remain)
{
    if (!drm_path || !time_remain) return error(SCE_NP_DRM_ERROR_INVALID_PARAM);
    s64 sec = 0, nsec = 0;
    lv2::host_current_time(sec, nsec);
    const std::optional<std::string> path = drm_string(ctx, "sceNpDrmGetTimelimit", "path", drm_path, kDrmPathMax);
    if (!path) return error(SCE_NP_DRM_ERROR_INVALID_PARAM);
    lv2::PpuThread& t = thread_of(ctx);
    if (!guest_readable(t, time_remain, 8)) {
        log_call_site(ctx, t.kernel.base(),
            std::format("sceNpDrmGetTimelimit: the result's address {:#x} is not guest memory", time_remain));
        return error(SCE_NP_DRM_ERROR_INVALID_PARAM);
    }
    const GuestFile head = read_guest_file(ctx, *path, kSelfHeadMax);
    if (!head.opened) {
        PS_LOG_INFO(kChan, "sceNpDrmGetTimelimit(\"{}\"): can't be opened: {}", *path, lv2::error_name(head.result));
        return head.result;
    }
    const std::vector<u8>& b = head.bytes;
    std::vector<u8> npd(kNpdHeaderSize, 0);
    if (b.size() >= 4 && std::memcmp(b.data(), "SCE", 4) == 0) {
        std::optional<std::vector<u8>> found = self_npd_header(b);
        if (!found) {
            PS_LOG_ERROR(kChan, "sceNpDrmGetTimelimit(\"{}\"): no NPD header in the SELF: bad format", *path);
            return error(SCE_NP_DRM_ERROR_BAD_FORMAT);
        }
        npd = std::move(*found);
    } else if (b.size() >= 4 && std::memcmp(b.data(), "NPD", 4) == 0) {
        std::memcpy(npd.data(), b.data(), std::min<usize>(b.size(), kNpdHeaderSize)); // zeros past a short file's end
    } else {
        PS_LOG_INFO(kChan, "sceNpDrmGetTimelimit(\"{}\"): neither a SELF nor an EDAT: bad format", *path);
        return error(SCE_NP_DRM_ERROR_BAD_FORMAT);
    }
    const s64 now_ms = sec * 1000 + nsec / 1'000'000;
    const s64 activate = static_cast<s64>(ps_ld64(npd.data() + kNpdActivateTime));
    const s64 expire = static_cast<s64>(ps_ld64(npd.data() + kNpdExpireTime));
    if (activate != 0 && now_ms < activate) return error(SCE_NP_DRM_ERROR_SERVICE_NOT_STARTED);
    u64 remain = SCE_NP_DRM_TIME_INFO_ENDLESS;
    if (expire != 0) {
        if (now_ms >= expire) return error(SCE_NP_DRM_ERROR_TIME_LIMIT);
        remain = static_cast<u64>(expire - now_ms) * 1000;
    }
    ps_write64(t.kernel.base(), time_remain, remain);
    PS_LOG_INFO(kChan, "sceNpDrmGetTimelimit(\"{}\"): {}", *path,
        expire ? std::format("{} us left", remain) : std::string("no time limit"));
    return lv2::CELL_OK;
}

// RPCS3 sceNpDrmVerifyUpgradeLicense / ...2 (npDrmVerifyUpgradeLicense, rpcs3::utils::verify_c00_unlock_edat):
// whether the user has the unlock licence of a trial game's full version, the EDAT
// /dev_hdd0/home/00000001/exdata/<content ID>.edat (content ID: at most 0x2F characters). It must pass the EDAT header
// checks with the free klicensee (NP_KLIC_FREE) for its name, carry that content ID, decrypt (its content key from its
// licence type, D-580: the free klicensee, or the player's RAP for it) and hold "GOMA" and the content ID at 0x10.
// Every failure is LICENSE_NOT_FOUND, with a log (the game stays a trial); without the build's NPDRM keys a licence
// that is there cannot be verified.
s32 drm_verify_upgrade_license(PPUContext& ctx, std::string_view name, u32 content_id)
{
    if (!content_id) return error(SCE_NP_DRM_ERROR_INVALID_PARAM);
    const std::optional<std::string> id = drm_string(ctx, name, "content ID", content_id, kUpgradeContentIdMax);
    if (!id) return error(SCE_NP_DRM_ERROR_INVALID_PARAM);
    if (!recomp::valid_content_id(*id)) {
        PS_LOG_INFO(kChan, "{}(\"{}\"): not a valid content ID: licence not found", name, *id);
        return error(SCE_NP_DRM_ERROR_LICENSE_NOT_FOUND);
    }
    const std::string edat = std::string(recomp::kLicenceDir) + "/" + *id + ".edat";
    constexpr u64 kUnlockMax = 1u << 20; // an unlock licence is a few hundred bytes
    const GuestFile file = read_guest_file(ctx, edat, kUnlockMax);
    if (!file.opened) {
        PS_LOG_INFO(kChan, "{}(\"{}\"): no unlock licence {}: licence not found", name, *id, edat);
        return error(SCE_NP_DRM_ERROR_LICENSE_NOT_FOUND);
    }
    const auto not_verified = [&](std::string_view why) {
        PS_LOG_ERROR(kChan, "{}(\"{}\"): the unlock licence {} {}: licence not found", name, *id, edat, why);
        return error(SCE_NP_DRM_ERROR_LICENSE_NOT_FOUND);
    };
    const recomp::NpdrmKeys& keys = recomp::builtin_npdrm_keys();
    if (!keys.present) return not_verified("is there, but this build has no NPDRM keys to verify it");
    const std::vector<u8>& b = file.bytes;
    const std::optional<recomp::NpdHeader> header = recomp::parse_npd_header(b);
    if (!header || (header->flags & recomp::kNpdSdatFlag) ||
        !recomp::npd_hashes_valid(b, drm_file_name(edat), keys.klic_free, keys)) {
        return not_verified("fails the EDAT header checks");
    }
    if (header->content_id != *id) return not_verified("names another content (" + header->content_id + ")");
    lv2::PpuThread& t = thread_of(ctx);
    const recomp::Result<recomp::Key128> key = recomp::npdrm_content_key(recomp::NpdrmContent::edat, header->license,
        header->content_id, process_licence(t.kernel, keys.klic_free));
    if (!key) return not_verified("has no key: " + key.error().message);
    auto npd = recomp::NpdFile::open(
        [&b](u64 offset, void* dst, usize n) {
            if (offset > b.size() || n > b.size() - offset) return false;
            std::memcpy(dst, b.data() + offset, n);
            return true;
        },
        b.size(), recomp::builtin_npd_keys(), *key);
    if (!npd) return not_verified("does not decrypt (" + npd.error().message + ")");
    u8 plain[0x40]{};
    if ((*npd)->size() < sizeof(plain) || !(*npd)->read(0, plain, sizeof(plain)))
        return not_verified("is too short");
    const std::string_view held(reinterpret_cast<const char*>(plain + 0x10), 0x30);
    if (std::memcmp(plain, "GOMA", 4) != 0 || held.substr(0, held.find('\0')) != *id)
        return not_verified("does not hold \"GOMA\" and its content ID");
    PS_LOG_INFO(kChan, "{}(\"{}\"): the unlock licence {} is verified: the full game", name, *id, edat);
    return lv2::CELL_OK;
}

s32 sceNpDrmVerifyUpgradeLicense(PPUContext& ctx, u32 content_id)
{
    return drm_verify_upgrade_license(ctx, "sceNpDrmVerifyUpgradeLicense", content_id);
}

s32 sceNpDrmVerifyUpgradeLicense2(PPUContext& ctx, u32 content_id)
{
    return drm_verify_upgrade_license(ctx, "sceNpDrmVerifyUpgradeLicense2", content_id);
}

// ------------------------------------------------------------------------------------------------- lookup

// RPCS3 sceNpLookupInit / sceNpLookupTerm: the lookup (community) library on top of NP. Term leaves the title
// contexts, as RPCS3 does.
s32 sceNpLookupInit(PPUContext&)
{
    NpState& s = np();
    std::lock_guard lock(s.mutex);
    if (s.lookup_initialized) return error(SCE_NP_COMMUNITY_ERROR_ALREADY_INITIALIZED);
    if (!s.initialized) return error(SCE_NP_ERROR_NOT_INITIALIZED);
    s.lookup_initialized = true;
    PS_LOG_INFO(kChan, "sceNpLookupInit(): up; offline every lookup transaction is refused");
    return lv2::CELL_OK;
}

s32 sceNpLookupTerm(PPUContext&)
{
    NpState& s = np();
    std::lock_guard lock(s.mutex);
    if (!s.lookup_initialized) return error(SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED);
    if (!s.initialized) return error(SCE_NP_ERROR_NOT_INITIALIZED);
    s.lookup_initialized = false;
    return lv2::CELL_OK;
}

// RPCS3 sceNpLookupCreateTitleCtx: returns the new title context's ID. selfNpId is only checked for null. Two
// deviations, both where RPCS3 has no answer for a real console: RPCS3 ensure()s the communication ID is valid
// (lookup_title_ctx's constructor), refused here with a log; and with all 32 contexts in use RPCS3 returns idm's
// invalid ID 0 as if it were a context, where its score and TUS contexts report
// SCE_NP_COMMUNITY_ERROR_TOO_MANY_OBJECTS, as this does.
s32 sceNpLookupCreateTitleCtx(PPUContext& ctx, u32 communication_id_addr, u32 self_np_id)
{
    NpState& s = np();
    std::lock_guard lock(s.mutex);
    if (!s.lookup_initialized) return error(SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED);
    if (!communication_id_addr || !self_np_id) return error(SCE_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT);
    u8* base = guest_base(ctx);
    const std::optional<std::string> id = communication_id(base, communication_id_addr);
    if (!id) {
        log_call_site(ctx, base, std::format("sceNpLookupCreateTitleCtx: {:#x} is not a valid communication ID (RPCS3 "
                                             "aborts)", communication_id_addr));
        return error(SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
    }
    const u32 title_ctx = s.lookup_title_ctxs.make(*id);
    if (!title_ctx) {
        PS_LOG_ERROR(kChan, "sceNpLookupCreateTitleCtx({}): all {} title contexts are in use", *id,
            SCE_NP_LOOKUP_MAX_CTX_NUM);
        return error(SCE_NP_COMMUNITY_ERROR_TOO_MANY_OBJECTS);
    }
    PS_LOG_INFO(kChan, "sceNpLookupCreateTitleCtx({}): title context {:#x}", *id, title_ctx);
    return static_cast<s32>(title_ctx);
}

s32 sceNpLookupDestroyTitleCtx(PPUContext&, s32 title_ctx_id)
{
    NpState& s = np();
    std::lock_guard lock(s.mutex);
    if (!s.lookup_initialized) return error(SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED);
    if (!s.lookup_title_ctxs.remove(static_cast<u32>(title_ctx_id))) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ID);
    return lv2::CELL_OK;
}

// RPCS3 sceNpLookupCreateTransactionCtx: a transaction talks to PSN, so offline none is made (the title context isn't
// looked at).
s32 sceNpLookupCreateTransactionCtx(PPUContext&, s32 title_ctx_id)
{
    if (!lookup_initialized(np())) return error(SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED);
    PS_LOG_INFO(kChan, "sceNpLookupCreateTransactionCtx(title context {:#x}): offline, refused", title_ctx_id);
    return error(SCE_NP_COMMUNITY_ERROR_INVALID_ONLINE_ID);
}

// RPCS3 sceNpLookupDestroyTransactionCtx: no transaction context exists offline, so no ID is valid.
s32 sceNpLookupDestroyTransactionCtx(PPUContext&, s32 trans_id)
{
    if (!lookup_initialized(np())) return error(SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED);
    PS_LOG_DEBUG(kChan, "sceNpLookupDestroyTransactionCtx({:#x}): no such transaction", trans_id);
    return error(SCE_NP_COMMUNITY_ERROR_INVALID_ID);
}

// RPCS3 sceNpLookupSetTimeout: a TODO that checks the timeout (at least 10 s) but not the context ID.
s32 sceNpLookupSetTimeout(PPUContext&, s32 ctx_id, u64 timeout_us)
{
    if (!lookup_initialized(np())) return error(SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED);
    if (timeout_us < 10'000'000) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
    PS_LOG_DEBUG(kChan, "sceNpLookupSetTimeout({:#x}, {} us)", ctx_id, timeout_us);
    return lv2::CELL_OK;
}

// RPCS3 sceNpLookupNpIdAsync: the NP ID of an online ID, which PSN answers; offline refused before anything is
// written. The option must be null ("option check at least until fw 4.71").
s32 sceNpLookupNpIdAsync(PPUContext&, s32 trans_id, u32 online_id, u32 np_id, s32 prio, u32 option)
{
    if (!lookup_initialized(np())) return error(SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED);
    if (!online_id || !np_id) return error(SCE_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT);
    if (option) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
    PS_LOG_DEBUG(kChan, "sceNpLookupNpIdAsync(transaction {:#x}, prio {}): offline", trans_id, prio);
    return error(SCE_NP_COMMUNITY_ERROR_INVALID_ONLINE_ID);
}

// RPCS3 sceNpLookupTitleSmallStorageAsync: the title's small storage on PSN; offline refused before anything is
// written. The option must be null, as above.
s32 sceNpLookupTitleSmallStorageAsync(PPUContext&, s32 trans_id, u32 data, u32 max_size, u32 content_length, s32 prio,
    u32 option)
{
    if (!lookup_initialized(np())) return error(SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED);
    if (!data) return error(SCE_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT);
    if (option) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
    PS_LOG_DEBUG(kChan, "sceNpLookupTitleSmallStorageAsync(transaction {:#x}, {} bytes, length {:#x}, prio {}): "
                        "offline", trans_id, max_size, content_length, prio);
    return error(SCE_NP_COMMUNITY_ERROR_INVALID_ONLINE_ID);
}

// RPCS3 sceNpLookupPollAsync: a TODO that reports every transaction finished with result 0 (it checks neither the
// transaction ID nor the result pointer).
s32 sceNpLookupPollAsync(PPUContext& ctx, s32 trans_id, u32 result)
{
    if (!lookup_initialized(np())) return error(SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED);
    PS_LOG_DEBUG(kChan, "sceNpLookupPollAsync({:#x}): finished, result 0", trans_id);
    ps_write32(guest_base(ctx), result, 0);
    return lv2::CELL_OK;
}

// RPCS3 sceNpLookupWaitAsync: a TODO that answers CELL_OK and, unlike sceNpLookupPollAsync, writes no result.
s32 sceNpLookupWaitAsync(PPUContext&, s32 trans_id, u32 result)
{
    if (!lookup_initialized(np())) return error(SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED);
    PS_LOG_DEBUG(kChan, "sceNpLookupWaitAsync({:#x}, result {:#x}): nothing to wait for", trans_id, result);
    return lv2::CELL_OK;
}

// RPCS3 sceNpLookupAbortTransaction: checks only the library (no transaction exists offline anyway).
s32 sceNpLookupAbortTransaction(PPUContext&, s32 trans_id)
{
    if (!lookup_initialized(np())) return error(SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED);
    PS_LOG_DEBUG(kChan, "sceNpLookupAbortTransaction({:#x})", trans_id);
    return lv2::CELL_OK;
}

// What RPCS3's lookups that need PSN answer once their checks pass (the library, the pointers they need, a null
// option: "option check at least until fw 4.71"): SCE_NP_COMMUNITY_ERROR_INVALID_ONLINE_ID, nothing written.
s32 lookup_offline(std::string_view name, s32 trans_id, std::initializer_list<u32> needed, u32 option)
{
    if (!lookup_initialized(np())) return error(SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED);
    for (const u32 p : needed)
        if (!p) return error(SCE_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT);
    if (option) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
    PS_LOG_DEBUG(kChan, "{}(transaction {:#x}): offline", name, trans_id);
    return error(SCE_NP_COMMUNITY_ERROR_INVALID_ONLINE_ID);
}

s32 sceNpLookupNpId(PPUContext&, s32 trans_id, u32 online_id, u32 np_id, u32 option)
{
    return lookup_offline("sceNpLookupNpId", trans_id, {online_id, np_id}, option);
}

s32 sceNpLookupTitleSmallStorage(PPUContext&, s32 trans_id, u32 data, u32 max_size, u32 content_length, u32 option)
{
    PS_LOG_DEBUG(kChan, "sceNpLookupTitleSmallStorage: {} bytes, length {:#x}", max_size, content_length);
    return lookup_offline("sceNpLookupTitleSmallStorage", trans_id, {data}, option);
}

s32 sceNpLookupAvatarImage(PPUContext&, s32 trans_id, u32 avatar_url, u32 avatar_image, u32 option)
{
    return lookup_offline("sceNpLookupAvatarImage", trans_id, {avatar_url, avatar_image}, option);
}

s32 sceNpLookupAvatarImageAsync(PPUContext&, s32 trans_id, u32 avatar_url, u32 avatar_image, s32 prio, u32 option)
{
    PS_LOG_DEBUG(kChan, "sceNpLookupAvatarImageAsync: prio {}", prio);
    return lookup_offline("sceNpLookupAvatarImageAsync", trans_id, {avatar_url, avatar_image}, option);
}

// The user profile lookups: only the NP ID is needed (the outputs are each optional).
s32 sceNpLookupUserProfile(PPUContext&, s32 trans_id, u32 np_id, u32 user_info, u32 about_me, u32 languages,
    u32 country_code, u32 avatar_image, u32 option)
{
    PS_LOG_DEBUG(kChan, "sceNpLookupUserProfile: outputs {:#x} {:#x} {:#x} {:#x} {:#x}", user_info, about_me, languages,
        country_code, avatar_image);
    return lookup_offline("sceNpLookupUserProfile", trans_id, {np_id}, option);
}

s32 sceNpLookupUserProfileAsync(PPUContext&, s32 trans_id, u32 np_id, u32 user_info, u32 about_me, u32 languages,
    u32 country_code, u32 avatar_image, s32 prio, u32 option)
{
    PS_LOG_DEBUG(kChan, "sceNpLookupUserProfileAsync: outputs {:#x} {:#x} {:#x} {:#x} {:#x}, prio {}", user_info,
        about_me, languages, country_code, avatar_image, prio);
    return lookup_offline("sceNpLookupUserProfileAsync", trans_id, {np_id}, option);
}

s32 sceNpLookupUserProfileWithAvatarSize(PPUContext&, s32 trans_id, s32 avatar_size_type, u32 np_id, u32 user_info,
    u32 about_me, u32 languages, u32 country_code, u32 avatar_data, u32 avatar_data_max_size, u32 avatar_data_size,
    u32 option)
{
    PS_LOG_DEBUG(kChan, "sceNpLookupUserProfileWithAvatarSize: avatar size type {}, outputs {:#x} {:#x} {:#x} {:#x} "
                        "{:#x} ({} bytes) {:#x}", avatar_size_type, user_info, about_me, languages, country_code,
        avatar_data, avatar_data_max_size, avatar_data_size);
    return lookup_offline("sceNpLookupUserProfileWithAvatarSize", trans_id, {np_id}, option);
}

s32 sceNpLookupUserProfileWithAvatarSizeAsync(PPUContext&, s32 trans_id, s32 avatar_size_type, u32 np_id,
    u32 user_info, u32 about_me, u32 languages, u32 country_code, u32 avatar_data, u32 avatar_data_max_size,
    u32 avatar_data_size, s32 prio, u32 option)
{
    PS_LOG_DEBUG(kChan, "sceNpLookupUserProfileWithAvatarSizeAsync: avatar size type {}, outputs {:#x} {:#x} {:#x} "
                        "{:#x} {:#x} ({} bytes) {:#x}, prio {}", avatar_size_type, user_info, about_me, languages,
        country_code, avatar_data, avatar_data_max_size, avatar_data_size, prio);
    return lookup_offline("sceNpLookupUserProfileWithAvatarSizeAsync", trans_id, {np_id}, option);
}

// RPCS3 sceNpLookupTitleStorage / ...Async: UNIMPLEMENTED_FUNC after the library check, answering CELL_OK (their
// arguments aren't known). Answered the same, with a warning.
s32 sceNpLookupTitleStorage(PPUContext&)
{
    if (!lookup_initialized(np())) return error(SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED);
    PS_LOG_WARN(kChan, "sceNpLookupTitleStorage: not implemented in RPCS3 either; CELL_OK as RPCS3 answers");
    return lv2::CELL_OK;
}

s32 sceNpLookupTitleStorageAsync(PPUContext&)
{
    if (!lookup_initialized(np())) return error(SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED);
    PS_LOG_WARN(kChan, "sceNpLookupTitleStorageAsync: not implemented in RPCS3 either; CELL_OK as RPCS3 answers");
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------------- score

// The score (ranking) library: RPCS3 sceNpScore*, on top of NP. Title contexts are made and kept as RPCS3 keeps them;
// transactions talk to PSN, so offline none is made (sceNpScoreCreateTransactionCtx refuses) and each function that
// looks a transaction up finds none (SCE_NP_COMMUNITY_ERROR_INVALID_ID). Each function makes RPCS3's checks in
// RPCS3's order; whichever comes first, the transaction lookup or the online check
// (SCE_NP_COMMUNITY_ERROR_INVALID_ONLINE_ID), ends it. A few asynchronous clan functions are TODOs in RPCS3 that answer
// CELL_OK after their checks: so do they here, and their transaction stays unknown to sceNpScorePollAsync.

bool score_initialized(NpState& s)
{
    std::lock_guard lock(s.mutex);
    return s.score_initialized;
}

// The end of a score call that needs PSN.
s32 score_offline(std::string_view name, s32 trans_id)
{
    PS_LOG_DEBUG(kChan, "{}(transaction {:#x}): offline", name, trans_id);
    return error(SCE_NP_COMMUNITY_ERROR_INVALID_ONLINE_ID);
}

// The end of a score call that looks its transaction up (get_score_transaction_context): there is none.
s32 score_no_transaction(std::string_view name, s32 trans_id)
{
    PS_LOG_DEBUG(kChan, "{}(transaction {:#x}): no such transaction", name, trans_id);
    return error(SCE_NP_COMMUNITY_ERROR_INVALID_ID);
}

// An asynchronous clan call RPCS3 leaves as a TODO: CELL_OK once its checks pass.
s32 score_todo_ok(std::string_view name, s32 trans_id)
{
    PS_LOG_WARN(kChan, "{}(transaction {:#x}): a TODO in RPCS3, answered CELL_OK as there; nothing is fetched", name,
        trans_id);
    return lv2::CELL_OK;
}

// The first check of every score function.
#define PS_SCORE_INIT()                                                                                                \
    if (!score_initialized(np())) return error(SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED)

s32 sceNpScoreInit(PPUContext&)
{
    NpState& s = np();
    std::lock_guard lock(s.mutex);
    if (s.score_initialized) return error(SCE_NP_COMMUNITY_ERROR_ALREADY_INITIALIZED);
    if (!s.initialized) return error(SCE_NP_ERROR_NOT_INITIALIZED);
    s.score_initialized = true;
    PS_LOG_INFO(kChan, "sceNpScoreInit(): up; offline every score transaction is refused");
    return lv2::CELL_OK;
}

s32 sceNpScoreTerm(PPUContext&)
{
    NpState& s = np();
    std::lock_guard lock(s.mutex);
    if (!s.score_initialized) return error(SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED);
    if (!s.initialized) return error(SCE_NP_ERROR_NOT_INITIALIZED);
    s.score_initialized = false;
    return lv2::CELL_OK;
}

// RPCS3 sceNpScoreCreateTitleCtx: the new title context's ID. The passphrase and the user's NP ID are only checked for
// null. As for sceNpLookupCreateTitleCtx, a communication ID that isn't valid (RPCS3 aborts in score_ctx's
// constructor) is refused with a log.
s32 sceNpScoreCreateTitleCtx(PPUContext& ctx, u32 communication_id_addr, u32 passphrase, u32 self_np_id)
{
    PS_SCORE_INIT();
    if (!communication_id_addr || !passphrase || !self_np_id) {
        return error(SCE_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT);
    }
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    if (!guest_readable(t, communication_id_addr, 12)) {
        log_call_site(ctx, base, std::format("sceNpScoreCreateTitleCtx: the communication ID {:#x} is not guest memory",
                                     communication_id_addr));
        return error(SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
    }
    const std::optional<std::string> id = communication_id(base, communication_id_addr);
    if (!id) {
        log_call_site(ctx, base, std::format("sceNpScoreCreateTitleCtx: {:#x} is not a valid communication ID (RPCS3 "
                                             "aborts)", communication_id_addr));
        return error(SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
    }
    NpState& s = np();
    std::lock_guard lock(s.mutex);
    const u32 title_ctx = s.score_ctxs.make(ScoreCtx{*id});
    if (!title_ctx) {
        PS_LOG_ERROR(kChan, "sceNpScoreCreateTitleCtx({}): all {} title contexts are in use", *id,
            SCE_NP_SCORE_MAX_CTX_NUM);
        return error(SCE_NP_COMMUNITY_ERROR_TOO_MANY_OBJECTS);
    }
    PS_LOG_INFO(kChan, "sceNpScoreCreateTitleCtx({}): title context {:#x}", *id, title_ctx);
    return static_cast<s32>(title_ctx);
}

s32 sceNpScoreDestroyTitleCtx(PPUContext&, s32 title_ctx_id)
{
    PS_SCORE_INIT();
    NpState& s = np();
    std::lock_guard lock(s.mutex);
    if (!s.score_ctxs.remove(static_cast<u32>(title_ctx_id))) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ID);
    return lv2::CELL_OK;
}

// RPCS3 sceNpScoreCreateTransactionCtx: offline refused before the title context is looked at.
s32 sceNpScoreCreateTransactionCtx(PPUContext&, s32 title_ctx_id)
{
    PS_SCORE_INIT();
    PS_LOG_INFO(kChan, "sceNpScoreCreateTransactionCtx(title context {:#x}): offline, refused", title_ctx_id);
    return error(SCE_NP_COMMUNITY_ERROR_INVALID_ONLINE_ID);
}

s32 sceNpScoreDestroyTransactionCtx(PPUContext&, s32 trans_id)
{
    PS_SCORE_INIT();
    return score_no_transaction("sceNpScoreDestroyTransactionCtx", trans_id);
}

s32 sceNpScoreAbortTransaction(PPUContext&, s32 trans_id)
{
    PS_SCORE_INIT();
    return score_no_transaction("sceNpScoreAbortTransaction", trans_id);
}

s32 sceNpScorePollAsync(PPUContext&, s32 trans_id, u32 result)
{
    PS_SCORE_INIT();
    PS_LOG_DEBUG(kChan, "sceNpScorePollAsync: result {:#x}", result);
    return score_no_transaction("sceNpScorePollAsync", trans_id);
}

s32 sceNpScoreWaitAsync(PPUContext&, s32 trans_id, u32 result)
{
    PS_SCORE_INIT();
    PS_LOG_DEBUG(kChan, "sceNpScoreWaitAsync: result {:#x}", result);
    return score_no_transaction("sceNpScoreWaitAsync", trans_id);
}

// RPCS3 sceNpScoreSetPlayerCharacterId / sceNpScoreSetTimeout: on a transaction (none offline) or a title context,
// whose later transactions would start with it. The character ID is checked before the library; the timeout (at least
// 10 s) after it.
s32 sceNpScoreSetPlayerCharacterId(PPUContext&, s32 ctx_id, s32 pc_id)
{
    if (pc_id < 0) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
    PS_SCORE_INIT();
    NpState& s = np();
    std::lock_guard lock(s.mutex);
    ScoreCtx* c = s.score_ctxs.find(static_cast<u32>(ctx_id));
    if (!c) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ID);
    c->pc_id = pc_id;
    return lv2::CELL_OK;
}

s32 sceNpScoreSetTimeout(PPUContext&, s32 ctx_id, u64 timeout_us)
{
    PS_SCORE_INIT();
    if (timeout_us < kCommunityTimeoutMin) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
    NpState& s = np();
    std::lock_guard lock(s.mutex);
    ScoreCtx* c = s.score_ctxs.find(static_cast<u32>(ctx_id));
    if (!c) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ID);
    c->timeout = timeout_us;
    return lv2::CELL_OK;
}

// RPCS3 scenp_score_get_board_info: the board info pointer and a null option, then offline.
s32 score_board_info(std::string_view name, s32 trans_id, u32 board_info, u32 option)
{
    PS_SCORE_INIT();
    if (!board_info) return error(SCE_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT);
    if (option) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
    return score_offline(name, trans_id);
}

s32 sceNpScoreGetBoardInfo(PPUContext&, s32 trans_id, u32 board_id, u32 board_info, u32 option)
{
    PS_LOG_DEBUG(kChan, "sceNpScoreGetBoardInfo: board {}", board_id);
    return score_board_info("sceNpScoreGetBoardInfo", trans_id, board_info, option);
}

s32 sceNpScoreGetBoardInfoAsync(PPUContext&, s32 trans_id, u32 board_id, u32 board_info, s32 prio, u32 option)
{
    PS_LOG_DEBUG(kChan, "sceNpScoreGetBoardInfoAsync: board {}, prio {}", board_id, prio);
    return score_board_info("sceNpScoreGetBoardInfoAsync", trans_id, board_info, option);
}

// RPCS3 scenp_score_record_score: offline before anything else is looked at.
s32 sceNpScoreRecordScore(PPUContext&, s32 trans_id, u32 board_id, s64 score, u32 comment, u32 game_info,
    u32 tmp_rank, u32 option)
{
    PS_SCORE_INIT();
    PS_LOG_DEBUG(kChan, "sceNpScoreRecordScore: board {}, score {}, comment {:#x}, info {:#x}, rank {:#x}, option {:#x}",
        board_id, score, comment, game_info, tmp_rank, option);
    return score_offline("sceNpScoreRecordScore", trans_id);
}

s32 sceNpScoreRecordScoreAsync(PPUContext&, s32 trans_id, u32 board_id, s64 score, u32 comment, u32 game_info,
    u32 tmp_rank, s32 prio, u32 option)
{
    PS_SCORE_INIT();
    PS_LOG_DEBUG(kChan, "sceNpScoreRecordScoreAsync: board {}, score {}, comment {:#x}, info {:#x}, rank {:#x}, prio "
                        "{}, option {:#x}", board_id, score, comment, game_info, tmp_rank, prio, option);
    return score_offline("sceNpScoreRecordScoreAsync", trans_id);
}

// RPCS3 scenp_score_record_game_data: the data, then the transaction.
s32 score_record_game_data(std::string_view name, s32 trans_id, u32 data)
{
    PS_SCORE_INIT();
    if (!data) return error(SCE_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT);
    return score_no_transaction(name, trans_id);
}

s32 sceNpScoreRecordGameData(PPUContext&, s32 trans_id, u32 board_id, s64 score, u32 total_size, u32 send_size,
    u32 data, u32 option)
{
    PS_LOG_DEBUG(kChan, "sceNpScoreRecordGameData: board {}, score {}, {} of {} bytes, option {:#x}", board_id, score,
        send_size, total_size, option);
    return score_record_game_data("sceNpScoreRecordGameData", trans_id, data);
}

s32 sceNpScoreRecordGameDataAsync(PPUContext&, s32 trans_id, u32 board_id, s64 score, u32 total_size, u32 send_size,
    u32 data, s32 prio, u32 option)
{
    PS_LOG_DEBUG(kChan, "sceNpScoreRecordGameDataAsync: board {}, score {}, {} of {} bytes, prio {}, option {:#x}",
        board_id, score, send_size, total_size, prio, option);
    return score_record_game_data("sceNpScoreRecordGameDataAsync", trans_id, data);
}

// RPCS3 scenp_score_get_game_data: the NP ID and the buffer, then the transaction.
s32 score_get_game_data(std::string_view name, s32 trans_id, u32 np_id, u32 data)
{
    PS_SCORE_INIT();
    if (!np_id || !data) return error(SCE_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT);
    return score_no_transaction(name, trans_id);
}

s32 sceNpScoreGetGameData(PPUContext&, s32 trans_id, u32 board_id, u32 np_id, u32 total_size, u32 recv_size, u32 data,
    u32 option)
{
    PS_LOG_DEBUG(kChan, "sceNpScoreGetGameData: board {}, total size {:#x}, {} bytes, option {:#x}", board_id,
        total_size, recv_size, option);
    return score_get_game_data("sceNpScoreGetGameData", trans_id, np_id, data);
}

s32 sceNpScoreGetGameDataAsync(PPUContext&, s32 trans_id, u32 board_id, u32 np_id, u32 total_size, u32 recv_size,
    u32 data, s32 prio, u32 option)
{
    PS_LOG_DEBUG(kChan, "sceNpScoreGetGameDataAsync: board {}, total size {:#x}, {} bytes, prio {}, option {:#x}",
        board_id, total_size, recv_size, prio, option);
    return score_get_game_data("sceNpScoreGetGameDataAsync", trans_id, np_id, data);
}

// RPCS3 scenp_score_get_ranking_by_npid (for NP IDs and NP ID + character ID pairs): the array, a count of 1 to 101,
// then the transaction.
s32 score_ranking_by_npid(std::string_view name, s32 trans_id, u32 id_array, u32 array_num)
{
    PS_SCORE_INIT();
    if (!id_array) return error(SCE_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT);
    if (!array_num) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
    if (array_num > SCE_NP_SCORE_MAX_NPID_NUM_PER_TRANS) return error(SCE_NP_COMMUNITY_ERROR_TOO_MANY_NPID);
    return score_no_transaction(name, trans_id);
}

s32 sceNpScoreGetRankingByNpId(PPUContext&, s32 trans_id, u32 board_id, u32 id_array, u32 id_array_size,
    u32 rank_array, u32 rank_array_size, u32 comment_array, u32 comment_array_size, u32 info_array,
    u32 info_array_size, u32 array_num, u32 last_sort_date, u32 total_record, u32 option)
{
    PS_LOG_DEBUG(kChan, "sceNpScoreGetRankingByNpId: board {}, ids {:#x} ({}), ranks {:#x} ({}), comments {:#x} ({}), "
                        "infos {:#x} ({}), {} entries, {:#x} {:#x}, option {:#x}", board_id, id_array, id_array_size,
        rank_array, rank_array_size, comment_array, comment_array_size, info_array, info_array_size, array_num,
        last_sort_date, total_record, option);
    return score_ranking_by_npid("sceNpScoreGetRankingByNpId", trans_id, id_array, array_num);
}

s32 sceNpScoreGetRankingByNpIdAsync(PPUContext&, s32 trans_id, u32 board_id, u32 id_array, u32 id_array_size,
    u32 rank_array, u32 rank_array_size, u32 comment_array, u32 comment_array_size, u32 info_array,
    u32 info_array_size, u32 array_num, u32 last_sort_date, u32 total_record, s32 prio, u32 option)
{
    PS_LOG_DEBUG(kChan, "sceNpScoreGetRankingByNpIdAsync: board {}, ids {:#x} ({}), ranks {:#x} ({}), comments {:#x} "
                        "({}), infos {:#x} ({}), {} entries, {:#x} {:#x}, prio {}, option {:#x}", board_id, id_array,
        id_array_size, rank_array, rank_array_size, comment_array, comment_array_size, info_array, info_array_size,
        array_num, last_sort_date, total_record, prio, option);
    return score_ranking_by_npid("sceNpScoreGetRankingByNpIdAsync", trans_id, id_array, array_num);
}

s32 sceNpScoreGetRankingByNpIdPcId(PPUContext&, s32 trans_id, u32 board_id, u32 id_array, u32 id_array_size,
    u32 rank_array, u32 rank_array_size, u32 comment_array, u32 comment_array_size, u32 info_array,
    u32 info_array_size, u32 array_num, u32 last_sort_date, u32 total_record, u32 option)
{
    PS_LOG_DEBUG(kChan, "sceNpScoreGetRankingByNpIdPcId: board {}, ids {:#x} ({}), ranks {:#x} ({}), comments {:#x} "
                        "({}), infos {:#x} ({}), {} entries, {:#x} {:#x}, option {:#x}", board_id, id_array,
        id_array_size, rank_array, rank_array_size, comment_array, comment_array_size, info_array, info_array_size,
        array_num, last_sort_date, total_record, option);
    return score_ranking_by_npid("sceNpScoreGetRankingByNpIdPcId", trans_id, id_array, array_num);
}

s32 sceNpScoreGetRankingByNpIdPcIdAsync(PPUContext&, s32 trans_id, u32 board_id, u32 id_array, u32 id_array_size,
    u32 rank_array, u32 rank_array_size, u32 comment_array, u32 comment_array_size, u32 info_array,
    u32 info_array_size, u32 array_num, u32 last_sort_date, u32 total_record, s32 prio, u32 option)
{
    PS_LOG_DEBUG(kChan, "sceNpScoreGetRankingByNpIdPcIdAsync: board {}, ids {:#x} ({}), ranks {:#x} ({}), comments "
                        "{:#x} ({}), infos {:#x} ({}), {} entries, {:#x} {:#x}, prio {}, option {:#x}", board_id,
        id_array, id_array_size, rank_array, rank_array_size, comment_array, comment_array_size, info_array,
        info_array_size, array_num, last_sort_date, total_record, prio, option);
    return score_ranking_by_npid("sceNpScoreGetRankingByNpIdPcIdAsync", trans_id, id_array, array_num);
}

// RPCS3 scenp_score_get_ranking_by_range and scenp_score_get_friends_ranking look the transaction up first.
s32 sceNpScoreGetRankingByRange(PPUContext&, s32 trans_id, u32 board_id, u32 start_serial_rank, u32 rank_array,
    u32 rank_array_size, u32 comment_array, u32 comment_array_size, u32 info_array, u32 info_array_size, u32 array_num,
    u32 last_sort_date, u32 total_record, u32 option)
{
    PS_SCORE_INIT();
    PS_LOG_DEBUG(kChan, "sceNpScoreGetRankingByRange: board {}, from rank {}, ranks {:#x} ({}), comments {:#x} ({}), "
                        "infos {:#x} ({}), {} entries, {:#x} {:#x}, option {:#x}", board_id, start_serial_rank,
        rank_array, rank_array_size, comment_array, comment_array_size, info_array, info_array_size, array_num,
        last_sort_date, total_record, option);
    return score_no_transaction("sceNpScoreGetRankingByRange", trans_id);
}

s32 sceNpScoreGetRankingByRangeAsync(PPUContext&, s32 trans_id, u32 board_id, u32 start_serial_rank, u32 rank_array,
    u32 rank_array_size, u32 comment_array, u32 comment_array_size, u32 info_array, u32 info_array_size, u32 array_num,
    u32 last_sort_date, u32 total_record, s32 prio, u32 option)
{
    PS_SCORE_INIT();
    PS_LOG_DEBUG(kChan, "sceNpScoreGetRankingByRangeAsync: board {}, from rank {}, ranks {:#x} ({}), comments {:#x} "
                        "({}), infos {:#x} ({}), {} entries, {:#x} {:#x}, prio {}, option {:#x}", board_id,
        start_serial_rank, rank_array, rank_array_size, comment_array, comment_array_size, info_array, info_array_size,
        array_num, last_sort_date, total_record, prio, option);
    return score_no_transaction("sceNpScoreGetRankingByRangeAsync", trans_id);
}

s32 sceNpScoreGetFriendsRanking(PPUContext&, s32 trans_id, u32 board_id, s32 include_self, u32 rank_array,
    u32 rank_array_size, u32 comment_array, u32 comment_array_size, u32 info_array, u32 info_array_size, u32 array_num,
    u32 last_sort_date, u32 total_record, u32 option)
{
    PS_SCORE_INIT();
    PS_LOG_DEBUG(kChan, "sceNpScoreGetFriendsRanking: board {}, self {}, ranks {:#x} ({}), comments {:#x} ({}), infos "
                        "{:#x} ({}), {} entries, {:#x} {:#x}, option {:#x}", board_id, include_self, rank_array,
        rank_array_size, comment_array, comment_array_size, info_array, info_array_size, array_num, last_sort_date,
        total_record, option);
    return score_no_transaction("sceNpScoreGetFriendsRanking", trans_id);
}

s32 sceNpScoreGetFriendsRankingAsync(PPUContext&, s32 trans_id, u32 board_id, s32 include_self, u32 rank_array,
    u32 rank_array_size, u32 comment_array, u32 comment_array_size, u32 info_array, u32 info_array_size, u32 array_num,
    u32 last_sort_date, u32 total_record, s32 prio, u32 option)
{
    PS_SCORE_INIT();
    PS_LOG_DEBUG(kChan, "sceNpScoreGetFriendsRankingAsync: board {}, self {}, ranks {:#x} ({}), comments {:#x} ({}), "
                        "infos {:#x} ({}), {} entries, {:#x} {:#x}, prio {}, option {:#x}", board_id, include_self,
        rank_array, rank_array_size, comment_array, comment_array_size, info_array, info_array_size, array_num,
        last_sort_date, total_record, prio, option);
    return score_no_transaction("sceNpScoreGetFriendsRankingAsync", trans_id);
}

// The length of a comment, as RPCS3's strlen; nullopt when it runs out of guest memory (logged: RPCS3 faults).
std::optional<u32> comment_length(PPUContext& ctx, std::string_view name, u32 comment)
{
    lv2::PpuThread& t = thread_of(ctx);
    const std::optional<u32> length = guest_strnlen(t, comment, SCE_NP_SCORE_CENSOR_COMMENT_MAXLEN + 1);
    if (!length) {
        log_call_site(ctx, t.kernel.base(), std::format("{}: the comment at {:#x} runs out of guest memory", name,
                                                comment));
    }
    return length;
}

// RPCS3 scenp_score_censor_comment: the comment (at most 255 characters) and a null option, then offline.
s32 score_censor_comment(PPUContext& ctx, std::string_view name, s32 trans_id, u32 comment, u32 option)
{
    PS_SCORE_INIT();
    if (!comment) return error(SCE_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT);
    const std::optional<u32> length = comment_length(ctx, name, comment);
    if (!length || *length > SCE_NP_SCORE_CENSOR_COMMENT_MAXLEN || option) {
        return error(SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
    }
    return score_offline(name, trans_id);
}

s32 sceNpScoreCensorComment(PPUContext& ctx, s32 trans_id, u32 comment, u32 option)
{
    return score_censor_comment(ctx, "sceNpScoreCensorComment", trans_id, comment, option);
}

s32 sceNpScoreCensorCommentAsync(PPUContext& ctx, s32 trans_id, u32 comment, s32 prio, u32 option)
{
    PS_LOG_DEBUG(kChan, "sceNpScoreCensorCommentAsync: prio {}", prio);
    return score_censor_comment(ctx, "sceNpScoreCensorCommentAsync", trans_id, comment, option);
}

// RPCS3 scenp_score_sanitize_comment: the output first (before the library), the comment (at most 255 characters),
// then the transaction.
s32 score_sanitize_comment(PPUContext& ctx, std::string_view name, s32 trans_id, u32 comment, u32 sanitized)
{
    if (!sanitized) return error(SCE_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT);
    PS_SCORE_INIT();
    if (!comment) return error(SCE_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT);
    const std::optional<u32> length = comment_length(ctx, name, comment);
    if (!length || *length > SCE_NP_SCORE_CENSOR_COMMENT_MAXLEN) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
    return score_no_transaction(name, trans_id);
}

s32 sceNpScoreSanitizeComment(PPUContext& ctx, s32 trans_id, u32 comment, u32 sanitized, u32 option)
{
    PS_LOG_DEBUG(kChan, "sceNpScoreSanitizeComment: option {:#x}", option);
    return score_sanitize_comment(ctx, "sceNpScoreSanitizeComment", trans_id, comment, sanitized);
}

s32 sceNpScoreSanitizeCommentAsync(PPUContext& ctx, s32 trans_id, u32 comment, u32 sanitized, s32 prio, u32 option)
{
    PS_LOG_DEBUG(kChan, "sceNpScoreSanitizeCommentAsync: prio {}, option {:#x}", prio, option);
    return score_sanitize_comment(ctx, "sceNpScoreSanitizeCommentAsync", trans_id, comment, sanitized);
}

// ------------------------------------------------------------------------------------------------- score: clans

// RPCS3 sceNpScoreGetClanMemberGameData: the NP ID, sizes and buffer and a null option, then offline. Its
// asynchronous form checks only the option (a TODO: CELL_OK).
s32 sceNpScoreGetClanMemberGameData(PPUContext&, s32 trans_id, u32 board_id, u32 clan_id, u32 np_id, u32 total_size,
    u32 recv_size, u32 data, u32 option)
{
    PS_SCORE_INIT();
    if (!np_id || !total_size || !data) return error(SCE_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT);
    if (option) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
    PS_LOG_DEBUG(kChan, "sceNpScoreGetClanMemberGameData: board {}, clan {}, {} bytes", board_id, clan_id, recv_size);
    return score_offline("sceNpScoreGetClanMemberGameData", trans_id);
}

s32 sceNpScoreGetClanMemberGameDataAsync(PPUContext&, s32 trans_id, u32 board_id, u32 clan_id, u32 np_id,
    u32 total_size, u32 recv_size, u32 data, s32 prio, u32 option)
{
    PS_SCORE_INIT();
    if (option) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
    PS_LOG_DEBUG(kChan, "sceNpScoreGetClanMemberGameDataAsync: board {}, clan {}, NP ID {:#x}, sizes {:#x} {}, data "
                        "{:#x}, prio {}", board_id, clan_id, np_id, total_size, recv_size, data, prio);
    return score_todo_ok("sceNpScoreGetClanMemberGameDataAsync", trans_id);
}

// RPCS3 sceNpScoreGetClansMembersRankingByNpId(PcId): the ID and rank arrays, the dates, a count of 1 to 101 and a
// null option, then offline. The asynchronous forms check only the ID array, the count and the option (TODOs:
// CELL_OK).
s32 clans_members_by_npid(std::string_view name, s32 trans_id, u32 id_array, u32 rank_array, u32 array_num,
    u32 last_sort_date, u32 total_record, u32 option)
{
    PS_SCORE_INIT();
    if (!id_array || !rank_array || !total_record || !last_sort_date || !array_num) {
        return error(SCE_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT);
    }
    if (option) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
    if (array_num > SCE_NP_SCORE_MAX_NPID_NUM_PER_TRANS) return error(SCE_NP_COMMUNITY_ERROR_TOO_MANY_NPID);
    return score_offline(name, trans_id);
}

s32 clans_members_by_npid_async(std::string_view name, s32 trans_id, u32 id_array, u32 array_num, u32 option)
{
    PS_SCORE_INIT();
    if (!id_array || !array_num) return error(SCE_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT);
    if (option) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
    return score_todo_ok(name, trans_id);
}

s32 sceNpScoreGetClansMembersRankingByNpId(PPUContext&, s32 trans_id, u32 clan_id, u32 board_id, u32 id_array,
    u32 id_array_size, u32 rank_array, u32 rank_array_size, u32 comment_array, u32 comment_array_size, u32 info_array,
    u32 info_array_size, u32 descript_array, u32 descript_array_size, u32 array_num, u32 clan_info,
    u32 last_sort_date, u32 total_record, u32 option)
{
    PS_LOG_DEBUG(kChan, "sceNpScoreGetClansMembersRankingByNpId: clan {}, board {}, sizes {} {} {} {} {}, {:#x} {:#x} "
                        "{:#x} {:#x}", clan_id, board_id, id_array_size, rank_array_size, comment_array_size,
        info_array_size, descript_array_size, comment_array, info_array, descript_array, clan_info);
    return clans_members_by_npid("sceNpScoreGetClansMembersRankingByNpId", trans_id, id_array, rank_array, array_num,
        last_sort_date, total_record, option);
}

s32 sceNpScoreGetClansMembersRankingByNpIdAsync(PPUContext&, s32 trans_id, u32 clan_id, u32 board_id, u32 id_array,
    u32 id_array_size, u32 rank_array, u32 rank_array_size, u32 comment_array, u32 comment_array_size, u32 info_array,
    u32 info_array_size, u32 descript_array, u32 descript_array_size, u32 array_num, u32 clan_info,
    u32 last_sort_date, u32 total_record, s32 prio, u32 option)
{
    PS_LOG_DEBUG(kChan, "sceNpScoreGetClansMembersRankingByNpIdAsync: clan {}, board {}, sizes {} {} {} {} {}, {:#x} "
                        "{:#x} {:#x} {:#x} {:#x} {:#x} {:#x}, prio {}", clan_id, board_id, id_array_size,
        rank_array_size, comment_array_size, info_array_size, descript_array_size, rank_array, comment_array,
        info_array, descript_array, clan_info, last_sort_date, total_record, prio);
    return clans_members_by_npid_async("sceNpScoreGetClansMembersRankingByNpIdAsync", trans_id, id_array, array_num,
        option);
}

s32 sceNpScoreGetClansMembersRankingByNpIdPcId(PPUContext&, s32 trans_id, u32 clan_id, u32 board_id, u32 id_array,
    u32 id_array_size, u32 rank_array, u32 rank_array_size, u32 comment_array, u32 comment_array_size, u32 info_array,
    u32 info_array_size, u32 descript_array, u32 descript_array_size, u32 array_num, u32 clan_info,
    u32 last_sort_date, u32 total_record, u32 option)
{
    PS_LOG_DEBUG(kChan, "sceNpScoreGetClansMembersRankingByNpIdPcId: clan {}, board {}, sizes {} {} {} {} {}, {:#x} "
                        "{:#x} {:#x} {:#x}", clan_id, board_id, id_array_size, rank_array_size, comment_array_size,
        info_array_size, descript_array_size, comment_array, info_array, descript_array, clan_info);
    return clans_members_by_npid("sceNpScoreGetClansMembersRankingByNpIdPcId", trans_id, id_array, rank_array,
        array_num, last_sort_date, total_record, option);
}

s32 sceNpScoreGetClansMembersRankingByNpIdPcIdAsync(PPUContext&, s32 trans_id, u32 clan_id, u32 board_id,
    u32 id_array, u32 id_array_size, u32 rank_array, u32 rank_array_size, u32 comment_array, u32 comment_array_size,
    u32 info_array, u32 info_array_size, u32 descript_array, u32 descript_array_size, u32 array_num, u32 clan_info,
    u32 last_sort_date, u32 total_record, s32 prio, u32 option)
{
    PS_LOG_DEBUG(kChan, "sceNpScoreGetClansMembersRankingByNpIdPcIdAsync: clan {}, board {}, sizes {} {} {} {} {}, "
                        "{:#x} {:#x} {:#x} {:#x} {:#x} {:#x} {:#x}, prio {}", clan_id, board_id, id_array_size,
        rank_array_size, comment_array_size, info_array_size, descript_array_size, rank_array, comment_array,
        info_array, descript_array, clan_info, last_sort_date, total_record, prio);
    return clans_members_by_npid_async("sceNpScoreGetClansMembersRankingByNpIdPcIdAsync", trans_id, id_array,
        array_num, option);
}

// RPCS3 sceNpScoreGetClansMembersRankingByRange: the rank array, the dates, a count; a start rank and a null option;
// at most 100 entries; then offline. Its asynchronous form checks the count, the start rank and the option (a TODO:
// CELL_OK).
s32 sceNpScoreGetClansMembersRankingByRange(PPUContext&, s32 trans_id, u32 clan_id, u32 board_id,
    u32 start_serial_rank, u32 rank_array, u32 rank_array_size, u32 comment_array, u32 comment_array_size,
    u32 info_array, u32 info_array_size, u32 descript_array, u32 descript_array_size, u32 array_num, u32 clan_info,
    u32 last_sort_date, u32 total_record, u32 option)
{
    PS_SCORE_INIT();
    if (!rank_array || !total_record || !last_sort_date || !array_num) {
        return error(SCE_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT);
    }
    if (!start_serial_rank || option) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
    if (array_num > SCE_NP_SCORE_MAX_RANGE_NUM_PER_TRANS) return error(SCE_NP_COMMUNITY_ERROR_TOO_LARGE_RANGE);
    PS_LOG_DEBUG(kChan, "sceNpScoreGetClansMembersRankingByRange: clan {}, board {}, sizes {} {} {} {}, {:#x} {:#x} "
                        "{:#x} {:#x}", clan_id, board_id, rank_array_size, comment_array_size, info_array_size,
        descript_array_size, comment_array, info_array, descript_array, clan_info);
    return score_offline("sceNpScoreGetClansMembersRankingByRange", trans_id);
}

s32 sceNpScoreGetClansMembersRankingByRangeAsync(PPUContext&, s32 trans_id, u32 clan_id, u32 board_id,
    u32 start_serial_rank, u32 rank_array, u32 rank_array_size, u32 comment_array, u32 comment_array_size,
    u32 info_array, u32 info_array_size, u32 descript_array, u32 descript_array_size, u32 array_num, u32 clan_info,
    u32 last_sort_date, u32 total_record, s32 prio, u32 option)
{
    PS_SCORE_INIT();
    if (!array_num) return error(SCE_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT);
    if (!start_serial_rank || option) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
    PS_LOG_DEBUG(kChan, "sceNpScoreGetClansMembersRankingByRangeAsync: clan {}, board {}, sizes {} {} {} {}, {:#x} "
                        "{:#x} {:#x} {:#x} {:#x} {:#x} {:#x}, prio {}", clan_id, board_id, rank_array_size,
        comment_array_size, info_array_size, descript_array_size, rank_array, comment_array, info_array,
        descript_array, clan_info, last_sort_date, total_record, prio);
    return score_todo_ok("sceNpScoreGetClansMembersRankingByRangeAsync", trans_id);
}

// RPCS3 sceNpScoreGetClansRankingByClanId: the arrays, the dates, a count; reserved arguments all 0 and a null
// option; at most 101 clans; then offline. Its asynchronous form checks the count, the reserved arguments and the
// option (a TODO: CELL_OK).
s32 sceNpScoreGetClansRankingByClanId(PPUContext&, s32 trans_id, u32 clan_board_id, u32 clan_id_array,
    u32 clan_id_array_size, u32 rank_array, u32 rank_array_size, u32 reserved1, u32 reserved_size1, u32 reserved2,
    u32 reserved_size2, u32 array_num, u32 last_sort_date, u32 total_record, u32 option)
{
    PS_SCORE_INIT();
    if (!clan_id_array || !rank_array || !total_record || !last_sort_date || !array_num) {
        return error(SCE_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT);
    }
    if (reserved1 || reserved_size1 || reserved2 || reserved_size2 || option) {
        return error(SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
    }
    if (array_num > SCE_NP_SCORE_MAX_NPID_NUM_PER_TRANS) return error(SCE_NP_COMMUNITY_ERROR_TOO_MANY_NPID);
    PS_LOG_DEBUG(kChan, "sceNpScoreGetClansRankingByClanId: board {}, sizes {} {}", clan_board_id, clan_id_array_size,
        rank_array_size);
    return score_offline("sceNpScoreGetClansRankingByClanId", trans_id);
}

s32 sceNpScoreGetClansRankingByClanIdAsync(PPUContext&, s32 trans_id, u32 clan_board_id, u32 clan_id_array,
    u32 clan_id_array_size, u32 rank_array, u32 rank_array_size, u32 reserved1, u32 reserved_size1, u32 reserved2,
    u32 reserved_size2, u32 array_num, u32 last_sort_date, u32 total_record, s32 prio, u32 option)
{
    PS_SCORE_INIT();
    if (!array_num) return error(SCE_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT);
    if (reserved1 || reserved_size1 || reserved2 || reserved_size2 || option) {
        return error(SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
    }
    PS_LOG_DEBUG(kChan, "sceNpScoreGetClansRankingByClanIdAsync: board {}, {:#x} ({}), {:#x} ({}), {:#x} {:#x}, prio "
                        "{}", clan_board_id, clan_id_array, clan_id_array_size, rank_array, rank_array_size,
        last_sort_date, total_record, prio);
    return score_todo_ok("sceNpScoreGetClansRankingByClanIdAsync", trans_id);
}

// RPCS3 sceNpScoreGetClansRankingByRange: the rank array, the dates, a count; a start rank, reserved arguments all 0
// and a null option; at most 101 clans; then offline. Its asynchronous form checks the count, the start rank, the
// reserved arguments and the option (a TODO: CELL_OK).
s32 sceNpScoreGetClansRankingByRange(PPUContext&, s32 trans_id, u32 clan_board_id, u32 start_serial_rank,
    u32 rank_array, u32 rank_array_size, u32 reserved1, u32 reserved_size1, u32 reserved2, u32 reserved_size2,
    u32 array_num, u32 last_sort_date, u32 total_record, u32 option)
{
    PS_SCORE_INIT();
    if (!rank_array || !total_record || !last_sort_date || !array_num) {
        return error(SCE_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT);
    }
    if (!start_serial_rank || reserved1 || reserved_size1 || reserved2 || reserved_size2 || option) {
        return error(SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
    }
    if (array_num > SCE_NP_SCORE_MAX_CLAN_NUM_PER_TRANS) return error(SCE_NP_COMMUNITY_ERROR_TOO_LARGE_RANGE);
    PS_LOG_DEBUG(kChan, "sceNpScoreGetClansRankingByRange: board {}, size {}", clan_board_id, rank_array_size);
    return score_offline("sceNpScoreGetClansRankingByRange", trans_id);
}

s32 sceNpScoreGetClansRankingByRangeAsync(PPUContext&, s32 trans_id, u32 clan_board_id, u32 start_serial_rank,
    u32 rank_array, u32 rank_array_size, u32 reserved1, u32 reserved_size1, u32 reserved2, u32 reserved_size2,
    u32 array_num, u32 last_sort_date, u32 total_record, s32 prio, u32 option)
{
    PS_SCORE_INIT();
    if (!array_num) return error(SCE_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT);
    if (!start_serial_rank || reserved1 || reserved_size1 || reserved2 || reserved_size2 || option) {
        return error(SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
    }
    PS_LOG_DEBUG(kChan, "sceNpScoreGetClansRankingByRangeAsync: board {}, {:#x} ({}), {:#x} {:#x}, prio {}",
        clan_board_id, rank_array, rank_array_size, last_sort_date, total_record, prio);
    return score_todo_ok("sceNpScoreGetClansRankingByRangeAsync", trans_id);
}

#undef PS_SCORE_INIT

// ------------------------------------------------------------------------------------------------- signaling

// RPCS3 sceNpSignalingCreateCtx: a signaling context for peer-to-peer connections, holding the given NP ID and the
// connection event handler (kept; offline no connection ever calls it).
s32 sceNpSignalingCreateCtx(PPUContext& ctx, u32 np_id, u32 handler, u32 arg, u32 ctx_id)
{
    NpState& s = np();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) return error(SCE_NP_SIGNALING_ERROR_NOT_INITIALIZED);
    if (!np_id || !ctx_id) return error(SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT);
    u8* base = guest_base(ctx);
    SignalingCtx c;
    for (u32 i = 0; i < kNpIdSize; i++) c.npid[i] = ps_read8(base, np_id + i);
    c.handler = handler;
    c.arg = arg;
    const u32 id = s.signaling_ctxs.make(c);
    if (!id) return error(SCE_NP_SIGNALING_ERROR_CTX_MAX);
    ps_write32(base, ctx_id, id);
    PS_LOG_INFO(kChan, "sceNpSignalingCreateCtx(handler {:#x}, arg {:#x}): context {:#x}; offline, no connection ever "
                       "calls the handler", handler, arg, id);
    return lv2::CELL_OK;
}

s32 sceNpSignalingDestroyCtx(PPUContext&, u32 ctx_id)
{
    NpState& s = np();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) return error(SCE_NP_SIGNALING_ERROR_NOT_INITIALIZED);
    if (!s.signaling_ctxs.remove(ctx_id)) return error(SCE_NP_SIGNALING_ERROR_CTX_NOT_FOUND);
    return lv2::CELL_OK;
}

// RPCS3 sceNpSignalingAddExtendedHandler: a second handler for the context's connection events (kept, never called
// offline). A null handler is accepted.
s32 sceNpSignalingAddExtendedHandler(PPUContext&, u32 ctx_id, u32 handler, u32 arg)
{
    NpState& s = np();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) return error(SCE_NP_SIGNALING_ERROR_NOT_INITIALIZED);
    SignalingCtx* c = s.signaling_ctxs.find(ctx_id);
    if (!c) return error(SCE_NP_SIGNALING_ERROR_CTX_NOT_FOUND);
    c->ext_handler = handler;
    c->ext_arg = arg;
    PS_LOG_INFO(kChan, "sceNpSignalingAddExtendedHandler(context {:#x}, handler {:#x}, arg {:#x}): kept; offline, it "
                       "is never called", ctx_id, handler, arg);
    return lv2::CELL_OK;
}

// RPCS3 sceNpSignalingActivateConnection: starts a peer-to-peer connection to another user. After its checks (the
// context, both pointers, not the user's own NP ID: offline, any NP ID with an empty online ID) RPCS3 makes the
// connection pending and asks RPCN for the peer's address, which with PSN disabled aborts the emulator ("RPCN is
// required to use PSN online features"). pspocket differs there: no connection is made and the call is refused with
// SCE_NP_SIGNALING_ERROR_PEER_UNREACHABLE, the signaling library's error for a peer it can't reach, so the other
// signaling functions keep finding no connection. RPCS3 reads the NP ID unchecked: one that isn't guest memory is
// refused here with a log.
s32 sceNpSignalingActivateConnection(PPUContext& ctx, u32 ctx_id, u32 np_id, u32 conn_id)
{
    NpState& s = np();
    if (!s.initialized) return error(SCE_NP_SIGNALING_ERROR_NOT_INITIALIZED);
    {
        std::lock_guard lock(s.mutex);
        if (!s.signaling_ctxs.find(ctx_id)) return error(SCE_NP_SIGNALING_ERROR_CTX_NOT_FOUND);
    }
    if (!np_id || !conn_id) return error(SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT);
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    if (!guest_readable(t, np_id, 1)) {
        log_call_site(ctx, base,
            std::format("sceNpSignalingActivateConnection: the NP ID {:#x} is not guest memory", np_id));
        return error(SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT);
    }
    if (!ps_read8(base, np_id)) return error(SCE_NP_SIGNALING_ERROR_OWN_NP_ID);
    PS_LOG_ERROR(kChan, "sceNpSignalingActivateConnection(context {:#x}, peer \"{}\"): offline the peer can't be "
                        "reached (RPCS3 needs RPCN here and aborts); no connection", ctx_id,
        guest_string(t, np_id, kOnlineIdLength));
    return error(SCE_NP_SIGNALING_ERROR_PEER_UNREACHABLE);
}

// RPCS3 sceNpSignalingGetLocalNetInfo: the console's local and public addresses (0 without a network) and RPCS3's
// guesses: NAT type 2, UPnP invalid (RPCS3 enables it only with a network), NP port open. The struct's size picks its
// layout: SceNpSignalingNetInfo (28 bytes, with the NP port) or the deprecated one without the port (24). The context
// ID is not checked.
s32 sceNpSignalingGetLocalNetInfo(PPUContext& ctx, u32 ctx_id, u32 info)
{
    if (!np().initialized) return error(SCE_NP_SIGNALING_ERROR_NOT_INITIALIZED);
    u8* base = guest_base(ctx);
    if (!info) return error(SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT);
    const u32 size = ps_read32(base, info);
    if (size != kNetInfoSize && size != kNetInfoDeprecatedSize) return error(SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT);
    ps_write32(base, info + 4, 0); // local_addr
    ps_write32(base, info + 8, 0); // mapped_addr
    ps_write32(base, info + 12, SCE_NP_SIGNALING_NETINFO_NAT_STATUS_TYPE2);
    ps_write32(base, info + 16, SCE_NP_SIGNALING_NETINFO_UPNP_STATUS_INVALID);
    ps_write32(base, info + 20, SCE_NP_SIGNALING_NETINFO_NPPORT_STATUS_OPEN);
    if (size == kNetInfoSize) ps_write16(base, info + 24, SCE_NP_PORT);
    PS_LOG_DEBUG(kChan, "sceNpSignalingGetLocalNetInfo(context {:#x}): no network", ctx_id);
    return lv2::CELL_OK;
}

// RPCS3 sceNpSignalingGetConnectionStatus, GetConnectionInfo, GetConnectionFromNpId, GetConnectionFromPeerAddress and
// TerminateConnection work on the signaling handler's connections. Offline there are none
// (sceNpSignalingActivateConnection, which would make one, needs RPCN), and RPCS3 checks neither ID against a context.
s32 sceNpSignalingGetConnectionStatus(PPUContext& ctx, u32 ctx_id, u32 conn_id, u32 conn_status, u32 peer_addr,
    u32 peer_port)
{
    if (!np().initialized) return error(SCE_NP_SIGNALING_ERROR_NOT_INITIALIZED);
    if (!conn_status) return error(SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT);
    PS_LOG_DEBUG(kChan, "sceNpSignalingGetConnectionStatus(context {:#x}, connection {:#x}, addr {:#x}, port {:#x}): "
                        "inactive, no such connection", ctx_id, conn_id, peer_addr, peer_port);
    ps_write32(guest_base(ctx), conn_status, SCE_NP_SIGNALING_CONN_STATUS_INACTIVE);
    return error(SCE_NP_SIGNALING_ERROR_CONN_NOT_FOUND);
}

s32 sceNpSignalingGetConnectionInfo(PPUContext&, u32 ctx_id, u32 conn_id, s32 code, u32 info)
{
    if (!np().initialized) return error(SCE_NP_SIGNALING_ERROR_NOT_INITIALIZED);
    if (!info) return error(SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT);
    PS_LOG_DEBUG(kChan, "sceNpSignalingGetConnectionInfo(context {:#x}, connection {:#x}, code {}): no such connection",
        ctx_id, conn_id, code);
    return error(SCE_NP_SIGNALING_ERROR_CONN_NOT_FOUND);
}

// The user's own NP ID is refused first. Offline it is all zeros, and np::is_same_npid compares the online IDs'
// first 16 characters as strings, so any NP ID with an empty online ID is the user's.
s32 sceNpSignalingGetConnectionFromNpId(PPUContext& ctx, u32 ctx_id, u32 np_id, u32 conn_id)
{
    if (!np().initialized) return error(SCE_NP_SIGNALING_ERROR_NOT_INITIALIZED);
    if (!np_id || !conn_id) return error(SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT);
    if (ps_read8(guest_base(ctx), np_id) == 0) return error(SCE_NP_SIGNALING_ERROR_OWN_NP_ID);
    PS_LOG_DEBUG(kChan, "sceNpSignalingGetConnectionFromNpId(context {:#x}): no such connection", ctx_id);
    return error(SCE_NP_SIGNALING_ERROR_CONN_NOT_FOUND);
}

s32 sceNpSignalingGetConnectionFromPeerAddress(PPUContext&, u32 ctx_id, u32 peer_addr, u16 peer_port, u32 conn_id)
{
    if (!np().initialized) return error(SCE_NP_SIGNALING_ERROR_NOT_INITIALIZED);
    if (!conn_id) return error(SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT);
    PS_LOG_DEBUG(kChan, "sceNpSignalingGetConnectionFromPeerAddress(context {:#x}, {:#010x}:{}): no such connection",
        ctx_id, peer_addr, peer_port);
    return error(SCE_NP_SIGNALING_ERROR_CONN_NOT_FOUND);
}

// Stopping a connection that doesn't exist does nothing.
s32 sceNpSignalingTerminateConnection(PPUContext&, u32 ctx_id, u32 conn_id)
{
    if (!np().initialized) return error(SCE_NP_SIGNALING_ERROR_NOT_INITIALIZED);
    PS_LOG_DEBUG(kChan, "sceNpSignalingTerminateConnection(context {:#x}, connection {:#x}): no such connection",
        ctx_id, conn_id);
    return lv2::CELL_OK;
}

// RPCS3 sceNpSignalingDeactivateConnection (signaling_handler::stop_sig): stopping a connection that doesn't exist does
// nothing, as for sceNpSignalingTerminateConnection.
s32 sceNpSignalingDeactivateConnection(PPUContext&, u32 ctx_id, u32 conn_id)
{
    if (!np().initialized) return error(SCE_NP_SIGNALING_ERROR_NOT_INITIALIZED);
    PS_LOG_DEBUG(kChan, "sceNpSignalingDeactivateConnection(context {:#x}, connection {:#x}): no such connection",
        ctx_id, conn_id);
    return lv2::CELL_OK;
}

// RPCS3 sceNpSignalingGetCtxOpt / sceNpSignalingSetCtxOpt: TODOs that check the option, its value (non-zero) and the
// context, then answer CELL_OK; the getter writes nothing.
s32 signaling_ctx_opt(std::string_view name, u32 ctx_id, s32 optname, u32 optval)
{
    NpState& s = np();
    if (!s.initialized) return error(SCE_NP_SIGNALING_ERROR_NOT_INITIALIZED);
    if (!optname || !optval) return error(SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT);
    {
        std::lock_guard lock(s.mutex);
        if (!s.signaling_ctxs.find(ctx_id)) return error(SCE_NP_SIGNALING_ERROR_CTX_NOT_FOUND);
    }
    PS_LOG_WARN(kChan, "{}(context {:#x}, option {}, {:#x}): a TODO in RPCS3; nothing is read or written", name, ctx_id,
        optname, optval);
    return lv2::CELL_OK;
}

s32 sceNpSignalingGetCtxOpt(PPUContext&, u32 ctx_id, s32 optname, u32 optval)
{
    return signaling_ctx_opt("sceNpSignalingGetCtxOpt", ctx_id, optname, optval);
}

s32 sceNpSignalingSetCtxOpt(PPUContext&, u32 ctx_id, s32 optname, s32 optval)
{
    return signaling_ctx_opt("sceNpSignalingSetCtxOpt", ctx_id, optname, static_cast<u32>(optval));
}

// RPCS3 sceNpSignalingGetPeerNetInfo, sceNpSignalingGetPeerNetInfoResult and sceNpSignalingCancelPeerNetInfo: TODOs
// that check NP and their pointers and answer CELL_OK, writing nothing (no request ID, no result).
s32 sceNpSignalingGetPeerNetInfo(PPUContext&, u32 ctx_id, u32 np_id, u32 req_id)
{
    if (!np().initialized) return error(SCE_NP_SIGNALING_ERROR_NOT_INITIALIZED);
    if (!np_id || !req_id) return error(SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT);
    PS_LOG_WARN(kChan, "sceNpSignalingGetPeerNetInfo(context {:#x}): a TODO in RPCS3; no request is made", ctx_id);
    return lv2::CELL_OK;
}

s32 sceNpSignalingGetPeerNetInfoResult(PPUContext&, u32 ctx_id, u32 req_id, u32 info)
{
    if (!np().initialized) return error(SCE_NP_SIGNALING_ERROR_NOT_INITIALIZED);
    if (!info) return error(SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT); // RPCS3: "TODO: check info->size"
    PS_LOG_WARN(kChan, "sceNpSignalingGetPeerNetInfoResult(context {:#x}, request {:#x}): a TODO in RPCS3; nothing "
                       "written", ctx_id, req_id);
    return lv2::CELL_OK;
}

s32 sceNpSignalingCancelPeerNetInfo(PPUContext&, u32 ctx_id, u32 req_id)
{
    if (!np().initialized) return error(SCE_NP_SIGNALING_ERROR_NOT_INITIALIZED);
    PS_LOG_DEBUG(kChan, "sceNpSignalingCancelPeerNetInfo(context {:#x}, request {:#x})", ctx_id, req_id);
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------------- matching

// The matching library (RPCS3 sceNpMatching*, np_requests_gui.cpp, np_contexts.cpp), the older lobbies and rooms. Its
// context works offline; every request goes to RPCN, which RPCS3 needs there and without which it aborts ("RPCN is
// required to use PSN online features"). pspocket answers instead, as a console without PSN would leave the game:
// - A GUI call (create, join, search and join, quick match, room list) makes RPCS3's checks, marks the context busy and
//   queues COMMON_LOAD for its handler (gui_prelude); then, with no dialog (dialogs are answered at once), its event
//   with SCE_NP_MATCHING_ERROR_CANCELED and COMMON_UNLOAD (gui_epilog), through cellSysutilCheckCallback.
// - A room request (leave, room info, search flag) after its checks finds no room: SCE_NP_MATCHING_ERROR_ROOM_NOT_FOUND
//   (no room is ever joined), and no request ID is written.
// - RPCS3's TODOs (ownership, kicks, invitations) answer CELL_OK after their checks, as there.
// Guest lists (attributes, conditions) are read with RPCS3's checks; memory that isn't guest memory, or a list longer
// than 256 entries (a cycle would hang RPCS3), is refused with SCE_NP_MATCHING_ERROR_INVALID_ARG and a log.

// RPCS3's first checks of most matching calls: matching 2 not in use, then NP.
s32 matching_check(bool need_np)
{
    if (np_match2_initialized() || np().matching2_running) return error(SCE_NP_MATCHING_ERROR_UTILITY_UNAVAILABLE);
    if (need_np && !np().initialized) return error(SCE_NP_MATCHING_ERROR_NOT_INITIALIZED);
    return lv2::CELL_OK;
}

struct MatchingAttr {
    s32 type = 0;
    u32 id = 0, num = 0, data_size = 0;
};

// The attribute list at `attr` (none when 0); nullopt when a node isn't guest memory or the list is too long (logged).
std::optional<std::vector<MatchingAttr>> read_matching_attrs(PPUContext& ctx, std::string_view name, u32 attr)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    std::vector<MatchingAttr> out;
    for (u32 at = attr; at; at = ps_read32(base, at)) {
        if (out.size() == kMatchingListMax || !guest_readable(t, at, kMatchingAttrSize)) {
            log_call_site(ctx, base, std::format("{}: the attribute list at {:#x} {} at {:#x}", name, attr,
                out.size() == kMatchingListMax ? "has no end" : "leaves guest memory", at));
            return std::nullopt;
        }
        out.push_back({static_cast<s32>(ps_read32(base, at + 4)), ps_read32(base, at + 8), ps_read32(base, at + 12),
            ps_read32(base, at + 16)});
    }
    return out;
}

// RPCS3 check_attr_id.
s32 check_attr_id(const MatchingAttr& a)
{
    switch (a.type) {
    case SCE_NP_MATCHING_ATTR_TYPE_BASIC_BIN: return error(SCE_NP_MATCHING_ERROR_INVALID_ATTR_ID);
    case SCE_NP_MATCHING_ATTR_TYPE_BASIC_NUM:
        if (a.id < SCE_NP_MATCHING_ROOM_ATTR_ID_TOTAL_SLOT || a.id > SCE_NP_MATCHING_ROOM_ATTR_ID_ROOM_SEARCH_FLAG) {
            return error(SCE_NP_MATCHING_ERROR_INVALID_ATTR_ID);
        }
        break;
    case SCE_NP_MATCHING_ATTR_TYPE_GAME_BIN:
    case SCE_NP_MATCHING_ATTR_TYPE_GAME_NUM:
        if (a.id < 1 || a.id > 16) return error(SCE_NP_MATCHING_ERROR_INVALID_ATTR_ID);
        break;
    }
    return lv2::CELL_OK;
}

// RPCS3 check_duplicate_attr: no type and ID twice.
s32 check_duplicate_attr(const std::vector<MatchingAttr>& attrs)
{
    std::vector<s32> seen;
    for (const MatchingAttr& a : attrs) {
        const s32 type_id = (a.type - 1) * 16 + static_cast<s32>(a.id) - 1;
        if (std::find(seen.begin(), seen.end(), type_id) != seen.end()) return error(SCE_NP_MATCHING_ERROR_DUPLICATE);
        seen.push_back(type_id);
    }
    return lv2::CELL_OK;
}

// RPCS3 check_attr_id_and_duplicate.
s32 check_attr_id_and_duplicate(const std::vector<MatchingAttr>& attrs)
{
    for (const MatchingAttr& a : attrs)
        if (const s32 r = check_attr_id(a)) return r;
    return check_duplicate_attr(attrs);
}

// RPCS3 check_attr_create_room: valid IDs, no current-count attributes, binary flags, game data within its size, no
// duplicates, then the slots: the first point where both the total and the private slot have been given decides.
s32 check_attr_create_room(const std::vector<MatchingAttr>& attrs)
{
    for (const MatchingAttr& a : attrs) {
        if (const s32 r = check_attr_id(a)) return r;
        if (a.type == SCE_NP_MATCHING_ATTR_TYPE_BASIC_NUM) {
            if (a.id >= SCE_NP_MATCHING_ROOM_ATTR_ID_CUR_TOTAL_NUM && a.id <= SCE_NP_MATCHING_ROOM_ATTR_ID_CUR_PRIVATE_NUM) {
                return error(SCE_NP_MATCHING_ERROR_INVALID_ATTR_ID);
            }
            if (a.num > 1 && (a.id == SCE_NP_MATCHING_ROOM_ATTR_ID_PRIVILEGE_TYPE ||
                                 a.id == SCE_NP_MATCHING_ROOM_ATTR_ID_ROOM_SEARCH_FLAG)) {
                return error(SCE_NP_MATCHING_ERROR_INVALID_ATTR_VALUE);
            }
        } else if (a.type == SCE_NP_MATCHING_ATTR_TYPE_GAME_BIN) {
            if (a.data_size > (a.id < 3 ? 256u : 64u)) return error(SCE_NP_MATCHING_ERROR_INVALID_ATTR);
        }
    }
    if (const s32 r = check_duplicate_attr(attrs)) return r;
    bool found_total = false, found_private = false;
    u32 total = 0, priv = 0;
    for (const MatchingAttr& a : attrs) {
        if (a.type != SCE_NP_MATCHING_ATTR_TYPE_BASIC_NUM) continue;
        if (a.id == SCE_NP_MATCHING_ROOM_ATTR_ID_TOTAL_SLOT) {
            total = a.num;
            found_total = true;
        } else if (a.id == SCE_NP_MATCHING_ROOM_ATTR_ID_PRIVATE_SLOT) {
            priv = a.num;
            found_private = true;
        }
        if (found_total && found_private) {
            if (total > 0 && total <= 16 && static_cast<s32>(priv) >= 0 && static_cast<s32>(total - priv) > 0) {
                return lv2::CELL_OK;
            }
            return error(SCE_NP_MATCHING_ERROR_INVALID_ARG);
        }
    }
    return error(SCE_NP_MATCHING_ERROR_ATTR_NOT_SPECIFIED);
}

struct MatchingCond {
    s32 attr_type = 0;
    u32 attr_id = 0;
    s32 comp_op = 0, comp_type = 0;
};

// The search conditions at `cond`, at most `limit` + 1 of them (enough for every check: each call refuses more than
// `limit` with SCE_NP_MATCHING_ERROR_COND_MAX); nullopt when a node isn't guest memory (logged).
std::optional<std::vector<MatchingCond>> read_matching_conds(PPUContext& ctx, std::string_view name, u32 cond,
    u32 limit)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    std::vector<MatchingCond> out;
    for (u32 at = cond; at && out.size() <= limit; at = ps_read32(base, at)) {
        if (!guest_readable(t, at, kMatchingCondSize)) {
            log_call_site(ctx, base, std::format("{}: the condition list at {:#x} leaves guest memory at {:#x}", name,
                                         cond, at));
            return std::nullopt;
        }
        out.push_back({static_cast<s32>(ps_read32(base, at + 4)), ps_read32(base, at + 8),
            static_cast<s32>(ps_read32(base, at + 12)), static_cast<s32>(ps_read32(base, at + 16))});
    }
    return out;
}

// The conditions of a room search (RPCS3 sceNpMatchingSearchJoinRoomGUI and check_room_list_params with param_7 1):
// at most 9; value comparisons EQ to GE, at most 9 of them inequalities; numeric attributes only: for a search and
// join, only the total slots among the basic ones, for a room list any basic one but the total slots; game numbers
// 1 to 8.
s32 check_search_conds(const std::vector<MatchingCond>& conds, bool room_list)
{
    constexpr u32 kMax = 9;
    u32 total = 0, inequalities = 0;
    for (const MatchingCond& c : conds) {
        if (++total > kMax) return error(SCE_NP_MATCHING_ERROR_COND_MAX);
        if (c.comp_type != SCE_NP_MATCHING_CONDITION_TYPE_VALUE || c.comp_op < SCE_NP_MATCHING_CONDITION_SEARCH_EQ ||
            c.comp_op > SCE_NP_MATCHING_CONDITION_SEARCH_GE) {
            return error(SCE_NP_MATCHING_ERROR_INVALID_COND);
        }
        if (c.comp_op > SCE_NP_MATCHING_CONDITION_SEARCH_NE && ++inequalities > kMax) {
            return error(SCE_NP_MATCHING_ERROR_COMP_OP_INEQUALITY_MAX);
        }
        switch (c.attr_type) {
        case SCE_NP_MATCHING_ATTR_TYPE_BASIC_BIN:
        case SCE_NP_MATCHING_ATTR_TYPE_GAME_BIN: return error(SCE_NP_MATCHING_ERROR_INVALID_COND);
        case SCE_NP_MATCHING_ATTR_TYPE_BASIC_NUM:
            if (room_list ? c.attr_id == SCE_NP_MATCHING_ROOM_ATTR_ID_TOTAL_SLOT
                          : c.attr_id != SCE_NP_MATCHING_ROOM_ATTR_ID_TOTAL_SLOT) {
                return error(SCE_NP_MATCHING_ERROR_INVALID_COND);
            }
            break;
        case SCE_NP_MATCHING_ATTR_TYPE_GAME_NUM:
            if (c.attr_id < 1 || c.attr_id > 8) return error(SCE_NP_MATCHING_ERROR_INVALID_COND);
            break;
        }
    }
    return lv2::CELL_OK;
}

// RPCS3 gui_prelude, then the offline end of the GUI call: the context must exist and not be busy; its handler gets
// COMMON_LOAD, `event` with SCE_NP_MATCHING_ERROR_CANCELED, and COMMON_UNLOAD, each through cellSysutilCheckCallback
// (SceNpMatchingGUIHandler: ctx_id, event, error, arg).
s32 matching_gui_offline(std::string_view name, u32 ctx_id, s32 event, u32 handler, u32 arg)
{
    NpState& s = np();
    {
        std::lock_guard lock(s.mutex);
        MatchingCtx* c = s.matching_ctxs.find(ctx_id);
        if (!c) return error(SCE_NP_MATCHING_ERROR_CTX_NOT_FOUND);
        if (c->busy) return error(SCE_NP_MATCHING_ERROR_CTX_STILL_RUNNING);
        c->gui_handler = handler;
        c->gui_arg = arg;
    }
    const auto queue = [=](s32 ev, s32 code) {
        sysutil_queue_call([=](PPUContext& cb_ctx) -> s32 {
            call_guest(cb_ctx, handler, {ctx_id, static_cast<u64>(static_cast<s64>(ev)),
                static_cast<u64>(static_cast<s64>(code)), arg});
            return lv2::CELL_OK;
        });
    };
    queue(SCE_NP_MATCHING_GUI_EVENT_COMMON_LOAD, 0);
    queue(event, error(SCE_NP_MATCHING_ERROR_CANCELED));
    queue(SCE_NP_MATCHING_GUI_EVENT_COMMON_UNLOAD, 0);
    PS_LOG_INFO(kChan, "{}(context {:#x}): offline, no dialog; the handler {:#x} hears it was cancelled", name, ctx_id,
        handler);
    return lv2::CELL_OK;
}

// RPCS3 sceNpMatchingCreateCtx: the one matching context (0x9001), with the user's NP ID and the request handler; it
// becomes the context GUI results are reported on. RPCS3 reads the NP ID unchecked: one that isn't guest memory is
// refused here with a log.
s32 sceNpMatchingCreateCtx(PPUContext& ctx, u32 np_id, u32 handler, u32 arg, u32 ctx_id)
{
    if (const s32 r = matching_check(true)) return r;
    if (!np_id || !handler || !ctx_id) return error(SCE_NP_MATCHING_ERROR_INVALID_ARG);
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    if (!guest_readable(t, np_id, kNpIdSize) || !guest_readable(t, ctx_id, 4)) {
        log_call_site(ctx, base, std::format("sceNpMatchingCreateCtx: the NP ID {:#x} or the result's address {:#x} is "
                                             "not guest memory", np_id, ctx_id));
        return error(SCE_NP_MATCHING_ERROR_INVALID_ARG);
    }
    MatchingCtx c;
    std::memcpy(c.npid.data(), base + np_id, kNpIdSize);
    c.handler = handler;
    c.arg = arg;
    NpState& s = np();
    std::lock_guard lock(s.mutex);
    const u32 id = s.matching_ctxs.make(c);
    if (!id) return error(SCE_NP_MATCHING_ERROR_CTX_MAX);
    s.matching_gui_ctx = id;
    ps_write32(base, ctx_id, id);
    PS_LOG_INFO(kChan, "sceNpMatchingCreateCtx(handler {:#x}, arg {:#x}): context {:#x}; offline its requests find no "
                       "room", handler, arg, id);
    return lv2::CELL_OK;
}

s32 sceNpMatchingDestroyCtx(PPUContext&, u32 ctx_id)
{
    if (const s32 r = matching_check(true)) return r;
    NpState& s = np();
    std::lock_guard lock(s.mutex);
    if (!s.matching_ctxs.remove(ctx_id)) return error(SCE_NP_MATCHING_ERROR_CTX_NOT_FOUND);
    s.matching_gui_ctx = 0;
    return lv2::CELL_OK;
}

// RPCS3 sceNpMatchingGetResult (np_handler::get_matching_result): the notification a request left on the GUI's
// context. Offline no request is ever sent, so none is found.
s32 sceNpMatchingGetResult(PPUContext&, u32 ctx_id, u32 req_id, u32 buf, u32 size, u32 event)
{
    if (const s32 r = matching_check(true)) return r;
    if (!size) return error(SCE_NP_MATCHING_ERROR_INVALID_ARG);
    NpState& s = np();
    std::lock_guard lock(s.mutex);
    if (!s.matching_gui_ctx || !s.matching_ctxs.find(ctx_id)) return error(SCE_NP_MATCHING_ERROR_CTX_NOT_FOUND);
    PS_LOG_DEBUG(kChan, "sceNpMatchingGetResult(context {:#x}, request {:#x}, buf {:#x}, event {:#x}): no such request",
        ctx_id, req_id, buf, event);
    return error(SCE_NP_MATCHING_ERROR_INVALID_REQ_ID);
}

// RPCS3 sceNpMatchingGetResultGUI (np_handler::get_result_gui): the last GUI call's result. Offline every GUI call is
// cancelled, so there is none. NP isn't checked.
s32 sceNpMatchingGetResultGUI(PPUContext&, u32 buf, u32 size, u32 event)
{
    if (const s32 r = matching_check(false)) return r;
    if (!size) return error(SCE_NP_MATCHING_ERROR_INVALID_ARG);
    PS_LOG_DEBUG(kChan, "sceNpMatchingGetResultGUI(buf {:#x}, event {:#x}): no result", buf, event);
    return error(SCE_NP_MATCHING_ERROR_RESULT_NOT_FOUND);
}

// RPCS3 matching_create_room (sceNpMatchingCreateRoomGUI, and sceNpMatchingInt's sceNpMatchingCreateRoomWithoutGUI):
// the communication ID, the handler and the room's attributes (check_attr_create_room); NP isn't checked.
s32 matching_create_room(PPUContext& ctx, std::string_view name, u32 ctx_id, u32 comm_id, u32 attr, u32 handler,
    u32 arg)
{
    if (const s32 r = matching_check(false)) return r;
    if (!comm_id || !handler) return error(SCE_NP_MATCHING_ERROR_INVALID_ARG);
    const std::optional<std::vector<MatchingAttr>> attrs = read_matching_attrs(ctx, name, attr);
    if (!attrs) return error(SCE_NP_MATCHING_ERROR_INVALID_ARG);
    if (const s32 r = check_attr_create_room(*attrs)) return r;
    return matching_gui_offline(name, ctx_id, SCE_NP_MATCHING_GUI_EVENT_CREATE_ROOM, handler, arg);
}

s32 sceNpMatchingCreateRoomGUI(PPUContext& ctx, u32 ctx_id, u32 comm_id, u32 attr, u32 handler, u32 arg)
{
    return matching_create_room(ctx, "sceNpMatchingCreateRoomGUI", ctx_id, comm_id, attr, handler, arg);
}

s32 sceNpMatchingCreateRoomWithoutGUI(PPUContext& ctx, u32 ctx_id, u32 comm_id, u32 attr, u32 handler, u32 arg)
{
    return matching_create_room(ctx, "sceNpMatchingCreateRoomWithoutGUI", ctx_id, comm_id, attr, handler, arg);
}

// RPCS3 matching_join_room (sceNpMatchingJoinRoomGUI, and sceNpMatchingInt's sceNpMatchingJoinRoomWithoutGUI and its
// own sceNpMatchingJoinRoomGUI): the room and the handler.
s32 matching_join_room(std::string_view name, u32 ctx_id, u32 room_id, u32 handler, u32 arg)
{
    if (const s32 r = matching_check(false)) return r;
    if (!room_id || !handler) return error(SCE_NP_MATCHING_ERROR_INVALID_ARG);
    return matching_gui_offline(name, ctx_id, SCE_NP_MATCHING_GUI_EVENT_JOIN_ROOM, handler, arg);
}

s32 sceNpMatchingJoinRoomGUI(PPUContext&, u32 ctx_id, u32 room_id, u32 handler, u32 arg)
{
    return matching_join_room("sceNpMatchingJoinRoomGUI", ctx_id, room_id, handler, arg);
}

s32 sceNpMatchingJoinRoomWithoutGUI(PPUContext&, u32 ctx_id, u32 room_id, u32 handler, u32 arg)
{
    return matching_join_room("sceNpMatchingJoinRoomWithoutGUI", ctx_id, room_id, handler, arg);
}

// RPCS3 sceNpMatchingQuickMatchGUI: the communication ID, the handler, at least 2 players and a timeout of at least
// 1 s, then the conditions: at most 9, EQ or NE, comp_type not 1 (as RPCS3 writes it: "weird"), game numbers 1 to 8
// only.
s32 sceNpMatchingQuickMatchGUI(PPUContext& ctx, u32 ctx_id, u32 comm_id, u32 cond, s32 available_num, s32 timeout,
    u32 handler, u32 arg)
{
    if (const s32 r = matching_check(false)) return r;
    if (!comm_id || !handler || available_num < 2 || timeout < 1) return error(SCE_NP_MATCHING_ERROR_INVALID_ARG);
    const std::optional<std::vector<MatchingCond>> conds = read_matching_conds(ctx, "sceNpMatchingQuickMatchGUI", cond, 9);
    if (!conds) return error(SCE_NP_MATCHING_ERROR_INVALID_ARG);
    u32 total = 0;
    for (const MatchingCond& c : *conds) {
        if (++total > 9) return error(SCE_NP_MATCHING_ERROR_COND_MAX);
        if (c.comp_op < SCE_NP_MATCHING_CONDITION_SEARCH_EQ || c.comp_op > SCE_NP_MATCHING_CONDITION_SEARCH_NE) {
            return error(SCE_NP_MATCHING_ERROR_INVALID_COMP_OP);
        }
        if (c.comp_type == 1) return error(SCE_NP_MATCHING_ERROR_INVALID_COMP_TYPE);
        switch (c.attr_type) {
        case SCE_NP_MATCHING_ATTR_TYPE_BASIC_BIN:
        case SCE_NP_MATCHING_ATTR_TYPE_BASIC_NUM:
        case SCE_NP_MATCHING_ATTR_TYPE_GAME_BIN: return error(SCE_NP_MATCHING_ERROR_INVALID_COND);
        case SCE_NP_MATCHING_ATTR_TYPE_GAME_NUM:
            if (c.attr_id < 1 || c.attr_id > 8) return error(SCE_NP_MATCHING_ERROR_INVALID_COND);
            break;
        }
    }
    PS_LOG_DEBUG(kChan, "sceNpMatchingQuickMatchGUI: {} players, timeout {} s", available_num, timeout);
    return matching_gui_offline("sceNpMatchingQuickMatchGUI", ctx_id, SCE_NP_MATCHING_GUI_EVENT_QUICK_MATCH, handler,
        arg);
}

// RPCS3 sceNpMatchingSearchJoinRoomGUI: the communication ID and the handler, the conditions (check_search_conds),
// then the attributes asked for (valid IDs, no duplicates).
s32 sceNpMatchingSearchJoinRoomGUI(PPUContext& ctx, u32 ctx_id, u32 comm_id, u32 cond, u32 attr, u32 handler, u32 arg)
{
    if (const s32 r = matching_check(false)) return r;
    if (!comm_id || !handler) return error(SCE_NP_MATCHING_ERROR_INVALID_ARG);
    const std::optional<std::vector<MatchingCond>> conds =
        read_matching_conds(ctx, "sceNpMatchingSearchJoinRoomGUI", cond, 9);
    if (!conds) return error(SCE_NP_MATCHING_ERROR_INVALID_ARG);
    if (const s32 r = check_search_conds(*conds, false)) return r;
    const std::optional<std::vector<MatchingAttr>> attrs =
        read_matching_attrs(ctx, "sceNpMatchingSearchJoinRoomGUI", attr);
    if (!attrs) return error(SCE_NP_MATCHING_ERROR_INVALID_ARG);
    if (const s32 r = check_attr_id_and_duplicate(*attrs)) return r;
    return matching_gui_offline("sceNpMatchingSearchJoinRoomGUI", ctx_id, SCE_NP_MATCHING_GUI_EVENT_SEARCH_JOIN, handler,
        arg);
}

// RPCS3 matching_get_room_list (check_room_list_params with param_7 1, param_8 0): the communication ID, the range (a
// start, at most 20) and the handler, the conditions, then the attributes. The limited form
// (sceNpMatchingGetRoomListLimitGUI) answers with GET_ROOM_LIST_LIMIT, sceNpMatchingInt's sceNpMatchingGetRoomListGUI
// and sceNpMatchingGetRoomListWithoutGUI with GET_ROOM_LIST (np_handler::reply_get_room_list_gui).
s32 matching_get_room_list(PPUContext& ctx, std::string_view name, bool limit, u32 ctx_id, u32 comm_id, u32 range,
    u32 cond, u32 attr, u32 handler, u32 arg)
{
    if (const s32 r = matching_check(false)) return r;
    if (!comm_id || !range || !handler) return error(SCE_NP_MATCHING_ERROR_INVALID_ARG);
    lv2::PpuThread& t = thread_of(ctx);
    if (!guest_readable(t, range, 8)) {
        log_call_site(ctx, t.kernel.base(), std::format("{}: the range {:#x} is not guest memory", name, range));
        return error(SCE_NP_MATCHING_ERROR_INVALID_ARG);
    }
    if (!ps_read32(t.kernel.base(), range) || ps_read32(t.kernel.base(), range + 4) > 20) {
        return error(SCE_NP_MATCHING_ERROR_INVALID_ARG);
    }
    const std::optional<std::vector<MatchingCond>> conds = read_matching_conds(ctx, name, cond, 9);
    if (!conds) return error(SCE_NP_MATCHING_ERROR_INVALID_ARG);
    if (const s32 r = check_search_conds(*conds, true)) return r;
    const std::optional<std::vector<MatchingAttr>> attrs = read_matching_attrs(ctx, name, attr);
    if (!attrs) return error(SCE_NP_MATCHING_ERROR_INVALID_ARG);
    if (const s32 r = check_attr_id_and_duplicate(*attrs)) return r;
    return matching_gui_offline(name, ctx_id,
        limit ? SCE_NP_MATCHING_GUI_EVENT_GET_ROOM_LIST_LIMIT : SCE_NP_MATCHING_GUI_EVENT_GET_ROOM_LIST, handler, arg);
}

s32 sceNpMatchingGetRoomListLimitGUI(PPUContext& ctx, u32 ctx_id, u32 comm_id, u32 range, u32 cond, u32 attr,
    u32 handler, u32 arg)
{
    return matching_get_room_list(ctx, "sceNpMatchingGetRoomListLimitGUI", true, ctx_id, comm_id, range, cond, attr,
        handler, arg);
}

s32 sceNpMatchingGetRoomListGUI(PPUContext& ctx, u32 ctx_id, u32 comm_id, u32 range, u32 cond, u32 attr, u32 handler,
    u32 arg)
{
    return matching_get_room_list(ctx, "sceNpMatchingGetRoomListGUI", false, ctx_id, comm_id, range, cond, attr,
        handler, arg);
}

s32 sceNpMatchingGetRoomListWithoutGUI(PPUContext& ctx, u32 ctx_id, u32 comm_id, u32 range, u32 cond, u32 attr,
    u32 handler, u32 arg)
{
    return matching_get_room_list(ctx, "sceNpMatchingGetRoomListWithoutGUI", false, ctx_id, comm_id, range, cond, attr,
        handler, arg);
}

// The offline end of a room request (np_handler's *_gui requests): the context must exist; then no room is found.
s32 matching_room_offline(std::string_view name, u32 ctx_id)
{
    NpState& s = np();
    {
        std::lock_guard lock(s.mutex);
        if (!s.matching_ctxs.find(ctx_id)) return error(SCE_NP_MATCHING_ERROR_CTX_NOT_FOUND);
    }
    PS_LOG_INFO(kChan, "{}(context {:#x}): offline no room is ever joined: room not found", name, ctx_id);
    return error(SCE_NP_MATCHING_ERROR_ROOM_NOT_FOUND);
}

// RPCS3 matching_get_room_info / matching_set_room_info: the limited forms let one call through per 30 s (before any
// other check; SCE_NP_MATCHING_ERROR_BUSY meanwhile); then the lobby, room, request ID and attributes, the attributes'
// IDs, and the context.
s32 matching_room_info(PPUContext& ctx, std::string_view name, u64 NpState::*last_call, bool limit, u32 ctx_id,
    u32 lobby_id, u32 room_id, u32 attr, u32 req_id)
{
    if (const s32 r = matching_check(true)) return r;
    if (limit) {
        NpState& s = np();
        std::lock_guard lock(s.mutex);
        const u64 now = static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        if (s.*last_call != 0 && now - s.*last_call < kRoomInfoInterval) return error(SCE_NP_MATCHING_ERROR_BUSY);
        s.*last_call = now;
    }
    if (!lobby_id || !room_id || !req_id || !attr) return error(SCE_NP_MATCHING_ERROR_INVALID_ARG);
    const std::optional<std::vector<MatchingAttr>> attrs = read_matching_attrs(ctx, name, attr);
    if (!attrs) return error(SCE_NP_MATCHING_ERROR_INVALID_ARG);
    if (const s32 r = check_attr_id_and_duplicate(*attrs)) return r;
    return matching_room_offline(name, ctx_id);
}

s32 sceNpMatchingGetRoomInfo(PPUContext& ctx, u32 ctx_id, u32 lobby_id, u32 room_id, u32 attr, u32 req_id)
{
    return matching_room_info(ctx, "sceNpMatchingGetRoomInfo", &NpState::last_get_room_info, true, ctx_id, lobby_id,
        room_id, attr, req_id);
}

s32 sceNpMatchingGetRoomInfoNoLimit(PPUContext& ctx, u32 ctx_id, u32 lobby_id, u32 room_id, u32 attr, u32 req_id)
{
    return matching_room_info(ctx, "sceNpMatchingGetRoomInfoNoLimit", &NpState::last_get_room_info, false, ctx_id,
        lobby_id, room_id, attr, req_id);
}

s32 sceNpMatchingSetRoomInfo(PPUContext& ctx, u32 ctx_id, u32 lobby_id, u32 room_id, u32 attr, u32 req_id)
{
    return matching_room_info(ctx, "sceNpMatchingSetRoomInfo", &NpState::last_set_room_info, true, ctx_id, lobby_id,
        room_id, attr, req_id);
}

s32 sceNpMatchingSetRoomInfoNoLimit(PPUContext& ctx, u32 ctx_id, u32 lobby_id, u32 room_id, u32 attr, u32 req_id)
{
    return matching_room_info(ctx, "sceNpMatchingSetRoomInfoNoLimit", &NpState::last_set_room_info, false, ctx_id,
        lobby_id, room_id, attr, req_id);
}

s32 sceNpMatchingGetRoomSearchFlag(PPUContext&, u32 ctx_id, u32 lobby_id, u32 room_id, u32 req_id)
{
    if (const s32 r = matching_check(true)) return r;
    if (!lobby_id || !room_id || !req_id) return error(SCE_NP_MATCHING_ERROR_INVALID_ARG);
    return matching_room_offline("sceNpMatchingGetRoomSearchFlag", ctx_id);
}

s32 sceNpMatchingSetRoomSearchFlag(PPUContext&, u32 ctx_id, u32 lobby_id, u32 room_id, s32 flag, u32 req_id)
{
    if (const s32 r = matching_check(true)) return r;
    if (!lobby_id || !room_id || !req_id || flag < SCE_NP_MATCHING_ROOM_SEARCH_FLAG_OPEN ||
        flag > SCE_NP_MATCHING_ROOM_SEARCH_FLAG_STEALTH) {
        return error(SCE_NP_MATCHING_ERROR_INVALID_ARG);
    }
    return matching_room_offline("sceNpMatchingSetRoomSearchFlag", ctx_id);
}

// RPCS3 sceNpMatchingLeaveRoom: the context, then (RPCS3 ensure()s it) a room.
s32 sceNpMatchingLeaveRoom(PPUContext& ctx, u32 ctx_id, u32 room_id, u32 req_id)
{
    if (const s32 r = matching_check(true)) return r;
    NpState& s = np();
    {
        std::lock_guard lock(s.mutex);
        if (!s.matching_ctxs.find(ctx_id)) return error(SCE_NP_MATCHING_ERROR_CTX_NOT_FOUND);
    }
    if (!room_id) {
        log_call_site(ctx, guest_base(ctx), "sceNpMatchingLeaveRoom: no room (RPCS3 aborts)");
        return error(SCE_NP_MATCHING_ERROR_INVALID_ARG);
    }
    PS_LOG_DEBUG(kChan, "sceNpMatchingLeaveRoom: request ID's address {:#x}", req_id);
    return matching_room_offline("sceNpMatchingLeaveRoom", ctx_id);
}

// RPCS3 matching_get_room_member_list (sceNpMatchingGetRoomMemberListLocal, and sceNpMatchingInt's
// sceNpMatchingGetRoomMemberList), np_handler::get_room_member_list_local_gui: the members of a room in the GUI cache,
// which only RPCN's replies fill: none offline.
s32 matching_get_room_member_list(std::string_view name, u32 ctx_id, u32 room_id, u32 buflen, u32 buf)
{
    if (const s32 r = matching_check(true)) return r;
    if (!buflen) return error(SCE_NP_MATCHING_ERROR_INVALID_ARG);
    NpState& s = np();
    {
        std::lock_guard lock(s.mutex);
        if (!s.matching_ctxs.find(ctx_id)) return error(SCE_NP_MATCHING_ERROR_CTX_NOT_FOUND);
    }
    PS_LOG_DEBUG(kChan, "{}(context {:#x}, room {:#x}, buf {:#x}): no room", name, ctx_id, room_id, buf);
    return error(SCE_NP_MATCHING_ERROR_ROOM_NOT_FOUND);
}

s32 sceNpMatchingGetRoomMemberListLocal(PPUContext&, u32 ctx_id, u32 room_id, u32 buflen, u32 buf)
{
    return matching_get_room_member_list("sceNpMatchingGetRoomMemberListLocal", ctx_id, room_id, buflen, buf);
}

s32 sceNpMatchingGetRoomMemberList(PPUContext&, u32 ctx_id, u32 room_id, u32 buflen, u32 buf)
{
    return matching_get_room_member_list("sceNpMatchingGetRoomMemberList", ctx_id, room_id, buflen, buf);
}

// RPCS3 sceNpMatchingGrantOwnership, sceNpMatchingKickRoomMember(WithOpt): TODOs that answer CELL_OK after checking
// their pointers (and the option's length, at most 16), with no request made or ID written.
s32 matching_todo(std::string_view name, u32 ctx_id)
{
    PS_LOG_WARN(kChan, "{}(context {:#x}): a TODO in RPCS3, answered CELL_OK as there; nothing is sent", name, ctx_id);
    return lv2::CELL_OK;
}

s32 sceNpMatchingGrantOwnership(PPUContext&, u32 ctx_id, u32 room_id, u32 user_id, u32 req_id)
{
    if (const s32 r = matching_check(true)) return r;
    if (!room_id || !user_id || !req_id) return error(SCE_NP_MATCHING_ERROR_INVALID_ARG);
    return matching_todo("sceNpMatchingGrantOwnership", ctx_id);
}

s32 sceNpMatchingKickRoomMember(PPUContext&, u32 ctx_id, u32 room_id, u32 user_id, u32 req_id)
{
    if (const s32 r = matching_check(true)) return r;
    if (!room_id || !user_id || !req_id) return error(SCE_NP_MATCHING_ERROR_INVALID_ARG);
    return matching_todo("sceNpMatchingKickRoomMember", ctx_id);
}

s32 sceNpMatchingKickRoomMemberWithOpt(PPUContext&, u32 ctx_id, u32 room_id, u32 user_id, u32 opt, s32 opt_len,
    u32 req_id)
{
    if (const s32 r = matching_check(true)) return r;
    if (!room_id || !user_id || !req_id) return error(SCE_NP_MATCHING_ERROR_INVALID_ARG);
    if (opt && (opt_len < 0 || opt_len > 16)) return error(SCE_NP_MATCHING_ERROR_INVALID_ARG);
    return matching_todo("sceNpMatchingKickRoomMemberWithOpt", ctx_id);
}

// RPCS3 sceNpMatchingAcceptInvitationGUI: a TODO after its checks; no dialog, the handler isn't called. NP isn't
// checked.
s32 sceNpMatchingAcceptInvitationGUI(PPUContext&, u32 ctx_id, u32 comm_id, u32 container_id, u32 handler, u32 arg)
{
    if (const s32 r = matching_check(false)) return r;
    if (!comm_id || !handler) return error(SCE_NP_MATCHING_ERROR_INVALID_ARG);
    PS_LOG_DEBUG(kChan, "sceNpMatchingAcceptInvitationGUI: container {:#x}, handler {:#x}, arg {:#x}", container_id,
        handler, arg);
    return matching_todo("sceNpMatchingAcceptInvitationGUI", ctx_id);
}

// RPCS3 sceNpMatchingSendInvitationGUI: the room, the communication ID, the texts; the recipients (1 to 12 when
// given); a subject of at most 16 characters and a body of at most 128 (RPCS3's check_text); a public or private
// slot. Then a TODO: no dialog, the handler isn't called. RPCS3 reads the texts unchecked: ones that aren't guest
// memory are refused here with a log.
s32 sceNpMatchingSendInvitationGUI(PPUContext& ctx, u32 ctx_id, u32 room_id, u32 comm_id, u32 dsts, s32 num,
    s32 slot_type, u32 subject, u32 body, u32 container_id, u32 handler, u32 arg)
{
    if (const s32 r = matching_check(false)) return r;
    if (!room_id || !comm_id || !subject || !body) return error(SCE_NP_MATCHING_ERROR_INVALID_ARG);
    if (dsts && (num <= 0 || num > SCE_NP_MATCHING_INVITATION_DESTINATION_MAX)) {
        return error(SCE_NP_MATCHING_ERROR_INVALID_ARG);
    }
    lv2::PpuThread& t = thread_of(ctx);
    for (const auto& [text, max, what] : {std::tuple{subject, 16, "subject"}, std::tuple{body, 128, "body"}}) {
        const std::optional<s32> n = check_text(t, text);
        if (!n) {
            log_call_site(ctx, t.kernel.base(), std::format("sceNpMatchingSendInvitationGUI: the {} at {:#x} runs out "
                                                            "of guest memory", what, text));
            return error(SCE_NP_MATCHING_ERROR_INVALID_ARG);
        }
        if (*n < 0) return *n;
        if (*n > max) return error(SCE_NP_MATCHING_ERROR_INVALID_ARG);
    }
    if (slot_type < SCE_NP_MATCHING_ROOM_SLOT_TYPE_PUBLIC || slot_type > SCE_NP_MATCHING_ROOM_SLOT_TYPE_PRIVATE) {
        return error(SCE_NP_MATCHING_ERROR_INVALID_ARG);
    }
    PS_LOG_DEBUG(kChan, "sceNpMatchingSendInvitationGUI: container {:#x}, handler {:#x}, arg {:#x}", container_id,
        handler, arg);
    return matching_todo("sceNpMatchingSendInvitationGUI", ctx_id);
}

// ------------------------------------------------------------------------------------------------- commerce

// The commerce library (version 1): RPCS3 sceNpCommerce*, nearly all TODOs. Its answers are RPCS3's: the checks it
// makes (versions, counts, sizes), then CELL_OK, mostly writing nothing (no context ID, no request ID, no count); the
// product category helpers fill what RPCS3 fills. No store is reached offline. RPCS3 writes the product category and
// info structs unchecked: ones that aren't guest memory are refused here with a log.

// Whether `size` bytes at `addr` are guest memory; if not, the call site is logged and false returned.
bool commerce_usable(PPUContext& ctx, std::string_view name, u32 addr, u32 size)
{
    lv2::PpuThread& t = thread_of(ctx);
    if (guest_readable(t, addr, size)) return true;
    log_call_site(ctx, t.kernel.base(), std::format("{}: {:#x} ({} bytes) is not guest memory", name, addr, size));
    return false;
}

s32 commerce_todo(std::string_view name)
{
    PS_LOG_WARN(kChan, "{}: a TODO in RPCS3, answered CELL_OK as there; offline no store is reached", name);
    return lv2::CELL_OK;
}

s32 sceNpCommerceCreateCtx(PPUContext&, u32 version, u32 np_id, u32 handler, u32 arg, u32 ctx_id)
{
    if (version != SCE_NP_COMMERCE_VERSION) return error(SCE_NP_COMMERCE_ERROR_UNSUPPORTED_VERSION);
    PS_LOG_DEBUG(kChan, "sceNpCommerceCreateCtx(NP ID {:#x}, handler {:#x}, arg {:#x}): no context ID written ({:#x})",
        np_id, handler, arg, ctx_id);
    return commerce_todo("sceNpCommerceCreateCtx");
}

s32 sceNpCommerceDestroyCtx(PPUContext&, u32 ctx_id)
{
    PS_LOG_DEBUG(kChan, "sceNpCommerceDestroyCtx({:#x})", ctx_id);
    return commerce_todo("sceNpCommerceDestroyCtx");
}

s32 sceNpCommerceGetProductCategoryStart(PPUContext&, u32 ctx_id, u32 category_id, s32 lang_code, u32 req_id)
{
    PS_LOG_DEBUG(kChan, "sceNpCommerceGetProductCategoryStart(context {:#x}, category {:#x}, language {}, request "
                        "{:#x})", ctx_id, category_id, lang_code, req_id);
    return commerce_todo("sceNpCommerceGetProductCategoryStart");
}

s32 sceNpCommerceGetProductCategoryFinish(PPUContext&, u32 req_id)
{
    PS_LOG_DEBUG(kChan, "sceNpCommerceGetProductCategoryFinish({:#x})", req_id);
    return commerce_todo("sceNpCommerceGetProductCategoryFinish");
}

s32 sceNpCommerceGetProductCategoryAbort(PPUContext&, u32 req_id)
{
    PS_LOG_DEBUG(kChan, "sceNpCommerceGetProductCategoryAbort({:#x})", req_id);
    return commerce_todo("sceNpCommerceGetProductCategoryAbort");
}

// RPCS3 sceNpCommerceGetProductCategoryResult: a buffer size and a fill size are needed; the result is empty.
s32 sceNpCommerceGetProductCategoryResult(PPUContext& ctx, u32 req_id, u32 buf, u32 buf_size, u32 fill_size)
{
    if (!buf_size || !fill_size) return error(SCE_NP_COMMERCE_ERROR_INSUFFICIENT_BUFFER);
    if (!commerce_usable(ctx, "sceNpCommerceGetProductCategoryResult", fill_size, 4)) {
        return error(SCE_NP_COMMERCE_ERROR_INSUFFICIENT_BUFFER);
    }
    ps_write32(guest_base(ctx), fill_size, 0);
    PS_LOG_DEBUG(kChan, "sceNpCommerceGetProductCategoryResult(request {:#x}, buf {:#x}): empty", req_id, buf);
    return lv2::CELL_OK;
}

// The product category helpers, as Sony's libsysutil_np (4.92) parses the store's data: plain functions over the game's
// memory, compared with Sony's (tests/hle/test_np_commerce_compare.cpp, D-921). The data (version 1): be32 version, be32
// status (a browse server error, 0 for none), the currency (8 bytes from +8: its code's three letters, then be32
// decimals), the category (760 bytes from +16), be32 child category count at +776, the child categories (760 bytes
// each) from +780, then be32 SKU count and the SKUs (1004 bytes each) after them. An info is {pc, data}: the record's
// address. Each info written NUL-terminates its record's strings in the data, as Sony's does. Sony's checks no pointer;
// one that is not guest memory is refused here with a log (the call's "not found" error, or 0).
constexpr u32 kCurrencyAt = 8, kCategoryAt = 16, kChildCountAt = 776, kChildrenAt = 780, kCategoryBytes = 760;
constexpr u32 kSkuBytes = 1004;
constexpr u32 kCategoryNuls[] = {759, 55, 247, 631};
constexpr u32 kSkuNuls[] = {999, 55, 103, 295, 359, 743, 871};

// The category's data and its size (pc->data, pc->dataSize), or nullopt when pc is not guest memory (logged).
struct PcData {
    u32 data, size;
};
std::optional<PcData> pc_data(PPUContext& ctx, std::string_view name, u32 pc)
{
    if (!commerce_usable(ctx, name, pc, 16)) return std::nullopt;
    return PcData{ps_read32(guest_base(ctx), pc + kPcData), ps_read32(guest_base(ctx), pc + kPcDataSize)};
}

// A be32 of the data, or nullopt when it is not guest memory (logged).
std::optional<u32> data32(PPUContext& ctx, std::string_view name, u32 addr)
{
    if (!commerce_usable(ctx, name, addr, 4)) return std::nullopt;
    return ps_read32(guest_base(ctx), addr);
}

// Writes info {pc, record} and NUL-terminates the record's strings at `nuls`.
bool write_info(PPUContext& ctx, std::string_view name, u32 info, u32 pc, u32 record, std::span<const u32> nuls)
{
    if (!commerce_usable(ctx, name, info, 8)) return false;
    for (const u32 n : nuls)
        if (!commerce_usable(ctx, name, record + n, 1)) return false;
    u8* base = guest_base(ctx);
    ps_write32(base, info, pc);
    ps_write32(base, info + 4, record);
    for (const u32 n : nuls) ps_write8(base, record + n, 0);
    return true;
}

s32 sceNpCommerceGetCurrencyInfo(PPUContext& ctx, u32 pc, u32 info)
{
    const auto d = pc_data(ctx, "sceNpCommerceGetCurrencyInfo", pc);
    if (!d || d->size <= 15) return error(SCE_NP_COMMERCE_ERROR_CURRENCY_INFO_NOT_FOUND);
    const u32 nul[] = {3};
    if (!write_info(ctx, "sceNpCommerceGetCurrencyInfo", info, pc, d->data + kCurrencyAt, nul)) {
        return error(SCE_NP_COMMERCE_ERROR_CURRENCY_INFO_NOT_FOUND);
    }
    return lv2::CELL_OK;
}

// sceNpCommerceInitProductCategory(pc, data, size): the data and size kept first; then the version (1), the status
// (a browse server error 0x80029600 | status, 0x80029600 past 255), the currency, whose decimals make dval (10 to their
// power in 32 bits, 0 for none: Sony multiplies that many times; done here by squaring).
s32 sceNpCommerceInitProductCategory(PPUContext& ctx, u32 pc, u32 data, u32 data_size)
{
    constexpr std::string_view kName = "sceNpCommerceInitProductCategory";
    if (!commerce_usable(ctx, kName, pc, 16)) return error(SCE_NP_COMMERCE_ERROR_PARSE_PRODUCT_CATEGORY);
    u8* base = guest_base(ctx);
    ps_write32(base, pc + kPcData, data);
    ps_write32(base, pc + kPcDataSize, data_size);
    if (data_size <= 3) return error(SCE_NP_COMMERCE_ERROR_PARSE_PRODUCT_CATEGORY);
    const auto version = data32(ctx, kName, data);
    if (!version) return error(SCE_NP_COMMERCE_ERROR_PARSE_PRODUCT_CATEGORY);
    ps_write32(base, pc + kPcVersion, *version);
    if (*version != SCE_NP_COMMERCE_VERSION) return error(SCE_NP_COMMERCE_ERROR_INVALID_GPC_PROTOCOL_VERSION);
    if (data_size <= 7) return error(SCE_NP_COMMERCE_ERROR_PARSE_PRODUCT_CATEGORY);
    const auto status = data32(ctx, kName, data + 4);
    if (!status) return error(SCE_NP_COMMERCE_ERROR_PARSE_PRODUCT_CATEGORY);
    if (*status) {
        return error(*status > 255 ? SCE_NP_COMMERCE_BROWSE_SERVER_ERROR_UNKNOWN
                                   : SCE_NP_COMMERCE_BROWSE_SERVER_ERROR_UNKNOWN | *status);
    }
    StackVar info(ctx, base, 16);
    if (sceNpCommerceGetCurrencyInfo(ctx, pc, info.addr()) < 0) return error(SCE_NP_COMMERCE_ERROR_PARSE_PRODUCT_CATEGORY);
    const auto decimals = data32(ctx, kName, ps_read32(base, info.addr() + 4) + 4);
    if (!decimals) return error(SCE_NP_COMMERCE_ERROR_PARSE_PRODUCT_CATEGORY);
    u32 dval = 0;
    if (*decimals) {
        dval = 1;
        u32 b = 10;
        for (u32 e = *decimals; e; e >>= 1, b *= b)
            if (e & 1) dval *= b;
    }
    ps_write32(base, pc + kPcDval, dval);
    return lv2::CELL_OK;
}

// sceNpCommerceDestroyProductCategory: forgets the data (its size, then its address).
void sceNpCommerceDestroyProductCategory(PPUContext& ctx, u32 pc)
{
    if (!commerce_usable(ctx, "sceNpCommerceDestroyProductCategory", pc, 16)) return;
    ps_write32(guest_base(ctx), pc + kPcDataSize, 0);
    ps_write32(guest_base(ctx), pc + kPcData, 0);
}

s32 sceNpCommerceGetCategoryInfo(PPUContext& ctx, u32 pc, u32 info)
{
    const auto d = pc_data(ctx, "sceNpCommerceGetCategoryInfo", pc);
    if (!d || d->size <= 775) return error(SCE_NP_COMMERCE_ERROR_CATEGORY_INFO_NOT_FOUND);
    if (!write_info(ctx, "sceNpCommerceGetCategoryInfo", info, pc, d->data + kCategoryAt, kCategoryNuls)) {
        return error(SCE_NP_COMMERCE_ERROR_CATEGORY_INFO_NOT_FOUND);
    }
    return lv2::CELL_OK;
}

// The record an info points at (info->data), or nullopt when the info is not guest memory (logged).
std::optional<u32> info_record(PPUContext& ctx, std::string_view name, u32 info)
{
    if (!commerce_usable(ctx, name, info, 8)) return std::nullopt;
    return ps_read32(guest_base(ctx), info + 4);
}

// sceNpCommerceGetCurrencyDecimals(info): the currency's be32 decimals.
u32 sceNpCommerceGetCurrencyDecimals(PPUContext& ctx, u32 info)
{
    const auto record = info_record(ctx, "sceNpCommerceGetCurrencyDecimals", info);
    if (!record) return 0;
    return data32(ctx, "sceNpCommerceGetCurrencyDecimals", *record + 4).value_or(0);
}

s32 sceNpCommerceGetNumOfChildCategory(PPUContext& ctx, u32 pc, u32 num)
{
    constexpr std::string_view kName = "sceNpCommerceGetNumOfChildCategory";
    const auto d = pc_data(ctx, kName, pc);
    if (!d || d->size <= 779) return error(SCE_NP_COMMERCE_ERROR_CHILD_CATEGORY_COUNT_NOT_FOUND);
    const auto count = data32(ctx, kName, d->data + kChildCountAt);
    if (!count || !commerce_usable(ctx, kName, num, 4)) return error(SCE_NP_COMMERCE_ERROR_CHILD_CATEGORY_COUNT_NOT_FOUND);
    ps_write32(guest_base(ctx), num, *count);
    return lv2::CELL_OK;
}

// sceNpCommerceGetNumOfChildProductSku(pc, num): the SKU count after the child categories, once the data reaches it
// (32-bit sizes, as Sony's compares them).
s32 sceNpCommerceGetNumOfChildProductSku(PPUContext& ctx, u32 pc, u32 num)
{
    constexpr std::string_view kName = "sceNpCommerceGetNumOfChildProductSku";
    const auto d = pc_data(ctx, kName, pc);
    if (!d || d->size <= 779) return error(SCE_NP_COMMERCE_ERROR_SKU_COUNT_NOT_FOUND);
    const auto children = data32(ctx, kName, d->data + kChildCountAt);
    if (!children) return error(SCE_NP_COMMERCE_ERROR_SKU_COUNT_NOT_FOUND);
    const u32 skip = *children * kCategoryBytes;
    if (d->size < skip + 784) return error(SCE_NP_COMMERCE_ERROR_SKU_COUNT_NOT_FOUND);
    const auto count = data32(ctx, kName, d->data + skip + kChildrenAt);
    if (!count || !commerce_usable(ctx, kName, num, 4)) return error(SCE_NP_COMMERCE_ERROR_SKU_COUNT_NOT_FOUND);
    ps_write32(guest_base(ctx), num, *count);
    return lv2::CELL_OK;
}

// sceNpCommerceGetChildCategoryInfo(pc, child, info): child `child`'s whole record must lie in the data.
s32 sceNpCommerceGetChildCategoryInfo(PPUContext& ctx, u32 pc, u32 child_index, u32 info)
{
    constexpr std::string_view kName = "sceNpCommerceGetChildCategoryInfo";
    const auto d = pc_data(ctx, kName, pc);
    const u32 at = child_index * kCategoryBytes;
    if (!d || d->size < at + kChildrenAt + kCategoryBytes) return error(SCE_NP_COMMERCE_ERROR_CHILD_CATEGORY_INFO_NOT_FOUND);
    if (!write_info(ctx, kName, info, pc, d->data + at + kChildrenAt, kCategoryNuls)) {
        return error(SCE_NP_COMMERCE_ERROR_CHILD_CATEGORY_INFO_NOT_FOUND);
    }
    return lv2::CELL_OK;
}

// sceNpCommerceGetChildProductSkuInfo(pc, sku, info): SKU `sku` after the child categories; Sony's checks only that its
// first four bytes lie in the data.
s32 sceNpCommerceGetChildProductSkuInfo(PPUContext& ctx, u32 pc, u32 child_index, u32 info)
{
    constexpr std::string_view kName = "sceNpCommerceGetChildProductSkuInfo";
    const auto d = pc_data(ctx, kName, pc);
    if (!d || d->size <= 779) return error(SCE_NP_COMMERCE_ERROR_SKU_INFO_NOT_FOUND);
    const auto children = data32(ctx, kName, d->data + kChildCountAt);
    if (!children) return error(SCE_NP_COMMERCE_ERROR_SKU_INFO_NOT_FOUND);
    const u32 at = *children * kCategoryBytes + child_index * kSkuBytes;
    if (d->size < at + 788) return error(SCE_NP_COMMERCE_ERROR_SKU_INFO_NOT_FOUND);
    if (!write_info(ctx, kName, info, pc, d->data + at + 784, kSkuNuls)) return error(SCE_NP_COMMERCE_ERROR_SKU_INFO_NOT_FOUND);
    return lv2::CELL_OK;
}

// sceNpCommerceGetSkuPrice(info, price): the SKU's be32 price at +1000 split by the category's dval (whole and
// fraction), or whole as it is when dval is 0.
void sceNpCommerceGetSkuPrice(PPUContext& ctx, u32 info, u32 price)
{
    constexpr std::string_view kName = "sceNpCommerceGetSkuPrice";
    if (!commerce_usable(ctx, kName, info, 8) || !commerce_usable(ctx, kName, price, 8)) return;
    u8* base = guest_base(ctx);
    const auto value = data32(ctx, kName, ps_read32(base, info + 4) + 1000);
    const auto dval = data32(ctx, kName, ps_read32(base, info) + kPcDval);
    if (!value || !dval) return;
    if (!*dval) {
        ps_write32(base, price + kPriceFractional, 0);
        ps_write32(base, price, *value);
        return;
    }
    ps_write32(base, price, *value / *dval);
    ps_write32(base, price + kPriceFractional, *value % *dval);
}

// The getters: the address of a field of the record an info points at (Sony's adds the field's offset to info->data).
u32 info_field(PPUContext& ctx, std::string_view name, u32 info, u32 offset)
{
    const auto record = info_record(ctx, name, info);
    return record ? *record + offset : 0;
}

u32 sceNpCommerceGetCurrencyCode(PPUContext& ctx, u32 info) { return info_field(ctx, "sceNpCommerceGetCurrencyCode", info, 0); }
u32 sceNpCommerceGetCategoryId(PPUContext& ctx, u32 info) { return info_field(ctx, "sceNpCommerceGetCategoryId", info, 0); }
u32 sceNpCommerceGetCategoryName(PPUContext& ctx, u32 info) { return info_field(ctx, "sceNpCommerceGetCategoryName", info, 56); }
u32 sceNpCommerceGetCategoryDescription(PPUContext& ctx, u32 info)
{
    return info_field(ctx, "sceNpCommerceGetCategoryDescription", info, 248);
}
u32 sceNpCommerceGetCategoryImageURL(PPUContext& ctx, u32 info)
{
    return info_field(ctx, "sceNpCommerceGetCategoryImageURL", info, 632);
}
u32 sceNpCommerceGetSkuId(PPUContext& ctx, u32 info) { return info_field(ctx, "sceNpCommerceGetSkuId", info, 0); }
u32 sceNpCommerceGetProductId(PPUContext& ctx, u32 info) { return info_field(ctx, "sceNpCommerceGetProductId", info, 56); }
u32 sceNpCommerceGetProductName(PPUContext& ctx, u32 info) { return info_field(ctx, "sceNpCommerceGetProductName", info, 104); }
u32 sceNpCommerceGetSkuName(PPUContext& ctx, u32 info) { return info_field(ctx, "sceNpCommerceGetSkuName", info, 296); }
u32 sceNpCommerceGetSkuDescription(PPUContext& ctx, u32 info)
{
    return info_field(ctx, "sceNpCommerceGetSkuDescription", info, 360);
}
u32 sceNpCommerceGetSkuImageURL(PPUContext& ctx, u32 info) { return info_field(ctx, "sceNpCommerceGetSkuImageURL", info, 744); }
u32 sceNpCommerceGetSkuUserData(PPUContext& ctx, u32 info) { return info_field(ctx, "sceNpCommerceGetSkuUserData", info, 872); }

// RPCS3 sceNpCommerceDoCheckoutStartAsync: 1 to 16 SKUs, then a TODO (no request ID written, nothing bought).
s32 sceNpCommerceDoCheckoutStartAsync(PPUContext&, u32 ctx_id, u32 sku_ids, u32 sku_num, u32 container_id, u32 req_id)
{
    if (!sku_num || sku_num > 16) return error(SCE_NP_COMMERCE_ERROR_INVALID_SKU_NUM);
    PS_LOG_DEBUG(kChan, "sceNpCommerceDoCheckoutStartAsync(context {:#x}, SKUs {:#x} ({}), container {:#x}, request "
                        "{:#x})", ctx_id, sku_ids, sku_num, container_id, req_id);
    return commerce_todo("sceNpCommerceDoCheckoutStartAsync");
}

s32 sceNpCommerceDoCheckoutFinishAsync(PPUContext&, u32 req_id)
{
    PS_LOG_DEBUG(kChan, "sceNpCommerceDoCheckoutFinishAsync({:#x})", req_id);
    return commerce_todo("sceNpCommerceDoCheckoutFinishAsync");
}

// RPCS3 sceNpCommerceGetDataFlagStart / SetDataFlagStart (fourth argument: 1 to 16 flags) and GetDataFlagState (third
// argument): TODOs after that count.
s32 sceNpCommerceGetDataFlagStart(PPUContext&, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5)
{
    if (a4 == 0 || a4 > 16) return error(SCE_NP_COMMERCE_ERROR_INVALID_DATA_FLAG_NUM);
    PS_LOG_DEBUG(kChan, "sceNpCommerceGetDataFlagStart({:#x}, {:#x}, {:#x}, {}, {:#x})", a1, a2, a3, a4, a5);
    return commerce_todo("sceNpCommerceGetDataFlagStart");
}

s32 sceNpCommerceSetDataFlagStart(PPUContext&, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5)
{
    if (a4 == 0 || a4 > 16) return error(SCE_NP_COMMERCE_ERROR_INVALID_DATA_FLAG_NUM);
    PS_LOG_DEBUG(kChan, "sceNpCommerceSetDataFlagStart({:#x}, {:#x}, {:#x}, {}, {:#x})", a1, a2, a3, a4, a5);
    return commerce_todo("sceNpCommerceSetDataFlagStart");
}

s32 sceNpCommerceGetDataFlagState(PPUContext&, u32 a1, u32 a2, u32 a3)
{
    if (a3 == 0 || a3 > 16) return error(SCE_NP_COMMERCE_ERROR_INVALID_DATA_FLAG_NUM);
    PS_LOG_DEBUG(kChan, "sceNpCommerceGetDataFlagState({:#x}, {:#x}, {})", a1, a2, a3);
    return commerce_todo("sceNpCommerceGetDataFlagState");
}

// RPCS3 sceNpCommerceGetDataFlagAbort, GetDataFlagFinish and SetDataFlagFinish: UNIMPLEMENTED_FUNC, answering CELL_OK
// (their arguments aren't known).
s32 sceNpCommerceGetDataFlagAbort(PPUContext&) { return commerce_todo("sceNpCommerceGetDataFlagAbort"); }
s32 sceNpCommerceGetDataFlagFinish(PPUContext&) { return commerce_todo("sceNpCommerceGetDataFlagFinish"); }
s32 sceNpCommerceSetDataFlagFinish(PPUContext&) { return commerce_todo("sceNpCommerceSetDataFlagFinish"); }

// ------------------------------------------------------------------------------------------------- util

// strncmp of the two NP IDs' online IDs (16 characters), clamped to -1, 0, 1 (np::is_same_npid compares the same way).
s32 online_id_order(u8* base, u32 id1, u32 id2)
{
    for (u32 i = 0; i < kOnlineIdLength; i++) {
        const u8 a = ps_read8(base, id1 + i), b = ps_read8(base, id2 + i);
        if (a != b) return a < b ? -1 : 1;
        if (!a) break;
    }
    return 0;
}

// RPCS3 sceNpUtilCmpNpId: two NP IDs match when their online IDs do (np::is_same_npid: strncmp of 16 characters); the
// other fields are not compared. NP need not be initialized.
s32 sceNpUtilCmpNpId(PPUContext& ctx, u32 id1, u32 id2)
{
    if (!id1 || !id2) return error(SCE_NP_UTIL_ERROR_INVALID_ARGUMENT);
    return online_id_order(guest_base(ctx), id1, id2) ? error(SCE_NP_UTIL_ERROR_NOT_MATCH) : lv2::CELL_OK;
}

// RPCS3 sceNpUtilCmpOnlineId: the same comparison as sceNpUtilCmpNpId.
s32 sceNpUtilCmpOnlineId(PPUContext& ctx, u32 id1, u32 id2)
{
    if (!id1 || !id2) return error(SCE_NP_UTIL_ERROR_INVALID_ARGUMENT);
    return online_id_order(guest_base(ctx), id1, id2) ? error(SCE_NP_UTIL_ERROR_NOT_MATCH) : lv2::CELL_OK;
}

// memcmp of `n` guest bytes, clamped to -1, 0, 1.
s32 compare_bytes(u8* base, u32 a, u32 b, u32 n)
{
    const s32 r = std::memcmp(base + a, base + b, n);
    return r < 0 ? -1 : r > 0 ? 1 : 0;
}

// RPCS3 sceNpUtilCmpNpIdInOrder: the order of two NP IDs: their online IDs (strncmp of 16 characters), then the first
// opt word; then, when both or neither have a platform (opt[4] set), the platform words (neither: equal); when one
// has, its platform against "ps3".
s32 sceNpUtilCmpNpIdInOrder(PPUContext& ctx, u32 id1, u32 id2, u32 order)
{
    if (!id1 || !id2) return error(SCE_NP_UTIL_ERROR_INVALID_ARGUMENT);
    u8* base = guest_base(ctx);
    s32 result = online_id_order(base, id1, id2);
    if (!result) result = compare_bytes(base, id1 + kNpIdOpt, id2 + kNpIdOpt, 4);
    if (!result) {
        const u8 platform1 = ps_read8(base, id1 + kNpIdPlatform), platform2 = ps_read8(base, id2 + kNpIdPlatform);
        if (platform1 && platform2) {
            result = compare_bytes(base, id1 + kNpIdPlatform, id2 + kNpIdPlatform, 4);
        } else if (platform1 || platform2) {
            const int r = std::memcmp(base + (platform1 ? id1 : id2) + kNpIdPlatform, "ps3", 4);
            result = r < 0 ? -1 : r > 0 ? 1 : 0;
        }
    }
    ps_write32(base, order, static_cast<u32>(result));
    return lv2::CELL_OK;
}

// RPCS3 sceNpUtilCanonicalizeNpIdForPs3 / ...ForPsp: the checks and the platform they would set are commented out in
// RPCS3 ("for compatibility with RPCN"): only the pointer is checked.
s32 sceNpUtilCanonicalizeNpIdForPs3(PPUContext&, u32 np_id)
{
    return np_id ? lv2::CELL_OK : error(SCE_NP_UTIL_ERROR_INVALID_ARGUMENT);
}

s32 sceNpUtilCanonicalizeNpIdForPsp(PPUContext&, u32 np_id)
{
    return np_id ? lv2::CELL_OK : error(SCE_NP_UTIL_ERROR_INVALID_ARGUMENT);
}

// The platform words of SceNpId (its second opt word, RPCS3 unk1[1]).
constexpr u32 kPlatformPs4 = 0x70733400, kPlatformVita = 0x70737032, kPlatformPs3 = 0x70733300; // "ps4\0" "psp2" "ps3\0"

// RPCS3 sceNpUtilGetPlatformType: the platform the NP ID's platform word names, as the result (not an error).
s32 sceNpUtilGetPlatformType(PPUContext& ctx, u32 np_id)
{
    if (!np_id) return error(SCE_NP_UTIL_ERROR_INVALID_ARGUMENT);
    switch (ps_read32(guest_base(ctx), np_id + kNpIdPlatform)) {
    case kPlatformPs4: return SCE_NP_PLATFORM_TYPE_PS4;
    case kPlatformVita: return SCE_NP_PLATFORM_TYPE_VITA;
    case kPlatformPs3: return SCE_NP_PLATFORM_TYPE_PS3;
    case 0: return SCE_NP_PLATFORM_TYPE_NONE;
    }
    return error(SCE_NP_UTIL_ERROR_UNKNOWN_PLATFORM_TYPE);
}

// RPCS3 sceNpUtilSetPlatformType: writes the platform word of a known platform type.
s32 sceNpUtilSetPlatformType(PPUContext& ctx, u32 np_id, u32 platform_type)
{
    if (!np_id) return error(SCE_NP_UTIL_ERROR_INVALID_ARGUMENT);
    u32 word = 0;
    switch (platform_type) {
    case SCE_NP_PLATFORM_TYPE_PS4: word = kPlatformPs4; break;
    case SCE_NP_PLATFORM_TYPE_VITA: word = kPlatformVita; break;
    case SCE_NP_PLATFORM_TYPE_PS3: word = kPlatformPs3; break;
    case SCE_NP_PLATFORM_TYPE_NONE: word = 0; break;
    default: return error(SCE_NP_UTIL_ERROR_UNKNOWN_PLATFORM_TYPE);
    }
    ps_write32(guest_base(ctx), np_id + kNpIdPlatform, word);
    return lv2::CELL_OK;
}


// ------------------------------------------------------------------------------------------------- the system's NP service

// libsysutil_np's helpers for requests to the system software's NP service (D-921): a cxml document written into one of
// libsysutil's packet channels (D-534) under a request number, the reply read back into a document and its "result".
// Sony's sibling modules (libsysutil_np2, _np_commerce2, _np_tus, _np_installer) build their requests with them;
// pspocket serves those itself, so a game's process reaches these only by importing them. They do what Sony's
// libsysutil_np 4.92 does, call for call, over pspocket's own cxml and packet functions (the exports, called as Sony's
// module calls its imports). pspocket's console has no system software: its channels are closed, so a request ends
// with the channel's error, as Sony's code ends on it. Unverified (D-594): Sony's need its sceNpInit's whole set-up
// (heap, buffer, request threads) to be compared.

// A host module's export, called as libsysutil_np calls its import; the caller's argument registers kept.
u64 np_import(PPUContext& ctx, std::string_view module, std::string_view name, std::initializer_list<u64> args)
{
    const Function* f = registry().find(module, recomp::nid_of(name));
    if (!f) PS_FATAL(kChan, "{}::{}, which libsysutil_np imports, is not pspocket's", module, name);
    std::array<u64, 8> saved{};
    for (u32 k = 0; k < 8; k++) saved[k] = ctx.gpr[3 + k];
    u32 i = 0;
    for (const u64 a : args) ctx.gpr[3 + i++] = a;
    f->fn(ctx, guest_base(ctx));
    const u64 r = ctx.gpr[3];
    for (u32 k = 0; k < 8; k++) ctx.gpr[3 + k] = saved[k];
    return r;
}

constexpr std::string_view kCxml = "cellSysutil";
constexpr s32 kNpReplyTooLarge = static_cast<s32>(0x8002B006); // a reply larger than the buffer (read and dropped)
constexpr u32 kNpReceiveBuffer = 0x18000;
constexpr u32 kNpFixedBlock = 0x6000; // a FixedMemory instance: six 4 KB tables

// The client heap and receive buffer Sony's sceNpInit makes over the game's NP pool: _sys_heap_create_heap(
// "SceNpSysutilClientHeap", pool size, 0x80000000, {8, pool}) and 0x18000 bytes from it. Made here at first use while
// NP is up; none otherwise (Sony's heap is 0 then). sceNpTerm deletes the heap, as Sony's does.
struct NpClient {
    u32 heap = 0, buffer = 0;
};

NpClient np_client(PPUContext& ctx)
{
    NpState& s = np();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) return {};
    if (!s.client_heap) {
        u8* base = guest_base(ctx);
        constexpr std::string_view kName = "SceNpSysutilClientHeap";
        StackVar name(ctx, base, 32);
        for (usize i = 0; i < kName.size(); i++) ps_write8(base, name.addr() + static_cast<u32>(i), static_cast<u8>(kName[i]));
        StackVar attr(ctx, base, 8);
        ps_write32(base, attr.addr(), 8);
        ps_write32(base, attr.addr() + 4, s.pool_ptr);
        s.client_heap = static_cast<u32>(np_import(ctx, "sysPrxForUser", "_sys_heap_create_heap",
            {name.addr(), s.pool_size, 0x80000000u, attr.addr()}));
        if (s.client_heap) {
            s.client_buffer = static_cast<u32>(
                np_import(ctx, "sysPrxForUser", "_sys_heap_malloc", {s.client_heap, kNpReceiveBuffer}));
        }
    }
    return {s.client_heap, s.client_buffer};
}

// _sceNpSysutilClientMalloc(size) / _sceNpSysutilClientFree(p): the client heap's (its answers as they are).
u32 _sceNpSysutilClientMalloc(PPUContext& ctx, u32 size)
{
    return static_cast<u32>(np_import(ctx, "sysPrxForUser", "_sys_heap_malloc", {np_client(ctx).heap, size}));
}

u64 _sceNpSysutilClientFree(PPUContext& ctx, u32 p)
{
    return np_import(ctx, "sysPrxForUser", "_sys_heap_free", {np_client(ctx).heap, p});
}

// sce_np_matching_set_matching2_running(bool): what matching 2 tells the older matching library while it runs (its
// calls then refuse with SCE_NP_MATCHING_ERROR_UTILITY_UNAVAILABLE, matching_check); a byte, kept over NP's term.
void np_set_matching2_running(PPUContext&, u32 running) { np().matching2_running = (running & 0xFF) != 0; }

// Reads the reply to request `id` on `channel` into `buffer` (`size` bytes): 0, the read's error, or kNpReplyTooLarge
// when the first read filled the buffer without finishing (the rest read and dropped first). Sony's answers 0 for a
// reply read whole, whatever its size.
s32 np_read_reply(PPUContext& ctx, s32 channel, s32 id, u32 buffer, u32 size)
{
    u8* base = guest_base(ctx);
    StackVar done(ctx, base, 8);
    const auto read = [&] {
        return static_cast<s32>(np_import(ctx, kCxml, "cellSysutilPacketRead",
            {static_cast<u64>(static_cast<s64>(channel)), static_cast<u64>(static_cast<s64>(id)), buffer, size, done.addr()}));
    };
    s32 r = read();
    if (r < 0) return r;
    if (ps_read8(base, done.addr()) || static_cast<u32>(r) != size) return 0;
    for (;;) {
        r = read();
        if (r < 0 || ps_read8(base, done.addr())) return kNpReplyTooLarge;
    }
}

// The reply in `buffer` made the document, its root element to `element` (a guest Element), and its "result": the
// result when it is negative, else GetInt's answer.
s32 np_parse_reply(PPUContext& ctx, u32 doc, u32 buffer, u32 size, u32 element)
{
    u8* base = guest_base(ctx);
    np_import(ctx, kCxml, "_ZN4cxml8Document16CreateFromBufferEPKvjb", {doc, buffer, size, 0});
    StackVar root(ctx, base, 8);
    np_import(ctx, kCxml, "_ZN4cxml8Document18GetDocumentElementEv", {root.addr(), doc});
    if (!guest_readable(thread_of(ctx), element, 8)) {
        log_call_site(ctx, base, std::format("libsysutil_np: element {:#x} is not guest memory", element));
        return static_cast<s32>(lv2::CELL_EFAULT);
    }
    ps_write32(base, element, ps_read32(base, root.addr()));
    ps_write32(base, element + 4, ps_read32(base, root.addr() + 4));
    StackVar result(ctx, base, 8);
    StackVar key(ctx, base, 8);
    std::memcpy(base + key.addr(), "result", 7);
    const s32 r = static_cast<s32>(np_import(ctx, kCxml, "_ZN8cxmlutil6GetIntERKN4cxml7ElementEPKcPi",
        {element, key.addr(), result.addr()}));
    if (r < 0) return r;
    const s32 v = static_cast<s32>(ps_read32(base, result.addr()));
    return v < 0 ? v : r;
}

// _sce_np_sysutil_recv_packet(id, doc, element): the reply to `id` on channel 0 into the receive buffer, then parsed.
s32 np_recv_packet(PPUContext& ctx, s32 id, u32 doc, u32 element)
{
    const u32 buffer = np_client(ctx).buffer;
    const s32 r = np_read_reply(ctx, 0, id, buffer, kNpReceiveBuffer);
    if (r < 0) return r;
    return np_parse_reply(ctx, doc, buffer, kNpReceiveBuffer, element);
}

// _sce_np_sysutil_recv_packet_fixedmem(id, mem, doc, element) and _sub: the reply into the FixedMemory instance itself
// (0x6000 bytes), on channel 0 or 1.
s32 np_recv_fixedmem(PPUContext& ctx, s32 channel, s32 id, u32 mem, u32 doc, u32 element)
{
    const s32 r = np_read_reply(ctx, channel, id, mem, kNpFixedBlock);
    if (r < 0) return r;
    return np_parse_reply(ctx, doc, mem, kNpFixedBlock, element);
}

s32 np_recv_packet_fixedmem(PPUContext& ctx, s32 id, u32 mem, u32 doc, u32 element)
{
    return np_recv_fixedmem(ctx, 0, id, mem, doc, element);
}

s32 np_recv_packet_fixedmem_sub(PPUContext& ctx, s32 id, u32 mem, u32 doc, u32 element)
{
    return np_recv_fixedmem(ctx, 1, id, mem, doc, element);
}

// _sce_np_sysutil_send_packet(id, doc) and _sub: Document::Write through a PacketWriter on channel 0 (1 for _sub).
s32 np_send(PPUContext& ctx, s32 channel, s32 id, u32 doc)
{
    try {
        sysutil::PacketWriter writer(ctx, channel, id, doc);
        return sysutil::document_write(ctx, doc, writer);
    } catch (const sysutil::Fault& f) {
        log_call_site(ctx, guest_base(ctx),
            std::format("libsysutil_np: guest memory at {:#010x} is not there (Sony's faults there)", f.addr));
        return static_cast<s32>(lv2::CELL_EFAULT);
    }
}

s32 np_send_packet(PPUContext& ctx, s32 id, u32 doc) { return np_send(ctx, 0, id, doc); }
s32 np_send_packet_sub(PPUContext& ctx, s32 id, u32 doc) { return np_send(ctx, 1, id, doc); }

// _sce_np_sysutil_cxml_prepare_doc(mem, doc, magic, element, name, attributes): the document's tables from the
// FixedMemory instance, its header's magic, and its root element.
s32 np_cxml_prepare_doc(PPUContext& ctx, u32 mem, u32 doc, u32 magic, u32 element, u32 name, s32 attributes)
{
    if (!guest_readable(thread_of(ctx), doc + 0x6C, 8)) {
        log_call_site(ctx, guest_base(ctx), std::format("libsysutil_np: document {:#x} is not guest memory", doc));
        return static_cast<s32>(lv2::CELL_EFAULT);
    }
    try {
        sysutil::document_use_fixed_memory(ctx, doc, mem);
    } catch (const sysutil::Fault& f) {
        log_call_site(ctx, guest_base(ctx), std::format("libsysutil_np: guest memory at {:#010x} is not there", f.addr));
        return static_cast<s32>(lv2::CELL_EFAULT);
    }
    np_import(ctx, kCxml, "_ZN4cxml8Document14SetHeaderMagicEPKc", {doc, magic});
    return static_cast<s32>(np_import(ctx, kCxml, "_ZN4cxml8Document13CreateElementEPKciPNS_7ElementE",
        {doc, name, static_cast<u64>(static_cast<s64>(attributes)), element}));
}

// _sce_np_sysutil_send_empty_packet(id, mem, magic, name): a request with only its root element.
s32 np_send_empty_packet(PPUContext& ctx, s32 id, u32 mem, u32 magic, u32 name)
{
    u8* base = guest_base(ctx);
    StackVar doc(ctx, base, 0x80);
    StackVar element(ctx, base, 8);
    np_import(ctx, kCxml, "_ZN4cxml8DocumentC1Ev", {doc.addr()});
    ps_write32(base, element.addr(), 0);
    ps_write32(base, element.addr() + 4, 0xFFFFFFFF);
    s32 r = np_cxml_prepare_doc(ctx, mem, doc.addr(), magic, element.addr(), name, 0);
    if (r >= 0) r = np_send_packet(ctx, id, doc.addr());
    np_import(ctx, kCxml, "_ZN4cxml8DocumentD1Ev", {doc.addr()});
    return r;
}

// Sony's helper: a file attribute `name` of `element` holding `size` bytes at `data` (registered in the document); no
// data with a size is SCE_NP_ERROR_INVALID_ARGUMENT.
s32 np_set_bytes(PPUContext& ctx, u32 doc, u32 element, u32 name, u32 data, u32 size)
{
    if (!data && size) return error(SCE_NP_ERROR_INVALID_ARGUMENT);
    u8* base = guest_base(ctx);
    StackVar file(ctx, base, 16);
    ps_write32(base, file.addr(), 0);
    ps_write32(base, file.addr() + 4, 0xFFFFFFFF);
    ps_write32(base, file.addr() + 8, 0);
    const s32 r = static_cast<s32>(
        np_import(ctx, kCxml, "_ZN4cxml8Document12RegisterFileEPKvjPNS_4FileE", {doc, data, size, file.addr()}));
    if (r < 0) return r;
    return static_cast<s32>(np_import(ctx, kCxml, "_ZN8cxmlutil7SetFileERKN4cxml7ElementEPKcRKNS0_4FileE",
        {element, name, file.addr()}));
}

// A guest copy of a host string for an export to read (in a StackVar the caller keeps).
u32 np_text(PPUContext& ctx, const StackVar& at, std::string_view text)
{
    std::memcpy(guest_base(ctx) + at.addr(), text.data(), text.size());
    ps_write8(guest_base(ctx), at.addr() + static_cast<u32>(text.size()), 0);
    return at.addr();
}

// _sce_np_sysutil_cxml_set_npid(doc, parent, name, npid): an element `name` under `parent` holding the NP ID: a child
// "handle" whose "data" is the online ID, then the ID's "opt" and "reserved" bytes as files (8 each, at +20 and +28).
s32 np_cxml_set_npid(PPUContext& ctx, u32 doc, u32 parent, u32 name, u32 npid)
{
    u8* base = guest_base(ctx);
    StackVar element(ctx, base, 8);
    StackVar child(ctx, base, 8);
    StackVar s1(ctx, base, 16), s2(ctx, base, 16), s3(ctx, base, 16), s4(ctx, base, 16);
    ps_write32(base, element.addr(), 0);
    ps_write32(base, element.addr() + 4, 0xFFFFFFFF);
    s32 r = static_cast<s32>(np_import(ctx, kCxml, "_ZN4cxml8Document13CreateElementEPKciPNS_7ElementE",
        {doc, name, 2, element.addr()}));
    if (r < 0) return r;
    ps_write32(base, child.addr(), 0);
    ps_write32(base, child.addr() + 4, 0xFFFFFFFF);
    r = static_cast<s32>(np_import(ctx, kCxml, "_ZN4cxml8Document13CreateElementEPKciPNS_7ElementE",
        {doc, np_text(ctx, s1, "handle"), 1, child.addr()}));
    if (r < 0) return r;
    r = static_cast<s32>(np_import(ctx, kCxml, "_ZN8cxmlutil9SetStringERKN4cxml7ElementEPKcS5_",
        {child.addr(), np_text(ctx, s2, "data"), npid}));
    if (r < 0) return r;
    r = static_cast<s32>(np_import(ctx, kCxml, "_ZN4cxml7Element11AppendChildERS0_", {element.addr(), child.addr()}));
    if (r < 0) return r;
    r = np_set_bytes(ctx, doc, element.addr(), np_text(ctx, s3, "opt"), npid + 20, 8);
    if (r < 0) return r;
    r = np_set_bytes(ctx, doc, element.addr(), np_text(ctx, s4, "reserved"), npid + 28, 8);
    if (r < 0) return r;
    return static_cast<s32>(np_import(ctx, kCxml, "_ZN4cxml7Element11AppendChildERS0_", {parent, element.addr()}));
}

// sceNpDrmExecuteGamePurchase(): Sony's asks the system software to open the store at the game's full version: before
// sceNpInit SCE_NP_DRM_ERROR_INTERNAL; then under its purchase mutex a "drmrequest" (magic "NPDR", reqtype 2, an
// empty reqarg) sent as request 268 in a FixedMemory document between cellSysutilPacketBegin and End, and the reply's
// result. pspocket's console has no system software: the request ends with the closed channel's error, as Sony's does
// on it, and nothing opens.
s32 sceNpDrmExecuteGamePurchase(PPUContext& ctx)
{
    if (!np().initialized) return error(SCE_NP_DRM_ERROR_INTERNAL);
    constexpr s32 kRequest = 268;
    u8* base = guest_base(ctx);
    np_import(ctx, kCxml, "cellSysutilPacketBegin", {0, kRequest});
    const u32 mem = static_cast<u32>(np_import(ctx, kCxml, "_ZN16sysutil_cxmlutil11FixedMemory5BeginEi", {0}));
    StackVar doc(ctx, base, 0x80);
    StackVar element(ctx, base, 8);
    StackVar s1(ctx, base, 16), s2(ctx, base, 16), s3(ctx, base, 16), s4(ctx, base, 16);
    np_import(ctx, kCxml, "_ZN4cxml8DocumentC1Ev", {doc.addr()});
    ps_write32(base, element.addr(), 0);
    ps_write32(base, element.addr() + 4, 0xFFFFFFFF);
    s32 r = np_cxml_prepare_doc(ctx, mem, doc.addr(), np_text(ctx, s1, "NPDR"), element.addr(),
        np_text(ctx, s2, "drmrequest"), 2);
    if (r >= 0) {
        r = static_cast<s32>(np_import(ctx, kCxml, "_ZN8cxmlutil6SetIntERKN4cxml7ElementEPKci",
            {element.addr(), np_text(ctx, s3, "reqtype"), 2}));
    }
    if (r >= 0) r = np_set_bytes(ctx, doc.addr(), element.addr(), np_text(ctx, s4, "reqarg"), 0, 0);
    if (r >= 0) r = np_send_packet(ctx, kRequest, doc.addr());
    if (r >= 0) {
        np_import(ctx, kCxml, "_ZN4cxml8DocumentD1Ev", {doc.addr()});
        np_import(ctx, kCxml, "_ZN4cxml8DocumentC1Ev", {doc.addr()});
        r = np_recv_packet(ctx, kRequest, doc.addr(), element.addr());
        if (r >= 0) {
            StackVar result(ctx, base, 8);
            StackVar s5(ctx, base, 16);
            r = static_cast<s32>(np_import(ctx, kCxml, "_ZN8cxmlutil6GetIntERKN4cxml7ElementEPKcPi",
                {element.addr(), np_text(ctx, s5, "result"), result.addr()}));
            if (r >= 0) r = static_cast<s32>(ps_read32(base, result.addr()));
        }
    }
    np_import(ctx, kCxml, "_ZN4cxml8DocumentD1Ev", {doc.addr()});
    np_import(ctx, kCxml, "_ZN16sysutil_cxmlutil11FixedMemory3EndEi", {0});
    np_import(ctx, kCxml, "cellSysutilPacketEnd", {0, kRequest});
    if (r < 0) PS_LOG_WARN(kChan, "sceNpDrmExecuteGamePurchase: the system software is not there ({:#x}); no store opens",
        static_cast<u32>(r));
    return r;
}

} // namespace

// ------------------------------------------------------------------------------------------------- NP's state (np.hpp)

bool np_initialized() { return np().initialized; }

u32 np_init(u32 poolsize, u32 poolptr)
{
    NpState& s = np();
    std::lock_guard lock(s.mutex);
    if (s.initialized) return SCE_NP_ERROR_ALREADY_INITIALIZED;
    if (poolsize == 0 || !poolptr) return SCE_NP_ERROR_INVALID_ARGUMENT;
    if (poolsize < SCE_NP_MIN_POOL_SIZE) return SCE_NP_ERROR_INSUFFICIENT_BUFFER;
    s.initialized = true;
    s.pool_size = poolsize;
    s.pool_ptr = poolptr;
    return lv2::CELL_OK;
}

std::optional<std::string> np_communication_id(u8* base, u32 addr) { return communication_id(base, addr); }

u32 np_pool_size()
{
    NpState& s = np();
    std::lock_guard lock(s.mutex);
    return s.pool_size;
}

// sceNpInit(poolsize, poolptr): RPCS3's, through np_init. NP comes up offline (D-059): the manager reports a signed-out
// user, as after sceNp2Init.
s32 sceNpInit(PPUContext&, u32 poolsize, u32 poolptr)
{
    const u32 r = np_init(poolsize, poolptr);
    PS_LOG_INFO(kChan, "sceNpInit(pool {:#x} bytes at {:#x}) -> {:#x}", poolsize, poolptr, r);
    return static_cast<s32>(r);
}

u32 np_term()
{
    NpState& s = np();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) return SCE_NP_ERROR_NOT_INITIALIZED;
    s.initialized = false;
    s.pool_size = 0;
    s.pool_ptr = 0;
    s.client_heap = s.client_buffer = 0;
    s.basic_handler_registered = false;
    s.manager_cb = s.manager_cb_arg = 0;
    s.signaling_ctxs.clear();
    return lv2::CELL_OK;
}

namespace {

// sceNpTerm: RPCS3's, through np_term, then the matching 2 room callbacks sceNp2.cpp keeps (terminate_NP drops them
// too).
s32 sceNpTerm(PPUContext& ctx)
{
    u32 heap = 0;
    {
        NpState& s = np();
        std::lock_guard lock(s.mutex);
        heap = s.client_heap;
    }
    const u32 r = np_term();
    if (r == lv2::CELL_OK) np2_on_np_term();
    if (r == lv2::CELL_OK && heap) np_import(ctx, "sysPrxForUser", "_sys_heap_delete_heap", {heap}); // as Sony's
    PS_LOG_INFO(kChan, "sceNpTerm() -> {:#x}", r);
    return static_cast<s32>(r);
}

} // namespace

void register_sceNp(Registry& r)
{
    r.add("sceNp", {
        PS_HLE_FUNC(0xBD28FDBF, sceNpInit),
        PS_HLE_FUNC(0xA7BFF757, sceNpManagerGetStatus),
        PS_HLE_FUNC(0xE7DCD3B4, sceNpManagerRegisterCallback),
        PS_HLE_FUNC(0x52A6B523, sceNpManagerUnregisterCallback),
        PS_HLE_FUNC(0xFE37A7F4, sceNpManagerGetNpId),
        PS_HLE_FUNC(0xB1E0718B, sceNpManagerGetAccountRegion),
        PS_HLE_FUNC(0x168FCECE, sceNpManagerGetAccountAge),
        PS_HLE_FUNC(0x6EE62ED2, sceNpManagerGetContentRatingFlag),
        PS_HLE_FUNC(0xEB7A3D84, sceNpManagerGetChatRestrictionFlag),
        PS_HLE_FUNC(0xBDC07FD5, sceNpManagerGetNetworkTime),
        PS_HLE_FUNC(0x8297F1EC, sceNpManagerRequestTicket2),
        PS_HLE_FUNC(0x0968AA36, sceNpManagerGetTicket),
        PS_HLE_FUNC(0x58FA4FCD, sceNpManagerGetTicketParam),
        PS_HLE_FUNC(0xB66D1C46, sceNpManagerGetEntitlementIdList),
        PS_HLE_FUNC(0xA1709ABD, sceNpManagerGetEntitlementById),
        PS_HLE_FUNC(0x4026EAC5, sceNpBasicRegisterContextSensitiveHandler),
        PS_HLE_FUNC(0xACB9EE8E, sceNpBasicUnregisterHandler),
        PS_HLE_FUNC(0xE035F7D6, sceNpBasicGetEvent),
        PS_HLE_FUNC(0x3F0808AA, sceNpBasicSetPresence),
        PS_HLE_FUNC(0x5E849303, sceNpBasicSetPresenceDetails2),
        PS_HLE_FUNC(0x73931BD0, sceNpBasicGetBlockListEntryCount),
        PS_HLE_FUNC(0xF2B3338A, sceNpBasicGetBlockListEntry),
        PS_HLE_FUNC(0xECD503DE, sceNpBasicGetMessageEntryCount),
        PS_HLE_FUNC(0x64A704CC, sceNpBasicRecvMessageAttachmentLoad),
        PS_HLE_FUNC(0xEC0A1FBF, sceNpBasicSendMessage),
        PS_HLE_FUNC(0x01FBBC9B, sceNpBasicSendMessageGui),
        PS_HLE_FUNC(0x27C69EBA, sceNpBasicAddFriend),
        PS_HLE_FUNC(0x806960AB, sceNpBasicRecvMessageCustom),
        PS_HLE_FUNC(0xBCDBB2AB, sceNpBasicAddPlayersHistoryAsync),
        PS_HLE_FUNC(0xCEEEBC7A, sceNpProfileCallGui),
        PS_HLE_FUNC(0xAD218FAF, sceNpDrmIsAvailable),
        PS_HLE_FUNC(0xF042B14F, sceNpDrmIsAvailable2),
        PS_HLE_FUNC(0x5F2D9257, sceNpLookupInit),
        PS_HLE_FUNC(0x8440537C, sceNpLookupTerm),
        PS_HLE_FUNC(0xCE81C7F0, sceNpLookupCreateTitleCtx),
        PS_HLE_FUNC(0x5DE61626, sceNpLookupDestroyTitleCtx),
        PS_HLE_FUNC(0xEA2E9FFC, sceNpLookupCreateTransactionCtx),
        PS_HLE_FUNC(0xFB87CF5E, sceNpLookupDestroyTransactionCtx),
        PS_HLE_FUNC(0x71E5AF7E, sceNpLookupSetTimeout),
        PS_HLE_FUNC(0xD12E40AE, sceNpLookupNpIdAsync),
        PS_HLE_FUNC(0x860B1756, sceNpLookupTitleSmallStorageAsync),
        PS_HLE_FUNC(0x7508112E, sceNpLookupPollAsync),
        PS_HLE_FUNC(0x6356082E, sceNpSignalingCreateCtx),
        PS_HLE_FUNC(0xA8CF8451, sceNpSignalingDestroyCtx),
        PS_HLE_FUNC(0x50B86D94, sceNpSignalingAddExtendedHandler),
        PS_HLE_FUNC(0x60897C38, sceNpSignalingActivateConnection),
        PS_HLE_FUNC(0x9AD7FBD1, sceNpSignalingGetLocalNetInfo),
        PS_HLE_FUNC(0xCA0A2D04, sceNpSignalingGetConnectionStatus),
        PS_HLE_FUNC(0x155DE760, sceNpSignalingGetConnectionInfo),
        PS_HLE_FUNC(0xE853D388, sceNpSignalingGetConnectionFromNpId),
        PS_HLE_FUNC(0x34CE82A0, sceNpSignalingGetConnectionFromPeerAddress),
        PS_HLE_FUNC(0x95C7BBA3, sceNpSignalingTerminateConnection),
        PS_HLE_FUNC(0xD208F91D, sceNpUtilCmpNpId),
        PS_HLE_FUNC(0x4885AA18, sceNpTerm),
        PS_HLE_FUNC(0xBCC09FE7, sceNpBasicRegisterHandler),
        PS_HLE_FUNC(0xBE81C71C, sceNpBasicSetPresenceDetails),
        PS_HLE_FUNC(0x481CE0E8, sceNpBasicAbortGui),
        PS_HLE_FUNC(0xE1C9F675, sceNpBasicMarkMessageAsUsed),
        PS_HLE_FUNC(0xBF607EC6, sceNpBasicGetClanMessageEntryCount),
        PS_HLE_FUNC(0x4D9C615D, sceNpBasicGetClanMessageEntry),
        PS_HLE_FUNC(0xA8AFA7D4, sceNpBasicGetCustomInvitationEntryCount),
        PS_HLE_FUNC(0xD053F113, sceNpBasicGetCustomInvitationEntry),
        PS_HLE_FUNC(0xAF505DEF, sceNpBasicGetMatchingInvitationEntryCount),
        PS_HLE_FUNC(0x05AF1CB8, sceNpBasicGetMatchingInvitationEntry),
        PS_HLE_FUNC(0x9153BDF4, sceNpBasicGetMessageAttachmentEntryCount),
        PS_HLE_FUNC(0x5D543BBE, sceNpBasicGetMessageAttachmentEntry),
        PS_HLE_FUNC(0x30D1CBDE, sceNpBasicGetMessageEntry),
        PS_HLE_FUNC(0xAFEF640D, sceNpBasicGetFriendListEntryCount),
        PS_HLE_FUNC(0x04372385, sceNpBasicGetFriendListEntry),
        PS_HLE_FUNC(0x32C78A6A, sceNpBasicGetFriendPresenceByIndex),
        PS_HLE_FUNC(0x6453B27B, sceNpBasicGetFriendPresenceByIndex2),
        PS_HLE_FUNC(0xFD39AE13, sceNpBasicGetFriendPresenceByNpId),
        PS_HLE_FUNC(0x260CAEDD, sceNpBasicGetFriendPresenceByNpId2),
        PS_HLE_FUNC(0x1AE8A549, sceNpBasicAddBlockListEntry),
        PS_HLE_FUNC(0xA15F35FE, sceNpBasicGetPlayersHistoryEntryCount),
        PS_HLE_FUNC(0xBAB91FC9, sceNpBasicGetPlayersHistoryEntry),
        PS_HLE_FUNC(0xB5CB2D56, sceNpBasicRecvMessageAttachment),
        PS_HLE_FUNC(0x43B989F5, sceNpBasicSendMessageAttachment),
        PS_HLE_FUNC(0x168A3117, sceNpBasicAddPlayersHistory),
        PS_HLE_FUNC(0xBE07C708, sceNpManagerGetOnlineId),
        PS_HLE_FUNC(0x16F88A6F, sceNpManagerGetPsHandle),
        PS_HLE_FUNC(0xF42C0DF8, sceNpManagerGetOnlineName),
        PS_HLE_FUNC(0x36D0C2C5, sceNpManagerGetAvatarUrl),
        PS_HLE_FUNC(0x32200389, sceNpManagerGetMyLanguages),
        PS_HLE_FUNC(0x7E2FEF28, sceNpManagerRequestTicket),
        PS_HLE_FUNC(0x4B9EFB7A, sceNpManagerGetCachedInfo),
        PS_HLE_FUNC(0x4210C462, sceNpManagerGetSigninId),
        PS_HLE_FUNC(0x442381F7, sceNpManagerSubSignin),
        PS_HLE_FUNC(0x60440C73, sceNpManagerSubSigninAbortGui),
        PS_HLE_FUNC(0x000E53CC, sceNpManagerSubSignout),
        PS_HLE_FUNC(0xF0A9182B, sceNpFriendlist),
        PS_HLE_FUNC(0xD7FB1FA6, sceNpFriendlistCustom),
        PS_HLE_FUNC(0xF59E1DA8, sceNpFriendlistAbortGui),
        PS_HLE_FUNC(0x45F8F3AA, sceNpCustomMenuRegisterActions),
        PS_HLE_FUNC(0xF9732AC8, sceNpCustomMenuActionSetActivation),
        PS_HLE_FUNC(0x9458F464, sceNpCustomMenuRegisterExceptionList),
        PS_HLE_FUNC(0x2F2C6B3E, sceNpProfileAbortGui),
        PS_HLE_FUNC(0xAA16695F, sceNpDrmProcessExitSpawn),
        PS_HLE_FUNC(0xE6C8F3F9, sceNpDrmProcessExitSpawn2),
        PS_HLE_FUNC(0xCF51864B, sceNpDrmGetTimelimit),
        PS_HLE_FUNC(0x2ECD48ED, sceNpDrmVerifyUpgradeLicense),
        PS_HLE_FUNC(0xBE0E3EE2, sceNpDrmVerifyUpgradeLicense2),
        PS_HLE_FUNC(0x8C760B52, sceNpUtilCmpOnlineId),
        PS_HLE_FUNC(0xF5FF5F31, sceNpUtilCmpNpIdInOrder),
        PS_HLE_FUNC(0x91840669, sceNpUtilCanonicalizeNpIdForPs3),
        PS_HLE_FUNC(0xBE3AF96E, sceNpUtilCanonicalizeNpIdForPsp),
        PS_HLE_FUNC(0xC611029A, sceNpUtilGetPlatformType),
        PS_HLE_FUNC(0xAFC62605, sceNpUtilSetPlatformType),
        PS_HLE_FUNC(0xD737FD2D, sceNpLookupWaitAsync),
        PS_HLE_FUNC(0x3D1760DC, sceNpLookupAbortTransaction),
        PS_HLE_FUNC(0x166DCC11, sceNpLookupNpId),
        PS_HLE_FUNC(0xCA39C4B2, sceNpLookupTitleSmallStorage),
        PS_HLE_FUNC(0xB6017827, sceNpLookupAvatarImage),
        PS_HLE_FUNC(0xBF9EEA93, sceNpLookupAvatarImageAsync),
        PS_HLE_FUNC(0xDFD63B62, sceNpLookupUserProfile),
        PS_HLE_FUNC(0xFF0A2378, sceNpLookupUserProfileAsync),
        PS_HLE_FUNC(0x2FCCBFE0, sceNpLookupUserProfileWithAvatarSize),
        PS_HLE_FUNC(0x1FDB3EC2, sceNpLookupUserProfileWithAvatarSizeAsync),
        PS_HLE_FUNC(0x9EE9F97E, sceNpLookupTitleStorage),
        PS_HLE_FUNC(0x5E117ED5, sceNpLookupTitleStorageAsync),
        PS_HLE_FUNC(0x32CF311F, sceNpScoreInit),
        PS_HLE_FUNC(0x9851F805, sceNpScoreTerm),
        PS_HLE_FUNC(0xB9F93BBB, sceNpScoreCreateTitleCtx),
        PS_HLE_FUNC(0x259113B8, sceNpScoreDestroyTitleCtx),
        PS_HLE_FUNC(0x6F5E8143, sceNpScoreCreateTransactionCtx),
        PS_HLE_FUNC(0xC5F4CF82, sceNpScoreDestroyTransactionCtx),
        PS_HLE_FUNC(0xEE5B20D9, sceNpScoreAbortTransaction),
        PS_HLE_FUNC(0xA7A090E5, sceNpScorePollAsync),
        PS_HLE_FUNC(0x1A2704F7, sceNpScoreWaitAsync),
        PS_HLE_FUNC(0x2706EAA1, sceNpScoreSetPlayerCharacterId),
        PS_HLE_FUNC(0x29DD45DC, sceNpScoreSetTimeout),
        PS_HLE_FUNC(0xF4E0F607, sceNpScoreGetBoardInfo),
        PS_HLE_FUNC(0xDDCE7D15, sceNpScoreGetBoardInfoAsync),
        PS_HLE_FUNC(0x1672170E, sceNpScoreRecordScore),
        PS_HLE_FUNC(0xF0B1E399, sceNpScoreRecordScoreAsync),
        PS_HLE_FUNC(0x04CA5E6A, sceNpScoreRecordGameData),
        PS_HLE_FUNC(0xF76847C2, sceNpScoreRecordGameDataAsync),
        PS_HLE_FUNC(0x3B02418D, sceNpScoreGetGameData),
        PS_HLE_FUNC(0xDB2E4DC2, sceNpScoreGetGameDataAsync),
        PS_HLE_FUNC(0x05D65DFF, sceNpScoreGetRankingByNpId),
        PS_HLE_FUNC(0x3DB7914D, sceNpScoreGetRankingByNpIdAsync),
        PS_HLE_FUNC(0xC3A991EE, sceNpScoreGetRankingByNpIdPcId),
        PS_HLE_FUNC(0xC4B6CD8F, sceNpScoreGetRankingByNpIdPcIdAsync),
        PS_HLE_FUNC(0xFBC82301, sceNpScoreGetRankingByRange),
        PS_HLE_FUNC(0x21206642, sceNpScoreGetRankingByRangeAsync),
        PS_HLE_FUNC(0xF33291EB, sceNpScoreGetFriendsRanking),
        PS_HLE_FUNC(0xCC946D56, sceNpScoreGetFriendsRankingAsync),
        PS_HLE_FUNC(0x7DEB244C, sceNpScoreCensorComment),
        PS_HLE_FUNC(0x7BE47E61, sceNpScoreCensorCommentAsync),
        PS_HLE_FUNC(0xF1B77918, sceNpScoreSanitizeComment),
        PS_HLE_FUNC(0x2CD2A1AF, sceNpScoreSanitizeCommentAsync),
        PS_HLE_FUNC(0x741FBF24, sceNpScoreGetClanMemberGameData),
        PS_HLE_FUNC(0xBEF887E5, sceNpScoreGetClanMemberGameDataAsync),
        PS_HLE_FUNC(0xDED17C26, sceNpScoreGetClansMembersRankingByNpId),
        PS_HLE_FUNC(0xE8A67160, sceNpScoreGetClansMembersRankingByNpIdAsync),
        PS_HLE_FUNC(0x41FFD4F2, sceNpScoreGetClansMembersRankingByNpIdPcId),
        PS_HLE_FUNC(0x433FCB30, sceNpScoreGetClansMembersRankingByNpIdPcIdAsync),
        PS_HLE_FUNC(0x6D4ADC3B, sceNpScoreGetClansMembersRankingByRange),
        PS_HLE_FUNC(0x4D5E0670, sceNpScoreGetClansMembersRankingByRangeAsync),
        PS_HLE_FUNC(0x2A76895A, sceNpScoreGetClansRankingByClanId),
        PS_HLE_FUNC(0x227F8763, sceNpScoreGetClansRankingByClanIdAsync),
        PS_HLE_FUNC(0xB082003B, sceNpScoreGetClansRankingByRange),
        PS_HLE_FUNC(0x7B7E9137, sceNpScoreGetClansRankingByRangeAsync),
        PS_HLE_FUNC(0xFD0EB5AE, sceNpSignalingDeactivateConnection),
        PS_HLE_FUNC(0x2687A127, sceNpSignalingGetCtxOpt),
        PS_HLE_FUNC(0x276C72B2, sceNpSignalingSetCtxOpt),
        PS_HLE_FUNC(0x75EB50CB, sceNpSignalingGetPeerNetInfo),
        PS_HLE_FUNC(0xD0958814, sceNpSignalingGetPeerNetInfoResult),
        PS_HLE_FUNC(0x64DBB89D, sceNpSignalingCancelPeerNetInfo),
        PS_HLE_FUNC(0xAC66568C, sceNpMatchingCreateCtx),
        PS_HLE_FUNC(0x2E1C5068, sceNpMatchingDestroyCtx),
        PS_HLE_FUNC(0x03C741A7, sceNpMatchingGetResult),
        PS_HLE_FUNC(0x26B3BC94, sceNpMatchingGetResultGUI),
        PS_HLE_FUNC(0x3CC8588A, sceNpMatchingCreateRoomGUI),
        PS_HLE_FUNC(0x474B7B13, sceNpMatchingJoinRoomGUI),
        PS_HLE_FUNC(0x14497465, sceNpMatchingQuickMatchGUI),
        PS_HLE_FUNC(0x32FEBB4C, sceNpMatchingSearchJoinRoomGUI),
        PS_HLE_FUNC(0xE24EEA19, sceNpMatchingGetRoomListLimitGUI),
        PS_HLE_FUNC(0x691F429D, sceNpMatchingGetRoomInfo),
        PS_HLE_FUNC(0xB020684E, sceNpMatchingGetRoomInfoNoLimit),
        PS_HLE_FUNC(0x6F8FD267, sceNpMatchingSetRoomInfo),
        PS_HLE_FUNC(0x4A18A89E, sceNpMatchingSetRoomInfoNoLimit),
        PS_HLE_FUNC(0xEE64CF8E, sceNpMatchingGetRoomSearchFlag),
        PS_HLE_FUNC(0xA284BD1D, sceNpMatchingSetRoomSearchFlag),
        PS_HLE_FUNC(0xF806C54C, sceNpMatchingLeaveRoom),
        PS_HLE_FUNC(0x73A2E36B, sceNpMatchingGetRoomMemberListLocal),
        PS_HLE_FUNC(0xDAE2D351, sceNpMatchingGrantOwnership),
        PS_HLE_FUNC(0x34CC0CA4, sceNpMatchingKickRoomMember),
        PS_HLE_FUNC(0xD20D7798, sceNpMatchingKickRoomMemberWithOpt),
        PS_HLE_FUNC(0x2AD7837D, sceNpMatchingAcceptInvitationGUI),
        PS_HLE_FUNC(0x8B7BBD73, sceNpMatchingSendInvitationGUI),
        PS_HLE_FUNC(0xFCAC355A, sceNpCommerceCreateCtx),
        PS_HLE_FUNC(0xE2877BEA, sceNpCommerceDestroyCtx),
        PS_HLE_FUNC(0x26F33146, sceNpCommerceGetProductCategoryStart),
        PS_HLE_FUNC(0xCFD469E4, sceNpCommerceGetProductCategoryFinish),
        PS_HLE_FUNC(0x674BB9FF, sceNpCommerceGetProductCategoryAbort),
        PS_HLE_FUNC(0x3F195B3A, sceNpCommerceGetProductCategoryResult),
        PS_HLE_FUNC(0x8D1D096C, sceNpCommerceInitProductCategory),
        PS_HLE_FUNC(0x6CB81EB2, sceNpCommerceDestroyProductCategory),
        PS_HLE_FUNC(0xB1C02D66, sceNpCommerceGetCurrencyInfo),
        PS_HLE_FUNC(0xEB9DF054, sceNpCommerceGetCategoryInfo),
        PS_HLE_FUNC(0x79225AA3, sceNpCommerceGetCurrencyCode),
        PS_HLE_FUNC(0xAEE8CF71, sceNpCommerceGetCategoryId),
        PS_HLE_FUNC(0x6E2AB18B, sceNpCommerceGetCategoryName),
        PS_HLE_FUNC(0x359642A6, sceNpCommerceGetCategoryDescription),
        PS_HLE_FUNC(0x9452F4F8, sceNpCommerceGetCategoryImageURL),
        PS_HLE_FUNC(0x39A69619, sceNpCommerceGetSkuId),
        PS_HLE_FUNC(0x936DF4AA, sceNpCommerceGetProductId),
        PS_HLE_FUNC(0xEB5F2544, sceNpCommerceGetProductName),
        PS_HLE_FUNC(0xEE530059, sceNpCommerceGetSkuName),
        PS_HLE_FUNC(0xA85A4951, sceNpCommerceGetSkuDescription),
        PS_HLE_FUNC(0xCCBE2E69, sceNpCommerceGetSkuImageURL),
        PS_HLE_FUNC(0x1A3FCB69, sceNpCommerceGetSkuUserData),
        Function{{}, "_sceNpSysutilClientMalloc", 0xEE0CC40C, &detail::thunk<&_sceNpSysutilClientMalloc>, false, true},
        Function{{}, "_sceNpSysutilClientFree", 0x816C6A5F, &detail::thunk<&_sceNpSysutilClientFree>, false, true},
        Function{{}, "_Z37sce_np_matching_set_matching2_runningb", 0xADCC5ADA, &detail::thunk<&np_set_matching2_running>, false, true},
        Function{{}, "_Z27_sce_np_sysutil_recv_packetiRN4cxml8DocumentERNS_7ElementE", 0x48B3774F, &detail::thunk<&np_recv_packet>, false, true},
        Function{{}, "_Z36_sce_np_sysutil_recv_packet_fixedmemiPN16sysutil_cxmlutil11FixedMemoryERN4cxml8DocumentERNS2_7ElementE", 0x2C5AD7B4, &detail::thunk<&np_recv_packet_fixedmem>, false, true},
        Function{{}, "_Z40_sce_np_sysutil_recv_packet_fixedmem_subiPN16sysutil_cxmlutil11FixedMemoryERN4cxml8DocumentERNS2_7ElementE", 0x4299A92C, &detail::thunk<&np_recv_packet_fixedmem_sub>, false, true},
        Function{{}, "_Z27_sce_np_sysutil_send_packetiRN4cxml8DocumentE", 0x25B8BD56, &detail::thunk<&np_send_packet>, false, true},
        Function{{}, "_Z31_sce_np_sysutil_send_packet_subiRN4cxml8DocumentE", 0xAD12E5C9, &detail::thunk<&np_send_packet_sub>, false, true},
        Function{{}, "_Z32_sce_np_sysutil_cxml_prepare_docPN16sysutil_cxmlutil11FixedMemoryERN4cxml8DocumentEPKcRNS2_7ElementES6_i", 0xFD61DFF5, &detail::thunk<&np_cxml_prepare_doc>, false, true},
        Function{{}, "_Z33_sce_np_sysutil_send_empty_packetiPN16sysutil_cxmlutil11FixedMemoryEPKcS3_", 0x1FA740DB, &detail::thunk<&np_send_empty_packet>, false, true},
        Function{{}, "_Z29_sce_np_sysutil_cxml_set_npidRN4cxml8DocumentERNS_7ElementEPKcPK7SceNpId", 0x9BA3E97F, &detail::thunk<&np_cxml_set_npid>, false, true},
        Function{{}, "sceNpDrmExecuteGamePurchase", 0xF283C143, &detail::thunk<&sceNpDrmExecuteGamePurchase>, false, true},
        PS_HLE_FUNC(0xAF57D9C9, sceNpCommerceGetCurrencyDecimals),
        PS_HLE_FUNC(0x2BE41ECE, sceNpCommerceGetNumOfChildCategory),
        PS_HLE_FUNC(0x7208DC08, sceNpCommerceGetNumOfChildProductSku),
        PS_HLE_FUNC(0xBA65DE6D, sceNpCommerceGetChildCategoryInfo),
        PS_HLE_FUNC(0x01CD9CFD, sceNpCommerceGetChildProductSkuInfo),
        PS_HLE_FUNC(0x78D7F9AD, sceNpCommerceGetSkuPrice),
        PS_HLE_FUNC(0xE36C660E, sceNpCommerceDoCheckoutStartAsync),
        PS_HLE_FUNC(0xAF3EBA5A, sceNpCommerceDoCheckoutFinishAsync),
        PS_HLE_FUNC(0xDBDB909F, sceNpCommerceGetDataFlagStart),
        PS_HLE_FUNC(0x99AC9952, sceNpCommerceSetDataFlagStart),
        PS_HLE_FUNC(0xD03CEA35, sceNpCommerceGetDataFlagState),
        PS_HLE_FUNC(0x0561448B, sceNpCommerceGetDataFlagAbort),
        PS_HLE_FUNC(0x9281E87A, sceNpCommerceGetDataFlagFinish),
        PS_HLE_FUNC(0x8D4518A0, sceNpCommerceSetDataFlagFinish),
    });
    // sceNpMatchingInt (RPCS3 sceNpMatchingInt.cpp): the matching library's internal entry points. Three share their
    // names, and so their NIDs, with sceNp's (RPCS3 registers its OLD_ functions under them); they do the same here.
    // RPCS3's sceNpMatchingCancelRequest(GUI) and sceNpMatchingSendRoomMessage are stubs (rpcs3_stubs.inc).
    r.add("sceNpMatchingInt", {
        PS_HLE_FUNC(0x170829D3, sceNpMatchingGetRoomMemberList),
        PS_HLE_FUNC(0x31B2B978, sceNpMatchingJoinRoomWithoutGUI),
        PS_HLE_FUNC(0x474B7B13, sceNpMatchingJoinRoomGUI),
        PS_HLE_FUNC(0x4A18A89E, sceNpMatchingSetRoomInfoNoLimit),
        PS_HLE_FUNC(0x92D5C77B, sceNpMatchingGetRoomListWithoutGUI),
        PS_HLE_FUNC(0xB00F408A, sceNpMatchingGetRoomListGUI),
        PS_HLE_FUNC(0xB020684E, sceNpMatchingGetRoomInfoNoLimit),
        PS_HLE_FUNC(0xEADCB5CA, sceNpMatchingCreateRoomWithoutGUI),
    });
}

} // namespace pspocket::hle
