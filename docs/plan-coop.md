# Crackdown: implementation plan for two-player PC co-op

**Target document:** `docs/plan-coop.md`  
**Research date:** 2026-10-13  
**Scope:** Read-only static research. No files modified; no multiplayer execution performed.

## 1. Recommended approach

Implement **serverless System Link first**, preserving Crackdown’s own discovery, connection, replication, and gameplay code. Then extend that transport to explicitly configured internet peers or a broadcast-capable virtual LAN.

Crackdown’s local assets contain **both Xbox Live and System Link co-op menus**. There is no evidence that co-op requires Xbox Live exclusively. Consequently, fake Live sign-in should be optional, rather than a prerequisite for the initial LAN implementation.

The current SDK cannot support this path merely by allowing multiplayer privileges. It lacks real session objects, key management, address conversion, QoS, and several socket operations. Its existing socket implementation also needs endian and error-handling corrections.

Port selected behavior and ABI definitions from the netplay fork; do not import its complete Live service architecture. The fork’s System Link session creation is local, but its reverse address lookup and QoS implementations retain master-server dependencies.

### Source notation

Paths below are relative to the repository root.

- `S/`: `sdk/`
- `R/`: `out/ref/xenia-netplay/`
- `G/`: `crackdown/generated/default/`
- `A/`: `game/`

Reference checkout:

- Branch: `netplay_canary_experimental`
- Commit: `ef09406de5e16badb4ad9d3a9df452020015946a`
- SDK HEAD: `f5337cdc947ff6d4c4196737e2c807a48f2a1fc2`

The SDK working files contain local changes; HEAD alone does not identify their complete contents.

## 2. Multiplayer modes and menu evidence

### Supported routes

The assets strongly establish these menu routes:

1. Solo/co-op selection.
2. System Link → Host/Join.
3. Xbox Live co-op → public matchmaking or friend-related actions.
4. Lobby → start gameplay.

Evidence:

| Local source | Hit |
|---|---|
| `A/Local/en-US/GameStrings.str`, byte `0x2E76`, UTF-16LE | `System Link` |
| Same, byte `0x2E8E` | `Xbox Live` |
| Same, byte `0x2F9A` | `Host Game` |
| Same, byte `0x1ECA2` | `Play a game over system link` |
| Same, byte `0x32DBF`, ASCII | `IDS_HELP_JOIN_INVITE_SYSLINK` |
| Same, byte `0x32D7D` | `IDS_HELP_JOIN_INVITE_HOST_PUBLIC` |
| Same, byte `0x32D9E` | `IDS_HELP_JOIN_INVITE_JOIN_PUBLIC` |
| Same, byte `0x2F875` | `IDS_XUIFE_SCREEN_MATCHMAKING_LOBBY_TITLE_COOP_2` |
| Same, byte `0x2F960` | `IDS_XUIFE_SCREEN_MATCHMAKING_LOBBY_START` |
| `A/Local/en-US/XUIFrontEnd/Scene.xzp`, byte `0x38E5E`, UTF-16LE | `solo-co-opSettingsPopUp` |
| Same, byte `0x3A3F8` | `systemLinkHostJoin` |
| Same, byte `0x3DF3E` | `xboxLiveJoinQuickMatch` |

The string table also contains commented System Link Host/Join descriptions at `0x3A89E` and `0x3A90B`. These corroborate intent, but comments are weaker evidence than the actual scene and displayed strings.

**Qualification:** Assets include other historical mode names, such as arena/deathmatch strings. Their presence does not establish playable modes in this build. This plan targets the two-player co-op route only.

## 3. Guest session and network path

### XSession is implemented through XMsg

There are no direct `XSessionCreate` imports on this path. Guest library wrappers build message buffers and call XMsg with **XGI app ID `0xFB`**.

The following table identifies actual guest instructions calling XMsg. The call address is the recorded return address minus four, not the import thunk address.

| Operation | XGI message | Guest call instruction | Generated evidence |
|---|---:|---:|---|
| Create | `0x000B0010` | `0x8292AAE0` | `G/crackdown_recomp.65.cpp:26890` |
| Delete | `0x000B0011` | `0x8292AB9C` | `G/crackdown_recomp.40.cpp:26557` |
| Join local | `0x000B0012` | `0x8292AC58` | `G/crackdown_recomp.144.cpp:23109` |
| Join remote | `0x000B0012` | `0x8292AD00` | `G/crackdown_recomp.79.cpp:25257` |
| Leave local | `0x000B0013` | `0x8292ADA4` | `G/crackdown_recomp.175.cpp:26205` |
| Leave remote | `0x000B0013` | `0x8292AE4C` | `G/crackdown_recomp.69.cpp:27281` |
| Modify | `0x000B0018` | `0x8292A8xx`¹ | Runtime trace required |
| Start | `0x000B0014` | `0x8292AFC0` | `G/crackdown_recomp.73.cpp:25484` |
| End | `0x000B0015` | `0x8292B05C` | `G/crackdown_recomp.59.cpp:25489` |
| SearchEx | `0x000B001C` | `0x8292B150` | `G/crackdown_recomp.91.cpp:25744` |
| SearchByID | `0x000B001B` | `0x8292B224` | `G/crackdown_recomp.9.cpp:27335` |
| Arbitration register | `0x000B001A` | `0x8292AF18` | `G/crackdown_recomp.114.cpp:26453` |
| Write/flush statistics | `0x000B0025` / `0x000B0026` | `0x8292B3A8` / `0x8292B2D8` | `G/crackdown_recomp.177.cpp:28655`; `.24.cpp:26358` |

