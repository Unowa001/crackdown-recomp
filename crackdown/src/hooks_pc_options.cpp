#include "generated/default/crackdown_init.h"
#include "pc_options.h"
#include "pc_settings.h"
#include "xui_patch.h"

#include <exception>
#include <string>
#include <string_view>
#include <utility>

#include <rex/hook.h>
#include <rex/logging/macros.h>

namespace {
std::function<bool()> request_quit;

bool GuestIdEquals(uint8_t* base, uint32_t address, std::u16string_view expected) {
  if (!address) return false;
  for (size_t i = 0; i < expected.size(); ++i)
    if (REX_LOAD_U16(address + static_cast<uint32_t>(2 * i)) != expected[i]) return false;
  return REX_LOAD_U16(address + static_cast<uint32_t>(2 * expected.size())) == 0;
}

// Side calls get their own ABI-aligned frame and copied register context.
// The out-pointer at +80 belongs to this frame, below the intercepted caller.
PPCContext SideCall(const PPCContext& parent, uint8_t* base) {
  PPCContext call = parent;
  call.r1.u32 -= 256;
  REX_STORE_U32(call.r1.u32, parent.r1.u32);
  return call;
}
// XuiElementGetChildById 0x826E1D00 (.49.cpp:52116): r3 parent hObj,
// r4 UTF-16 name, r5 output; "stw r29,0(r30)", "lwz r31,24(r3)",
// "bl 0x82ab9d98" compares child IDs, "stw r3,0(r30)" returns hObj.
uint32_t FindChild(const PPCContext& ctx, uint8_t* base, uint32_t parent, std::u16string_view name) {
  auto call = SideCall(ctx, base);
  const auto out = call.r1.u32 + 80;
  const auto text = call.r1.u32 + 96;
  for (size_t i = 0; i <= name.size(); ++i)
    REX_STORE_U16(text + static_cast<uint32_t>(i * 2), i == name.size() ? 0 : name[i]);
  REX_STORE_U32(out, 0);
  call.r3.u64 = parent;
  call.r4.u64 = text;
  call.r5.u64 = out;
  sub_826E1D00(call, base);
  return call.r3.s32 < 0 ? 0 : REX_LOAD_U32(out);
}

uint32_t ElementId(const PPCContext& ctx, uint8_t* base) {
  auto call = SideCall(ctx, base);
  const auto out = call.r1.u32 + 80;
  REX_STORE_U32(out, 0);
  call.r4.u64 = out;
  sub_826E1CA8(call, base);
  return call.r3.s32 < 0 ? 0 : REX_LOAD_U32(out);
}
std::string LogId(uint8_t* base, uint32_t id) {
  std::string text;
  if (!id) return "<none>";
  for (uint32_t i = 0; i < 80; ++i) {
    const auto c = REX_LOAD_U16(id + 2 * i);
    if (!c) break;
    text += c < 128 ? static_cast<char>(c) : '?';
  }
  return text;
}

// 0x82628C78 (.43.cpp:73983): "lwz r11,44(r28)", "lwz r31,-4(r11)"
// reads the last screen from the manager's 12-byte stack items. Base ctor
// 0x826275A0 (.43.cpp:70496): "stw r11,128(r31)" id, "stw r5,132(r31)" scene.
uint32_t TopScreen(const PPCContext& ctx, uint8_t* base) {
  auto call = SideCall(ctx, base);
  sub_823508F8(call, base);
  const auto manager = call.r3.u32;
  if (!manager) return 0;
  const auto begin = REX_LOAD_U32(manager + 40), end = REX_LOAD_U32(manager + 44);
  if (!begin || end <= begin || end - begin < 12) return 0;
  return REX_LOAD_U32(end - 4);
}

thread_local bool setting_selection = false;
void SetSelection(const PPCContext& ctx, uint8_t* base, uint32_t handle, int selection) {
  const bool previous = std::exchange(setting_selection, true);
  auto call = SideCall(ctx, base);
  call.r3.u64 = handle;
  call.r4.s64 = selection;
  sub_8269B5D8(call, base);
  setting_selection = previous;
}

// 0x8260A068 (.43.cpp:641): "cmplwi cr6,r10,14", "lwz r9,16(r31)",
// "lwz r9,0(r9)", "cmplwi cr6,r9,4", "addi r3,r11,52": radio change
// event. Code 2 dispatches +92 (focus). Code 4 is supported by disassembly,
// but actual controller/mouse notifications remain unverified at runtime.
// 0x8269C378 (.47.cpp:32162) sends XM 2007 via 0x826FA928 (.50.cpp:34676)
// and returns "lwz r3,80(r1)". Use selection, never an unguarded focus-index
// read: 0x82624198 loops forever without focus; 0x82624120 must guard it if
// that alternative is ever used. We do not call either focus helper here.
void NotifySettings(const PPCContext& ctx, uint8_t* base) {
  const auto screen = TopScreen(ctx, base);
  if (!screen || REX_LOAD_U32(screen + 128) != 26) return;
  const auto payload = REX_LOAD_U32(ctx.r4.u32 + 16);
  const auto code = payload ? REX_LOAD_U32(payload) : 0;
  const auto id = ElementId(ctx, base);
  REXLOG_DEBUG("pc settings: notify code={} id={} hObj=0x{:08X} initializing={}",
               code, LogId(base, id), ctx.r3.u32, setting_selection);
  if (code != 4 || setting_selection) return;
  const std::u16string_view ids[] = {u"framerate", u"resolution", u"display"};
  for (size_t i = 0; i < 3; ++i) {
    if (!GuestIdEquals(base, id, ids[i])) continue;
    auto call = SideCall(ctx, base);
    sub_8269C378(call, base);
    const auto selection = call.r3.s32;
    if (selection < 0 || selection > 1) return;
    const auto setting = static_cast<crackdown::PCSetting>(i);
    try {
      if (crackdown::PCSettings().Save(setting, selection)) {
        const char* keys[] = {"vsync", "resolution_scale", "fullscreen"};
        const char* values[] = {"true", "2", "false"};
        REXLOG_INFO("pc settings: saved {}={} (restart required)",
                    keys[i], selection == 0 ? "default (key removed)" : values[i]);
      }
    } catch (const std::exception& error) {
      REXLOG_WARN("pc settings: save failed; restoring selection ({})", error.what());
      SetSelection(ctx, base, ctx.r3.u32, crackdown::PCSettings().Selection(setting));
    }
    return;
  }
}
}  // namespace

