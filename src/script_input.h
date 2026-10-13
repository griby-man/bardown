// scripted pad for unattended runs: BARDOWN_INPUT="8:a;14:start;20:down;21:a;150:ly=1@4"
// each step is <seconds since boot>:<button>, held for a fifth of a second, or held longer with
// @<seconds>. buttons: a b x y start back up down left right lb rb. sticks and triggers take a
// value: lx ly rx ry (-1..1, up and right positive), lt rt (0..1)
// BARDOWN_INPUT_FILE=<path>: steps appended to that file as lines ("a", "down", "ly=1@0.5") are
// pressed as they arrive, for driving menus by hand from a shell

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <rex/input/input_driver.h>
#include <rex/logging.h>
#include <rex/system/xtypes.h>

// per-poll override (a capture hook can drive the pad from guest state; faceoff_capture.cpp defines it)
extern "C" void bardown_pad_override(double* axes, uint16_t* buttons);

namespace bardown {

using rex::X_RESULT;
using rex::X_STATUS;

class ScriptInputDriver : public rex::input::InputDriver {
 public:
  static constexpr auto kDevice = static_cast<rex::input::DeviceId>(0x42445300);  // "BDS"

  explicit ScriptInputDriver(std::string script)
      : InputDriver(nullptr, 0), start_(std::chrono::steady_clock::now()) {
    if (const char* f = std::getenv("BARDOWN_INPUT_FILE")) live_ = f;
    std::stringstream ss(script);
    std::string step;
    while (std::getline(ss, step, ';')) Add(step);
    REXLOG_INFO("[input-script] {} steps", steps_.size());
  }

  // one step, "<seconds>:<what>"
  void Add(const std::string& step) {
    {
      auto colon = step.find(':');
      if (colon == std::string::npos) return;
      std::string what = step.substr(colon + 1);
      double hold = 0.2;
      if (auto at = what.find('@'); at != std::string::npos) {
        hold = std::stod(what.substr(at + 1));
        what.resize(at);
      }
      Step s{std::stod(step.substr(0, colon)), hold, 0, -1, 0};
      if (auto eq = what.find('='); eq != std::string::npos) {
        static const char* axes[] = {"lx", "ly", "rx", "ry", "lt", "rt"};
        std::string axis = what.substr(0, eq);
        for (int i = 0; i < 6; ++i)
          if (axis == axes[i]) s.axis = i;
        s.value = std::stod(what.substr(eq + 1));
        if (s.axis < 0) {
          REXLOG_WARN("[input-script] unknown axis in '{}'", step);
          return;
        }
      } else {
        s.bit = ButtonBit(what);
        if (!s.bit) {
          REXLOG_WARN("[input-script] unknown button in '{}'", step);
          return;
        }
      }
      steps_.push_back(s);
    }
  }

  // new lines in the live file go in now
  void Poll(double now) {
    if (live_.empty() || now - last_read_ < 0.05) return;
    last_read_ = now;
    std::ifstream in(live_);
    if (!in) return;
    in.seekg(0, std::ios::end);
    const auto size = static_cast<std::streamoff>(in.tellg());
    if (size <= read_) return;
    in.seekg(read_);
    std::string line;
    while (std::getline(in, line)) {
      if (in.eof()) break;  // a half-written last line waits for next time
      read_ = static_cast<std::streamoff>(in.tellg());
      if (line.empty()) continue;
      Add(std::to_string(now) + ":" + line);
      REXLOG_INFO("[input-script] live '{}' at {:.1f}s", line, now);
    }
  }

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }

