#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <mutex>
#include <regex>
#include <locale>
#include <codecvt>
#include <cctype>

#include "ZmqServer.h"
#include "ZmqClient.h"

// =============================
// JSON-Lite (just enough for申报书框架)
// =============================
namespace json_lite {

static std::string trim(std::string s) {
    auto not_space = [](int ch) { return !std::isspace(ch); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

static std::string getString(const std::string& j, const std::string& key, const std::string& def = "") {
    // Finds: "key" : "value"
    std::string k = "\"" + key + "\"";
    auto pos = j.find(k);
    if (pos == std::string::npos) return def;
    pos = j.find(':', pos);
    if (pos == std::string::npos) return def;
    pos = j.find('"', pos);
    if (pos == std::string::npos) return def;
    auto end = j.find('"', pos + 1);
    if (end == std::string::npos) return def;
    return j.substr(pos + 1, end - (pos + 1));
}

static int getInt(const std::string& j, const std::string& key, int def = 0) {
    // Finds: "key" : 123
    std::string k = "\"" + key + "\"";
    auto pos = j.find(k);
    if (pos == std::string::npos) return def;
    pos = j.find(':', pos);
    if (pos == std::string::npos) return def;
    pos++;
    while (pos < j.size() && (j[pos] == ' ' || j[pos] == '\t')) pos++;
    size_t end = pos;
    while (end < j.size() && (std::isdigit((unsigned char)j[end]) || j[end] == '-')) end++;
    try {
        return std::stoi(j.substr(pos, end - pos));
    } catch (...) {
        return def;
    }
}

static std::string makeAck(const std::string& request_id) {
    return std::string("{\"ok\":true,\"request_id\":\"") + request_id + "\"}";
}

} // namespace json_lite

// =============================
// RAG-Lite: keyword retrieval over local file
// =============================
class RagLite {
public:
    explicit RagLite(const std::string& kb_path) : kb_path_(kb_path) {}

    std::string retrieve(const std::string& query, int top_k = 3) {
        std::ifstream in(kb_path_);
        if (!in.is_open()) return "[RAG] KB missing";

        std::vector<std::string> lines;
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty()) lines.push_back(line);
        }

        // naive scoring: count occurrences
        struct Hit { int score; std::string text; };
        std::vector<Hit> hits;
        for (auto& l : lines) {
            int score = 0;
            if (l.find(query) != std::string::npos) score += 5;
            // tokenize query by spaces (minimal)
            std::istringstream iss(query);
            std::string tok;
            while (iss >> tok) {
                if (tok.size() >= 2 && l.find(tok) != std::string::npos) score += 1;
            }
            if (score > 0) hits.push_back({score, l});
        }

        std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b){ return a.score > b.score; });
        std::ostringstream out;
        out << "[RAG context]\n";
        for (int i = 0; i < (int)hits.size() && i < top_k; ++i) {
            out << "- " << hits[i].text << "\n";
        }
        if (hits.empty()) out << "- (no local hits)\n";
        return out.str();
    }

private:
    std::string kb_path_;
};

// =============================
// Skills-Lite
// =============================
static bool starts_with(const std::string& s, const std::string& prefix) {
    return s.rfind(prefix, 0) == 0;
}

static std::string run_skill_if_any(const std::string& text) {
    // Minimal skill router (rule-based). In a real build, this can be LLM function-calling.
    if (starts_with(text, "导航到") || starts_with(text, "导航去")) {
        auto dest = text.substr(std::string("导航到").size());
        if (dest.empty()) dest = "(未识别目的地)";
        return "[SKILL:NAVIGATE] 已为你规划到" + dest + "的路线。";
    }
    if (text.find("空调") != std::string::npos) {
        return "[SKILL:HVAC_SET] 已调整空调设置（示例）。";
    }
    if (text.find("紧急") != std::string::npos || text.find("救命") != std::string::npos) {
        return "[SKILL:CALL_SOS] 已触发紧急呼叫流程（示例）。";
    }
    return "";
}

