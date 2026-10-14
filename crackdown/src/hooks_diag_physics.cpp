// crackdown - diagnostic: the time step Havok's simulation receives
//
// The game links Havok Physics (image strings "Havok libs built with version",
// .\world\hkWorld.cpp). These guest functions reference Havok's "Simulate"
// timer strings (0x8204D120 / 0x8204D134) and take a float argument in f1,
// copied at entry (fmr f29,f1 / fmr f31,f1): the per-simulation-type step
// functions, f1 = delta time in seconds.
//     sub_8299B808, sub_8299EFB8, sub_829BD048, sub_829BE628
//
// At 60 fps with only the step constant 0x820ED704 changed (--fps_cap=60
// --sim_game_clock --sim_game_clock_sites=0), physics looks twice as fast: cars
// too fast and turning on a dime, NPC cars driving into walls, speed building
// up running down stairs. Stock and --fps_cap=30 --sim_game_clock are fine.
// This logs what Havok is actually given, to see whether it follows the step.
//
// --diag_physics_dt: log f1, the caller (LR) and the step constant for the
// first 5 calls of each function, then every 600th call.

#include "generated/default/crackdown_init.h"

#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>

#include "input_replay.h"

REXCVAR_DEFINE_BOOL(diag_physics_dt, false, "Crackdown",
                    "Diagnostic: log the delta time Havok's simulation step functions receive");
REXCVAR_DEFINE_BOOL(diag_char_proxy, false, "Crackdown",
                    "Diagnostic: log the step info and velocities of Havok character proxy "
                    "integrations (sub_822BF268)");
REXCVAR_DEFINE_STRING(teleport, "", "Crackdown",
                      "Test tool: \"x,y,z\": move the player there once, --teleport_delay_s "
                      "after gameplay starts (positions are in --diag_char_proxy lines). "
                      "\"x,y,z;x,y,z\" adds hops, each --teleport_step_s after the previous");
REXCVAR_DEFINE_DOUBLE(teleport_delay_s, 1.0, "Crackdown",
                      "With --teleport: seconds after gameplay starts");
REXCVAR_DEFINE_DOUBLE(teleport_step_s, 6.0, "Crackdown",
                      "With a multi-hop --teleport: seconds between hops");
REXCVAR_DEFINE_BOOL(diag_vehicle, false, "Crackdown",
                    "Diagnostic: log the Havok vehicle nearest the player every step, and "
                    "velocity jumps of nearby vehicles (game vehicle step sub_824A1708)");
REXCVAR_DEFINE_DOUBLE(diag_vehicle_dv, 3.0, "Crackdown",
                      "With --diag_vehicle: log any vehicle within 40 m whose velocity changes by "
                      "more than this in one step (m/s)");
REXCVAR_DEFINE_STRING(teleport_vehicle, "", "Crackdown",
                      "Test tool, with --diag_vehicle: \"x,y,z,yaw\": move the vehicle the player "
                      "is in (or next to) there once, --teleport_vehicle_delay_s after gameplay "
                      "starts; yaw in degrees about +y, 0 = the chassis' local axes on the world's");
REXCVAR_DEFINE_DOUBLE(teleport_vehicle_delay_s, 5.5, "Crackdown",
                      "With --teleport_vehicle: seconds after gameplay starts");
REXCVAR_DEFINE_DOUBLE(diag_char_speed, 9.0, "Crackdown",
                      "With --diag_char_proxy: also log every integration whose speed in or out "
                      "exceeds this (m/s)");

namespace {
constexpr uint32_t kStepConstantAddress = 0x820ED704;

void LogStep(const char* name, std::atomic<uint32_t>& calls, PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(diag_physics_dt)) {
    return;
  }
  const uint32_t n = calls.fetch_add(1, std::memory_order_relaxed);
  if (n >= 5 && n % 600 != 0) {
    return;
  }
  const uint32_t step_bits = REX_LOAD_U32(kStepConstantAddress);
  float step;
  std::memcpy(&step, &step_bits, sizeof(step));
  REXLOG_INFO("diag_physics_dt: {} call {} dt={:.6f} (1/{:.2f}) lr={:08X} r3={:08X} step={:.6f}",
              name, n, ctx.f1.f64, ctx.f1.f64 != 0 ? 1.0 / ctx.f1.f64 : 0.0,
              static_cast<uint32_t>(ctx.lr), ctx.r3.u32, step);
}