  void EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) override {
    rex::input::DeviceInfo info;
    info.id = kDevice;
    info.name = "Input script";
    info.synthetic = true;
    out.push_back(info);
  }

  X_RESULT GetDeviceState(rex::input::DeviceId id, rex::input::X_INPUT_STATE* out) override {
    if (id != kDevice) return X_ERROR_DEVICE_NOT_CONNECTED;
    double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
    std::lock_guard<std::mutex> lock(mutex_);
    Poll(now);
    uint16_t buttons = 0;
    double axes[6] = {};
    for (const auto& s : steps_) {
      if (now < s.at || now >= s.at + s.hold) continue;
      if (s.axis < 0) buttons |= s.bit;
      else axes[s.axis] = s.value;
    }
    bardown_pad_override(axes, &buttons);
    // proof the game is polling us, and when presses go out (unattended runs are otherwise blind)
    if (!polled_) {
      polled_ = true;
      REXLOG_INFO("[input-script] first poll at {:.1f}s", now);
    }
    if (buttons && buttons != last_) REXLOG_INFO("[input-script] {:.1f}s buttons {:04x}", now, buttons);
    if (out) {
      std::memset(out, 0, sizeof(*out));
      if (buttons != last_) ++packet_;
      last_ = buttons;
      out->packet_number = packet_;
      out->gamepad.buttons = buttons;
      auto stick = [](double v) { return int16_t(std::clamp(v, -1.0, 1.0) * 32767); };
      auto trigger = [](double v) { return uint8_t(std::clamp(v, 0.0, 1.0) * 255); };
      out->gamepad.thumb_lx = stick(axes[0]);
      out->gamepad.thumb_ly = stick(axes[1]);
      out->gamepad.thumb_rx = stick(axes[2]);
      out->gamepad.thumb_ry = stick(axes[3]);
      out->gamepad.left_trigger = trigger(axes[4]);
      out->gamepad.right_trigger = trigger(axes[5]);
      // stick moves count as new packets too
      uint64_t sig = uint64_t(stick(axes[0]) & 0xffff) | uint64_t(stick(axes[1]) & 0xffff) << 16 |
                     uint64_t(stick(axes[2]) & 0xffff) << 32 | uint64_t(stick(axes[3]) & 0xffff) << 48;
      if (sig != last_axes_) ++packet_, last_axes_ = sig, out->packet_number = packet_;
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT GetDeviceCapabilities(rex::input::DeviceId id, uint32_t,
                                 rex::input::X_INPUT_CAPABILITIES* caps) override {
    if (id != kDevice) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (caps) {
      std::memset(caps, 0, sizeof(*caps));
      caps->type = 0x01;
      caps->sub_type = 0x01;
      caps->gamepad.buttons = 0xFFFF;
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT SetDeviceVibration(rex::input::DeviceId id, rex::input::X_INPUT_VIBRATION*) override {
    return id == kDevice ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }

  X_RESULT GetDeviceKeystroke(rex::input::DeviceId id, uint32_t,
                              rex::input::X_INPUT_KEYSTROKE*) override {
    return id == kDevice ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
  }

 private:
  std::string live_;
  std::streamoff read_ = 0;
  double last_read_ = -1;
  std::mutex mutex_;
  struct Step {
    double at, hold;
    uint16_t bit;
    int axis;  // -1 for a button
    double value;
  };

  static uint16_t ButtonBit(const std::string& name) {
    using namespace rex::input;
    if (name == "a") return X_INPUT_GAMEPAD_A;
    if (name == "b") return X_INPUT_GAMEPAD_B;
    if (name == "x") return X_INPUT_GAMEPAD_X;
    if (name == "y") return X_INPUT_GAMEPAD_Y;
    if (name == "start") return X_INPUT_GAMEPAD_START;
    if (name == "back") return X_INPUT_GAMEPAD_BACK;
    if (name == "up") return X_INPUT_GAMEPAD_DPAD_UP;
    if (name == "down") return X_INPUT_GAMEPAD_DPAD_DOWN;
    if (name == "left") return X_INPUT_GAMEPAD_DPAD_LEFT;
    if (name == "right") return X_INPUT_GAMEPAD_DPAD_RIGHT;
    if (name == "lb") return X_INPUT_GAMEPAD_LEFT_SHOULDER;
    if (name == "rb") return X_INPUT_GAMEPAD_RIGHT_SHOULDER;
    if (name == "ls") return X_INPUT_GAMEPAD_LEFT_THUMB;
    if (name == "rs") return X_INPUT_GAMEPAD_RIGHT_THUMB;
    return 0;
  }

  std::chrono::steady_clock::time_point start_;
  bool polled_ = false;
  std::vector<Step> steps_;
  uint16_t last_ = 0;
  uint64_t last_axes_ = 0;
  uint32_t packet_ = 0;
};

}  // namespace bardown
