#include <iostream>
#include <string>
#include <chrono>
#include <thread>
#include <random>

#include "ZmqPublisher.h"

// Pressure sensor service (skeleton)
// - Real implementation: read from ADC/I2C/SPI/UART driver, do calibration & filtering.
// - Here: generate plausible pressure values and levels.

static int pressure_to_level(float p) {
    // Example thresholds for申报书
    if (p < 0.15f) return 0; // no pressure
    if (p < 0.45f) return 1; // light
    if (p < 0.75f) return 2; // medium
    return 3;                // high/emergency
}

int main(int argc, char** argv) {
    const std::string BIND = "tcp://*:9001";
    zmq_component::ZmqPublisher pub(BIND);

    std::mt19937 rng((unsigned)std::chrono::high_resolution_clock::now().time_since_epoch().count());
    std::uniform_real_distribution<float> dist(0.0f, 1.0f);

    std::cout << "[pressure] publishing on " << BIND << "\n";

    while (true) {
        float p = dist(rng);
        int level = pressure_to_level(p);

        auto ts = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count();

        // JSON-ish payload for simplicity
        std::string payload = std::string("{\"ts_ms\":") + std::to_string(ts) +
                              ",\"pressure\":" + std::to_string(p) +
                              ",\"level\":" + std::to_string(level) +
                              ",\"seat\":1}";

        pub.publish("pressure", payload);

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    return 0;
}
