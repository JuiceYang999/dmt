#pragma once

#include <string>
#include <memory>
#include <cstring>
#include <zmq.hpp>
#include "ZmqInterface.h"

namespace zmq_component {

// Minimal PUB wrapper for event-style data (pressure, state, telemetry).
// Note: ZeroMQ sockets are not thread-safe; use one socket per thread.
class ZmqPublisher {
public:
    explicit ZmqPublisher(const std::string& bind_address = "tcp://*:9001") {
        try {
            context_ = std::make_unique<zmq::context_t>(1);
            socket_ = std::make_unique<zmq::socket_t>(*context_, ZMQ_PUB);
            socket_->bind(bind_address);
        } catch (const zmq::error_t& e) {
            throw ZmqCommunicationError(e.what());
        }
    }

    void publish(const std::string& message) {
        zmq::message_t msg(message.size());
        memcpy(msg.data(), message.data(), message.size());
        socket_->send(msg, zmq::send_flags::none);
    }

    void publish(const std::string& topic, const std::string& payload) {
        // Topic prefix enables subscriber-side filtering.
        publish(topic + "|" + payload);
    }

private:
    std::unique_ptr<zmq::context_t> context_;
    std::unique_ptr<zmq::socket_t> socket_;
};

} // namespace zmq_component
