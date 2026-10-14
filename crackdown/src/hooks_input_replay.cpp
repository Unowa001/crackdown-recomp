// crackdown - controller record/replay for unattended test runs
//
// Guest 0x82743798 is the game's XInputGetState(user r3, state r4): it tail-
// calls XamInputGetState(user, 0, state). Guest 0x827437C0 is its
// XInputGetKeystroke(user r3, flags r4, keystroke r5): user 255 becomes
// XINPUT_FLAG_ANY_USER, then XamInputGetKeystrokeEx(&user, flags, keystroke).
// These are the game's only two controller reads (imports at 0x82B67074 and
// 0x82B67094; the other XamInput imports are capabilities and rumble).
//
// --input_record=<file> writes every change of user 0's pad state and every
// keystroke the game receives. --input_replay=<file> feeds them back instead of
// the real pad, then closes the window a few seconds after the last event.
//
// Timing: loading times vary between runs, so the recording is split in two
// segments with their own clocks. Segment 0 (menus) starts at the first input
// poll; segment 1 (gameplay) starts once character proxy integrations
// (sub_822BF268, see NoteCharacterUpdate) have run without a break for 1 s. Each clock only advances while the
// game keeps polling: gaps over 250 ms (loading stalls) do not count. If the
// game reaches gameplay before segment 0's events run out, the rest are
// dropped; if they run out first, the pad stays neutral until gameplay starts.
// Times are in milliseconds, so one recording replays at any frame rate.

#include "generated/default/crackdown_init.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <fstream>
#include <filesystem>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging/macros.h>

#include "input_replay.h"

REXCVAR_DEFINE_STRING(input_record, "", "Crackdown",
                      "Test tool: record controller input (user 0) to this file");
REXCVAR_DEFINE_STRING(input_replay, "", "Crackdown",
                      "Test tool: replay controller input from this file instead of the pad, "
                      "then close the game");
REXCVAR_DEFINE_DOUBLE(input_replay_tail_s, 3.0, "Crackdown",
                      "With --input_replay: seconds to keep running after the last event");
REXCVAR_DEFINE_STRING(input_replay_commands, "", "Crackdown",
                      "With --input_replay: atomically replaced recording of relative input "
                      "for state-dependent unattended menu tests");

