#pragma once

#include <string>
#include <memory>
#include <cstring>
#include <zmq.hpp>
#include "ZmqInterface.h"

namespace zmq_component {

// Minimal SUB wrapper for event-style data.
class ZmqSubscriber {
public:
    explicit ZmqSubscriber(const std::string& connect_address = "tcp://localhost:9001",
                           const std::string& topic_filter = "") {
        try {
            context_ = std::make_unique<zmq::context_t>(1);
            socket_ = std::make_unique<zmq::socket_t>(*context_, ZMQ_SUB);
            socket_->connect(connect_address);
            socket_->set(zmq::sockopt::subscribe, topic_filter);
        } catch (const zmq::error_t& e) {
            throw ZmqCommunicationError(e.what());
        }
    }

    // Blocking receive. Returns full raw message (e.g., "topic|payload").
    std::string recv() {
        zmq::message_t msg;
        socket_->recv(msg, zmq::recv_flags::none);
        return {static_cast<char*>(msg.data()), msg.size()};
    }

    static void splitTopic(const std::string& raw, std::string& topic, std::string& payload) {
        auto pos = raw.find('|');
        if (pos == std::string::npos) {
            topic.clear();
            payload = raw;
            return;
        }
        topic = raw.substr(0, pos);
        payload = raw.substr(pos + 1);
    }

private:
    std::unique_ptr<zmq::context_t> context_;
    std::unique_ptr<zmq::socket_t> socket_;
};

} // namespace zmq_component