namespace crackdown {
void SetPCMenuQuitAction(std::function<bool()> quit) {
  request_quit = std::move(quit);
}
}  // namespace crackdown

// Guest 0x8269C3C0 (generated .47.cpp): "lwz r11,0(r5)", "mr r3,r5",
// "lwz r11,0(r11)", "mtctr r11", "bctr" dispatches every wrapper class.
// Use this common ObjectProc rather than assume all navbutton subclasses use
// sub_8260A628. GetId 0x826E1CA8 (.49.cpp) does "mr r8,r4", "stw r11,0(r8)",
// and on success "lwz r11,4(r3)", "li r3,0", "stw r11,0(r8)": UTF-16 Id out.
// Button handler 0x8260A628 (.43.cpp): "lwz r10,4(r31)", "cmplwi cr6,r10,2021",
// "li r11,1", "stw r11,8(r31)"; handled XM_PRESS returns "li r3,0".
// Quit callback 0x82678768 (.46.cpp): "bl 0x8268d318", "li r11,0",
// "stw r11,4(r3)", "bl 0x823508f8", "li r7,0", "li r6,0", "li r5,0",
// "li r4,76", "bl 0x82628918", "li r3,1". Parent r7 is explicitly NULL.
// We change only the new Id's mode to 2 and otherwise reproduce that setup.
// Confirmed on the PC 2026-10-12 (log crackdown_002; receipt in docs/pc-options.md).
REX_HOOK_RAW(sub_8269C3C0) {
  if (!crackdown::PCMenuGuestPath() || !ctx.r3.u32 || !ctx.r4.u32 ||
      (REX_LOAD_U32(ctx.r4.u32 + 4) != 2021 && REX_LOAD_U32(ctx.r4.u32 + 4) != 14)) {
    __imp__sub_8269C3C0(ctx, base);
    return;
  }
  if (REX_LOAD_U32(ctx.r4.u32 + 4) == 14) {
    NotifySettings(ctx, base);
    // Keep native notification propagation and Back processing untouched.
    __imp__sub_8269C3C0(ctx, base);
    return;
  }
  const uint32_t message = ctx.r4.u32;
  auto call = SideCall(ctx, base);
  const uint32_t id = ElementId(ctx, base);
  if (GuestIdEquals(base, id, u"quitGame")) {
    call = SideCall(ctx, base);
    sub_8268D318(call, base);
    REX_STORE_U32(call.r3.u32 + 4, 2);
    sub_823508F8(call, base);
    call.r4.s64 = 76;
    call.r5.s64 = 0;
    call.r6.s64 = 0;
    call.r7.s64 = 0;
    sub_82628918(call, base);
    REXLOG_INFO("pc menu: quitGame -> confirmation (screen 76, mode 2)");
  } else if (GuestIdEquals(base, id, u"pcSettings")) {
    // Tab1 Display callback 0x82678C88 (.46.cpp:26622): "bl 0x8268dc40",
    // "lwz r31,4(r3)", "bl 0x823508f8", "mr r7,r31", "li r6,0",
    // "li r5,0", "li r4,74", "bl 0x82628918". Replace only id with 26.
    // Native screen route is unverified at runtime (milestone 2).
    call = SideCall(ctx, base);
    sub_8268DC40(call, base);
    const auto parent = REX_LOAD_U32(call.r3.u32 + 4);
    sub_823508F8(call, base);
    call.r4.s64 = 26;
    call.r5.s64 = 0;
    call.r6.s64 = 0;
    call.r7.u64 = parent;
    sub_82628918(call, base);
    REXLOG_INFO("pc menu: pcSettings -> native PC Settings (screen 26)");
  } else {
    __imp__sub_8269C3C0(ctx, base);
    return;
  }
  REX_STORE_U32(message + 8, 1);
  ctx.r3.s64 = 0;  // ObjectProc HRESULT S_OK; callback bool is a different ABI.
}