// =============================
// Segmentation for pseudo-streaming TTS
// =============================
static std::vector<std::string> split_for_tts(const std::string& utf8) {
    static const std::wregex wide_delimiter(L"([。！？；：\n]|\\?\\s|\\!\\s|\\；|\\，|\\、|\\|)");
    std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;

    std::wstring w = converter.from_bytes(utf8);
    std::wsregex_iterator it(w.begin(), w.end(), wide_delimiter);
    std::wsregex_iterator end;

    std::vector<std::string> segs;
    size_t last = 0;
    while (it != end) {
        size_t pos = it->position();
        size_t len = it->length();
        if (pos > last) {
            auto piece = w.substr(last, pos - last);
            // trim
            while (!piece.empty() && iswspace(piece.front())) piece.erase(piece.begin());
            while (!piece.empty() && iswspace(piece.back())) piece.pop_back();
            if (!piece.empty()) segs.push_back(converter.to_bytes(piece));
        }
        last = pos + len;
        ++it;
    }
    if (last < w.size()) {
        auto tail = w.substr(last);
        while (!tail.empty() && iswspace(tail.front())) tail.erase(tail.begin());
        while (!tail.empty() && iswspace(tail.back())) tail.pop_back();
        if (!tail.empty()) segs.push_back(converter.to_bytes(tail));
    }
    if (segs.empty()) segs.push_back(utf8);
    return segs;
}

int main(int argc, char** argv) {
    // Endpoints kept compatible with existing demos
    const std::string AGENT_BIND = "tcp://*:6666";         // voice -> agent
    const std::string TTS_ADDR   = "tcp://localhost:7777"; // agent -> tts

    const std::string kb_path = "llm\\test\\rag_kb.txt";   // relative to repo root

    zmq_component::ZmqServer agent_server(AGENT_BIND);
    zmq_component::ZmqClient tts_client(TTS_ADDR);
    RagLite rag(kb_path);

    std::cout << "[agent] listening on " << AGENT_BIND << "\n";

    while (true) {
        std::string req = agent_server.receive();

        // Expect a JSON-ish request:
        // {"request_id":"...","text":"...","pressure_level":2,"mode":"auto"}
        std::string request_id = json_lite::getString(req, "request_id", "req-0");
        std::string text = json_lite::getString(req, "text", "");
        int pressure_level = json_lite::getInt(req, "pressure_level", 0);

        std::cout << "[voice -> agent] text=" << text << " pressure=" << pressure_level << "\n";

        // Immediate ACK to keep voice-side REQ/REP fast (首响优化中的“先回执”)。
        agent_server.send(json_lite::makeAck(request_id));

        // Pressure-driven policy
        if (pressure_level >= 3) {
            tts_client.request("已进入紧急模式。我将立即执行安全响应，并记录关键事件。 END");
            continue;
        }

        // Skills first
        std::string skill_out = run_skill_if_any(text);
        if (!skill_out.empty()) {
            for (auto& seg : split_for_tts(skill_out)) {
                tts_client.request(seg);
            }
            tts_client.request("END");
            continue;
        }

        // RAG + LLM-stub (offline)
        // 1) quick first chunk (TTFT optimization placeholder)
        tts_client.request("好的，我在本地检索并组织回答。" );

        // 2) retrieve context
        std::string ctx = rag.retrieve(text, 4);

        // 3) LLM stub: in real build, replace with RKLLM streaming inference
        std::ostringstream answer;
        answer << "\n" << ctx << "\n";
        answer << "[Answer]\n";
        answer << "你问的是：" << text << "。\n";
        answer << "我将结合离线知识库与座舱策略给出分级响应（pressure_level=" << pressure_level << "）。\n";
        answer << "（此处为申报书用的最简框架：可替换为 DeepSeek-R1-Distill-Qwen-1.5B w4a16 RKLLM 推理输出。）";

        auto segs = split_for_tts(answer.str());
        for (size_t i = 0; i < segs.size(); ++i) {
            if (i + 1 == segs.size()) {
                tts_client.request(segs[i] + " END");
            } else {
                tts_client.request(segs[i]);
            }
        }
    }

    return 0;
}
