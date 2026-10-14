// crackdown - precise guest Sleep() for the frame limiter
//
// Guest 0x82743788 is the game's Sleep(ms) thunk (li r4,0; b SleepEx at
// 0x82746520 -> KeDelayExecutionThread -> XThread::Delay -> ::Sleep). It has
// 18 call sites. Two belong to the frame limiter (return addresses 0x826A6834
// in sub_826A65F0 and 0x826A78DC in sub_826A7698), which sleeps
// (1000 / *(s32*)0x82BAA330) - elapsed ms, with elapsed from GetTickCount
// (sub_82744558, KeTimeStampBundle+16). The cap at 0x82BAA330 reads 32 in
// game (the debug commands at 0x8232BDD8/0x8232BE20 clamp it to 1..30 when
// changed).
//
// Measured with --diag_sleep (2026-10-10, crackdown_012.log): the limiter asks
// for 14..31 ms (avg ~23) but the host ::Sleep returns ~12-14 ms late, even
// Sleep(1) takes ~14.5 ms, so frames run ~46 ms (22 fps) instead of ~31 ms.
// A standalone process on the same PC gets Sleep(1) = 1.5 ms after raising the
// timer resolution, so the game's own resolution request is not taking effect
// (Windows keeps one request per process; another component can release it).
//
// Fix, for the limiter's two call sites only: don't depend on the process
// timer resolution. Wait on a high-resolution waitable timer
// (CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, Windows 10 1803+) for all but the
// last ~1 ms, then spin to the deadline. Same idea as EdgeOfTimeRecomp's
// src/core/sleep.cpp (BSD-3, adapted from ReBlue). Every other caller, the
// Sleep(1) retry loops included, keeps the stock blocking path, so none of them
// turn into busy spins (Codex review finding 2).
//
// Sleep is non-alertable, so skipping KeDelayExecutionThread loses no APCs. It
// does skip XThread::Delay's title-termination checks, so they're called here
// before the wait, between chunks of at most kChunk and after it (Codex
// review finding 1; second review: TerminateTitle only waits ~200 ms for guest
// threads to reach a check, and a cap of 1 means a 1000 ms limiter sleep).

#include "generated/default/crackdown_init.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging/macros.h>
#include <rex/system/xthread.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <immintrin.h>
#endif

REXCVAR_DEFINE_BOOL(precise_sleep, true, "Crackdown",
                    "Frame limiter sleeps use a high-resolution timer wait + short spin");
REXCVAR_DEFINE_BOOL(diag_sleep, false, "Crackdown",
                    "Log guest Sleep() requests every 2 s (caller, ms, frame cap, host time)");

namespace {
using Clock = std::chrono::steady_clock;

constexpr uint32_t kFrameCapAddr = 0x82BAA330;
constexpr uint32_t kLimiterReturnA = 0x826A6834;  // sub_826A65F0
constexpr uint32_t kLimiterReturnB = 0x826A78DC;  // sub_826A7698
// The limiter never asks for more than 1000 / 1 ms; anything longer keeps the stock path.
constexpr uint32_t kPreciseMaxMs = 1000;
constexpr auto kSpinTail = std::chrono::microseconds(1000);
// Longest single timer wait between title-termination checks (well under the
// ~200 ms TerminateTitle gives guest threads to exit).
constexpr auto kChunk = std::chrono::milliseconds(20);

#if defined(_WIN32)
struct ThreadTimer {
  HANDLE handle = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                         TIMER_ALL_ACCESS);
  ~ThreadTimer() {
    if (handle) {
      CloseHandle(handle);
    }
  }
};
#endif

// Waits until `deadline`. Returns false, possibly part-way, if the timer can't
// be created, set or waited on; the caller sleeps the rest on the stock path.
bool PreciseWaitUntil(Clock::time_point deadline) {
#if defined(_WIN32)
  thread_local ThreadTimer timer;
  if (!timer.handle) {
    return false;
  }
  for (;;) {
    rex::system::XThread::CheckTitleTermination();
    const auto coarse = std::min<Clock::duration>(deadline - kSpinTail - Clock::now(), kChunk);
    if (coarse <= Clock::duration::zero()) {
      break;
    }
    LARGE_INTEGER due;
    due.QuadPart = -static_cast<LONGLONG>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(coarse).count() / 100);
    if (!SetWaitableTimer(timer.handle, &due, 0, nullptr, nullptr, FALSE)) {
      return false;
    }
    if (WaitForSingleObject(timer.handle, INFINITE) != WAIT_OBJECT_0) {
      return false;
    }
  }
  while (Clock::now() < deadline) {
    _mm_pause();
  }
  return true;
#else
  (void)deadline;
  return false;
#endif
}

struct CallerStats {
  uint32_t count = 0;
  uint32_t req_min = UINT32_MAX, req_max = 0;
  uint64_t req_sum = 0;
  double took_min = 1e9, took_max = 0, took_sum = 0;
};

std::mutex g_stats_mutex;
std::map<uint32_t, CallerStats> g_stats;
Clock::time_point g_window_start = Clock::now();

void RecordDiag(uint32_t caller, uint32_t requested_ms, double took_ms, Clock::time_point now,
                uint8_t* base) {
  std::lock_guard lock(g_stats_mutex);
  auto& s = g_stats[caller];
  ++s.count;
  s.req_min = std::min(s.req_min, requested_ms);
  s.req_max = std::max(s.req_max, requested_ms);
  s.req_sum += requested_ms;
  s.took_min = std::min(s.took_min, took_ms);
  s.took_max = std::max(s.took_max, took_ms);
  s.took_sum += took_ms;
  if (now - g_window_start < std::chrono::seconds(2)) {
    return;
  }
  const int32_t frame_cap = static_cast<int32_t>(REX_LOAD_U32(kFrameCapAddr));
  for (const auto& [addr, st] : g_stats) {
    REXLOG_INFO(
        "diag_sleep: caller {:08X} n={} requested {}/{:.1f}/{} ms, took {:.1f}/{:.1f}/{:.1f} ms "
        "(min/avg/max), frame cap {}",
        addr, st.count, st.req_min, double(st.req_sum) / st.count, st.req_max, st.took_min,
        st.took_sum / st.count, st.took_max, frame_cap);
  }
  g_stats.clear();
  g_window_start = now;
}
}  // namespace

REX_HOOK_RAW(sub_82743788) {
  using rex::system::XThread;

  const uint32_t ms = ctx.r3.u32;
  const uint32_t caller = static_cast<uint32_t>(ctx.lr);  // before the stock path clobbers it
  const bool diag = REXCVAR_GET(diag_sleep);
  const auto start = Clock::now();

  const bool limiter = caller == kLimiterReturnA || caller == kLimiterReturnB;
  if (REXCVAR_GET(precise_sleep) && limiter && ms > 0 && ms <= kPreciseMaxMs) {
    const auto deadline = start + std::chrono::milliseconds(ms);
    if (PreciseWaitUntil(deadline)) {
      XThread::CheckTitleTermination();
      ctx.r3.u64 = 0;  // SleepEx returns 0 when the interval elapses
    } else {
      // Sleep whatever is left on the stock path (it does its own termination checks).
      const auto left = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
      ctx.r3.u64 = static_cast<uint64_t>(std::max<int64_t>(left.count(), 0));
      __imp__sub_82743788(ctx, base);
    }
  } else {
    __imp__sub_82743788(ctx, base);
  }

  if (diag) {
    const auto end = Clock::now();
    RecordDiag(caller, ms, std::chrono::duration<double, std::milli>(end - start).count(), end,
               base);
  }
}