¹ The modify wrapper’s exact XMsg instruction was not established during this reading; do not use this placeholder as a hook address.

The session ABI is visible in `S/src/kernel/xam/apps/xgi_app.cpp:95` onward. For create, the 28-byte message contains:

```text
+00 object pointer
+04 flags
+08 public slots
+0C private slots
+10 user index
+14 XSESSION_INFO guest pointer
+18 nonce guest pointer
```

The SDK currently labels `+10` as `user_xuid`, but it is a 32-bit field; the fork treats the create argument as a **user index**. See `R/src/xenia/kernel/xsession.h:179` and `R/src/xenia/kernel/xam/apps/xgi_app.cc:441`.

### Handle sequence

The create wrapper `sub_8292A9B8` performs:

1. `XamSessionCreateHandle` at guest instruction `0x8292AA80`.
2. `XamSessionRefObjByHandle` at `0x8292AA9C`.
3. XGI create at `0x8292AAE0`.

Evidence: `G/crackdown_recomp.65.cpp:26838`, `:26854`, `:26890`.

The game calls this create wrapper at **`0x8241C414`**, inside `sub_8241C380`: `G/crackdown_recomp.98.cpp:9323`.

Additional higher-level callers include:

- Remote join: `0x8241F4E0` → `sub_8292ACA0`; `G/crackdown_recomp.140.cpp:10710`.
- Local leave: `0x8241F598` → `sub_8292AD48`; `G/crackdown_recomp.127.cpp:9934`.
- Remote leave: `0x8241F5F0` → `sub_8292ADF0`; same file `:9983`.
- Start: `0x8241FC58` → `sub_8292AF70`; `G/crackdown_recomp.69.cpp:10491`.
- SearchEx: `0x82420ECC` → `sub_8292B0B8`; `G/crackdown_recomp.63.cpp:10110`.
- SearchByID: `0x82421A88` → `sub_8292B1A8`; `G/crackdown_recomp.136.cpp:10797`.

SearchEx’s caller passes procedure index **2**, a requested result count of **50**, and one user at `G/crackdown_recomp.63.cpp:10090`.

**Qualification:** These are established call chains, not a recorded chronological execution. Static reading did not prove that System Link executes XSession search. Its separate broadcast implementation suggests that discovery may bypass Live matchmaking entirely.

### Keys and address conversion

The game has an explicit System Link-style key sequence:

- `0x824621B4` → `XNetCreateKey` wrapper `sub_829293D0`.
- `0x824621C4` → `XNetRegisterKey` wrapper `sub_829293E0`.

Evidence: `G/crackdown_recomp.65.cpp:10673`, `:10682`.

Joining code registers a received key at `0x82462718`: `G/crackdown_recomp.102.cpp:10916`.

Address and connection chains include:

- `0x82462758` → `XNetXnAddrToInAddr`; `.102.cpp:10949`.
- `0x8243BA58` → `XNetInAddrToXnAddr`; `.100.cpp:10516`.
- `0x8241C600` → `XNetXnAddrToInAddr`, then `0x8241C69C` → `XNetConnect`; `.108.cpp:10107`, `:10192`.
- `0x8241CB78` → conversion, then `0x8241CB88` → `XNetGetConnectStatus`; `.109.cpp:10264`, `:10273`.
- Another polling chain at `0x824628B0` / `0x824628BC`; `.36.cpp:10939`, `:10947`.

The guest compares connection status against **3**, consistent with the fork’s `STATUS_CONNECTED`.

### QoS

- Listen wrapper: `sub_82929480`; import call at `.40.cpp:26484`.
- Lookup wrapper: `sub_829294A0`; import call at `.83.cpp:27237`.
- Release wrapper: `sub_82929508`; `.111.cpp:25305`.

Higher-level evidence:

- `0x8241E064` calls listen with flags **13**; `G/crackdown_recomp.18.cpp:9133`.
- `0x8241E0BC` calls listen with flags **16**; `.163.cpp:9725`.
- `0x8241E27C` calls lookup; `.104.cpp:10055`.

The exact lookup count, probes, event, and payload contents must be captured dynamically before implementing compatibility behavior.

### Socket types and ports

The wrapper `sub_829295E0` transforms guest `socket(af,type,protocol)` arguments into `NetDll_socket(caller=1,af,type,protocol)`: `G/crackdown_recomp.30.cpp:22953`.