std::atomic<uint32_t> g_calls_b808, g_calls_efb8, g_calls_d048, g_calls_e628;
std::atomic<uint32_t> g_calls_37d0, g_calls_6b40, g_calls_9ae8, g_calls_8a48, g_calls_9568,
    g_calls_a828;
}  // namespace

REX_HOOK_RAW(sub_8299B808) {
  LogStep("sub_8299B808", g_calls_b808, ctx, base);
  __imp__sub_8299B808(ctx, base);
}

REX_HOOK_RAW(sub_8299EFB8) {
  LogStep("sub_8299EFB8", g_calls_efb8, ctx, base);
  __imp__sub_8299EFB8(ctx, base);
}

REX_HOOK_RAW(sub_829BD048) {
  LogStep("sub_829BD048", g_calls_d048, ctx, base);
  __imp__sub_829BD048(ctx, base);
}

REX_HOOK_RAW(sub_829BE628) {
  LogStep("sub_829BE628", g_calls_e628, ctx, base);
  __imp__sub_829BE628(ctx, base);
}

// Havok entry points the game's character/vehicle code calls with a float in
// f1 (call sites found by scanning for `lfs/fmr f1` before `bl` into the
// Havok range 0x82940000..0x82A00000): sub_822BF268 ("updateCharacter",
// cCharacterMotion) and sub_822BE450 call 0x829D37D0 with f1 = [obj+164] once
// per character, and 0x82986B40 with f1 from the stack. The stairs bug at 60 fps
// survives a correct world step, so look for a character/vehicle step that
// stays at 1/30.
#define CRACKDOWN_DIAG_F1_HOOK(fn, counter)     REX_HOOK_RAW(fn) {                              LogStep(#fn, counter, ctx, base);             __imp__##fn(ctx, base);                     }
CRACKDOWN_DIAG_F1_HOOK(sub_829D37D0, g_calls_37d0)
CRACKDOWN_DIAG_F1_HOOK(sub_82986B40, g_calls_6b40)
CRACKDOWN_DIAG_F1_HOOK(sub_829D9AE8, g_calls_9ae8)
CRACKDOWN_DIAG_F1_HOOK(sub_82988A48, g_calls_8a48)
CRACKDOWN_DIAG_F1_HOOK(sub_82989568, g_calls_9568)
CRACKDOWN_DIAG_F1_HOOK(sub_8298A828, g_calls_a828)
#undef CRACKDOWN_DIAG_F1_HOOK

// hkCharacterProxy::integrateWithCollectors (Havok utilities linked into the
// game section; .pdata 0x820F66C8, vtable slot 0x8208A780): r3 = proxy,
// r4 = const hkStepInfo& {startTime, endTime, deltaTime, invDeltaTime},
// r5 = world gravity. It opens the "updateCharacter" timer, loops while
// remainingTime = [r4+8] > eps (at most 10 casts), then at 0x822BFDC0 stores the
// solver output velocity to proxy+16 (m_velocity) and calls
// setPosition(proxy+48 = phantom, position, [proxy+88]+[proxy+92]).
//
// --diag_char_proxy logs the step info, gravity and m_velocity before and after
// for the first 8 calls, every integration faster than --diag_char_speed, and
// once a second the fastest proxies seen in that second. Run down the same
// stairs at 30 and 60 fps: shows whether the game hands the proxy a stale rate
// (invDeltaTime) and whether the extra speed comes in with the input velocity
// (game state machine) or out of the solver.
namespace {
struct Vec4 {
  float x, y, z, w;
  float Length3() const { return std::sqrt(x * x + y * y + z * z); }
};

Vec4 LoadVec4(uint8_t* base, uint32_t ea) {
  return {std::bit_cast<float>(REX_LOAD_U32(ea)), std::bit_cast<float>(REX_LOAD_U32(ea + 4)),
          std::bit_cast<float>(REX_LOAD_U32(ea + 8)), std::bit_cast<float>(REX_LOAD_U32(ea + 12))};
}

struct ProxyStats {
  uint32_t proxy;
  uint32_t calls;
  float max_speed;
};

void NoteCharacterPosition(const Vec4& pos);

std::mutex g_proxy_mutex;
// Atomic: also read outside g_proxy_mutex (the call-chain sampling below).
std::atomic<uint32_t> g_proxy_calls{0};
ProxyStats g_proxy_stats[32];
uint32_t g_proxy_count = 0;
std::chrono::steady_clock::time_point g_proxy_window;

void FlushProxyStats() {
  // Up to the 4 fastest proxies this window.
  for (int k = 0; k < 4 && g_proxy_count; ++k) {
    uint32_t best = 0;
    for (uint32_t i = 1; i < g_proxy_count; ++i) {
      if (g_proxy_stats[i].max_speed > g_proxy_stats[best].max_speed) best = i;
    }
    if (g_proxy_stats[best].max_speed < 0) break;
    REXLOG_INFO("diag_char_proxy: 1s top{} proxy={:08X} calls={} max|v|={:.3f}", k,
                g_proxy_stats[best].proxy, g_proxy_stats[best].calls, g_proxy_stats[best].max_speed);
    g_proxy_stats[best].max_speed = -1.0f;
  }
  g_proxy_count = 0;
}
}  // namespace

