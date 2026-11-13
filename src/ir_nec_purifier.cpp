// ir_nec_purifier.cpp: NEC planner/encoder (host + device) and RMT executor (device).
#include "ir_nec_purifier.h"

namespace ir {

uint32_t nec_raw32(uint16_t addr_word, uint16_t cmd_word) {
  return (static_cast<uint32_t>(cmd_word) << 16) | addr_word;
}

void nec_encode(uint32_t raw32, NecSymbol out[kNecSymbols]) {
  out[0] = {9000, 4500};  // AGC leader
  for (int i = 0; i < 32; ++i) {
    const bool one = (raw32 >> i) & 1u;  // LSB first
    out[1 + i] = {560, static_cast<uint16_t>(one ? 1690 : 560)};
  }
  out[33] = {560, 0};  // stop mark
}

int plan_transition(int from, int to, bool force, bool wake_prefix, const uint16_t codes[kNumSpeeds],
                    Step out[kMaxSteps]) {
  if (to < 0 || to > 6) return 0;
  if (from < 0 || from > 6) from = 0;
  if (from == to && !force) return 0;
  int n = 0;
  if (wake_prefix && from == 0 && to >= 2) {
    out[n++] = {1, codes[1], 0};
    out[n++] = {static_cast<uint8_t>(to), codes[to], kWakeDelayMs};
  } else {
    out[n++] = {static_cast<uint8_t>(to), codes[to], 0};
  }
  return n;
}

int plan_executor_request(int sent_speed, int to_speed, bool force, bool assume_off,
                          bool wake_prefix, const uint16_t codes[kNumSpeeds], Step out[kMaxSteps]) {
  const int from = (assume_off || sent_speed < 0) ? 0 : sent_speed;
  const bool force_eff = force || assume_off || (sent_speed < 0);
  return plan_transition(from, to_speed, force_eff, wake_prefix, codes, out);
}

int next_sent_speed(int prev_sent_speed, uint8_t step_speed, int frame_idx, bool track_state) {
  if (!track_state || frame_idx + 1 < kFramesPerCommand) return prev_sent_speed;
  return step_speed;
}

}  // namespace ir

#ifndef UNIT_TEST
#include <driver/rmt_encoder.h>
#include <driver/rmt_tx.h>
#include <esp_log.h>

namespace {
constexpr const char* kTag = "ir";
rmt_symbol_word_t g_symbols[ir::kNecSymbols];
}  // namespace

bool IrNecPurifier::begin(int gpio, const AppConfig& cfg) {
  apply_config(cfg);

  rmt_tx_channel_config_t tx_cfg = {};
  tx_cfg.gpio_num = static_cast<gpio_num_t>(gpio);
  tx_cfg.clk_src = RMT_CLK_SRC_DEFAULT;
  tx_cfg.resolution_hz = 1000000;  // 1 tick = 1 us
  tx_cfg.mem_block_symbols = 64;   // one frame (34 symbols) fits in RAM block
  tx_cfg.trans_queue_depth = 2;
  tx_cfg.flags.invert_out = false;  // active-high LED
  tx_cfg.flags.with_dma = false;

  rmt_channel_handle_t chan = nullptr;
  if (rmt_new_tx_channel(&tx_cfg, &chan) != ESP_OK) {
    ESP_LOGE(kTag, "rmt_new_tx_channel failed on GPIO%d", gpio);
    return false;
  }
  rmt_carrier_config_t carrier = {};
  carrier.frequency_hz = 38000;
  carrier.duty_cycle = 0.33f;
  carrier.flags.polarity_active_low = false;
  if (rmt_apply_carrier(chan, &carrier) != ESP_OK) {
    ESP_LOGE(kTag, "rmt_apply_carrier failed");
    rmt_del_channel(chan);
    return false;
  }
  rmt_copy_encoder_config_t enc_cfg = {};
  rmt_encoder_handle_t enc = nullptr;
  if (rmt_new_copy_encoder(&enc_cfg, &enc) != ESP_OK) {
    ESP_LOGE(kTag, "rmt_new_copy_encoder failed");
    rmt_del_channel(chan);
    return false;
  }
  if (rmt_enable(chan) != ESP_OK) {
    ESP_LOGE(kTag, "rmt_enable failed");
    rmt_del_encoder(enc);
    rmt_del_channel(chan);
    return false;
  }
  chan_ = chan;
  encoder_ = enc;
  ESP_LOGI(kTag, "NEC TX ready on GPIO%d (addr word 0x%04X)", gpio, address_);
  return true;
}

void IrNecPurifier::apply_config(const AppConfig& cfg) {
  address_ = cfg.ir_nec_address;
  for (int i = 0; i < kNumSpeeds; ++i) codes_[i] = cfg.ir_codes[i];
  wake_prefix_ = cfg.wake_require_speed1_prefix;
}

void IrNecPurifier::request(int to_speed, bool force, bool assume_off, bool track_state) {
  // Always started from loop() so all timing is loop-driven; newest request wins.
  have_pending_ = true;
  pending_to_ = to_speed;
  pending_force_ = force;
  pending_assume_off_ = assume_off;
  pending_track_ = track_state;
}

