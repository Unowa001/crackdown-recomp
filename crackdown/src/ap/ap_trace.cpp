// Developer-only save lifecycle trace (--ap_trace_save). Logs calls into the
// native save system so the AP bridge can bind to proven new/save/load points.
// 8254EDC8 builds the "Crackdown.sav" (820C7A8C) job; 82504128 / 82504098 are
// the SkillSet save/load wrappers. The pickup manager constructor and orb
// save/load serializers are hooked (and logged) by the AP bridge, bridge.cpp.
#include "generated/default/crackdown_init.h"

#include <rex/cvar.h>
#include <rex/hook.h>

REXCVAR_DEFINE_BOOL(ap_trace_save, false, "Crackdown",
                    "Developer: log native save/load/new-game calls (no behavior change)");

namespace {
void Trace(const char* name, PPCContext& ctx) {
  REXLOG_INFO("ap_trace: {} lr={:08X} r3={:08X} r4={:08X} r5={:08X} r6={:08X} r7={:08X}", name,
              static_cast<uint32_t>(ctx.lr), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
              ctx.r7.u32);
}
}  // namespace

#define AP_TRACE(addr)                                  \
  REX_HOOK_RAW(sub_##addr) {                            \
    if (REXCVAR_GET(ap_trace_save)) Trace(#addr, ctx);  \
    __imp__sub_##addr(ctx, base);                       \
  }

AP_TRACE(8254EDC8)
AP_TRACE(826734C8)
AP_TRACE(826737A8)
AP_TRACE(8268F0A8)
AP_TRACE(8268F038)

namespace {
void TraceSkills(const char* operation, uint32_t wrapper, uint8_t* base) {
  // Both wrappers resolve character through wrapper-2448, then getter +1220.
  if (wrapper < 2448) return;
  const uint32_t component = REX_LOAD_U32(wrapper - 2448);
  if (component < 32) return;
  const uint32_t character = component - 32;
  const uint32_t vtable = REX_LOAD_U32(character);
  if (vtable != 0x8208D850 || REX_LOAD_U32(vtable + 1220) != 0x82299350) return;
  const uint32_t skills = character + 9056;
  if (REX_LOAD_U32(skills + 8) != character) return;
  for (uint32_t index = 0; index < 5; ++index)
    REXLOG_INFO("ap_trace: skills {} index={} progression={} base={} bonus={}",
                operation, index, REX_LOAD_U8(skills + 17 + index),
                REX_LOAD_U8(skills + 12 + index), REX_LOAD_U8(skills + 22 + index));
}
}

REX_HOOK_RAW(sub_82504128) {
  const uint32_t wrapper = ctx.r3.u32;
  if (REXCVAR_GET(ap_trace_save)) {
    Trace("82504128", ctx);
    TraceSkills("save", wrapper, base);
  }
  __imp__sub_82504128(ctx, base);
}

REX_HOOK_RAW(sub_82504098) {
  const uint32_t wrapper = ctx.r3.u32;
  if (REXCVAR_GET(ap_trace_save)) Trace("82504098", ctx);
  __imp__sub_82504098(ctx, base);
  if (REXCVAR_GET(ap_trace_save)) TraceSkills("load", wrapper, base);
}
