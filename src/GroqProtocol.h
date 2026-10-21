#pragma once

// Groq (OpenAI-compatible) chat protocol for Search's AI answers: request
// bodies, a minimal JSON reader, the SSE stream accumulator and web-search
// source extraction. Portable (std only): unit-tested off Windows; the
// WinHTTP transport lives in GroqClient.cpp.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace groq {

inline constexpr char kDefaultModel[] = "qwen/qwen3.8-27b";
// Groq's server-side browser_search runs on the GPT-OSS models only; other
// models reach it through a local web_search function backed by this model.
inline constexpr char kSearchModel[] = "openai/gpt-oss-20b";
inline constexpr char kSearchToolName[] = "web_search";

namespace json {

struct Value {
    enum class Type : uint8_t { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<Value> items;
    std::vector<std::pair<std::string, Value>> members;

    [[nodiscard]] const Value* Get(std::string_view key) const noexcept;
    // String value of `key`, or empty when absent / not a string.
    [[nodiscard]] std::string_view Str(std::string_view key) const noexcept;
    [[nodiscard]] bool IsArray() const noexcept { return type == Type::Array; }
    [[nodiscard]] bool IsObject() const noexcept { return type == Type::Object; }
    [[nodiscard]] bool IsString() const noexcept { return type == Type::String; }
};

[[nodiscard]] std::optional<Value> Parse(std::string_view text);
// Appends `utf8` as a quoted, escaped JSON string.
void AppendQuoted(std::string& out, std::string_view utf8);

}  // namespace json

[[nodiscard]] std::string WideToUtf8(std::wstring_view text);
[[nodiscard]] std::wstring Utf8ToWide(std::string_view text);
// Cuts to at most `maxBytes` without splitting a UTF-8 sequence.
[[nodiscard]] std::string TruncateUtf8(std::string_view text, size_t maxBytes);

struct Source {
    std::string title;
    std::string url;
    std::string snippet;
};

struct ToolCall {
    std::string id;
    std::string name;
    std::string arguments;
};

struct Message {
    std::string role;
    std::string content;
    std::string toolCallId;
    std::vector<ToolCall> toolCalls;
    // User images as data: (or https:) URLs; sent as a multimodal content array.
    std::vector<std::string> images;
};

// Most images a request may carry (Qwen 3.8 27B on Groq accepts 3).
inline constexpr size_t kMaxImages = 3;
[[nodiscard]] std::string Base64(std::string_view bytes);
// "data:<mime>;base64,<...>"
[[nodiscard]] std::string DataUrl(std::string_view mime, std::string_view bytes);
// Prompt used when the user attaches images without typing a question.
[[nodiscard]] std::string DefaultImagePrompt(size_t imageCount);

enum class SearchMode : uint8_t { None, Function, Builtin };

[[nodiscard]] bool HasBuiltinSearch(std::string_view model) noexcept;
[[nodiscard]] std::string SystemPrompt(std::string_view today, SearchMode search);
[[nodiscard]] std::string BuildChatRequest(std::string_view model, const std::vector<Message>& messages,
    SearchMode search, bool stream);
[[nodiscard]] std::string BuildSearchRequest(std::string_view query);
// Query argument of a web_search tool call ("" when unparsable).
[[nodiscard]] std::string SearchQueryFromArguments(std::string_view arguments);

struct SearchOutcome {
    std::string summary;
    std::vector<Source> sources;
    std::vector<std::string> images;
};

// Non-streamed browser_search reply -> summary text + sources.
[[nodiscard]] bool ParseSearchResponse(std::string_view body, SearchOutcome& outcome);
// web_search tool result handed back to the answering model. numbers[i] is
// the Sources-list number of outcome.sources[i] so citations line up.
[[nodiscard]] std::string BuildToolResult(std::string_view query, const SearchOutcome& outcome,
    const std::vector<size_t>& numbers);
// error.message of an API error body (falls back to a trimmed snippet).
[[nodiscard]] std::string ErrorMessage(std::string_view body);
// Removes <think>...</think> reasoning; an unterminated block hides the rest.
[[nodiscard]] std::string StripThinking(std::string_view content);
// Appends unless a source with the same URL is already present.
void AddSource(std::vector<Source>& sources, Source source);
// executed_tools[] (Groq built-in tools) -> sources / image URLs. Returns
// true when any of them was a search.
bool CollectExecutedTools(const json::Value& executedTools, std::vector<Source>& sources,
    std::vector<std::string>* images);

// Accumulates an SSE chat-completions stream (`data: {...}` events).
class StreamParser {
public:
    void Feed(std::string_view bytes);
    // Flushes a final line without a trailing newline.
    void Finish();

    [[nodiscard]] const std::string& Content() const noexcept { return m_content; }
    [[nodiscard]] const std::vector<ToolCall>& ToolCalls() const noexcept { return m_toolCalls; }
    [[nodiscard]] const std::vector<Source>& Sources() const noexcept { return m_sources; }
    [[nodiscard]] const std::string& FinishReason() const noexcept { return m_finishReason; }
    [[nodiscard]] const std::string& Error() const noexcept { return m_error; }
    [[nodiscard]] bool Done() const noexcept { return m_done; }
    [[nodiscard]] bool Searched() const noexcept { return m_searched; }
    [[nodiscard]] bool Reasoning() const noexcept { return m_reasoning; }

private:
    void HandleLine(std::string_view line);
    void HandleEvent(std::string_view data);

    std::string m_line;
    std::string m_content;
    std::vector<ToolCall> m_toolCalls;
    std::vector<Source> m_sources;
    std::string m_finishReason;
    std::string m_error;
    bool m_done = false;
    bool m_searched = false;
    bool m_reasoning = false;
};

}  // namespace groq