// Guest 0x8266B928 (.45.cpp:76162): "mr r31,r3", "mr r30,r5",
// "bl 0x82627528" constructs screen 26; child AgentProfiles lookup at
// 0x8266B9C4 -> "lwz r3,4(r3)" -> 0x826FA9E0; AudioSetup lookup at
// 0x8266B9FC -> "addi r3,r30,52" -> 0x82696898 subscribes Audio callback.
// options.xur includes both hidden/disabled wrappers; run this ctor unchanged.
// 0x8269B5D8 (.47.cpp:30069): "mr r31,r3", "mr r5,r4", "bl 0x8269b620",
// "mr r3,r31", "bl 0x826d9790" sends XM 2008 with the selection index.
// New initial-selection behavior is unverified at runtime (milestone 2).
REX_HOOK_RAW(sub_8266B928) {
  const uint32_t object = ctx.r3.u32;
  __imp__sub_8266B928(ctx, base);
  if (!crackdown::PCSceneGuestPath() || !object) return;
  const auto scene = REX_LOAD_U32(object + 132);
  if (!scene) return;
  const auto handle = REX_LOAD_U32(scene + 4);
  const std::u16string_view ids[] = {u"framerate", u"resolution", u"display"};
  for (size_t i = 0; i < 3; ++i) {
    const auto child = FindChild(ctx, base, handle, ids[i]);
    if (!child) {
      REXLOG_WARN("pc settings: missing radio group {}", i);
      continue;
    }
    SetSelection(ctx, base, child, crackdown::PCSettings().Selection(static_cast<crackdown::PCSetting>(i)));
  }
  REXLOG_INFO("pc settings: initialized screen 26 (framerate={}, resolution={}, display={}; restart required)",
              crackdown::PCSettings().Selection(crackdown::PCSetting::framerate),
              crackdown::PCSettings().Selection(crackdown::PCSetting::resolution),
              crackdown::PCSettings().Selection(crackdown::PCSetting::display));
}

// Guest 0x826587F8 (generated .45.cpp): "bl 0x8268d318", "lwz r11,4(r3)",
// "cmplwi cr6,r11,1", "blt cr6,0x82658874", "bne cr6,0x82658880".
// Mode 0 at 0x82658874 calls "bl 0x82691308", "li r4,1", "bl 0x82672dd8"
// (save/return to menu); mode 1 extracts; >=2 skips to "li r3,1" at 0x82658880.
// Only mode 2 while the mod is active queues Window::RequestClose on the UI
// thread, reusing ReXApp::OnClosing -> TerminateTitle -> FlushLogging -> _Exit.
// Confirmed on the PC 2026-10-12 (log crackdown_002; receipt in docs/pc-options.md).
REX_HOOK_RAW(sub_826587F8) {
  if (crackdown::PCMenuGuestPath()) {
    auto call = SideCall(ctx, base);
    sub_8268D318(call, base);
    if (REX_LOAD_U32(call.r3.u32 + 4) == 2 && request_quit && request_quit()) {
      REXLOG_INFO("pc menu: confirmed Quit to Desktop; window close queued");
      ctx.r3.s64 = 1;
      return;
    }
  }
  __imp__sub_826587F8(ctx, base);
}
