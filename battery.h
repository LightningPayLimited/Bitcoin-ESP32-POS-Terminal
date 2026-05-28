#pragma once
#include <Arduino.h>

class Battery {
public:
    void begin();
    void update();          // call from loop(); rate-limits internally

    int  millivolts() const { return _mv; }
    int  percent()    const { return _percent; }
    // Real Li-ion sits in 2.5-4.3 V. Above that is IP5306 boost back-feeding
    // through the inductor when no cell is connected — treat as absent.
    bool present()    const { return _mv >= 2500 && _mv <= 4300; }
    bool enabled()    const { return _enabled; }
    bool charging()   const { return _charging; }

private:
    static constexpr int HIST_LEN = 8;  // ~16 s of trend at 2 s/sample

    bool          _enabled    = false;
    int           _mv         = 0;
    int           _percent    = 0;
    bool          _charging   = false;
    unsigned long _lastSample = 0;
    int           _hist[HIST_LEN] = {0};
    int           _histCount  = 0;

    void  pushHistory(int mv);
    bool  computeCharging() const;
};

extern Battery battery;
