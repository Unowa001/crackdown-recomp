// crackdown - --diag_audio: XMA voice lifetimes (diagnostic, off by default)
//
// Speech at 60 fps (user report 2026-10-13, Beta 0.3.0): every Agency Director
// line stops about halfway. To see who stops it, log each XAudio XMA voice's
// context setup and teardown with its lifetime and guest call chain:
// - sub_82A524F8(voice r3): for each of [voice+0] streams (96-byte entries at
//   [voice+8]) calls XMACreateContext (0x82B67E74) into entry+64.
// - sub_82A51AA0(voice r3): the matching XMAReleaseContext (0x82B67E64) loop.
// A line cut in half shows up as a voice living half as long at 60 fps.
// The same file retimes the game wave safety counter at 0x8225B254 behind
// --sim_audio_timeout_30hz, independently of the opt-in diagnostic.

#include "generated/default/crackdown_init.h"
#include "midasm_hooks.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <mutex>
#include <string>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/hook.h>

REXCVAR_DEFINE_BOOL(diag_audio, false, "Crackdown",
                    "Diagnostic: log XMA voice context setup/teardown with lifetime and guest "
                    "call chain");
REXCVAR_DEFINE_BOOL(sim_audio_timeout_30hz, false, "Crackdown",
                    "With a measured simulation step: advance wave safety timeouts at 30 Hz "
                    "so speech is not stopped early above 30 fps. On by default");

namespace {
using Clock = std::chrono::steady_clock;
std::mutex g_mutex;
std::unordered_map<uint32_t, Clock::time_point> g_started;
std::unordered_map<uint32_t, std::string> g_clip;
thread_local std::string g_creating;
thread_local std::string g_game_clip;
thread_local uint32_t g_created_voice = 0;
std::mutex g_timeout_mutex;
std::unordered_map<uint32_t, double> g_timeout_carry;

bool AudioTimeout30Hz() {
  static const bool enabled = REXCVAR_GET(sim_audio_timeout_30hz);
  return enabled;
}

// Raw words retain evidence while the XAudio descriptor layout is being
// identified. Read only at the existing creation/release boundaries.
std::string Words(uint32_t address, uint32_t count, uint8_t* base) {
  std::string result;
  for (uint32_t i = 0; i < count; ++i) {
    result += std::format(" {:08X}", REX_LOAD_U32(address + i * 4));
  }
  return result;
}

std::string Streams(uint32_t voice, uint8_t* base) {
  std::string result;
  const uint32_t count = REX_LOAD_U32(voice);
  const uint32_t entries = REX_LOAD_U32(voice + 8);
  for (uint32_t i = 0; i < count && i < 8; ++i) {
    const uint32_t entry = entries + i * 96;
    const uint32_t context = REX_LOAD_U32(entry + 64);
    result += std::format(" stream{}={} context={:08X}", i, Words(entry, 24, base), context);
    if (context) result += Words(context, 10, base);
  }
  return result;
}

// Each frame's back chain is at [fp]; the return address it saved is at
// [caller fp - 8] (same walk as diag_char_proxy).
std::string Chain(PPCContext& ctx, uint8_t* base) {
  std::string chain = std::format("{:08X}", static_cast<uint32_t>(ctx.lr));
  uint32_t fp = ctx.r1.u32;
  // SDK X_KPCR / XThread::Create: r13+0x70 is the exclusive high stack
  // address, +0x74 the low address. The root back chain may equal the high
  // address, whose page is not mapped (CDB diag-crash-debug.txt).
  const uint32_t stack_high = REX_LOAD_U32(ctx.r13.u32 + 0x70);
  const uint32_t stack_low = REX_LOAD_U32(ctx.r13.u32 + 0x74);
  if (fp < stack_low || fp >= stack_high) return chain;
  for (int i = 0; i < 12 && fp; ++i) {
    const uint32_t next = REX_LOAD_U32(fp);
    // The host/guest entry frame ends the PPC back chain. Its saved word
    // need not be a guest stack pointer; never follow it outside this stack.
    if (next <= fp || next >= stack_high || (next & 15)) break;
    fp = next;
    chain += std::format(" {:08X}", REX_LOAD_U32(fp - 8));
  }
  return chain;
}
}  // namespace

REX_HOOK_RAW(sub_82A524F8) {
  if (!REXCVAR_GET(diag_audio)) {
    __imp__sub_82A524F8(ctx, base);
    return;
  }
  const uint32_t voice = ctx.r3.u32;
  const std::string chain = Chain(ctx, base);
  __imp__sub_82A524F8(ctx, base);
  std::lock_guard lock(g_mutex);
  g_started[voice] = Clock::now();
  g_created_voice = voice;
  g_clip[voice] = g_game_clip + " " + g_creating;
  REXLOG_INFO("diag_audio: start voice={:08X} streams={} ret={:08X} chain {}", voice,
              REX_LOAD_U32(voice), ctx.r3.u32, chain);
  REXLOG_INFO("diag_audio: detail voice={:08X} clip={}{}", voice, g_clip[voice],
              Streams(voice, base));
}

REX_HOOK_RAW(sub_82A51AA0) {
  if (!REXCVAR_GET(diag_audio)) {
    __imp__sub_82A51AA0(ctx, base);
    return;
  }
  const uint32_t voice = ctx.r3.u32;
  const std::string chain = Chain(ctx, base);
  const std::string streams = Streams(voice, base);
  __imp__sub_82A51AA0(ctx, base);
  std::lock_guard lock(g_mutex);
  double ms = -1.0;
  if (auto it = g_started.find(voice); it != g_started.end()) {
    ms = std::chrono::duration<double, std::milli>(Clock::now() - it->second).count();
    g_started.erase(it);
  }
  REXLOG_INFO("diag_audio: stop voice={:08X} life_ms={:.0f} chain {}", voice, ms, chain);
  REXLOG_INFO("diag_audio: final voice={:08X} clip={}{}", voice, g_clip[voice], streams);
  g_clip.erase(voice);
}

