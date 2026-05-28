#include "battery.h"
#include "config.h"

Battery battery;

void Battery::begin() {
    if (BATTERY_ADC_PIN < 0) { _enabled = false; return; }
    _enabled = true;
    pinMode(BATTERY_ADC_PIN, INPUT);
    analogReadResolution(12);
    // Force a first read so the icon doesn't sit at 0% for the first
    // sample interval.
    _lastSample = 0;
    update();
}

void Battery::update() {
    if (!_enabled) return;
    unsigned long now = millis();
    if (_lastSample && (now - _lastSample) < BATTERY_SAMPLE_INTERVAL_MS) return;
    _lastSample = now;

    // Average a few samples to suppress ADC noise (~22 mV typical on P4).
    long acc = 0;
    const int N = 8;
    for (int i = 0; i < N; i++) acc += analogReadMilliVolts(BATTERY_ADC_PIN);
    int adcMv = acc / N;
    _mv = (int)(adcMv * BATTERY_DIVIDER_RATIO);

    int range = BATTERY_FULL_MV - BATTERY_EMPTY_MV;
    int p = (_mv - BATTERY_EMPTY_MV) * 100 / range;
    if (p < 0)   p = 0;
    if (p > 100) p = 100;
    _percent = p;

    pushHistory(_mv);
    _charging = computeCharging();
}

void Battery::pushHistory(int mv) {
    // Battery insertion/removal causes a >1 V step at BAT+. Wipe history
    // on big steps so the trend detector compares fresh samples instead
    // of fighting stale pre-event readings for the next 16 s.
    if (_histCount > 0 && abs(mv - _hist[HIST_LEN - 1]) > 500) {
        _histCount = 0;
    }
    for (int i = 0; i < HIST_LEN - 1; i++) _hist[i] = _hist[i + 1];
    _hist[HIST_LEN - 1] = mv;
    if (_histCount < HIST_LEN) _histCount++;
}

bool Battery::computeCharging() const {
    // No real battery -> not charging. (Catches both flat-zero and the
    // phantom backfeed condition where BAT+ floats up to ~USB voltage.)
    if (!present()) return false;

    // Near full: Li-ion settles below ~4.05 V within a minute of unplugging,
    // so anything still up here is almost certainly on the charger.
    if (_mv >= 4150) return true;

    // Otherwise look at the trend across the history window. Compare the
    // average of the newer half against the older half; a clear positive
    // delta means the cell is being charged.
    if (_histCount < HIST_LEN) return false;

    long oldSum = 0, newSum = 0;
    int half = HIST_LEN / 2;
    for (int i = 0; i < half; i++)             oldSum += _hist[i];
    for (int i = half; i < HIST_LEN; i++)      newSum += _hist[i];
    int delta = (newSum - oldSum) / half;  // mV averaged

    return delta >= 25;  // > ADC noise floor (~22 mV)
}
