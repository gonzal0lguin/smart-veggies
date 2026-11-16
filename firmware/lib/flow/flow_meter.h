#pragma once
// Pulse-output flow sensor (YF-S201 and similar hall turbine sensors).
// Pulses are counted in an interrupt; call update() regularly from the loop.
#include <Arduino.h>

class FlowMeter {
   public:
    void begin(uint8_t pin, float pulsesPerLitre);
    // Recomputes the flow rate about once per second.
    void update();

    uint32_t pulses() const { return pulses_; }
    float litres() const { return pulses_ / pulsesPerLitre_; }
    float hz() const { return hz_; }
    float litresPerMinute() const { return hz_ * 60.0f / pulsesPerLitre_; }
    void resetTotal();

    float pulsesPerLitre() const { return pulsesPerLitre_; }
    void setPulsesPerLitre(float k) {
        if (k > 0) pulsesPerLitre_ = k;
    }

   private:
    static void IRAM_ATTR isr(void* arg);

    volatile uint32_t pulses_ = 0;
    volatile uint32_t lastMicros_ = 0;
    float pulsesPerLitre_ = 450.0f;
    float hz_ = 0;
    uint32_t windowStartMs_ = 0;
    uint32_t windowStartPulses_ = 0;
    uint8_t pin_ = 0;
};
