// ir_nec_purifier.h: discrete NEC IR actuator for the IQAir GC MultiGas (GPIO4).
//
// Requirements: SRS §3.2, prompt §7. Verified codes:
// .agents/skills/ir-code-capture/references/iqair_gc_multigas.md
//
// Frame layout (standard NEC, LSB first on the wire):
//   addr_lo, addr_hi, cmd_lo, cmd_hi  with  raw32 = (cmd_word << 16) | addr_word
//   e.g. Speed 1 = address word 0xFF00 + command word 0xE817 -> raw 0xE817FF00
//   (identical to IRremote's decodedRawData for the captured remote).
//
// The pure planner/encoder below compiles on the host for unit tests.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "sys_config.h"

namespace ir {

constexpr int kNecSymbols = 34;            // leader + 32 bits + stop
constexpr uint16_t kFrameGapMs = 100;      // between the two copies of a command
constexpr uint16_t kWakeDelayMs = 600;     // after the Speed-1 wake prefix
constexpr uint16_t kInterSequenceGapMs = 100;  // between consecutive command sequences
constexpr int kFramesPerCommand = 2;

struct NecSymbol {
  uint16_t mark_us;   // carrier on
  uint16_t space_us;  // carrier off (0 for the final stop mark)
};

uint32_t nec_raw32(uint16_t addr_word, uint16_t cmd_word);
void nec_encode(uint32_t raw32, NecSymbol out[kNecSymbols]);

// One "step" = one command word transmitted kFramesPerCommand times.
struct Step {
  uint8_t speed;            // purifier speed this command selects (0..6)
  uint16_t command_word;
  uint16_t delay_before_ms; // wait after the previous step finished
};
constexpr int kMaxSteps = 2;

// Plans the IR steps for a transition. `force` re-sends even if from == to.
// Wake-from-off: 0 -> S (S >= 2) with wake_prefix sends Speed 1 first, waits
// 600 ms, then sends S.
int plan_transition(int from_speed, int to_speed, bool force, bool wake_prefix,
                    const uint16_t codes[kNumSpeeds], Step out[kMaxSteps]);

// Executor planning helper: maps unknown sent_speed (< 0) or assume_off to
// from_speed = 0 and forces transmission when sent_speed < 0 or assume_off.
int plan_executor_request(int sent_speed, int to_speed, bool force, bool assume_off,
                          bool wake_prefix, const uint16_t codes[kNumSpeeds], Step out[kMaxSteps]);

// Updates tracked purifier speed on frame completion (`track_state = false`
// preserves `prev_sent_speed` for diagnostic test_ir transmissions).
int next_sent_speed(int prev_sent_speed, uint8_t step_speed, int frame_idx, bool track_state);

}  // namespace ir

#ifndef UNIT_TEST
// ---------------------------------------------------------------------------
// Device executor: ESP32-S3 RMT TX, 1 MHz resolution, 38 kHz / 33 % carrier.
// Fully non-blocking: call loop() from the main loop; it never waits.
// ---------------------------------------------------------------------------
#include <atomic>
#include <functional>

class IrNecPurifier {
 public:
  using TxCallback = std::function<void(uint8_t speed, uint16_t command_word)>;

  IrNecPurifier() = default;
  // Non-copyable/non-movable: begin() registers &tx_done_ with the RMT ISR.
  IrNecPurifier(const IrNecPurifier&) = delete;
  IrNecPurifier& operator=(const IrNecPurifier&) = delete;

  bool begin(int gpio, const AppConfig& cfg);
  void apply_config(const AppConfig& cfg);
  void on_transmit(TxCallback cb) { tx_cb_ = cb; }

  // Request the purifier to reach `to_speed`. The executor plans from the speed
  // it last physically transmitted (unknown at boot -> treated as Off, so the
  // wake prefix is applied). If a sequence is in progress, the newest request
  // replaces any pending one and starts after the current step completes
  // (frames are never cut), at least kInterSequenceGapMs after the last frame.
  // assume_off: plan as if the purifier were in standby (refresh / re-send,
  // so a unit that missed an earlier wake is woken again).
  // track_state: false for diagnostic test_ir sends so sent_speed_ is unchanged.
  void request(int to_speed, bool force, bool assume_off = false, bool track_state = true);
  // Diagnostic send without updating the controller's tracked sent_speed_.
  void test_send(int speed) { request(speed, true, false, false); }
  // Re-transmit the current speed's code (medium press / periodic refresh).
  void resend(int speed) { request(speed, true, true, true); }
  int sent_speed() const { return sent_speed_; }   // -1 = unknown

  void loop(uint32_t now_ms);
  bool busy() const { return state_ != State::IDLE || have_pending_; }

  uint32_t tx_frames() const { return tx_frames_; }
  uint32_t last_tx_ms() const { return last_tx_ms_; }
  uint16_t last_command() const { return last_command_; }

 private:
  enum class State : uint8_t { IDLE, WAIT_DELAY, SENDING, WAIT_GAP };
  bool start_frame(uint16_t cmd_word);
  bool frame_done();
  void start_plan(int to, bool force, bool assume_off, bool track_state, uint32_t now_ms);

  void* chan_ = nullptr;     // rmt_channel_handle_t
  void* encoder_ = nullptr;  // rmt_encoder_handle_t
  std::atomic<uint32_t> tx_done_{0};
  uint16_t address_ = kDefaultIrAddress;
  uint16_t codes_[kNumSpeeds] = {};
  bool wake_prefix_ = true;

  ir::Step steps_[ir::kMaxSteps];
  int n_steps_ = 0;
  int step_idx_ = 0;
  int frame_idx_ = 0;
  State state_ = State::IDLE;
  uint32_t t_mark_ = 0;
  bool track_state_ = true;

  bool have_pending_ = false;
  int pending_to_ = 0;
  bool pending_force_ = false;
  bool pending_assume_off_ = false;
  bool pending_track_ = true;
  int sent_speed_ = -1;            // last speed whose command was fully transmitted
  uint32_t last_frame_end_ms_ = 0;
  uint32_t last_wake_ms_ = 0;
  bool wake_settling_ = false;

  uint32_t tx_frames_ = 0;
  uint32_t last_tx_ms_ = 0;
  uint16_t last_command_ = 0;
  TxCallback tx_cb_;
};
#endif