| Transport | Static evidence |
|---|---|
| **VDP, protocol 254, datagram, AF_INET** | `G/crackdown_recomp.45.cpp:9236`; socket call instruction `0x82462350` |
| **VDP bind port 1000** | Same file `:9286`; bind call `0x8246238C` |
| **UDP, protocol 17, datagram, AF_INET** | `.165.cpp:11148`; socket call `0x82462AF8` |
| **UDP bind port 1001** | Same file `:11192`, `:11211`; bind call `0x82462B6C` |
| **TCP, protocol 6, stream, AF_INET** | `.48.cpp:10905`; socket call `0x82462410` |
| **TCP bind port 1002** | Same file `:10921`, `:11105`; bind call `0x82462560` |
| Other TCP sockets | `.114.cpp:10330`, `.135.cpp:11011`, `.53.cpp:8811` |

The game broadcasts **140 bytes** to `255.255.255.255:1001` at guest instruction **`0x82462CBC`**: `G/crackdown_recomp.177.cpp:11146`–`:11172`.

A second broadcast send exists at `0x82462A8C`, with a **16-byte** payload: `.117.cpp:11101`.

These establish discovery traffic. The complete message formats, host/client roles of every TCP socket, and port reuse behavior remain unverified.

### Sign-in, privileges, friends, and invites

SDK profile zero is locally signed in:

- `S/include/rex/system/xam/user_profile.h:221`: `signin_state()` returns **1**.
- `S/src/system/xam/user_profile.cpp:28`: fixed XUID `0xB13EBABEBABEBABE`.
- `S/src/kernel/xam/xam_user.cpp:373`: privilege checks deny access through `*out_value = 0`.

The game distinguishes local state **1** and online state **2**:

- Local-state comparisons: `G/crackdown_recomp.7.cpp:21652`, `:21679`.
- Online-state comparison: `.56.cpp:26204`, immediately followed by comparison against 2.
- Additional sign-in check at guest `0x82A8EDFC`: `.140.cpp:32988`.

Privilege import: `0x82B67104`. Calls at `0x82A8F034` and `0x82A8F058` use privilege IDs **252** and **251** respectively: `.140.cpp:33301`, `:33320`. The generic privilege wrapper also takes a caller-supplied privilege ID: `.7.cpp:21693`.

The fork identifies **254** as `XPRIVILEGE_MULTIPLAYER_SESSIONS`: `R/src/xenia/kernel/xsession.cc:82`. A Crackdown caller passing 254 was not conclusively traced here; do not mislabel its observed 251/252 checks as that privilege.

Friends UI:

- `sub_82744C80` tail-calls `XamShowFriendsUI`: `.168.cpp:19758`.
- Game caller instruction `0x82660164`: `.128.cpp:16742`.
- SDK implementation is an export stub: `S/src/kernel/xam/xam_ui.cpp:612`.

Live messages found in guest wrappers include:

- Friends enumeration `0x58020`: `.104.cpp:26964`.
- Accepted invitation information `0x58023`: `.30.cpp:23113`.
- XGI invitation data `0xB0019`: `.120.cpp:27360`.
- Logon ID `0x58004`: `.140.cpp:27562`.
- NAT type `0x58006`: `.160.cpp:27317`.

In the fork, XInvite send dispatches as `0x50002`, but **sending invites remains unsupported**: it displays “Invites aren't supported!” and returns success. See `R/src/xenia/kernel/xam/apps/xlivebase_app.cc:122`, `:899`–`:918`.

The precise Crackdown invite-send marshalling call site was not established. Native friends/invites are outside the minimum LAN milestone.

## 4. SDK behavior versus the fork

