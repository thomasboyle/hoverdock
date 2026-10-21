// GroqProtocol unit tests (portable; also built on Windows with -DDOCK_BUILD_TESTS=ON).

#include "GroqProtocol.h"

#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

#define EXPECT(condition)                                                              \
    do {                                                                               \
        if (!(condition)) {                                                            \
            std::fprintf(stderr, "%s:%d: EXPECT failed: %s\n", __FILE__, __LINE__, #condition); \
            ++g_failures;                                                              \
        }                                                                              \
    } while (false)

void TestJson() {
    const auto value = groq::json::Parse(
        R"( {"a":[1,2.5,-3e2,true,false,null],"s":"x\"\\\/\n\u00e9\ud83d\ude00","o":{"k":"v"}} )");
    EXPECT(value.has_value());
    if (!value.has_value()) {
        return;
    }
    const groq::json::Value* a = value->Get("a");
    EXPECT(a != nullptr && a->IsArray() && a->items.size() == 6 && a->items[2].number == -300.0);
    EXPECT(value->Str("s") == "x\"\\/\n\xC3\xA9\xF0\x9F\x98\x80");
    EXPECT(value->Get("o")->Str("k") == "v");
    EXPECT(!groq::json::Parse("{\"a\":}").has_value());
    EXPECT(!groq::json::Parse("[1,2").has_value());
    std::string deep(200, '[');
    EXPECT(!groq::json::Parse(deep).has_value());

    std::string quoted;
    groq::json::AppendQuoted(quoted, std::string("q\"\\\n\x01", 5));
    EXPECT(quoted == "\"q\\\"\\\\\\n\\u0001\"");
}

void TestUtf8() {
    const std::wstring wide = L"caf\u00E9 \U0001F600 \u4E2D";
    const std::string utf8 = groq::WideToUtf8(wide);
    EXPECT(utf8 == "caf\xC3\xA9 \xF0\x9F\x98\x80 \xE4\xB8\xAD");
    EXPECT(groq::Utf8ToWide(utf8) == wide);
    EXPECT(groq::Utf8ToWide("\xC3") == L"\uFFFD");
    EXPECT(groq::Utf8ToWide("a\xC0\x80" "b") == L"a\uFFFD\uFFFD" L"b");
    EXPECT(groq::TruncateUtf8("ab\xC3\xA9", 3) == "ab\xE2\x80\xA6");
}

