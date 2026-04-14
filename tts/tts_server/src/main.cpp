#include "TTSModel.h"
#include "MessageQueue.h"
#include "AudioPlayer.h"
#include "TextProcessor.h"
#include "Utils.h"
#include "ZmqServer.h"

#include <thread>
#include <iostream>
#include <atomic>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <cstdint>

static zmq_component::ZmqServer tts_text_server("tcp://*:7777");
// status_server moved to a dedicated thread (ZeroMQ sockets are not thread-safe)
// Playback-end notification (voice -> tts)
static std::atomic<uint64_t> g_play_end_seq{0};
static std::mutex g_play_mtx;
static std::condition_variable g_play_cv;

static void status_worker() {
    zmq_component::ZmqServer status_server("tcp://*:6677");
    uint64_t last_seq = 0;

    while (true) {
        // Voice sends any request to wait for the next playback end.
        std::string req = status_server.receive();
        (void)req;

        std::unique_lock<std::mutex> lk(g_play_mtx);
        g_play_cv.wait(lk, [&]{ return g_play_end_seq.load() > last_seq; });
        last_seq = g_play_end_seq.load();
        lk.unlock();

        status_server.send("[tts -> voice]play end success");
    }
}


void synthesis_worker(DoubleMessageQueue &queue, TTSModel &model) {
    // utils::set_realtime_priority(pthread_self(), 99);

    while (true) {
        std::string text = queue.pop_text();
        if (text.empty()) break;

        bool is_last = false;

        if (text.find("END") != std::string::npos) {
            is_last = true;
            size_t end_pos = text.find("END");
            text = text.substr(0, end_pos);
        }

        int32_t audio_len = 0;
        if (!text.empty()) {
            std::cout << "[TTS infer] Inferring text: " << text << std::endl;
            int16_t* wavData = model.infer(text, audio_len);
            
            if (wavData && audio_len > 0) {
                auto audio_data = std::make_unique<int16_t[]>(audio_len);
                memcpy(audio_data.get(), wavData, audio_len * sizeof(int16_t));
                queue.push_audio(std::move(audio_data), audio_len, is_last);
                model.free_data(wavData);
            }
        } else {
            auto empty_audio = std::make_unique<int16_t[]>(0);
            queue.push_audio(std::move(empty_audio), 0, is_last);
        }
    }
}

void playback_worker(DoubleMessageQueue &queue, AudioPlayer &player) {
    while (true) {
        auto msg = queue.pop_audio();
        if (msg.data == nullptr) break;
        
        player.play(msg.data.get(), msg.length * sizeof(int16_t), 1.0f);
        
        if (msg.is_last) {
            {
                std::lock_guard<std::mutex> lk(g_play_mtx);
                g_play_end_seq.fetch_add(1);
            }
            g_play_cv.notify_all();
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <model_path>" << std::endl;
        return 1;
    }

    try {
    
        TTSModel model(argv[1]);
        AudioPlayer player;
        DoubleMessageQueue queue;

        std::thread status_thread(status_worker);

        std::thread synthesis_thread(synthesis_worker, std::ref(queue), std::ref(model));
        std::thread playback_thread(playback_worker, std::ref(queue), std::ref(player));

        while (true) {
            // status is served by status_worker thread
            if (false) {
                // std::string req = status_server.receive();
                // std::cout << "[voice -> tts] received: " << req << std::endl;
            }
            // first_msg = false;

            std::string text = tts_text_server.receive();
            tts_text_server.send("OK");
            std::cout << "[llm -> tts] received: " << text << std::endl;

            if (!text.empty() && text.find("<think>") == std::string::npos) {
              
    
                queue.push_text(text);
            }
        }

        // 清理
        queue.stop();
        synthesis_thread.join();
        playback_thread.join();
    } catch (const std::exception &e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}