| Function or group | Current SDK | Netplay fork |
|---|---|---|
| `XNetStartup`, cleanup | Copies startup parameters; XNet construction and teardown commented out. `S/xam_net.cpp:185`² | Tracks startup/security flags; implemented export. `R/xam_net.cc:208`² |
| `WSAStartup` | Calls native Winsock; fills guest data. `S/xam_net.cpp:241` | Native startup plus broader network initialization; see `R/xam_net.cc:443` |
| `XNetRandom` | Implemented entry at `S/xam_net.cpp:233` | Implemented networking support; retain a suitable random source |
| `XNetCreateKey`, register/unregister | Export stubs, `S/xam_net.cpp:1034`, `:1044`, `:1051` | Generates typed ID, identity exchange key; caches a System Link ID or online-key flag. `R/xam_net.cc:2626` |
| `XNetGetTitleXnAddr` | Loopback, repeated `CC` MAC, zero online fields; returns STATIC. `S/xam_net.cpp:438` | Selected adapter or online address, console MAC, mode-dependent status. `R/xam_net.cc:697`; `R/XLiveAPI.cpp:109`³ |
| `XNetXnAddrToInAddr` | Returns 1 without output. `S/xam_net.cpp:481` | Chooses LAN `ina` or Live `inaOnline`; handles self address. `R/xam_net.cc:888` |
| `XNetInAddrToXnAddr` | Returns 1; input incorrectly typed as mapped pointer. `S/xam_net.cpp:488` | Scalar address input; fills address/MAC/session ID, including `FindPlayer` API lookup. `R/xam_net.cc:923` |
| `XNetInAddrToString` | Writes `666.666.666.666`. `S/xam_net.cpp:474` | Address formatting support |
| `XNetConnect`, status | Export stubs. `S/xam_net.cpp:1033`, `:1038` | Connect sleeps 150 ms and succeeds; status always connected. `R/xam_net.cc:787` |
| `XNetQosListen` | Returns FUNCTION_FAILED. `S/xam_net.cpp:560` | Caches data and posts it through XLiveAPI. `R/xam_net.cc:1277` |
| `XNetQosLookup` | Export stub. `S/xam_net.cpp:1043` | Allocates results, fetches data through HTTP, supports asynchronous lookup; reported RTT/bandwidth are synthetic. `R/xam_net.cc:1338`, `:1439` |
| `XNetQosRelease` | Frees top-level allocation only. `S/xam_net.cpp:552` | Tracks lookup cancellation and nested data ownership. `R/xam_net.cc:1240` |
| Session create/ref handle | `CAFEDEAD` / `DEADF00D`. `S/xam_user.cpp:715`² | Real `XSession` object, native guest object, retained handle. `R/xam_user.cc:1443`; `R/xsession.cc:30`⁴ |
| XGI create/join/start/search | Mostly logs and returns success without state/output. `S/xgi_app.cpp:95`⁵ | Dispatches to real session methods. `R/xgi_app.cc:441`, `:493`, `:538`, `:134` |
| XGI leave | No matching session-leave case in the inspected SDK dispatcher | Implements `0xB0013`. `R/xgi_app.cc:515` |
| User contexts/properties | Discards values. `S/xgi_app.cpp:36`, `:51` | Stores values for session/search/presence behavior. `R/xgi_app.cc:325`, `:367` |
| `XamUserGetSigninState` | Profile zero returns local state; other users absent. `S/xam_user.cpp:68` | Actual profile state. `R/xam_user.cc:101` |
| `XamUserCheckPrivilege` | Denies everything for existing user. `S/xam_user.cpp:373` | Requires Live sign-in, then permits privileges. `R/xam_user.cc:620` |
| Sign-in consistency | Sign-in info follows profile state, while online-enabled always returns 1. `S/xam_user.cpp:89`, `:425` | Profile-aware user state |
| Socket creation/VDP | Maps VDP to UDP. `S/src/system/xsocket.cpp:47` | Also maps VDP to UDP. `R/src/xenia/kernel/xsocket.cc:81` |
| Bind/connect/send/receive | Native wrappers exist; incomplete error and conversion handling. `S/xam_net.cpp:664`, `:682`, `:847`, `:858`, `:898`, `:909` | Explicit host sockaddr conversion, interface selection, mapped ports, error reporting |
| Socket options/ioctl | Forwards guest option data directly; ioctl lacks guest scalar conversion. `S/src/system/xsocket.cpp:72`, `:94` | Maps options and converts values; ioctl swaps guest scalar data. `R/src/xenia/kernel/xsocket.cc:163`, `:212` |
| `getsockname`, `getpeername`, `getsockopt` | Export stubs. `S/xam_net.cpp:1100` | Implemented socket query support |
| `WSAEventSelect` | Export stub. `S/xam_net.cpp:1004` | Native event selection plus readiness tracking. `R/src/xenia/kernel/xsocket.cc:1254` |
| `select`, socket events | SDK implementations exist; must verify readiness/error behavior. `S/xam_net.cpp:356`, `:379`, `:798` | More complete event/socket handling |
| WSA scatter/gather/overlapped | Receive immediately fails; send concatenates buffers, asserts no overlapped callback. `S/xam_net.cpp:298`, `:319` | Native buffers, asynchronous task tracking and cancellation |
| XMsg async | “Async” dispatch calls synchronous handler; then immediately completes overlapped. `S/src/system/xam/app_manager.cpp:41`; `S/src/kernel/xam/xam_msg.cpp:54` | App-specific asynchronous infrastructure; requires deliberate adaptation |
| Friends/presence/invites | Friends and accepted invites fail; presence initialize succeeds without service. `S/xlivebase_app.cpp:50`, `:59`, `:76`⁵ | Presence/friends infrastructure and accepted-info support; invite send still unsupported |

² Full paths: `src/kernel/xam/xam_net.*` or `xam_user.*`.  
³ `R/src/xenia/kernel/XLiveAPI.cpp`.  
⁴ `R/src/xenia/kernel/xsession.cc`.  
⁵ `src/kernel/xam/apps/`.

`S/src/kernel/xam/apps/xam_app.cpp` registers app `0xFE` and handles content-related messages. It is not the XGI session dispatcher. The requested `sdk/src/kernel/xam/xam_app.cpp` path does not exist in this checkout.

No separate socket/network implementation was found under SDK `src/kernel/xboxkrnl`; the network imports use XAM and `system/xsocket.cpp`. Xboxkrnl still participates through error conversion and kernel event operations.

## 5. Minimal serverless LAN design

### Preserve guest gameplay networking

The minimum transport is native UDP/TCP with Xbox API adaptation:

- VDP guest sockets use UDP between recomp instances.
- Preserve guest application payloads exactly.
- Preserve the guest’s 1000/1001/1002 port namespace.
- Bind wildcard guest addresses to the selected host interface.
- Permit the existing discovery broadcasts on that interface.
- Convert socket options, ioctl arguments, addresses, and errors correctly.
- Implement readiness and query operations used by the game.

Do not add a second replication engine, deterministic lockstep, or synchronized input transport. The game already contains its multiplayer simulation.

### Identity, keys, and peer mapping

Maintain a per-runtime network context containing:

- Selected adapter/local IPv4 address.
- Unique locally administered synthetic MAC.
- Unique profile XUID/name.
- Registered `XNKID → XNKEY` entries.
- Peer mappings containing XNADDR, session ID, guest address token, and host endpoints.
- Lookup/QoS ownership and cancellation state.

Prefer an opaque guest address token mapped to a peer record, rather than assuming an IPv4 address uniquely identifies a player. This supports two instances sharing a host address and two peers behind one public NAT address.

`XNetInAddrToXnAddr` must take its input address **by value**. Replace the SDK’s `mapped_void in_addr` signature; otherwise the input can be treated as a guest memory pointer.

Populate reverse mappings from actual advertisements, supplied XNADDRs, or a small host-side peer metadata exchange. Do not require a master-server `FindPlayer` request.

### Discovery

First let the guest’s existing port-1001 broadcasts run unchanged on two PCs. This is the smallest implementation.

Instrument the 16-byte query and 140-byte advertisement to establish:

- Header/version and title checks.
- XNADDR, XNKID, XNKEY, player and slot fields.
- Whether reverse address conversion runs before the advertisement exposes its identity.
- Whether QoS is used for System Link selection.

If additional peer metadata is necessary, use a separately versioned host control protocol. Do not place new headers inside game packets.

### Sessions and QoS

Implement real local session objects with:

- Creation flags, owner, nonce, and session info.
- Local/remote members and public/private slot accounting.
- Created/lobby/started/ended/deleted states.
- Valid detail and search output structures.
- Correct handle retention and release.

For LAN QoS, store the host’s opaque listen payload and answer probes over a host control socket. Return actual contacted/completed/data flags and measured RTT, with bounded timeouts.

If runtime tracing shows System Link bypasses XGI search and QoS, leave those operations outside the first playable milestone. Do not implement unnecessary matchmaking infrastructure before proving that the game needs it.

If XGI search is used, serve it from the LAN discovery cache with the captured procedure/context/property filtering and buffer-size contract.

## 6. Internet options and master-server dependencies

### Direct IP and port forwarding

Add explicit peer endpoint configuration. Translate the guest’s discovery broadcast into unicast delivery to configured peers, and translate gameplay endpoints through each peer’s advertised port map.

Manual forwarding must cover the actual sockets observed in a complete run:

- UDP VDP port 1000.
- UDP discovery port 1001.
- TCP port 1002.
- Any additional dynamically bound or control/QoS ports.

The fork’s `GetPlayerPort()` returns **36000** at `R/src/xenia/kernel/XLiveAPI.cpp:444`. This is not evidence that forwarding 36000 alone supports Crackdown.

Exchange endpoint maps explicitly; do not apply the local instance’s port offset to a remote peer.

### UPnP

Add UPnP after manual forwarding works. Borrow the fork’s mapping lifecycle selectively:

- Discovery: `R/src/xenia/kernel/upnp.cc:84`.
- Add/remove mapping: `:171`, `:245`.
- Tracked ports and cleanup: `:328`, `:358`.
- Lease refresh: `:383`.
- Bind/connect port mapping: `:401`, `:421`.

Use a separately reviewed miniupnpc dependency and retain its license. Track only mappings created by this process; remove those mappings on clean exit and refresh finite leases.

UPnP does not solve discovery, peer identity, CGNAT, or all NAT traversal cases.

### Virtual LAN

A broadcast-capable virtual LAN is an alternative deployment of the same System Link implementation:

- Select its adapter explicitly.
- Verify UDP broadcast delivery in both directions.
- Permit all required UDP/TCP traffic through host firewalls.

ZeroTier/Radmin are the broadcast-capable options identified in the task. Tailscale does not provide ordinary Ethernet broadcast discovery; explicit peer unicast discovery is needed there. These deployment assumptions were not independently tested during this local-only research.

### What XLiveAPI supplies

The fork uses its master service for:

- Player registration and IP-to-MAC/session lookup.
- Live session creation/search/details/join-related metadata.
- Session contexts/properties.
- QoS payload upload/download.
- Presence and related friends information.
- Port mapping metadata and additional online services.

Evidence: `R/src/xenia/kernel/XLiveAPI.cpp:814`, `:939`, `:985`, `:1002`, `:1093`, `:1202`, `:1409`, `:1489`, `:787`.

System Link host creation and joining bypass Live session registration:

- `R/src/xenia/kernel/xsession.cc:188`–`:197`.
- `:234`–`:236`.

However:

