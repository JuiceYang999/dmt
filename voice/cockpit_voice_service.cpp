#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstdlib>

#include "ZmqClient.h"
#include "ZmqSubscriber.h"

// Voice service (skeleton)
// - Real implementation: Sherpa-ONNX Zipformer streaming ASR + VAD.
// - Here: simulate ASR final results by reading stdin lines.
// - Pressure-driven state switching + TTS playback gating to prevent ASR self-trigger.

enum class AsrPowerState {
    DeepSleep = 0,
    LightWake = 1,
    Active    = 2,
    Emergency = 3
};

static AsrPowerState level_to_state(int level) {
    if (level <= 0) return AsrPowerState::DeepSleep;
    if (level == 1) return AsrPowerState::LightWake;
    if (level == 2) return AsrPowerState::Active;
    return AsrPowerState::Emergency;
}

int main(int argc, char** argv) {
    const std::string PRESSURE_SUB = "tcp://localhost:9001";
    const std::string AGENT_ADDR   = "tcp://localhost:6666"; // agent_server
    const std::string TTS_STATUS   = "tcp://localhost:6677"; // tts_server status

    zmq_component::ZmqSubscriber pressure_sub(PRESSURE_SUB, "pressure");
    zmq_component::ZmqClient agent_client(AGENT_ADDR);
    zmq_component::ZmqClient tts_status_client(TTS_STATUS);

    std::atomic<int> pressure_level{0};
    std::atomic<bool> tts_playing{false};

    std::thread pressure_thread([&](){
        while (true) {
            std::string raw = pressure_sub.recv();
            std::string topic, payload;
            zmq_component::ZmqSubscriber::splitTopic(raw, topic, payload);

            // payload contains: {"level":N,...}
            auto pos = payload.find("\"level\"");
            if (pos != std::string::npos) {
                auto colon = payload.find(':', pos);
                if (colon != std::string::npos) {
                    int lvl = std::atoi(payload.c_str() + colon + 1);
                    pressure_level.store(lvl);
                }
            }
        }
    });
    pressure_thread.detach();

    std::cout << "[voice] pressure_sub=" << PRESSURE_SUB
              << " agent=" << AGENT_ADDR
              << " tts_status=" << TTS_STATUS << "\n";
    std::cout << "[voice] (stub) Type a line as ASR final text, press Enter.\n";

    long long req_seq = 0;

    while (true) {
        int lvl = pressure_level.load();
        AsrPowerState state = level_to_state(lvl);

        if (state == AsrPowerState::DeepSleep) {
            // Deep sleep: minimal CPU, no ASR. Just wait.
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        if (tts_playing.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        // In real system:
        // - state==LightWake: enable low-power VAD with 0.4s endpointing
        // - state==Active: full streaming ASR
        // - state==Emergency: interrupt & prioritize

        std::string asr_text;
        if (!std::getline(std::cin, asr_text)) break;
        if (asr_text.empty()) continue;

        // Compose request
        std::string request_id = std::string("v-") + std::to_string(++req_seq);
        std::string req = std::string("{\"request_id\":\"") + request_id +
                          "\",\"text\":\"" + asr_text +
                          "\",\"pressure_level\":" + std::to_string(lvl) +
                          ",\"vad_silence_ms\":400,\"mode\":\"auto\"}";

        try {
            // Fast REQ/REP to agent (agent will ACK quickly)
            auto ack = agent_client.request(req);
            std::cout << "[agent ack] " << ack << "\n";

            // Gate ASR until TTS playback ends (prevents self-trigger)
            tts_playing.store(true);
            auto status = tts_status_client.request("WAIT_PLAY_END");
            std::cout << "[tts status] " << status << "\n";
            tts_playing.store(false);

        } catch (const std::exception& e) {
            std::cerr << "[voice] error: " << e.what() << "\n";
            tts_playing.store(false);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }

    return 0;
}