void TestRequest() {
    std::vector<groq::Message> messages;
    messages.push_back({"system", groq::SystemPrompt("2026-10-21", groq::SearchMode::Function), {}, {}});
    messages.push_back({"user", "What's \"new\"?", {}, {}});
    groq::Message assistant{"assistant", "", {}, {{"call_1", "web_search", R"({"query":"news"})"}}};
    messages.push_back(assistant);
    messages.push_back({"tool", "{}", "call_1", {}});
    const std::string body =
        groq::BuildChatRequest(groq::kDefaultModel, messages, groq::SearchMode::Function, true);
    const auto parsed = groq::json::Parse(body);
    EXPECT(parsed.has_value());
    if (parsed.has_value()) {
        EXPECT(parsed->Str("model") == "qwen/qwen3.8-27b");
        EXPECT(parsed->Get("stream")->boolean);
        EXPECT(parsed->Get("messages")->items.size() == 4);
        EXPECT(parsed->Get("messages")->items[2].Get("tool_calls")->items[0].Get("function")->Str("name") ==
            "web_search");
        EXPECT(parsed->Get("messages")->items[2].Get("content")->type == groq::json::Value::Type::Null);
        EXPECT(parsed->Get("messages")->items[3].Str("tool_call_id") == "call_1");
        EXPECT(parsed->Get("tools")->items[0].Get("function")->Str("name") == "web_search");
        EXPECT(parsed->Get("temperature")->number == 0.8);
        EXPECT(parsed->Get("top_p")->number == 0.95);
        EXPECT(parsed->Str("reasoning_effort") == "medium");
        EXPECT(parsed->Str("reasoning_format") == "parsed");
    }
    // Assistant history drops reasoning; content that was only thinking becomes null.
    std::vector<groq::Message> history = {
        {"assistant", "<think>secret plan</think>Final answer", {}, {}, {}},
        {"assistant", "<think>only thinking</think>", {}, {{"c2", "web_search", "{}"}}, {}},
        {"user", "<think>user text is untouched</think>", {}, {}, {}},
    };
    const auto stripped = groq::json::Parse(
        groq::BuildChatRequest(groq::kDefaultModel, history, groq::SearchMode::None, true));
    EXPECT(stripped.has_value());
    if (stripped.has_value()) {
        const auto& items = stripped->Get("messages")->items;
        EXPECT(items[0].Str("content") == "Final answer");
        EXPECT(items[1].Get("content")->type == groq::json::Value::Type::Null);
        EXPECT(items[2].Str("content") == "<think>user text is untouched</think>");
    }
    const auto other = groq::json::Parse(
        groq::BuildChatRequest("llama-3.3-70b-versatile", history, groq::SearchMode::None, true));
    EXPECT(other.has_value() && other->Get("reasoning_effort") == nullptr && other->Get("top_p") == nullptr &&
        other->Get("temperature")->number == 0.8);
    const auto builtin = groq::json::Parse(
        groq::BuildChatRequest("openai/gpt-oss-120b", messages, groq::SearchMode::Builtin, false));
    EXPECT(builtin.has_value() && builtin->Get("tools")->items[0].Str("type") == "browser_search" &&
        builtin->Str("reasoning_effort") == "medium" && builtin->Get("reasoning_format") == nullptr);
    const auto search = groq::json::Parse(groq::BuildSearchRequest("weather \"today\""));
    EXPECT(search.has_value() && search->Str("model") == groq::kSearchModel && search->Str("tool_choice") == "required");
    EXPECT(groq::SearchQueryFromArguments(R"({"query":"a b"})") == "a b");
    EXPECT(groq::SearchQueryFromArguments("not json").empty());
    EXPECT(groq::HasBuiltinSearch("openai/gpt-oss-20b") && !groq::HasBuiltinSearch(groq::kDefaultModel));
}

void TestStream() {
    const std::string stream =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\",\"content\":\"\"}}]}\n\n"
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Hel\"}}]}\n\n"
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"lo \\u00e9\"}}]}\r\n\r\n"
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"c1\",\"type\":"
        "\"function\",\"function\":{\"name\":\"web_search\",\"arguments\":\"{\\\"qu\"}}]}}]}\n\n"
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":"
        "\"ery\\\":\\\"x\\\"}\"}}]},\"finish_reason\":\"tool_calls\"}]}\n\n"
        "data: [DONE]\n\n";
    // Feed in awkward slices to exercise line reassembly.
    for (size_t slice : {1U, 3U, 7U, 64U, 4096U}) {
        groq::StreamParser parser;
        for (size_t i = 0; i < stream.size(); i += slice) {
            parser.Feed(std::string_view(stream).substr(i, slice));
        }
        parser.Finish();
        EXPECT(parser.Content() == "Hello \xC3\xA9");
        EXPECT(parser.Done());
        EXPECT(parser.FinishReason() == "tool_calls");
        EXPECT(parser.ToolCalls().size() == 1 && parser.ToolCalls()[0].id == "c1" &&
            parser.ToolCalls()[0].name == "web_search" && parser.ToolCalls()[0].arguments == R"({"query":"x"})");
    }

    groq::StreamParser builtin;
    builtin.Feed(
        "data: {\"choices\":[{\"delta\":{\"executed_tools\":[{\"index\":0,\"type\":\"browser_search\",\"arguments\":"
        "\"{}\",\"search_results\":{\"results\":[{\"title\":\"T\",\"url\":\"https://a.example\",\"content\":\"c\"},"
        "{\"title\":\"Dup\",\"url\":\"https://a.example\"},{\"title\":\"Bad\",\"url\":\"file:///x\"}]}}]}}]}\n");
    EXPECT(builtin.Searched());
    EXPECT(builtin.Sources().size() == 1 && builtin.Sources()[0].title == "T");

    groq::StreamParser error;
    error.Feed("data: {\"error\":{\"message\":\"rate limited\"}}\n");
    EXPECT(error.Error() == "rate limited");
    groq::StreamParser plainError;
    plainError.Feed("{\"error\":{\"message\":\"Invalid API Key\",\"type\":\"invalid_request_error\"}}");
    plainError.Finish();
    EXPECT(plainError.Error() == "Invalid API Key");
}