- Reverse conversion calls `FindPlayer`: `R/src/xenia/kernel/xam/xam_net.cc:974`.
- QoS uses HTTP: same file `:1325`, `:1447`.
- LAN initialization explicitly says the server is needed for QoS and performs a heartbeat: `R/src/xenia/kernel/XLiveAPI.cpp:396`–`:399`.
- XGI session search requires Live sign-in and calls `SessionSearch`: `R/src/xenia/kernel/xsession.cc:975`–`:980`.

**Conclusion:** System Link can avoid a master server in the proposed implementation. The reference fork is not wholly server-independent simply because LAN mode is selected.

## 7. Two instances on one PC

Use separate processes, profile identities, user data, caches, configuration, and logs.

Existing path overrides are defined at `S/src/system/runtime.cpp:31`–`:35`. Patch `0009-utf8-path-cvars.patch` already changes path handling in `src/ui/rex_app.cpp`; do not edit its added lines.

Proposed test configuration:

| Setting | Instance A | Instance B |
|---|---|---|
| XUID/MAC | Unique A | Unique B |
| User data/cache | `test/A/...` | `test/B/...` |
| Guest ports | 1000/1001/1002 | 1000/1001/1002 |
| Host ports | 21000/21001/21002 | 22000/22001/22002 |
| Discovery mode | Explicit peer | Explicit peer |
| Peer discovery endpoint | B:22001 | A:21001 |

Implement bind translation and reverse source-port translation so the guest continues seeing its expected ports. Route destination ports using the **remote** mapping.

Use loopback unicast discovery for this first test. Ordinary broadcasts to fixed port 1001 will not reach both remapped instances automatically. An alternative is two separately assigned local IPv4 addresses with identical ports, but adapter/address setup and broadcast behavior must be verified.

Read-only game assets may be shared. Writable saves and profiles must not be shared.

## 8. Guest layouts and byte order

Add compile-time size and offset assertions.

| Structure | Required layout |
|---|---|
| `XNADDR` | 36 bytes: `ina +0`, `inaOnline +4`, `wPortOnline +8`, MAC `+10`, online identification `+16` |
| `XNKID` | 8 opaque bytes |
| `XNKEY` | 16 opaque bytes |
| `XSESSION_INFO` | 60 bytes: ID `+0`, XNADDR `+8`, key `+44` |
| `X_KSESSION` | Four-byte big-endian handle |
| XGI create/manage/search/search-ex | 28 / 20 / 32 / 36 bytes |
| `XSESSION_SEARCHRESULT` | 92 bytes |
| Search-result header | Eight bytes |
| `XAM_OVERLAPPED` | 28 bytes; result, length, context, event, routine, completion context, extended error |

Evidence:

- `R/src/xenia/kernel/xnet.h:472`–`:554`, `:570`–`:587`.
- `R/src/xenia/kernel/xsession.h:73`–`:215`.
- `S/src/kernel/xam/xam_net.cpp:64`–`:96`.
- `S/include/rex/system/xio.h:37`.

Rules:

1. Guest scalar integers, pointers, lengths, XUIDs, and nonce values use guest big-endian representation.
2. IPv4 address bytes and socket ports use network byte order.
3. Native sockaddr family is a host-order scalar.
4. MAC, key, and opaque payload bytes are copied unchanged.
5. Convert at explicit guest/native boundaries; never blanket-swap an entire structure.

For example, IPv4 `192.168.1.2` must appear as bytes `C0 A8 01 02`; port 1000 as `03 E8`.

The SDK uses native `in_addr` fields in XNADDR, but `rex::be` fields in sockaddr wrappers. Audit the existing conversions carefully: `S/include/rex/system/xsocket.h:40`–`:65` and `S/src/system/xsocket.cpp:195` contain different representations. The current receive path’s `ntohl` followed by assignment into a big-endian wrapper warrants a byte-level regression test.

The fork classifies XNKID using the first byte’s high nibble:

- System Link: `0x00`.
- Online peer: `0x80`.

See `R/src/xenia/kernel/xnet.h:1516`. Its integer conversions explicitly serialize IDs in big-endian order at `:1573`.

The fork’s exchange-key generator produces fixed bytes `00..0F`, not a random secret: `:1584`. Do not mistake that for authentication.

## 9. Proposed patch series

Place new files under `out/full-integration/patches/sdk/`, starting after existing `0009`.

Each patch must own its added lines permanently. Later patches may call earlier interfaces but must not edit their implementation or declarations. Plan the shared interfaces before landing the series; consolidate any interface correction into its owning patch.

