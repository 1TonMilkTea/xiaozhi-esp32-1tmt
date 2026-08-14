#include "sound_follow.h"

#include "application.h"
#include "board.h"
#include "display.h"
#include "assets/lang_config.h"

#include <algorithm>
#include <cmath>
#include <esp_log.h>
#include <esp_timer.h>

#define TAG "SoundFollow"

namespace {
constexpr int kTickMs = 40;
constexpr int kBeatCooldownMs = 200;
constexpr int kSilenceMs = 450;
constexpr uint8_t kMinLevel = 18;
constexpr uint8_t kMinFlux = 10;

// Keep the robot inside a 15cm-diameter circle (7.5cm radius).
constexpr float kMaxRadiusCm = 7.5f;
constexpr float kSoftRadiusCm = 5.0f;
constexpr float kHomeRadiusCm = 1.5f;
constexpr float kPi = 3.14159265f;
// Overestimate speed so we pull back earlier rather than drifting out.
constexpr float kSpeedAt100Cms = 25.0f;
// Notes: ~600ms at 100% ≈ 90°, so 0.15 deg/ms at full speed.
constexpr float kTurnDegPerMsAt100 = 0.15f;
constexpr int kMaxTurnMs = 70;
constexpr int kMaxTranslateMs = 80;
constexpr int kMaxSpeedPercent = 45;
}

SoundFollow& SoundFollow::GetInstance() {
    static SoundFollow instance;
    return instance;
}

SoundFollow::~SoundFollow() {
    Stop();
}

void SoundFollow::TimerCallback(void* arg) {
    static_cast<SoundFollow*>(arg)->OnTick();
}

void SoundFollow::Start() {
    if (running_.load()) {
        return;
    }

    auto& app = Application::GetInstance();
    restored_eye_mode_ = (app.GetDisplayMode() == kDisplayModeEyeOnly);
    if (restored_eye_mode_) {
        app.SetDisplayMode(kDisplayModeDefault);
    }

    if (timer_ == nullptr) {
        esp_timer_create_args_t args = {
            .callback = TimerCallback,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "sound_follow",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&args, &timer_));
    }

    noise_floor_ = 8.0f;
    prev_level_ = 0;
    beat_index_ = 0;
    last_beat_us_ = 0;
    motor_until_us_ = 0;
    last_sound_us_ = 0;
    pending_dir_ = 0;
    pending_speed_ = 0;
    pending_duration_ms_ = 0;
    ResetPose();
    running_.store(true);

    app.GetAudioService().EnableWakeWordDetection(true);
    ShowVisualizer(true);
    auto display = Board::GetInstance().GetDisplay();
    if (display != nullptr) {
        display->ShowNotification("拾音跟随: 15cm内", 1500);
    }

    ESP_ERROR_CHECK(esp_timer_start_periodic(timer_, kTickMs * 1000));
    ESP_LOGI(TAG, "Sound follow started");
}

void SoundFollow::Stop() {
    if (!running_.exchange(false)) {
        if (timer_ != nullptr) {
            esp_timer_stop(timer_);
        }
        return;
    }

    if (timer_ != nullptr) {
        esp_timer_stop(timer_);
    }

    auto& app = Application::GetInstance();
    app.StopMotor();
    motor_until_us_ = 0;
    pending_dir_ = 0;
    ShowVisualizer(false);

    auto display = Board::GetInstance().GetDisplay();
    if (display != nullptr) {
        display->ShowNotification("拾音跟随已关闭");
        if (!restored_eye_mode_) {
            display->SetStatus(Lang::Strings::STANDBY);
        }
    }
    if (restored_eye_mode_) {
        app.SetDisplayMode(kDisplayModeEyeOnly);
        restored_eye_mode_ = false;
    }

    ESP_LOGI(TAG, "Sound follow stopped");
}

void SoundFollow::Toggle() {
    if (running_.load()) {
        Stop();
    } else {
        Start();
    }
}

void SoundFollow::ShowVisualizer(bool show) {
    auto display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        visualizer_shown_ = show;
        return;
    }
    display->SetAudioVisualizerEnabled(show);
    visualizer_shown_ = show;
}

void SoundFollow::ResetPose() {
    pos_x_cm_ = 0;
    pos_y_cm_ = 0;
    heading_rad_ = 0;
    last_dir_ = 0;
    last_speed_ = 0;
    last_duration_ms_ = 0;
}

void SoundFollow::ApplyMoveEstimate(int direction, int speed, int duration_ms) {
    float speed_ratio = static_cast<float>(std::max(0, std::min(100, speed))) / 100.0f;
    float dt_s = static_cast<float>(duration_ms) / 1000.0f;

    if (direction == 1 || direction == 3) {
        float deg = kTurnDegPerMsAt100 * speed_ratio * static_cast<float>(duration_ms);
        if (direction == 3) {
            heading_rad_ += deg * kPi / 180.0f;
        } else {
            heading_rad_ -= deg * kPi / 180.0f;
        }
        return;
    }

    float dist = kSpeedAt100Cms * speed_ratio * dt_s;
    if (direction == 2) {
        dist = -dist;
    } else if (direction != 4) {
        return;
    }
    pos_x_cm_ += dist * std::cos(heading_rad_);
    pos_y_cm_ += dist * std::sin(heading_rad_);
}

