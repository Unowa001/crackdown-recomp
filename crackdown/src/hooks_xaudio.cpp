// crackdown - XAudio multi-core mix barrier
//
// Guest 0x82A50000 is the barrier XAudio's mix worker threads meet at
// (called from sub_82A509D8, which the workers sub_82A50FD8 / sub_82A510A0 run
// once per audio frame while [engine+304] is set):
//     lbz  r11,268(r13)      ; this hardware thread's number (KPCR+0x10C)
//     stbx 1,r11,r4          ; mark it in the 8-byte word at r4
//     ...                    ; expected = byte i set for each i in 0..5 with
//                            ; [engine+308+4*i] != 0 (byte 0 = MSB)
//     ld   r11,0(r4)         ; 0 -> barrier already released, return
//   loc_82A500DC:
//     cmpd r10,r11 / beq     ; all participants in -> std 0,0(r4), return
//     db16cyc / ld / cmpdi / bne loc_82A500DC ; spin until complete or 0
//
// A worker that marks its byte and is then preempted before its first ld
// can miss the release entirely: another worker sees the word complete,
// resets it to 0, finishes the frame and, a frame later, marks its byte in
// the next round. The late worker then reads a word that is neither 0 nor
// complete and spins forever, and so does the early one, waiting for it. The
// audio callback (sub_82A51150) waits on the workers with no timeout, so all
// audio stops. On the console each worker owns its hardware thread and is
// never descheduled in that window; on a PC it happens after a few minutes.
//
// Seen live (cdb, 2026-10-11): XThread8328 (KPCR cpu 4) and XThread4CB0
// (cpu 5) both spinning at loc_82A500DC, expected 0x0000000001010000, word
// 0x0000000001000000 (cpu 5's mark wiped by the release), audio worker
// blocked in KeWaitForMultipleObjects from sub_82A51150.
//
// The hook keeps the protocol (the same expected participants; release once
// all of them are in) but runs it on a host word per barrier: arrivals of the current
// round in the low byte (bit = cpu number), the round number above it.
// Arrival and release are one compare-exchange, so a worker can never mark
// into a round that has already been released, and a worker still waiting in
// a released round sees the round number move on and returns. (A first
// version cleared the guest word and then bumped a separate round counter; a
// worker re-entering between the two could leave the next round early.) The
// guest word is no longer written; only this function uses it.

#include "generated/default/crackdown_init.h"

#include <atomic>
#include <cstdint>

#include <immintrin.h>

#include <rex/hook.h>

namespace {
struct Barrier {
  std::atomic<uint32_t> word_addr{0};
  std::atomic<uint64_t> state{0};  // round << 8 | arrivals
};

// One per barrier word; XAudio has one engine, so one is expected in use.
Barrier g_barriers[8];
Barrier g_overflow;  // shared: never expected

Barrier& BarrierFor(uint32_t word_addr) {
  for (auto& b : g_barriers) {
    uint32_t cur = b.word_addr.load(std::memory_order_acquire);
    if (cur == 0 && b.word_addr.compare_exchange_strong(cur, word_addr)) {
      return b;
    }
    if (cur == word_addr) {
      return b;
    }
  }
  return g_overflow;
}
}  // namespace

REX_HOOK_RAW(sub_82A50000) {
  const uint32_t engine = ctx.r3.u32;
  const uint32_t word_addr = ctx.r4.u32;
  if (REX_LOAD_U32(engine + 304) == 0) {
    return;
  }
  const uint8_t cpu = REX_LOAD_U8(ctx.r13.u32 + 268);
  if (cpu >= 8) {  // never seen; keep the guest code for it
    __imp__sub_82A50000(ctx, base);
    return;
  }

  uint64_t expected = 0;
  for (uint32_t i = 0; i < 6; ++i) {
    if (REX_LOAD_U32(engine + 308 + 4 * i) != 0) {
      expected |= uint64_t{1} << i;
    }
  }

  Barrier& b = BarrierFor(word_addr);
  uint64_t s = b.state.load(std::memory_order_acquire);
  uint64_t next;
  do {
    const uint64_t arrived = (s & 0xFF) | (uint64_t{1} << cpu);
    next = (arrived & expected) == expected ? (s | 0xFF) + 1  // release: next round
                                            : (s & ~uint64_t{0xFF}) | arrived;
  } while (!b.state.compare_exchange_weak(s, next, std::memory_order_acq_rel,
                                          std::memory_order_acquire));
  const uint64_t round = s >> 8;
  if ((next >> 8) != round) {
    return;  // this worker completed the round
  }
  while ((b.state.load(std::memory_order_acquire) >> 8) == round) {
    _mm_pause();
  }
}