namespace {
using Clock = std::chrono::steady_clock;
constexpr double kStallMs = 250.0;
constexpr uint32_t kErrorSuccess = 0;
constexpr uint32_t kErrorEmpty = 0x10D2;

struct PadState {
  uint16_t buttons = 0;
  uint8_t lt = 0, rt = 0;
  int16_t lx = 0, ly = 0, rx = 0, ry = 0;
  bool operator==(const PadState&) const = default;
};

struct PadEvent {
  int seg;
  double t;
  PadState s;
};

struct KeyEvent {
  int seg;
  double t;
  uint16_t vk, unicode, flags;
  uint8_t user, hid;
};

std::atomic<bool> g_character_seen{false};
std::function<void()> g_quit;

std::mutex g_mutex;
// Set once Init has run with neither flag given: the hooks then skip the lock.
std::atomic<bool> g_inactive{false};
bool g_init = false;
bool g_recording = false;
bool g_replaying = false;
std::ofstream g_record_file;

int g_seg = -1;  // -1 until the first poll
double g_seg_ms = 0;
Clock::time_point g_last_poll;

PadState g_last_recorded;
bool g_have_recorded = false;

std::vector<PadEvent> g_pad_events;
std::vector<KeyEvent> g_key_events;
size_t g_pad_next = 0, g_key_next = 0;
PadState g_replay_state;
uint32_t g_packet = 1;
double g_last_event_ms[2] = {-1, -1};
bool g_quit_sent = false;
Clock::time_point g_command_check;
std::filesystem::file_time_type g_command_stamp{};

// Returns false (after logging the line) on any malformed or out-of-range
// record: a silently dropped or truncated release would leave an input held.
bool LoadReplay(const std::string& path) {
  std::ifstream f(path);
  if (!f) {
    REXLOG_ERROR("input_replay: cannot open {}", path);
    return false;
  }
  std::string line;
  int line_no = 0;
  auto bad = [&](const char* why) {
    REXLOG_ERROR("input_replay: {} line {}: {}: '{}'", path, line_no, why, line);
    return false;
  };
  while (std::getline(f, line)) {
    ++line_no;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    std::istringstream in(line);
    char kind;
    int seg;
    double t;
    unsigned a, b, c, d, e;
    int lx, ly, rx, ry;
    // Unsigned extraction would wrap a negative value into range.
    if (line.find('-') != std::string::npos) {
      std::istringstream check(line);
      std::string tok;
      int field = 0;
      bool neg_unsigned = false;
      bool pad_record = false;
      while (check >> tok) {
        if (field == 0) pad_record = tok == "S";
        // S: kind seg t buttons lt rt lx ly rx ry (sticks, fields 6-9, are signed)
        const bool is_signed_field = pad_record && field >= 6;
        if (tok[0] == '-' && !is_signed_field) neg_unsigned = true;
        ++field;
      }
      if (neg_unsigned) return bad("negative value");
    }
    if (!(in >> kind >> seg >> t)) return bad("bad header");
    if ((seg != 0 && seg != 1) || !std::isfinite(t) || t < 0) return bad("bad segment or time");
    if (kind == 'S') {
      if (!(in >> std::hex >> a >> std::dec >> b >> c >> lx >> ly >> rx >> ry)) {
        return bad("truncated pad record");
      }
      auto stick = [](int v) { return v >= INT16_MIN && v <= INT16_MAX; };
      if (a > 0xFFFF || b > 0xFF || c > 0xFF || !stick(lx) || !stick(ly) || !stick(rx) ||
          !stick(ry)) {
        return bad("pad value out of range");
      }
      g_pad_events.push_back({seg, t,
                              {uint16_t(a), uint8_t(b), uint8_t(c), int16_t(lx), int16_t(ly),
                               int16_t(rx), int16_t(ry)}});
    } else if (kind == 'K') {
      if (!(in >> std::hex >> a >> b >> c >> std::dec >> d >> e)) {
        return bad("truncated key record");
      }
      if (a > 0xFFFF || b > 0xFFFF || c > 0xFFFF || d > 0xFF || e > 0xFF) {
        return bad("key value out of range");
      }
      g_key_events.push_back({seg, t, uint16_t(a), uint16_t(b), uint16_t(c), uint8_t(d), uint8_t(e)});
    } else {
      return bad("unknown record kind");
    }
    std::string rest;
    if (in >> rest) return bad("trailing data");
    g_last_event_ms[seg] = std::max(g_last_event_ms[seg], t);
  }
  auto order = [](const auto& x, const auto& y) { return x.seg != y.seg ? x.seg < y.seg : x.t < y.t; };
  std::stable_sort(g_pad_events.begin(), g_pad_events.end(), order);
  std::stable_sort(g_key_events.begin(), g_key_events.end(), order);
  g_replaying = true;
  REXLOG_INFO("input_replay: {} pad / {} key events from {} (menus {:.0f} ms, gameplay {:.0f} ms)",
              g_pad_events.size(), g_key_events.size(), path, g_last_event_ms[0], g_last_event_ms[1]);
  return true;
}

void Init() {
  if (g_init) return;
  g_init = true;
  const std::string replay = REXCVAR_GET(input_replay);
  const std::string record = REXCVAR_GET(input_record);
  if (!replay.empty()) {
    if (!LoadReplay(replay)) {
      // An unattended run must not continue on a damaged recording.
      g_pad_events.clear();
      g_key_events.clear();
      REXLOG_ERROR("input_replay: recording rejected, closing the game");
      g_quit_sent = true;
      if (g_quit) g_quit();
    }
  } else if (!record.empty()) {
    g_record_file.open(record);
    if (g_record_file) {
      g_recording = true;
      g_record_file << "# crackdown input recording v1: S seg ms buttons lt rt lx ly rx ry / "
                       "K seg ms vk unicode flags user hid\n";
      REXLOG_INFO("input_record: writing {}", record);
    } else {
      REXLOG_ERROR("input_record: cannot open {}", record);
    }
  }
  if (!g_recording && !g_replaying) g_inactive.store(true, std::memory_order_relaxed);
}

// Uses the same validated replay format. Segments in a command file must be 0;
// times are relative to receipt, and apply to the current menu/gameplay segment.
// The producer replaces the file atomically after observing its checkpoint.
void PollCommands(Clock::time_point now) {
  const auto path=REXCVAR_GET(input_replay_commands);
  if (!g_replaying || path.empty() || now-g_command_check<std::chrono::milliseconds(100)) return;
  g_command_check=now;
  std::error_code error;
  auto stamp=std::filesystem::last_write_time(path,error);
  if (error || stamp==g_command_stamp) return;
  g_command_stamp=stamp;
  auto previous_pad=std::move(g_pad_events); auto previous_keys=std::move(g_key_events);
  const double previous_last[2]={g_last_event_ms[0],g_last_event_ms[1]};
  g_pad_events.clear(); g_key_events.clear(); g_last_event_ms[0]=g_last_event_ms[1]=-1;
  bool valid=LoadReplay(path);
  for (const auto& event:g_pad_events) valid=valid && event.seg==0;
  for (const auto& event:g_key_events) valid=valid && event.seg==0;
  if (!valid) {
    g_pad_events=std::move(previous_pad); g_key_events=std::move(previous_keys);
    g_last_event_ms[0]=previous_last[0]; g_last_event_ms[1]=previous_last[1]; return;
  }
  double last=-1;
  for (auto& event:g_pad_events) { event.seg=g_seg; event.t+=g_seg_ms; last=std::max(last,event.t); }
  for (auto& event:g_key_events) { event.seg=g_seg; event.t+=g_seg_ms; last=std::max(last,event.t); }
  g_last_event_ms[0]=g_last_event_ms[1]=-1; g_last_event_ms[g_seg]=last;
  g_pad_next=g_key_next=0; g_replay_state={}; g_quit_sent=false;
  REXLOG_INFO("input_replay: command accepted at segment {} time {:.0f} ms",g_seg,g_seg_ms);
}

// Advance the segment clock; call with g_mutex held.
void Tick() {
  const auto now = Clock::now();
  if (g_seg < 0) {
    g_seg = 0;
  } else {
    const double gap = std::chrono::duration<double, std::milli>(now - g_last_poll).count();
    if (gap < kStallMs) g_seg_ms += gap;
  }
  g_last_poll = now;
  if (g_seg == 0 && g_character_seen.load(std::memory_order_relaxed)) {
    REXLOG_INFO("input: gameplay segment starts (menus took {:.0f} ms of polling)", g_seg_ms);
    g_seg = 1;
    g_seg_ms = 0;
    // Drop menu events that did not get played.
    while (g_pad_next < g_pad_events.size() && g_pad_events[g_pad_next].seg == 0) ++g_pad_next;
    while (g_key_next < g_key_events.size() && g_key_events[g_key_next].seg == 0) ++g_key_next;
    g_replay_state = {};
    // Recording: write the full pad state at the start of gameplay even if it
    // didn't change, so an input held across the boundary replays.
    g_have_recorded = false;
  }
  PollCommands(now);
  if (g_replaying && !g_quit_sent) {
    const bool done = g_pad_next >= g_pad_events.size() && g_key_next >= g_key_events.size();
    const double last = g_last_event_ms[1] >= 0 ? g_last_event_ms[1] : g_last_event_ms[0];
    const int last_seg = g_last_event_ms[1] >= 0 ? 1 : 0;
    const double tail_ms = REXCVAR_GET(input_replay_tail_s) * 1000.0;
    // A menus-only recording can see gameplay start before its tail is over;
    // the tail then runs on the gameplay clock.
    const bool tail_over = g_seg == last_seg ? g_seg_ms >= last + tail_ms
                                             : g_seg > last_seg && g_seg_ms >= tail_ms;
    if (done && tail_over) {
      g_quit_sent = true;
      REXLOG_INFO("input_replay: done, closing the game");
      if (g_quit) g_quit();
    }
  }
}

void StorePad(uint8_t* base, uint32_t ea, uint32_t packet, const PadState& s) {
  REX_STORE_U32(ea + 0, packet);
  REX_STORE_U16(ea + 4, s.buttons);
  REX_STORE_U8(ea + 6, s.lt);
  REX_STORE_U8(ea + 7, s.rt);
  REX_STORE_U16(ea + 8, uint16_t(s.lx));
  REX_STORE_U16(ea + 10, uint16_t(s.ly));
  REX_STORE_U16(ea + 12, uint16_t(s.rx));
  REX_STORE_U16(ea + 14, uint16_t(s.ry));
}

PadState LoadPad(uint8_t* base, uint32_t ea) {
  return {REX_LOAD_U16(ea + 4),          REX_LOAD_U8(ea + 6),           REX_LOAD_U8(ea + 7),
          int16_t(REX_LOAD_U16(ea + 8)), int16_t(REX_LOAD_U16(ea + 10)), int16_t(REX_LOAD_U16(ea + 12)),
          int16_t(REX_LOAD_U16(ea + 14))};
}
}  // namespace