void TestSearchResponse() {
    const std::string body =
        R"({"choices":[{"message":{"role":"assistant","content":"- Fact )"
        "\xE3\x80\x90" "1\xE2\x80\xA0L1-L2" "\xE3\x80\x91"
        R"( done","executed_tools":[{"index":0,"type":"browser_search","arguments":"{}",)"
        R"("search_results":{"results":[{"title":"One","url":"https://one.example","content":"snippet"}],)"
        R"("images":["https://img.example/a.png","javascript:x"]},)"
        R"("browser_results":[{"title":"Two","url":"https://two.example","content":"page"}]}]}}]})";
    groq::SearchOutcome outcome;
    EXPECT(groq::ParseSearchResponse(body, outcome));
    EXPECT(outcome.summary == "- Fact  done");
    EXPECT(outcome.sources.size() == 2 && outcome.sources[1].url == "https://two.example");
    EXPECT(outcome.images.size() == 1);
    const auto result = groq::json::Parse(groq::BuildToolResult("q", outcome, {3, 1}));
    EXPECT(result.has_value() && result->Get("results")->items[1].Get("n")->number == 1.0);
    EXPECT(groq::ErrorMessage(R"({"error":{"message":"Invalid API Key"}})") == "Invalid API Key");
    EXPECT(groq::ErrorMessage("oops\nbad") == "oops bad");
}

void TestImages() {
    EXPECT(groq::Base64("") == "");
    EXPECT(groq::Base64("f") == "Zg==");
    EXPECT(groq::Base64("fo") == "Zm8=");
    EXPECT(groq::Base64("foo") == "Zm9v");
    EXPECT(groq::Base64(std::string("\xFF\x00\x10\x80", 4)) == "/wAQgA==");
    EXPECT(groq::DataUrl("image/jpeg", "foob") == "data:image/jpeg;base64,Zm9vYg==");
    groq::Message user{"user", "What is this?", {}, {}, {"data:image/jpeg;base64,AAAA", "a", "b", "dropped"}};
    const auto body = groq::json::Parse(groq::BuildChatRequest(groq::kDefaultModel, {user}, groq::SearchMode::None, true));
    EXPECT(body.has_value());
    if (body.has_value()) {
        const groq::json::Value* content = body->Get("messages")->items[0].Get("content");
        EXPECT(content != nullptr && content->IsArray() && content->items.size() == 1 + groq::kMaxImages);
        EXPECT(content->items[0].Str("type") == "text" && content->items[0].Str("text") == "What is this?");
        EXPECT(content->items[1].Get("image_url")->Str("url") == "data:image/jpeg;base64,AAAA");
    }
}

void TestThinking() {
    EXPECT(groq::StripThinking("<think>plan</think>\n\nAnswer") == "Answer");
    EXPECT(groq::StripThinking("<think>still going").empty());
    EXPECT(groq::StripThinking("A <think>x</think>B") == "A B");
}

}  // namespace

int main() {
    TestJson();
    TestUtf8();
    TestRequest();
    TestStream();
    TestSearchResponse();
    TestThinking();
    TestImages();
    if (g_failures == 0) {
        std::printf("GroqProtocolTests: all passed\n");
        return 0;
    }
    std::fprintf(stderr, "GroqProtocolTests: %d failure(s)\n", g_failures);
    return 1;
}
