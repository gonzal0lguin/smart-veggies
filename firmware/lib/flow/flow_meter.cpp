#include "flow_meter.h"

void IRAM_ATTR FlowMeter::isr(void* arg) {
    FlowMeter* self = static_cast<FlowMeter*>(arg);
    uint32_t now = micros();
    // Ignore edges closer than 1 ms (1 kHz): far above any real flow, so it is noise.
    if (now - self->lastMicros_ < 1000) return;
    self->lastMicros_ = now;
    self->pulses_ = self->pulses_ + 1;
}

void FlowMeter::begin(uint8_t pin, float pulsesPerLitre) {
    pin_ = pin;
    setPulsesPerLitre(pulsesPerLitre);
    pinMode(pin_, INPUT);
    windowStartMs_ = millis();
    windowStartPulses_ = 0;
    attachInterruptArg(digitalPinToInterrupt(pin_), isr, this, RISING);
}

void FlowMeter::update() {
    uint32_t now = millis();
    uint32_t dt = now - windowStartMs_;
    if (dt < 1000) return;
    uint32_t p = pulses_;
    hz_ = (p - windowStartPulses_) * 1000.0f / dt;
    windowStartPulses_ = p;
    windowStartMs_ = now;
}

void FlowMeter::resetTotal() {
    noInterrupts();
    pulses_ = 0;
    interrupts();
    windowStartPulses_ = 0;
    windowStartMs_ = millis();
    hz_ = 0;
}
