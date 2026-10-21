// AI answer pipeline benchmark (portable). Frozen before the /optimise pass on
// the AI search feature; see .system-analysis/measurements.md.
//
//   W1 streaming: an answer arrives as SSE chunks; every ~50 ms of stream the
//      worker converts the content (StripThinking + UTF-8 -> UTF-16) and the UI
//      re-parses the Markdown and re-highlights code blocks (what
//      AiAnswerView::SetMarkdown + Layout do before DirectWrite).
//   W2 request: chat request JSON for a question with 3 image data URLs.
//
// Reports median / min / max wall time over 9 runs (after 2 warm-ups), the
// split per stage, heap allocations, and the "each byte once" bound.

#include "AiMarkdown.h"
#include "GroqProtocol.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

namespace {
std::atomic<size_t> g_allocations{0};
}

void* operator new(size_t size) {
    ++g_allocations;
    if (void* p = std::malloc(size == 0 ? 1 : size)) {
        return p;
    }
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept {
    std::free(p);
}
void operator delete(void* p, size_t) noexcept {
    std::free(p);
}

namespace {

using Clock = std::chrono::steady_clock;

double Ms(Clock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
}

// A realistic answer: headings, styled paragraphs with links and inline code,
// nested lists, a table, a quote and a 30-line code block, repeated.
std::string Answer(size_t targetBytes) {
    const std::string section =
        "## Setting up the project\n\n"
        "To get started, install **Python 3.12** and create a *virtual environment* with `python -m venv .venv`. "
        "See the [official docs](https://docs.python.org/3/library/venv.html) for details, or "
        "https://packaging.python.org/en/latest/ for packaging guidance.\n\n"
        "- Activate it with `source .venv/bin/activate`\n"
        "- Install dependencies: `pip install -r requirements.txt`\n"
        "  - Pin versions for **reproducible** builds\n"
        "  - Use ~~old~~ new resolver\n"
        "1. Run the tests\n"
        "2. Check coverage\n\n"
        "| Tool | Purpose | Speed |\n"
        "|:-----|:--------|------:|\n"
        "| ruff | lint | fast |\n"
        "| mypy | types | medium |\n"
        "| pytest | tests | varies |\n\n"
        "> **Note:** keep secrets out of the repo, e.g. use `.env` files and [dotenv](https://pypi.org/project/python-dotenv/).\n\n"
        "```python\n"
        "import asyncio\n"
        "from dataclasses import dataclass\n\n"
        "@dataclass\n"
        "class Job:\n"
        "    name: str\n"
        "    retries: int = 3  # default retry budget\n\n"
        "async def run(job: Job) -> bool:\n"
        "    for attempt in range(job.retries):\n"
        "        try:\n"
        "            await asyncio.sleep(0.1 * attempt)\n"
        "            print(f\"running {job.name} attempt {attempt}\")\n"
        "            return True\n"
        "        except Exception as error:  # noqa: BLE001\n"
        "            print('failed', error)\n"
        "    return False\n\n"
        "if __name__ == \"__main__\":\n"
        "    asyncio.run(run(Job(\"build\")))\n"
        "```\n\n"
        "That's it: the job retries with a linear back-off and reports each attempt [1](https://example.com/a).\n\n";
    std::string out;
    while (out.size() < targetBytes) {
        out += section;
    }
    return out;
}

// SSE stream of ~4-byte content deltas (Groq streams roughly one token per event).
std::string Stream(const std::string& answer) {
    std::string out;
    for (size_t i = 0; i < answer.size(); i += 4) {
        out += R"(data: {"id":"chatcmpl-x","object":"chat.completion.chunk","model":"qwen/qwen3.8-27b","choices":[{"index":0,"delta":{"content":)";
        groq::json::AppendQuoted(out, std::string_view(answer).substr(i, 4));
        out += "},\"finish_reason\":null}]}\n\n";
    }
    out += "data: [DONE]\n\n";
    return out;
}

struct Split {
    double total = 0;
    double feed = 0;
    double convert = 0;
    double parse = 0;
    double highlight = 0;
    size_t allocations = 0;
    size_t publishes = 0;
    // DirectWrite layout work model (AiAnswerView::Layout runs on Windows only):
    // blocks laid out when every block is rebuilt per update, versus only blocks
    // whose content differs from the previous update.
    size_t layoutBlocksAll = 0;
    size_t layoutBlocksChanged = 0;
};

bool SameInline(const aimd::InlineText& a, const aimd::InlineText& b) {
    if (a.text != b.text || a.spans.size() != b.spans.size()) {
        return false;
    }
    for (size_t i = 0; i < a.spans.size(); ++i) {
        if (a.spans[i].start != b.spans[i].start || a.spans[i].length != b.spans[i].length ||
            a.spans[i].flags != b.spans[i].flags || a.spans[i].url != b.spans[i].url) {
            return false;
        }
    }
    return true;
}

bool SameBlockModel(const aimd::Block& a, const aimd::Block& b) {
    if (a.kind != b.kind || a.level != b.level || a.quote != b.quote || a.marker != b.marker || a.code != b.code ||
        a.language != b.language || a.url != b.url || a.align != b.align || !SameInline(a.text, b.text) ||
        a.rows.size() != b.rows.size()) {
        return false;
    }
    for (size_t r = 0; r < a.rows.size(); ++r) {
        if (a.rows[r].size() != b.rows[r].size()) {
            return false;
        }
        for (size_t c = 0; c < a.rows[r].size(); ++c) {
            if (!SameInline(a.rows[r][c], b.rows[r][c])) {
                return false;
            }
        }
    }
    return true;
}

// One streamed answer: 1 KB network reads; publish every `publishBytes` of
// content (50 ms at ~1800 chars/s = 90 bytes).
Split StreamOnce(const std::string& stream, size_t publishBytes) {
    Split split;
    const size_t allocationsBefore = g_allocations.load();
    const auto start = Clock::now();
    groq::StreamParser parser;
    size_t published = 0;
    std::wstring shown;
    std::vector<aimd::Block> previous;
    size_t sink = 0;
    auto publish = [&]() {
        auto t0 = Clock::now();
        std::wstring markdown = groq::Utf8ToWide(groq::StripThinking(parser.Content()));
        auto t1 = Clock::now();
        if (markdown != shown) {
            shown = markdown;
            std::vector<aimd::Block> blocks = aimd::Parse(shown);
            auto t2 = Clock::now();
            for (size_t b = 0; b < blocks.size(); ++b) {
                ++split.layoutBlocksAll;
                if (b >= previous.size() || !SameBlockModel(blocks[b], previous[b])) {
                    ++split.layoutBlocksChanged;
                }
            }
            for (const aimd::Block& block : blocks) {
                if (block.kind == aimd::BlockKind::Code) {
                    sink += aimd::HighlightCode(block.code, block.language).size();
                }
            }
            auto t3 = Clock::now();
            split.parse += Ms(t2 - t1);
            split.highlight += Ms(t3 - t2);
            sink += blocks.size();
            previous = std::move(blocks);
        }
        split.convert += Ms(t1 - t0);
        published = parser.Content().size();
        ++split.publishes;
    };
    for (size_t i = 0; i < stream.size(); i += 1024) {
        auto t0 = Clock::now();
        parser.Feed(std::string_view(stream).substr(i, 1024));
        split.feed += Ms(Clock::now() - t0);
        if (parser.Content().size() - published >= publishBytes) {
            publish();
        }
    }
    parser.Finish();
    publish();
    split.total = Ms(Clock::now() - start);
    split.allocations = g_allocations.load() - allocationsBefore;
    if (sink == 0) {
        std::printf("unexpected\n");
    }
    return split;
}

// Speed-of-light: every byte handled once (one feed, one convert, one parse).
double BoundOnce(const std::string& stream) {
    const auto start = Clock::now();
    groq::StreamParser parser;
    parser.Feed(stream);
    parser.Finish();
    const std::wstring markdown = groq::Utf8ToWide(groq::StripThinking(parser.Content()));
    const std::vector<aimd::Block> blocks = aimd::Parse(markdown);
    size_t sink = blocks.size();
    for (const aimd::Block& block : blocks) {
        if (block.kind == aimd::BlockKind::Code) {
            sink += aimd::HighlightCode(block.code, block.language).size();
        }
    }
    const double ms = Ms(Clock::now() - start);
    return sink == 0 ? -1.0 : ms;
}

double RequestOnce(const std::vector<std::string>& images, size_t& bytes, size_t& allocations) {
    const size_t allocationsBefore = g_allocations.load();
    const auto start = Clock::now();
    std::vector<groq::Message> messages;
    messages.push_back({"system", groq::SystemPrompt("Tuesday, 21 October 2026", groq::SearchMode::Function), {}, {}, {}});
    messages.push_back({"user", "What is in these screenshots?", {}, {}, images});
    const std::string body = groq::BuildChatRequest(groq::kDefaultModel, messages, groq::SearchMode::Function, true);
    const double ms = Ms(Clock::now() - start);
    bytes = body.size();
    allocations = g_allocations.load() - allocationsBefore;
    return ms;
}

template <typename F>
void Report(const char* name, int runs, F&& once) {
    std::vector<Split> samples;
    for (int i = 0; i < 2; ++i) {
        once();
    }
    for (int i = 0; i < runs; ++i) {
        samples.push_back(once());
    }
    std::sort(samples.begin(), samples.end(), [](const Split& a, const Split& b) { return a.total < b.total; });
    const Split& median = samples[samples.size() / 2];
    std::printf("%-22s median %8.3f ms  [min %8.3f, max %8.3f]  feed %7.3f convert %7.3f parse %8.3f highlight %7.3f"
                "  publishes %zu  allocs %zu  layout blocks all %zu / changed %zu\n",
        name, median.total, samples.front().total, samples.back().total, median.feed, median.convert, median.parse,
        median.highlight, median.publishes, median.allocations, median.layoutBlocksAll, median.layoutBlocksChanged);
}

}  // namespace

int main() {
    constexpr int kRuns = 9;
    for (const size_t size : {static_cast<size_t>(6000), static_cast<size_t>(24000)}) {
        const std::string answer = Answer(size);
        const std::string stream = Stream(answer);
        char name[64];
        std::snprintf(name, sizeof(name), "W1 stream %zu KB", answer.size() / 1000);
        Report(name, kRuns, [&]() { return StreamOnce(stream, 90); });
        std::vector<double> bounds;
        for (int i = 0; i < kRuns; ++i) {
            bounds.push_back(BoundOnce(stream));
        }
        std::sort(bounds.begin(), bounds.end());
        std::printf("%-22s median %8.3f ms  (each byte once; stream %zu bytes)\n", "  bound", bounds[kRuns / 2],
            stream.size());
    }

    // W2: three ~300 KB JPEG data URLs (typical 1568 px screenshots).
    std::string jpeg(300000, '\0');
    for (size_t i = 0; i < jpeg.size(); ++i) {
        jpeg[i] = static_cast<char>((i * 2654435761U) >> 13U);
    }
    std::vector<double> encodeTimes;
    std::vector<double> requestTimes;
    size_t bytes = 0;
    size_t allocations = 0;
    size_t encodeAllocations = 0;
    std::vector<std::string> images;
    for (int i = 0; i < kRuns + 2; ++i) {
        const size_t before = g_allocations.load();
        const auto start = Clock::now();
        images.assign(3, groq::DataUrl("image/jpeg", jpeg));
        encodeTimes.push_back(Ms(Clock::now() - start));
        encodeAllocations = g_allocations.load() - before;
        requestTimes.push_back(RequestOnce(images, bytes, allocations));
    }
    encodeTimes.erase(encodeTimes.begin(), encodeTimes.begin() + 2);
    requestTimes.erase(requestTimes.begin(), requestTimes.begin() + 2);
    std::sort(encodeTimes.begin(), encodeTimes.end());
    std::sort(requestTimes.begin(), requestTimes.end());
    std::printf("W2 data URLs (3x300KB) median %8.3f ms  [min %8.3f, max %8.3f]  allocs %zu\n", encodeTimes[kRuns / 2],
        encodeTimes.front(), encodeTimes.back(), encodeAllocations);
    std::printf("W2 request JSON        median %8.3f ms  [min %8.3f, max %8.3f]  body %zu bytes  allocs %zu\n",
        requestTimes[kRuns / 2], requestTimes.front(), requestTimes.back(), bytes, allocations);
    return 0;
}