// 0x82A4A030 -> 0x82A53DE8 -> 0x82A53690 creates the source synchronously.
// The descriptor is copied as 56 bytes at 0x82A536B4; its additional fields
// at +56..+88 are read by 0x82A536B8..0x82A53748.
REX_HOOK_RAW(sub_82A4A030) {
  if (!REXCVAR_GET(diag_audio)) {
    __imp__sub_82A4A030(ctx, base);
    return;
  }
  const std::string previous = g_creating;
  const uint32_t descriptor = ctx.r3.u32;
  g_creating = std::format("desc={:08X} words={} chain={}", descriptor,
                           Words(descriptor, 23, base), Chain(ctx, base));
  __imp__sub_82A4A030(ctx, base);
  g_creating = previous;
}

// Source Start: flags bit 0 chooses deferred activation (0x82A53B18),
// otherwise immediate activation (0x82A53770). The game Stop wrapper is
// 0x82A49AC0 (vtable +60); timeout producer is logged by 0x8225B0A8 below.
REX_HOOK_RAW(sub_82A53EE8) {
  if (REXCVAR_GET(diag_audio)) {
    REXLOG_INFO("diag_audio: request_start source={:08X} flags={:08X} chain {}",
                ctx.r3.u32, ctx.r4.u32, Chain(ctx, base));
  }
  __imp__sub_82A53EE8(ctx, base);
}

// Game wave instance: 0x8225AB30 reads a wave-bank entry's sample duration
// through 0x8225BBD8, sets +8 = floor(samples / 48000) * 30 + 70,
// initializes elapsed frames +12, then creates the XAudio source into +0.
REX_HOOK_RAW(sub_8225AB30) {
  if (AudioTimeout30Hz()) {
    std::lock_guard lock(g_timeout_mutex);
    g_timeout_carry.erase(ctx.r3.u32);  // pooled instance reused for a new clip
  }
  if (!REXCVAR_GET(diag_audio)) {
    __imp__sub_8225AB30(ctx, base);
    return;
  }
  const uint32_t instance = ctx.r3.u32;
  const uint32_t bank = ctx.r4.u32;
  const uint32_t index = ctx.r5.u32;
  const uint32_t header = REX_LOAD_U32(bank + 8);
  const uint32_t entries = REX_LOAD_U32(bank + 12);
  uint32_t samples = 0;
  if (index < REX_LOAD_U32(header + 4)) {
    samples = (REX_LOAD_U32(header) & 0x20000)
                  ? REX_LOAD_U32(entries + index * 8 + 4)
                  : REX_LOAD_U32(entries + index * 24) >> 4;
  }
  std::string name;
  for (uint32_t i = 0; i < 64; ++i) {
    const char c = static_cast<char>(REX_LOAD_U8(header + 8 + i));
    if (!c) break;
    name += (c >= 32 && c < 127) ? c : '?';
  }
  const std::string previous = g_game_clip;
  g_game_clip = std::format("instance={:08X} bank={:08X} name={} index={} samples={} "
                            "length_ms={:.1f} loop={}", instance, bank, name, index,
                            samples, double(samples) / 48.0, ctx.r6.u32 & 0xFF);
  g_created_voice = 0;
  __imp__sub_8225AB30(ctx, base);
  REXLOG_INFO("diag_audio: game_start {} source={:08X} voice={:08X} deadline={} state={}",
              g_game_clip, REX_LOAD_U32(instance), g_created_voice,
              REX_LOAD_U32(instance + 8), REX_LOAD_U8(instance + 16));
  g_game_clip = previous;
}

// +12 increments at 0x8225B254; > +8 stops the source at 0x8225B274.
REX_HOOK_RAW(sub_8225B0A8) {
  if (!REXCVAR_GET(diag_audio)) {
    __imp__sub_8225B0A8(ctx, base);
    return;
  }
  const uint32_t instance = ctx.r3.u32;
  const uint32_t source = REX_LOAD_U32(instance);
  const uint32_t elapsed = REX_LOAD_U32(instance + 12);
  const uint32_t deadline = REX_LOAD_U32(instance + 8);
  const uint8_t state = REX_LOAD_U8(instance + 16);
  __imp__sub_8225B0A8(ctx, base);
  if (state == 2 && REX_LOAD_U8(instance + 16) == 3) {
    REXLOG_INFO("diag_audio: game_stop instance={:08X} source={:08X} elapsed={}->{} "
                "deadline={} chain {}", instance, source, elapsed,
                REX_LOAD_U32(instance + 12), deadline, Chain(ctx, base));
  }
}

// 0x8225B250..0x8225B274: lwz r11,12(r30); addi r11,r11,1;
// stw r11,12(r30); ... cmplw elapsed,deadline; ble ...; bl Stop.
// Change only that increment, after the addi and before the store/comparison.
// Each instance carries its own fractional ticks; completed/looping/paused
// paths that skip this instruction keep their original behavior.
void CrackdownAudioTimeoutTick(PPCRegister& elapsed, PPCRegister& instance) {
  if (!AudioTimeout30Hz()) return;
  const double ticks = CrackdownMeasuredStepSeconds() * 30.0;
  std::lock_guard lock(g_timeout_mutex);
  double& carry = g_timeout_carry[instance.u32];
  carry += ticks;
  const uint32_t whole = static_cast<uint32_t>(std::floor(carry));
  carry -= whole;
  elapsed.u64 = elapsed.u32 - 1u + whole;
}