| Patch | Changes and ownership |
|---|---|
| `0010-coop-network-abi.patch` | New shared ABI header: XNADDR/XNKID/XNKEY/session/QoS layouts, constants, assertions, endian helpers. Replace the original local XNADDR/QoS declarations in `xam_net.cpp`. |
| `0011-coop-network-context.patch` | New per-runtime network context/configuration files and stable service interfaces. Lifecycle ownership in `KernelState`. Selected adapter, peer maps, unique MAC, endpoint configuration. |
| `0012-socket-options-errors.patch` | Original `XSocket::SetOption`, `IOControl`, close/error behavior; guest scalar conversion and native Winsock errors. |
| `0013-socket-address-routing.patch` | Original bind/connect/sendto/recvfrom/accept address boundaries; interface binding and bidirectional endpoint translation. Own query implementations and their original stub replacements. |
| `0014-xnet-session-keys.patch` | New key registry implementation; replace original create/register/unregister export stubs. Generate typed nonzero IDs and per-session keys. |
| `0015-xnet-peer-addresses.patch` | Replace original title address, conversion, formatting, connect/status handlers or stubs. Correct reverse-conversion signature and use peer mappings. |
| `0016-socket-readiness.patch` | Implement original `WSAEventSelect` stub; validate existing select/event waits, readiness and shutdown wakeups. Add WSA overlapped support only if tracing shows it is exercised. |
| `0017-session-object-handles.patch` | New `XSession` object files; object type/deserialization handling as required; replace original create/ref handle sentinels. |
| `0018-xgi-session-lifecycle.patch` | Replace original XGI session cases; add missing leave; context/property state and valid local details/search outputs through stable backend interfaces. |
| `0019-xmsg-pending-completion.patch` | Original XMsg dispatch/completion path only: explicit pending-operation ownership, cancellation, and requesting-thread APC context. Preserve synchronous behavior for unaffected apps. |
| `0020-peer-qos.patch` | New peer QoS implementation; replace original listen/lookup/release behavior. Bounded probes, nested allocation ownership, release/cancellation races. |
| `0021-lan-discovery-backend.patch` | New discovery-cache/backend implementation behind previously declared interfaces. Preserve guest discovery; add metadata exchange only if needed. |
| `0022-direct-peer-discovery.patch` | New explicit-peer discovery transport behind the existing routing interface. Broadcast-to-unicast fan-out and peer endpoint-map exchange. |
| `0023-upnp-port-mappings.patch` | Optional UPnP implementation/dependency wiring, mapping ownership, refresh and cleanup. |
| `0024-emulated-live-profile.patch` | Optional profile state/XUID/name configuration and privilege policy. Modify original profile/sign-in/privilege lines only. Add coherent Live facade behavior only where traces demonstrate a gate. |

Keep transport translation, key handling, session ownership, QoS, and asynchronous completion independently reviewable. If a patch grows substantially, split by previously untouched function groups or new implementation files.

For build-system changes, each patch inserts its own source entry at a distinct original anchor. No patch edits another patch’s added source list.

Validate the patch contract in a disposable writable checkout during implementation:

```text
Apply the complete series.
For every patch independently:
    git apply --reverse --check <patch>
    reverse-apply that patch
    reapply it
Verify tracked contents are unchanged after each round trip.
```

Passing reverse-apply does not imply the SDK must build with an individual prerequisite removed. Also audit overlapping hunks and added-line ownership explicitly.

### Game-side hooks

Add `crackdown/src/hooks_coop_trace.cpp` for opt-in tracing of the established guest wrappers and higher-level call sites. Record:

- Guest caller PC/LR and guest thread.
- Message IDs, flags, counts, pointers, and bounded decoded structures.
- Socket type, guest/native endpoints, payload length and hash.
- Key registration and address mappings.
- Session lifecycle and completion results.

Do not log raw keys or unbounded packet contents by default.

Add `hooks_coop_test.cpp` for unattended state checkpoints and scripted progression. Reuse `hooks_input_replay.cpp`, whose existing controller recording/replay facility is documented at `crackdown/src/hooks_input_replay.cpp:1`.

A release gameplay hook should only be added after a specific incompatibility is demonstrated. Do not bypass the guest’s connection or replication state machines.

### Proposed cvars

- `net_mode=off|lan|direct`
- `net_interface`
- `net_bind_ip`
- `net_advertise_ip`
- `net_peer_endpoints`
- `net_port_offset` or explicit `net_port_map`
- `net_control_port`
- `net_instance_id`
- `net_mac`
- `net_discovery_broadcast`
- `net_upnp=false`
- `net_trace=false`
- `coop_profile_xuid`, `coop_profile_name`
- `coop_fake_live=false`
- `coop_allow_multiplayer`
- `coop_test_role`, `coop_test_timeout_s`, `coop_test_report`

Keep existing `game_data_root`, `user_data_root`, and `cache_root` options.

If fake Live is enabled, state, sign-in info, profile type, XUID masks, privilege results, and network status must agree. Returning online state 2 in only one function is insufficient.

## 10. Unattended validation and required evidence

### Stage A: ABI and transport tests

Meaningful tests must exercise behavior across boundaries:

- Guest-memory byte fixtures for addresses, ports, keys, session buffers, and overlapped data.
- Two-process UDP/VDP round trip and TCP connect/accept.
- Broadcast/unicast discovery delivery.
- Nonblocking empty receive and proper last error.
- Socket query output sizes and values.
- Unknown handles, truncated buffers, malformed peer traffic.
- QoS cancellation/release and session deletion while work is pending.