namespace {
// --teleport. The character controller update sub_822A9E78 (r3 = controller,
// f1 = dt; the proxy is [controller+660]) starts with a branch to 0x822AB5CC
// that, while the player is pinned (spawn fade-in, ...), only does
// setPosition(proxy, controller+336) - so the controller's own position at
// +336 wins over the proxy's. Moving only the proxy (first version, PC run
// crackdown_052) was undone the same frame. So write both, before the update:
// controller+336 and setPosition (sub_829D3560: r3 = proxy, r4 = &pos; it
// adds the proxy's keep distance and calls the phantom's sub_82986B40, which
// reads the vector before pushing any frame), and zero the velocity at
// controller+352 and the proxy's m_velocity (+16). hkCharacterProxy::
// getPosition (sub_829D3550) is [[proxy+48]+28]+48, logged by --diag_char_proxy.
// Done once, --teleport_delay_s after gameplay starts.
bool ParseVec3(const std::string& spec, float (&v)[3]) {
  const char* p = spec.data();
  const char* end = p + spec.size();
  for (int i = 0; i < 3; ++i) {
    const auto r = std::from_chars(p, end, v[i]);
    if (r.ec != std::errc{} || (i == 2 ? r.ptr != end : r.ptr >= end || *r.ptr != ',')) return false;
    // from_chars accepts nan/inf, which would poison the Havok state.
    if (!std::isfinite(v[i])) return false;
    p = r.ptr + 1;
  }
  return true;
}

void MaybeTeleport(PPCContext& ctx, uint8_t* base, uint32_t controller) {
  // Character controllers may update on several guest threads.
  static std::atomic<bool> done{false};
  static std::mutex mutex;
  static std::chrono::steady_clock::time_point start;
  static size_t hop = 0;
  if (done.load(std::memory_order_acquire) || !crackdown::GameplayStarted()) return;
  std::lock_guard lock(mutex);
  if (done.load(std::memory_order_relaxed)) return;
  std::string spec = REXCVAR_GET(teleport);
  std::vector<std::string> hops;
  for (size_t at = 0; at <= spec.size();) {
    const size_t end = std::min(spec.find(';', at), spec.size());
    if (end > at) hops.push_back(spec.substr(at, end - at));
    at = end + 1;
  }
  if (hop >= hops.size()) {
    done.store(true, std::memory_order_release);
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (start == std::chrono::steady_clock::time_point{}) start = now;
  const double due = REXCVAR_GET(teleport_delay_s) + double(hop) * REXCVAR_GET(teleport_step_s);
  if (now - start < std::chrono::duration<double>(due)) return;
  spec = hops[hop++];
  if (hop == hops.size()) done.store(true, std::memory_order_release);
  float v[3];
  if (!ParseVec3(spec, v)) {
    REXLOG_ERROR("teleport: bad position \"{}\" (want x,y,z)", spec);
    return;
  }
  const uint32_t proxy = REX_LOAD_U32(controller + 660);
  const uint32_t pos = controller + 336;
  for (int i = 0; i < 3; ++i) REX_STORE_U32(pos + 4 * i, std::bit_cast<uint32_t>(v[i]));
  for (uint32_t i = 0; i < 16; i += 4) {
    REX_STORE_U32(controller + 352 + i, 0);
    REX_STORE_U32(proxy + 16 + i, 0);
  }
  const PPCContext saved = ctx;
  ctx.r3.u64 = proxy;
  ctx.r4.u64 = pos;
  sub_829D3560(ctx, base);
  ctx = saved;
  REXLOG_INFO("teleport: controller {:08X} proxy {:08X} -> ({:.3f} {:.3f} {:.3f})", controller, proxy, v[0],
              v[1], v[2]);
}
}  // namespace

REX_HOOK_RAW(sub_822A9E78) {
  MaybeTeleport(ctx, base, ctx.r3.u32);
  __imp__sub_822A9E78(ctx, base);
}

REX_HOOK_RAW(sub_822BF268) {
  crackdown::NoteCharacterUpdate();
  if (!REXCVAR_GET(diag_char_proxy)) {
    const uint32_t proxy = ctx.r3.u32;
    __imp__sub_822BF268(ctx, base);
    if (REXCVAR_GET(diag_vehicle)) {
      NoteCharacterPosition(LoadVec4(base, REX_LOAD_U32(REX_LOAD_U32(proxy + 48) + 28) + 48));
    }
    return;
  }
  const uint32_t proxy = ctx.r3.u32;
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  const Vec4 step = LoadVec4(base, ctx.r4.u32);
  const Vec4 gravity = LoadVec4(base, ctx.r5.u32);
  const Vec4 v_in = LoadVec4(base, proxy + 16);
  // Guest call chain: r1 is still sub_822BE368's frame here; each frame's back
  // chain is at [fp] and the return address it saved is at [caller fp - 8].
  std::string chain;
  {
    static std::atomic<int> chains{0};
    if ((g_proxy_calls == 20 || v_in.y < -3.0f) && chains.fetch_add(1) < 4) {
      uint32_t fp = ctx.r1.u32;
      for (int i = 0; i < 8 && fp; ++i) {
        fp = REX_LOAD_U32(fp);
        if (!fp) break;
        chain += std::format(" {:08X}", REX_LOAD_U32(fp - 8));
      }
    }
  }
  __imp__sub_822BF268(ctx, base);
  const Vec4 v_out = LoadVec4(base, proxy + 16);
  const Vec4 pos = LoadVec4(base, REX_LOAD_U32(REX_LOAD_U32(proxy + 48) + 28) + 48);
  NoteCharacterPosition(pos);

  std::lock_guard lock(g_proxy_mutex);
  const uint32_t n = g_proxy_calls++;
  const float speed = std::max(v_in.Length3(), v_out.Length3());
  if (n < 8 || speed > float(REXCVAR_GET(diag_char_speed))) {
    REXLOG_INFO(
        "diag_char_proxy: call {} proxy={:08X} lr={:08X} step=[{:.6f} {:.6f} dt={:.6f} inv={:.3f}] "
        "g=({:.2f} {:.2f} {:.2f}) vin=({:.3f} {:.3f} {:.3f}) {:.3f} vout=({:.3f} {:.3f} {:.3f}) {:.3f} "
        "pos=({:.3f} {:.3f} {:.3f})",
        n, proxy, lr, step.x, step.y, step.z, step.w, gravity.x, gravity.y, gravity.z, v_in.x, v_in.y,
        v_in.z, v_in.Length3(), v_out.x, v_out.y, v_out.z, v_out.Length3(), pos.x, pos.y, pos.z);
  }
  if (!chain.empty()) REXLOG_INFO("diag_char_proxy: call {} guest chain{}", n, chain);
  ProxyStats* stats = nullptr;
  for (uint32_t i = 0; i < g_proxy_count; ++i) {
    if (g_proxy_stats[i].proxy == proxy) stats = &g_proxy_stats[i];
  }
  if (!stats && g_proxy_count < std::size(g_proxy_stats)) {
    stats = &g_proxy_stats[g_proxy_count++];
    *stats = {proxy, 0, 0.0f};
  }
  if (stats) {
    stats->calls++;
    stats->max_speed = std::max(stats->max_speed, speed);
  }
  const auto now = std::chrono::steady_clock::now();
  if (now - g_proxy_window >= std::chrono::seconds(1)) {
    FlushProxyStats();
    g_proxy_window = now;
  }
}

// --diag_vehicle. Havok's vehicle kit (hkVehicleInstance, an hkUnaryAction):
// the finish-loading constructor sub_829ED808 stores vtable 0x8204FF58, whose
// slot +8 (applyAction, r4 = hkStepInfo {start, end, deltaTime, invDeltaTime})
// is sub_829ED828; it loads deltaTime from [r4+8] at entry. The game derives
// its own class: sub_824A0670 calls the hkVehicleInstance constructor
// sub_829ECB30, then stores vtable 0x820B5100, whose slot +8 is sub_824A1708.
// The base sub_829ED828 never ran in 40 s of replayed city gameplay
// (crackdown_056/057), so hook the override. Offsets from
// Havok's reflection member tables in the image (tools/diag/hkclass.py):
// hkVehicleInstance +24 entity (hkUnaryAction), +68 chassis, +184 torque,
// +188 rpm, +192 mainSteeringAngle, +209 currentGear; hkEntity +80 motion
// (pointer); hkMotion +16 motionState (transform, translation at +48 of it),
// +208 linearVelocity, +224 angularVelocity.
//
// Logs, per step, the vehicle nearest the player (character proxy position,
// which the game keeps updating while the player drives, or the last one
// before it stopped), and any vehicle within 40 m whose linear velocity
// changes by more than --diag_vehicle_dv in one step (impacts). Driving down
// the same stairs at 30 and 60 fps shows whether the car is faster at 60 fps
// and whether it takes bigger hits.
namespace {
std::mutex g_vehicle_mutex;
Vec4 g_char_pos{};
bool g_char_pos_valid = false;

struct VehicleState {
  uint32_t vehicle;
  Vec4 vel;
  Vec4 pos;
  uint32_t steps;
  std::chrono::steady_clock::time_point seen;
};
VehicleState g_vehicles[128];
uint32_t g_vehicle_count = 0;
uint32_t g_tracked = 0;

float Dist3(const Vec4& a, const Vec4& b) {
  const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

void NoteCharacterPosition(const Vec4& pos) {
  std::lock_guard lock(g_vehicle_mutex);
  g_char_pos = pos;
  g_char_pos_valid = true;
}
}  // namespace

namespace {
// --teleport_vehicle. hkMotionState at motion+16: transform (rotation columns
// +16/+32/+48, translation +64), hkSweptTransform at +80: centerOfMass0 +80
// (w = t0), centerOfMass1 +96 (w = 1/dt), rotation0 +112, rotation1 +128
// (quaternions), centerOfMassLocal +144. Put both swept positions at the new
// centre of mass, both rotations at the new yaw, keep the w terms, zero the
// velocities. Done before the vehicle's own step, once.
void StoreF(uint8_t* base, uint32_t ea, float v) { REX_STORE_U32(ea, std::bit_cast<uint32_t>(v)); }

bool ParseVec4(const std::string& spec, float (&v)[4]) {
  const char* p = spec.data();
  const char* end = p + spec.size();
  for (int i = 0; i < 4; ++i) {
    const auto r = std::from_chars(p, end, v[i]);
    if (r.ec != std::errc{} || (i == 3 ? r.ptr != end : r.ptr >= end || *r.ptr != ',')) return false;
    if (!std::isfinite(v[i])) return false;
    p = r.ptr + 1;
  }
  return true;
}

void MaybeTeleportVehicle(uint8_t* base, uint32_t vehicle) {
  static bool done = false;
  static std::chrono::steady_clock::time_point start;
  if (done || !crackdown::GameplayStarted() || vehicle != g_tracked) return;
  const std::string spec = REXCVAR_GET(teleport_vehicle);
  if (spec.empty()) {
    done = true;
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (start == std::chrono::steady_clock::time_point{}) start = now;
  if (now - start < std::chrono::duration<double>(REXCVAR_GET(teleport_vehicle_delay_s))) return;
  done = true;
  float v[4];
  if (!ParseVec4(spec, v)) {
    REXLOG_ERROR("teleport_vehicle: bad spec \"{}\" (want x,y,z,yaw)", spec);
    return;
  }
  const uint32_t entity = REX_LOAD_U32(vehicle + 24);
  const uint32_t motion = entity ? REX_LOAD_U32(entity + 80) : 0;
  if (!motion) return;
  // Reduce first: a huge finite yaw would overflow * pi to inf and give NaN.
  const float yaw = std::fmod(v[3], 360.0f) * (3.14159265f / 180.0f), c = std::cos(yaw),
              s = std::sin(yaw);
  // Rotation about +y, columns: (c,0,-s), (0,1,0), (s,0,c).
  const float cols[3][3] = {{c, 0, -s}, {0, 1, 0}, {s, 0, c}};
  for (int k = 0; k < 3; ++k)
    for (int i = 0; i < 3; ++i) StoreF(base, motion + 16 + 16 * k + 4 * i, cols[k][i]);
  for (int i = 0; i < 3; ++i) StoreF(base, motion + 64 + 4 * i, v[i]);
  const Vec4 local = LoadVec4(base, motion + 144);
  const float com[3] = {v[0] + c * local.x + s * local.z, v[1] + local.y, v[2] - s * local.x + c * local.z};
  for (uint32_t swept : {80u, 96u})
    for (int i = 0; i < 3; ++i) StoreF(base, motion + swept + 4 * i, com[i]);
  const float q[4] = {0, std::sin(yaw / 2), 0, std::cos(yaw / 2)};
  for (uint32_t rot : {112u, 128u})
    for (int i = 0; i < 4; ++i) StoreF(base, motion + rot + 4 * i, q[i]);
  for (uint32_t i = 0; i < 32; i += 4) REX_STORE_U32(motion + 208 + i, 0);
  REXLOG_INFO("teleport_vehicle: {:08X} motion {:08X} -> ({:.2f} {:.2f} {:.2f}) yaw {:.1f}", vehicle, motion,
              v[0], v[1], v[2], v[3]);
}
}  // namespace

REX_HOOK_RAW(sub_824A1708) {
  if (!REXCVAR_GET(diag_vehicle)) {
    __imp__sub_824A1708(ctx, base);
    return;
  }
  const uint32_t vehicle = ctx.r3.u32;
  {
    std::lock_guard lock(g_vehicle_mutex);
    MaybeTeleportVehicle(base, vehicle);
  }
  const float dt = std::bit_cast<float>(REX_LOAD_U32(ctx.r4.u32 + 8));
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  __imp__sub_824A1708(ctx, base);
  const uint32_t entity = REX_LOAD_U32(vehicle + 24);
  const uint32_t chassis = REX_LOAD_U32(vehicle + 68);
  const uint32_t motion = entity ? REX_LOAD_U32(entity + 80) : 0;
  if (!motion) return;
  const Vec4 pos = LoadVec4(base, motion + 16 + 48);
  const Vec4 vel = LoadVec4(base, motion + 208);
  const Vec4 ang = LoadVec4(base, motion + 224);
  const Vec4 xaxis = LoadVec4(base, motion + 16);  // rotation columns: chassis local x and z in world
  const Vec4 zaxis = LoadVec4(base, motion + 48);
  const auto now = std::chrono::steady_clock::now();

  std::lock_guard lock(g_vehicle_mutex);
  VehicleState* st = nullptr;
  for (uint32_t i = 0; i < g_vehicle_count; ++i) {
    if (g_vehicles[i].vehicle == vehicle) st = &g_vehicles[i];
  }
  if (!st) {
    // Reuse the stalest slot once the table is full (vehicles get streamed out).
    uint32_t slot = g_vehicle_count;
    if (slot == std::size(g_vehicles)) {
      slot = 0;
      for (uint32_t i = 1; i < g_vehicle_count; ++i) {
        if (g_vehicles[i].seen < g_vehicles[slot].seen) slot = i;
      }
    } else {
      ++g_vehicle_count;
    }
    st = &g_vehicles[slot];
    *st = {vehicle, vel, pos, 0, now};
    REXLOG_INFO("diag_vehicle: new vehicle {:08X} entity {:08X} chassis {:08X} motion {:08X} lr={:08X} "
                "pos=({:.2f} {:.2f} {:.2f})",
                vehicle, entity, chassis, motion, lr, pos.x, pos.y, pos.z);
  }
  if (Dist3(pos, st->pos) > 5.0f) {
    st->vel = vel;  // address reused by another vehicle (or it was moved): no history
    st->steps = 0;
  }
  const Vec4 dv{vel.x - st->vel.x, vel.y - st->vel.y, vel.z - st->vel.z, 0};
  st->vel = vel;
  st->pos = pos;
  st->seen = now;
  st->steps++;

  // Nearest recently stepped vehicle to the player.
  if (g_char_pos_valid) {
    float best = 1e30f;
    uint32_t pick = 0;
    for (uint32_t i = 0; i < g_vehicle_count; ++i) {
      if (now - g_vehicles[i].seen > std::chrono::milliseconds(200)) continue;
      const float d = Dist3(g_vehicles[i].pos, g_char_pos);
      if (d < best) {
        best = d;
        pick = g_vehicles[i].vehicle;
      }
    }
    if (best < 6.0f && pick != g_tracked) {
      g_tracked = pick;
      REXLOG_INFO("diag_vehicle: tracking {:08X} ({:.2f} m from the player)", pick, best);
    }
  }
  // Once a second: the three nearest vehicles, to find one to drive.
  static std::chrono::steady_clock::time_point window;
  if (g_char_pos_valid && now - window >= std::chrono::seconds(1)) {
    window = now;
    bool used[std::size(g_vehicles)] = {};
    std::string nearest;
    for (int k = 0; k < 3; ++k) {
      int pick = -1;
      for (uint32_t i = 0; i < g_vehicle_count; ++i) {
        if (used[i] || now - g_vehicles[i].seen > std::chrono::milliseconds(200)) continue;
        if (pick < 0 || Dist3(g_vehicles[i].pos, g_char_pos) < Dist3(g_vehicles[pick].pos, g_char_pos)) pick = int(i);
      }
      if (pick < 0) break;
      used[pick] = true;
      const VehicleState& v = g_vehicles[pick];
      nearest += std::format(" {:08X} {:.1f}m ({:.1f} {:.1f} {:.1f}) |v|={:.1f};", v.vehicle,
                          Dist3(v.pos, g_char_pos), v.pos.x, v.pos.y, v.pos.z, v.vel.Length3());
    }
    REXLOG_INFO("diag_vehicle: player ({:.1f} {:.1f} {:.1f}) nearest:{}", g_char_pos.x, g_char_pos.y,
                g_char_pos.z, nearest);
  }
  const float hspeed = std::sqrt(vel.x * vel.x + vel.z * vel.z);
  const bool tracked = vehicle == g_tracked;
  const bool jump = dv.Length3() > float(REXCVAR_GET(diag_vehicle_dv)) && g_char_pos_valid &&
                    Dist3(pos, g_char_pos) < 40.0f && st->steps > 1;
  if (tracked && dv.Length3() > 8.0f && st->steps > 1) {
    REXLOG_INFO("diag_vehicle: hit dv={:.2f} at ({:.2f} {:.2f} {:.2f})", dv.Length3(), pos.x, pos.y, pos.z);
  }
  if ((tracked && hspeed > 0.5f) || jump) {
    REXLOG_INFO(
        "diag_vehicle: {} {:08X} dt={:.4f} pos=({:.2f} {:.2f} {:.2f}) v=({:.2f} {:.2f} {:.2f}) |vh|={:.2f} "
        "dv=({:.2f} {:.2f} {:.2f}) {:.2f} w=({:.2f} {:.2f} {:.2f}) rpm={:.0f} torque={:.0f} gear={} steer={:.3f} "
        "xaxis=({:.2f} {:.2f} {:.2f}) zaxis=({:.2f} {:.2f} {:.2f})",
        tracked ? "player" : "impact", vehicle, dt, pos.x, pos.y, pos.z, vel.x, vel.y, vel.z, hspeed, dv.x, dv.y,
        dv.z, dv.Length3(), ang.x, ang.y, ang.z, std::bit_cast<float>(REX_LOAD_U32(vehicle + 188)),
        std::bit_cast<float>(REX_LOAD_U32(vehicle + 184)), int(int8_t(REX_LOAD_U8(vehicle + 209))),
        std::bit_cast<float>(REX_LOAD_U32(vehicle + 192)), xaxis.x, xaxis.y, xaxis.z, zaxis.x, zaxis.y, zaxis.z);
  }
}
