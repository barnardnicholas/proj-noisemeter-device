#ifndef TIMESTAMP_H
#define TIMESTAMP_H

#include <Arduino.h>
#include <ctime>

class Timestamp
{
public:
    Timestamp(): tm(std::time(nullptr)) {}

    bool valid() const noexcept {
        return tm >= 8 * 3600 * 2;
    }

    operator String() const noexcept {
        char tsbuf[32];
        const auto timeinfo = std::gmtime(&tm);
        const auto success = std::strftime(tsbuf, sizeof(tsbuf), "%c", timeinfo) > 0;

        return success ? tsbuf : "(error)";
    }

    static void synchronize() {
        configTime(0, 0, "pool.ntp.org");

        do {
            delay(1000);
        } while (!Timestamp().valid());
    }

private:
    std::time_t tm;
};

#endif // TIMESTAMP_H