void IrNecPurifier::start_plan(int to, bool force, bool assume_off, bool track_state, uint32_t now_ms) {
  n_steps_ = ir::plan_executor_request(sent_speed_, to, force, assume_off, wake_prefix_, codes_, steps_);
  step_idx_ = 0;
  frame_idx_ = 0;
  track_state_ = track_state;
  if (n_steps_ == 0) {
    state_ = State::IDLE;
    return;
  }
  t_mark_ = now_ms;
  state_ = State::WAIT_DELAY;
}

bool IrNecPurifier::start_frame(uint16_t cmd_word) {
  if (chan_ == nullptr) return false;
  ir::NecSymbol sym[ir::kNecSymbols];
  ir::nec_encode(ir::nec_raw32(address_, cmd_word), sym);
  for (int i = 0; i < ir::kNecSymbols; ++i) {
    g_symbols[i].duration0 = sym[i].mark_us;
    g_symbols[i].level0 = 1;
    // A zero-length duration terminates RMT output; keep the stop mark's space at 1 us.
    g_symbols[i].duration1 = sym[i].space_us == 0 ? 1 : sym[i].space_us;
    g_symbols[i].level1 = 0;
  }
  rmt_transmit_config_t tc = {};
  tc.loop_count = 0;
  tc.flags.eot_level = 0;
  const esp_err_t err = rmt_transmit(static_cast<rmt_channel_handle_t>(chan_),
                                     static_cast<rmt_encoder_handle_t>(encoder_), g_symbols,
                                     sizeof(g_symbols), &tc);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "rmt_transmit failed: %s", esp_err_to_name(err));
    return false;
  }
  return true;
}

bool IrNecPurifier::frame_done() {
  return rmt_tx_wait_all_done(static_cast<rmt_channel_handle_t>(chan_), 0) == ESP_OK;
}

void IrNecPurifier::loop(uint32_t now_ms) {
  if (wake_settling_ && now_ms - last_wake_ms_ >= ir::kWakeDelayMs) wake_settling_ = false;
  switch (state_) {
    case State::IDLE:
      if (have_pending_ && (tx_frames_ == 0 || now_ms - last_frame_end_ms_ >= ir::kInterSequenceGapMs) &&
          (!wake_settling_ || pending_to_ < 2)) {
        have_pending_ = false;
        start_plan(pending_to_, pending_force_, pending_assume_off_, pending_track_, now_ms);
      }
      break;

    case State::WAIT_DELAY:
      if (now_ms - t_mark_ >= steps_[step_idx_].delay_before_ms) {
        if (step_idx_ > 0 && steps_[step_idx_].delay_before_ms >= ir::kWakeDelayMs) wake_settling_ = false;
        if (have_pending_) {
          have_pending_ = false;
          start_plan(pending_to_, pending_force_, pending_assume_off_, pending_track_, now_ms);
          break;
        }
        if (start_frame(steps_[step_idx_].command_word)) {
          state_ = State::SENDING;
        } else {
          state_ = State::IDLE;  // hardware error already logged
        }
      }
      break;

    case State::SENDING:
      if (frame_done()) {
        const ir::Step& s = steps_[step_idx_];
        ++tx_frames_;
        last_tx_ms_ = now_ms;
        last_command_ = s.command_word;
        if (frame_idx_ == 0 && tx_cb_) tx_cb_(s.speed, s.command_word);
        ESP_LOGI(kTag, "TX speed %u cmd 0x%04X raw 0x%08lX (frame %d/%d)", s.speed, s.command_word,
                 static_cast<unsigned long>(ir::nec_raw32(address_, s.command_word)), frame_idx_ + 1,
                 ir::kFramesPerCommand);
        t_mark_ = now_ms;
        last_frame_end_ms_ = now_ms;
        if (track_state_ && frame_idx_ + 1 >= ir::kFramesPerCommand) {
          if (s.speed == 1 && sent_speed_ <= 0 && n_steps_ > 1) {
            last_wake_ms_ = now_ms;
            wake_settling_ = true;
          } else if (s.speed != 1) {
            wake_settling_ = false;
          }
        }
        sent_speed_ = ir::next_sent_speed(sent_speed_, s.speed, frame_idx_, track_state_);
        state_ = State::WAIT_GAP;
      }
      break;

    case State::WAIT_GAP:
      if (frame_idx_ + 1 < ir::kFramesPerCommand) {
        if (now_ms - t_mark_ >= ir::kFrameGapMs) {
          ++frame_idx_;
          state_ = start_frame(steps_[step_idx_].command_word) ? State::SENDING : State::IDLE;
        }
      } else if (step_idx_ + 1 < n_steps_ && !have_pending_) {
        ++step_idx_;
        frame_idx_ = 0;
        state_ = State::WAIT_DELAY;  // delay_before_ms counted from end of last frame
      } else {
        state_ = State::IDLE;  // done, or a newer request supersedes the rest
      }
      break;
  }
}
#endif  // UNIT_TEST