namespace crackdown {
std::atomic<uint64_t> g_character_updates{0};
uint64_t CharacterUpdateCount() { return g_character_updates.load(std::memory_order_relaxed); }
void NoteCharacterUpdate() {
  g_character_updates.fetch_add(1, std::memory_order_relaxed);
  // Gameplay = integrations running steadily for 1 s. A proxy is also
  // integrated a couple of times in the menus before the New Game load
  // (crackdown_046), which must not start the clock.
  static std::mutex mutex;
  static Clock::time_point run_start, last;
  if (g_character_seen.load(std::memory_order_relaxed)) return;
  std::lock_guard lock(mutex);
  const auto now = Clock::now();
  if (now - last > std::chrono::milliseconds(int(kStallMs))) run_start = now;
  last = now;
  if (now - run_start >= std::chrono::seconds(1)) g_character_seen.store(true, std::memory_order_relaxed);
}
bool GameplayStarted() { return g_character_seen.load(std::memory_order_relaxed); }
void SetInputReplayQuit(std::function<void()> quit) { g_quit = std::move(quit); }
}  // namespace crackdown

REX_HOOK_RAW(sub_82743798) {
  const uint32_t user = ctx.r3.u32;
  const uint32_t state_ea = ctx.r4.u32;
  __imp__sub_82743798(ctx, base);
  if (user != 0 || !state_ea || g_inactive.load(std::memory_order_relaxed)) return;
  std::lock_guard lock(g_mutex);
  Init();
  if (!g_recording && !g_replaying) return;
  Tick();
  if (g_replaying) {
    while (g_pad_next < g_pad_events.size() && g_pad_events[g_pad_next].seg == g_seg &&
           g_pad_events[g_pad_next].t <= g_seg_ms) {
      g_replay_state = g_pad_events[g_pad_next++].s;
      ++g_packet;
    }
    StorePad(base, state_ea, g_packet, g_replay_state);
    ctx.r3.u64 = kErrorSuccess;
  } else if (ctx.r3.u32 == kErrorSuccess) {
    const PadState s = LoadPad(base, state_ea);
    if (!g_have_recorded || !(s == g_last_recorded)) {
      g_have_recorded = true;
      g_last_recorded = s;
      g_record_file << std::format("S {} {:.1f} {:04X} {} {} {} {} {} {}\n", g_seg, g_seg_ms, s.buttons, unsigned(s.lt),
                                   unsigned(s.rt), s.lx, s.ly, s.rx, s.ry)
                    << std::flush;
    }
  }
}