bool SoundFollow::StartPulse(int direction, int speed, int duration_ms) {
    auto& app = Application::GetInstance();
    if (!app.PulseMotor(direction, speed, duration_ms)) {
        return false;
    }
    last_dir_ = direction;
    last_speed_ = speed;
    last_duration_ms_ = duration_ms;
    motor_until_us_ = esp_timer_get_time() + static_cast<int64_t>(duration_ms) * 1000;
    return true;
}

void SoundFollow::FinishCurrentPulse() {
    if (last_duration_ms_ > 0 && last_dir_ != 0) {
        ApplyMoveEstimate(last_dir_, last_speed_, last_duration_ms_);
    }
    last_dir_ = 0;
    last_duration_ms_ = 0;
}

bool SoundFollow::TryReturnHome() {
    float r2 = pos_x_cm_ * pos_x_cm_ + pos_y_cm_ * pos_y_cm_;
    if (r2 <= kSoftRadiusCm * kSoftRadiusCm) {
        return false;
    }

    float radius = std::sqrt(r2);
    if (radius <= kHomeRadiusCm) {
        return false;
    }

    float along = pos_x_cm_ * std::cos(heading_rad_) + pos_y_cm_ * std::sin(heading_rad_);
    int direction = (along >= 0) ? 2 : 4;
    int duration_ms = static_cast<int>((radius / kSpeedAt100Cms) * 1000.0f);
    if (duration_ms < 40) {
        duration_ms = 40;
    }
    if (duration_ms > kMaxTranslateMs) {
        duration_ms = kMaxTranslateMs;
    }
    pending_dir_ = 0;
    ESP_LOGI(TAG, "Return home r=%.1fcm dir=%d", radius, direction);
    return StartPulse(direction, 35, duration_ms);
}

void SoundFollow::PulseToBeat(uint8_t level) {
    if (TryReturnHome()) {
        beat_index_++;
        return;
    }

    int speed = 28 + (static_cast<int>(level) * 17) / 255;
    if (speed < 28) {
        speed = 28;
    }
    if (speed > kMaxSpeedPercent) {
        speed = kMaxSpeedPercent;
    }

    int duration_ms = (level > 80) ? kMaxTurnMs : 50;
    int direction = 3;
    pending_dir_ = 0;

    switch (beat_index_ % 4) {
        case 0:
            direction = 3; // in-place left
            break;
        case 1:
            direction = 1; // in-place right
            break;
        case 2:
            direction = 3;
            break;
        default: {
            // Tiny forward, then mandatory equal reverse so net travel ~ 0.
            direction = 4;
            duration_ms = (level > 80) ? 60 : 45;
            if (duration_ms > kMaxTranslateMs) {
                duration_ms = kMaxTranslateMs;
            }
            pending_dir_ = 2;
            pending_speed_ = speed;
            pending_duration_ms_ = duration_ms;
            break;
        }
    }
    beat_index_++;
    if (!StartPulse(direction, speed, duration_ms)) {
        pending_dir_ = 0;
    }
}

void SoundFollow::OnTick() {
    if (!running_.load()) {
        return;
    }

    auto& app = Application::GetInstance();
    const bool idle = (app.GetDeviceState() == kDeviceStateIdle);
    int64_t now = esp_timer_get_time();

    if (!idle) {
        if (visualizer_shown_) {
            ShowVisualizer(false);
        }
        if (motor_until_us_ != 0) {
            app.StopMotor();
            motor_until_us_ = 0;
            pending_dir_ = 0;
        }
        return;
    }

    if (!visualizer_shown_) {
        ShowVisualizer(true);
    }

    if (motor_until_us_ != 0 && now >= motor_until_us_) {
        app.StopMotor();
        motor_until_us_ = 0;
        FinishCurrentPulse();
        if (pending_dir_ != 0) {
            int dir = pending_dir_;
            int speed = pending_speed_;
            int duration_ms = pending_duration_ms_;
            pending_dir_ = 0;
            StartPulse(dir, speed, duration_ms);
        } else {
            float r2 = pos_x_cm_ * pos_x_cm_ + pos_y_cm_ * pos_y_cm_;
            if (r2 > kMaxRadiusCm * kMaxRadiusCm) {
                TryReturnHome();
            }
        }
    }

    uint8_t level = app.GetAudioService().GetAudioLevel();
    uint8_t flux = app.GetAudioService().GetAudioFlux();
    auto display = Board::GetInstance().GetDisplay();
    if (display != nullptr) {
        display->UpdateAudioVisualizer(level);
    }

    noise_floor_ = noise_floor_ * 0.97f + static_cast<float>(level) * 0.03f;
    if (noise_floor_ < 6.0f) {
        noise_floor_ = 6.0f;
    }

    bool loud = level >= kMinLevel && level > static_cast<uint8_t>(noise_floor_ + 12.0f);
    bool rising = (level > prev_level_ + 6) || (flux >= kMinFlux);
    bool cooled = (now - last_beat_us_) > static_cast<int64_t>(kBeatCooldownMs) * 1000;
    if (loud && rising && cooled && motor_until_us_ == 0) {
        last_beat_us_ = now;
        last_sound_us_ = now;
        PulseToBeat(level);
    } else if (loud) {
        last_sound_us_ = now;
    } else if (motor_until_us_ == 0 && last_sound_us_ != 0 &&
               (now - last_sound_us_) > static_cast<int64_t>(kSilenceMs) * 1000) {
        app.StopMotor();
        last_sound_us_ = 0;
    }

    prev_level_ = level;
}
