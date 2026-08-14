#ifndef SOUND_FOLLOW_H
#define SOUND_FOLLOW_H

#include <esp_timer.h>
#include <atomic>
#include <cstdint>

class SoundFollow {
public:
    static SoundFollow& GetInstance();
    ~SoundFollow();

    void Start();
    void Stop();
    void Toggle();
    bool IsRunning() const { return running_.load(); }

private:
    SoundFollow() = default;
    SoundFollow(const SoundFollow&) = delete;
    SoundFollow& operator=(const SoundFollow&) = delete;

    static void TimerCallback(void* arg);
    void OnTick();
    void ShowVisualizer(bool show);
    void PulseToBeat(uint8_t level);
    void ResetPose();
    void ApplyMoveEstimate(int direction, int speed, int duration_ms);
    bool StartPulse(int direction, int speed, int duration_ms);
    void FinishCurrentPulse();
    bool TryReturnHome();

    std::atomic<bool> running_{false};
    bool visualizer_shown_ = false;
    bool restored_eye_mode_ = false;
    esp_timer_handle_t timer_ = nullptr;

    float noise_floor_ = 8.0f;
    uint8_t prev_level_ = 0;
    int beat_index_ = 0;
    int64_t last_beat_us_ = 0;
    int64_t motor_until_us_ = 0;
    int64_t last_sound_us_ = 0;

    // Dead-reckoning pose so dance stays inside a 15cm-diameter circle.
    float pos_x_cm_ = 0;
    float pos_y_cm_ = 0;
    float heading_rad_ = 0;
    int last_dir_ = 0;
    int last_speed_ = 0;
    int last_duration_ms_ = 0;
    int pending_dir_ = 0;
    int pending_speed_ = 0;
    int pending_duration_ms_ = 0;
};

#endif