The control/QoS protocols must reject mismatched versions/title/build identifiers.

### Stage B: two game processes on one PC

1. Start A and B with isolated profiles/data/logs and explicit port maps.
2. Launch A into System Link hosting using recorded input.
3. Wait for a trace checkpoint showing host discovery readiness.
4. Launch B into System Link search/join.
5. Require B to discover A’s real advertisement.
6. Require matching session ID/key association and successful address round trips.
7. Require bidirectional gameplay traffic and two agents present.
8. Replay distinct movement/actions on each process.
9. Disconnect B, verify A’s recovery, then rejoin where the guest supports it.

Use state-dependent barriers, not only fixed replay delays. Existing replay timing accounts for loading stalls but is not a network synchronization barrier.

Produce:

- `host.jsonl`, `client.jsonl`.
- Timestamped socket/message traces.
- Packet capture or decoded transport summary.
- Guest state checkpoints and screenshots.
- Machine-readable pass/fail report.
- SDK/game build hashes and configuration.

Pass requires evidence that one process’s actions affect the other process’s world: remote position changes, combat or vehicle interaction, and agreed session membership. Successful API returns or packet counts alone do not establish co-op.

### Stage C: two PCs on a physical LAN

Use distinct real LAN addresses and original guest ports. Verify:

- Discovery works without explicit peer configuration.
- Traffic uses the selected adapter.
- Host/client roles can be reversed.
- No master-service HTTP requests are required.
- Join, gameplay, disconnect, and rejoin behave correctly.
- Third-player join is rejected or unavailable.

### Stage D: internet deployment

Test separately:

1. Manual forwarding and explicit peers across different networks.
2. UPnP, with mapping evidence and cleanup.
3. Broadcast-capable virtual LAN.
4. Explicit unicast over a routed VPN.
5. Two processes behind one NAT.
6. NAT without hairpin support, unreachable peers, packet loss, and latency.

Run sustained gameplay and compare remote-agent state, mission progression, save ownership, disconnect behavior, and resource growth.

**No runtime evidence exists from this research task.** All logs and captures above are required implementation deliverables, not claimed results.

## 11. Risks and unresolved questions

### Async completion and object lifetime

The SDK’s `DispatchMessageAsync` is synchronous. XMsg immediately completes `XOVERLAPPED` and returns pending when supplied: `S/src/kernel/xam/xam_msg.cpp:54`–`:70`.

Real asynchronous work must:

- Preserve input data before the guest stack buffer expires.
- Capture the initiating guest thread.
- Complete outputs before signaling the event.
- Queue guest completion routines as APCs.
- Avoid double completion and use-after-free during cancellation/shutdown.

Existing completion machinery is at `S/src/system/kernel_state.cpp:1138`–`:1175`; deferred completion captures thread context at `:1216`. Do not call guest code directly from a native networking worker.

Socket overlapped structures and XAM overlapped structures are distinct ABIs.

### Transport and security

The SDK and fork bypass substantial Xbox network security. The proposed PC peer transport is not compatible with authenticated retail Xbox Live.

Treat the game’s peer parser as trusting input. Bound lengths, rates, session counts, and allocations at the host boundary; restrict control traffic to configured/discovered peers. Use an authenticated tunnel for untrusted internet paths unless a separately designed authenticated transport is added.

### Crackdown-specific behavior

- Two local processes currently share a fixed XUID and MAC unless corrected.
- VDP is definitely requested; mapping it to UDP must preserve the game’s expected framing.
- TCP 1002 exists, but its full gameplay role is unresolved.
- The discovery advertisement copies a 36-byte field in `G/crackdown_recomp.177.cpp:11137`; its exact semantic mapping requires decoding.
- Co-op save/progression ownership, join-in-progress restrictions, content-version checks, and host migration were not established.
- System Link’s exact XGI session flags and its use of QoS/search remain unproven.
- Presence and invites cannot be assumed to work by importing the fork: invite sending remains unsupported there.
- A fake Live facade may be needed for the Xbox Live menu route, but static evidence does not justify forcing it for System Link.

The first implementation milestone should resolve these uncertainties with traces before adding broad compatibility hooks.

## 12. Attribution and BSD-3 notices

Retain original per-file headers for copied/adapted code, including:

- Ben Vanik notices in inherited networking/XAM files.
- `Copyright 2026 Xenia Canary. All rights reserved.` in the fork’s session, XLiveAPI, and UPnP files where applicable.
- ReXGlue/Tom Clay notices in existing SDK files.

Add an adaptation note identifying the source fork, branch, and commit. Do not replace original notices with only a new project notice.

Keep the fork’s complete `LICENSE` text in source distributions and reproduce it in binary distribution documentation or other accompanying materials. Preserve the exact project/contributor non-endorsement clause. The local notice reads:

```text
Copyright (c) 2015, Ben Vanik.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the project nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL BEN VANIK BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

Source: `R/LICENSE:1`.

Third-party components retain their own notices independently. Porting UPnP code does not make miniupnpc or other bundled dependencies covered solely by Xenia’s BSD-3 notice.