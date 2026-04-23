#pragma once

#include <Arduino.h>

namespace lora_app {

inline uint16_t read_battery_millivolts(int adcPin, float dividerRatio, uint8_t samples) {
  if (adcPin < 0 || dividerRatio <= 0.0f) {
    return 0;
  }

#if defined(ESP32)
  if (samples == 0) {
    samples = 1;
  }

#if defined(ADC_11db)
  analogSetPinAttenuation(adcPin, ADC_11db);
#endif

  uint32_t sumMv = 0;
  uint8_t validSamples = 0;
  for (uint8_t i = 0; i < samples; i++) {
    const uint32_t sampleMv = analogReadMilliVolts(adcPin);
    if (sampleMv > 0) {
      sumMv += sampleMv;
      validSamples++;
    }
    delay(2);
  }

  if (validSamples == 0) {
    return 0;
  }

  const float adcMv = static_cast<float>(sumMv) / validSamples;
  const float batteryMv = adcMv * dividerRatio;
  if (batteryMv <= 0.0f) {
    return 0;
  }
  if (batteryMv >= 65535.0f) {
    return 65535;
  }
  return static_cast<uint16_t>(batteryMv + 0.5f);
#else
  (void)samples;
  return 0;
#endif
}

}  // namespace lora_app