REX_HOOK_RAW(sub_827437C0) {
  const uint32_t key_ea = ctx.r5.u32;
  if (!key_ea || g_inactive.load(std::memory_order_relaxed)) {
    __imp__sub_827437C0(ctx, base);
    return;
  }
  {
    std::lock_guard lock(g_mutex);
    Init();
    if (g_replaying) {
      Tick();
      if (g_key_next < g_key_events.size() && g_key_events[g_key_next].seg == g_seg &&
          g_key_events[g_key_next].t <= g_seg_ms) {
        const KeyEvent& k = g_key_events[g_key_next++];
        REX_STORE_U16(key_ea + 0, k.vk);
        REX_STORE_U16(key_ea + 2, k.unicode);
        REX_STORE_U16(key_ea + 4, k.flags);
        REX_STORE_U8(key_ea + 6, k.user);
        REX_STORE_U8(key_ea + 7, k.hid);
        ctx.r3.u64 = kErrorSuccess;
      } else {
        ctx.r3.u64 = kErrorEmpty;
      }
      return;
    }
  }
  __imp__sub_827437C0(ctx, base);
  std::lock_guard lock(g_mutex);
  if (g_recording && ctx.r3.u32 == kErrorSuccess) {
    Tick();
    g_record_file << std::format("K {} {:.1f} {:04X} {:04X} {:04X} {} {}\n", g_seg, g_seg_ms,
                                 uint16_t(REX_LOAD_U16(key_ea + 0)), uint16_t(REX_LOAD_U16(key_ea + 2)),
                                 uint16_t(REX_LOAD_U16(key_ea + 4)),
                                 unsigned(REX_LOAD_U8(key_ea + 6)), unsigned(REX_LOAD_U8(key_ea + 7)))
                  << std::flush;
  }
}
