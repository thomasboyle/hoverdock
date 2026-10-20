// Query/build benchmark for the file-name index at real-volume scale.
//   c++ -std=c++20 -O2 -I src tests/FileSearchIndexBench.cpp src/FileSearchIndex.cpp -o bench && ./bench
// Synthetic corpus: 1,669,484 names (the MEASURED MFT record count of a
// windows-2022 runner's C:), ~27 UTF-8 bytes each, Windows-like shapes.
#include "FileSearchIndex.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace filesearch;
using Clock = std::chrono::steady_clock;

namespace {

double Ms(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

std::string MakeName(std::mt19937& rng) {
    static const char* words[] = {"microsoft", "windows", "system", "report", "invoice", "photo", "img", "data",
        "config", "setup", "update", "chrome", "edge", "visual", "studio", "code", "python", "node", "module",
        "package", "resource", "theme", "icon", "font", "driver", "service", "manifest", "backup", "notes",
        "budget", "project", "assets", "build", "release", "debug", "test", "cache", "temp", "log", "mail",
        "Outlook", "Teams", "Office", "Library", "Runtime", "Native", "Interop", "Core", "Common", "Shared"};
    static const char* exts[] = {".dll", ".exe", ".txt", ".json", ".xml", ".png", ".jpg", ".pdf", ".docx",
        ".mui", ".cat", ".manifest", ".js", ".py", ".h", ".cpp", ".lnk", ".ini", ".log", ".dat", ""};
    std::string name;
    const unsigned shape = rng() % 10;
    if (shape == 0) {
        // GUID-ish / hash names (WinSxS, caches).
        static const char hex[] = "0123456789abcdef";
        for (int i = 0; i < 32; ++i) {
            name.push_back(hex[rng() % 16]);
            if (i == 7 || i == 11 || i == 15 || i == 19) name.push_back('-');
        }
    } else {
        const int parts = 1 + static_cast<int>(rng() % 3);
        for (int p = 0; p < parts; ++p) {
            if (p > 0) name.push_back("._- "[rng() % 4]);
            name += words[rng() % (sizeof(words) / sizeof(words[0]))];
        }
        if (rng() % 3 == 0) name += std::to_string(rng() % 1000);
    }
    name += exts[rng() % (sizeof(exts) / sizeof(exts[0]))];
    return name;
}

struct Stat {
    double median;
    double minimum;
    double maximum;
};

template <typename F>
Stat Time(F&& f, int runs = 9) {
    f();  // warm-up
    std::vector<double> samples;
    for (int i = 0; i < runs; ++i) {
        const auto t = Clock::now();
        f();
        samples.push_back(Ms(t));
    }
    std::sort(samples.begin(), samples.end());
    return {samples[samples.size() / 2], samples.front(), samples.back()};
}

}  // namespace

int main(int argc, char** argv) {
    const size_t count = argc > 1 ? static_cast<size_t>(std::stoul(argv[1])) : 1669484;
    std::mt19937 rng(1234);
    std::vector<std::string> names;
    names.reserve(count);
    size_t bytes = 0;
    for (size_t i = 0; i < count; ++i) {
        names.push_back(MakeName(rng));
        bytes += names.back().size();
    }
    std::printf("corpus: %zu names, %.1f MB, avg %.1f B\n", count, bytes / 1048576.0,
        static_cast<double>(bytes) / static_cast<double>(count));

    FileIndex index;
    const auto buildStart = Clock::now();
    const uint8_t v = index.AddVolume("C:", 5);
    index.Reserve(count, bytes);
    // Tree: ~1 directory per 12 entries, parents appear after children
    // (as in MFT order) for a third of them.
    for (size_t i = 0; i < count; ++i) {
        const uint64_t id = 100 + i;
        const uint64_t parent = i < 12 ? 5 : 100 + (i / 12) * 12 + ((i % 3) == 0 ? 0 : 0);
        index.Add(v, id, parent == id ? 5 : parent, (i % 12) == 0 ? kFlagDirectory : 0, names[i]);
    }
    index.FinishVolume(v);
    index.RecomputeZones();
    const double buildMs = Ms(buildStart);
    std::printf("build: %.1f ms, index memory %.1f MB (names %.1f MB)\n", buildMs,
        index.MemoryBytes() / 1048576.0, index.NameBytes() / 1048576.0);

    const unsigned hw = std::max(1U, std::thread::hardware_concurrency());
    const unsigned threads = std::clamp(hw / 2, 1U, 4U);
    const char* queries[] = {"a", "re", "mail", "report", "chrome.exe", "visual studio", "zzqx", "outlook 42"};
    std::printf("%-16s %10s %12s %12s %12s\n", "query", "matches", "1T med ms", "NT med ms", "NT max ms");
    for (const char* q : queries) {
        SearchState state;
        static_cast<void>(index.Search(q, 50, &state));
        const Stat one = Time([&] { static_cast<void>(index.Search(q, 50, nullptr, 1)); });
        const Stat many = Time([&] { static_cast<void>(index.Search(q, 50, nullptr, threads)); });
        std::printf("%-16s %10zu %12.2f %12.2f %12.2f\n", q, state.lastMatchCount, one.median, many.median,
            many.maximum);
    }

    // Typing "invoice" one key at a time with the incremental cache.
    const Stat typing = Time([&] {
        SearchState state;
        std::string q;
        for (char c : std::string("invoice")) {
            q.push_back(c);
            static_cast<void>(index.Search(q, 50, &state, threads));
        }
    });
    std::printf("type 'invoice' (7 keystrokes, %u threads): total median %.2f ms, max %.2f ms\n", threads,
        typing.median, typing.maximum);
    std::printf("threads used for NT: %u\n", threads);
    return 0;
}
