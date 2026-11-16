#pragma once
// dlog::Storage on the ESP32's internal flash (LittleFS, in the partition the
// default table calls "spiffs"): one file per segment under /log.
#ifdef ARDUINO
#include <Arduino.h>

#include "datalog.h"

class FsStorage : public dlog::Storage {
   public:
    bool begin();  // mounts, formatting on first use (takes several seconds)
    size_t usedBytes();
    size_t totalBytes();

    size_t list(uint32_t* segments, size_t max) override;
    size_t size(uint32_t segment) override;
    bool append(uint32_t segment, const uint8_t* data, size_t n) override;
    bool read(uint32_t segment, size_t offset, uint8_t* data, size_t n) override;
    void remove(uint32_t segment) override;
};
#endif
