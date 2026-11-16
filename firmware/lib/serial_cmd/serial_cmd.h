#pragma once
// Minimal non-blocking line reader for the test firmwares' serial consoles.
#include <Arduino.h>

class LineReader {
   public:
    // Returns a NUL-terminated line when one is complete, otherwise nullptr.
    // The pointer is valid until the next call.
    const char* poll(Stream& s = Serial) {
        while (s.available()) {
            char c = (char)s.read();
            if (c == '\r' || c == '\n') {
                if (n_ == 0) continue;
                buf_[n_] = 0;
                n_ = 0;
                return buf_;
            }
            if (n_ < sizeof(buf_) - 1) buf_[n_++] = c;
        }
        return nullptr;
    }

   private:
    char buf_[96] = {};
    size_t n_ = 0;
};
