// Search engine benchmark (frozen harness for /optimise; change => re-baseline).
//   c++ -std=c++20 -O2 -I src tests/FileSearchIndexBench.cpp src/FileSearchIndex.cpp -o bench && ./bench
// Synthetic corpus: 1,669,484 names (the MEASURED MFT record count of a
// windows-2022 runner's C:), Windows-like name shapes, a directory tree with
// realistic depth, and zone markers (Windows / Program Files / AppData /
// user folders) so ranking and pruning see real-disk distributions. Names are
// ingested through AddUtf16, the path the MFT reader uses.
#include "FileSearchIndex.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <Windows.h>
#include <Psapi.h>
#else
#include <sys/resource.h>
#endif

using namespace filesearch;
using Clock = std::chrono::steady_clock;

namespace {

double Ms(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

double PeakRssMb() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters));
    return static_cast<double>(counters.PeakWorkingSetSize) / 1048576.0;
#else
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
#if defined(__APPLE__)
    return static_cast<double>(usage.ru_maxrss) / 1048576.0;
#else
    return static_cast<double>(usage.ru_maxrss) / 1024.0;
#endif
#endif
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
    if (rng() % 10 == 0) {
        static const char hex[] = "0123456789abcdef";
        for (int i = 0; i < 32; ++i) {
            name.push_back(hex[rng() % 16]);
            if (i == 7 || i == 11 || i == 15 || i == 19) {
                name.push_back('-');
            }
        }
    } else {
        const int parts = 1 + static_cast<int>(rng() % 3);
        for (int p = 0; p < parts; ++p) {
            if (p > 0) {
                name.push_back("._- "[rng() % 4]);
            }
            name += words[rng() % (sizeof(words) / sizeof(words[0]))];
        }
        if (rng() % 3 == 0) {
            name += std::to_string(rng() % 1000);
        }
    }
    name += exts[rng() % (sizeof(exts) / sizeof(exts[0]))];
    return name;
}

struct Corpus {
    std::vector<std::u16string> names;
    std::vector<uint64_t> parents;
    std::vector<uint8_t> flags;
    std::vector<std::pair<uint64_t, Zone>> markers;
    size_t utf8Bytes = 0;
};

constexpr uint64_t kRootId = 5;
constexpr uint64_t kFirstId = 100;

Corpus MakeCorpus(size_t count) {
    Corpus corpus;
    std::mt19937 rng(1234);
    corpus.names.reserve(count);
    std::vector<uint64_t> directories{kRootId};
    for (size_t i = 0; i < count; ++i) {
        const std::string name = MakeName(rng);
        corpus.utf8Bytes += name.size();
        corpus.names.emplace_back(name.begin(), name.end());
        const bool directory = i % 12 == 0;
        // Parent: a recent directory (deep trees) or a random earlier one.
        uint64_t parent = directories[rng() % 4 == 0 ? rng() % directories.size()
                                                    : directories.size() - 1 - rng() % std::min<size_t>(8, directories.size())];
        corpus.parents.push_back(parent);
        corpus.flags.push_back(directory ? kFlagDirectory : 0);
        if (directory) {
            directories.push_back(kFirstId + i);
            if (directories.size() > 2 && directories.size() % 97 == 0) {
                static const Zone zones[] = {Zone::Windows, Zone::Windows, Zone::Windows, Zone::ProgramFiles,
                    Zone::AppData, Zone::AppData, Zone::UserFolder, Zone::UserHome, Zone::Hidden};
                corpus.markers.emplace_back(kFirstId + i, zones[rng() % 9]);
            }
        }
    }
    return corpus;
}

struct Built {
    FileIndex index;
    double ingestMs = 0;
    double finishMs = 0;
    double zonesMs = 0;
};

void Build(const Corpus& corpus, const std::vector<size_t>& arrival, bool reserve, Built& out) {
    auto t = Clock::now();
    const uint8_t v = out.index.AddVolume("C:", kRootId);
    if (reserve) {
        out.index.Reserve(corpus.names.size(), corpus.utf8Bytes);
    }
    for (const size_t i : arrival) {
        out.index.AddUtf16(v, kFirstId + i, corpus.parents[i], corpus.flags[i], corpus.names[i].data(),
            corpus.names[i].size());
    }
    out.ingestMs = Ms(t);
    t = Clock::now();
    out.index.FinishVolume(v);
    out.finishMs = Ms(t);
    t = Clock::now();
    for (const auto& [id, zone] : corpus.markers) {
        out.index.SetZoneMarker(v, id, zone);
    }
    out.index.RecomputeZones();
    out.zonesMs = Ms(t);
}

struct Stat {
    double median;
    double p95;
    double maximum;
};

template <typename F>
Stat Time(F&& f, int runs = 11) {
    f();
    std::vector<double> samples;
    for (int i = 0; i < runs; ++i) {
        const auto t = Clock::now();
        f();
        samples.push_back(Ms(t));
    }
    std::sort(samples.begin(), samples.end());
    return {samples[samples.size() / 2], samples[std::min(samples.size() - 1, samples.size() * 95 / 100)],
        samples.back()};
}

}  // namespace

int main(int argc, char** argv) {
    const size_t count = argc > 1 ? static_cast<size_t>(std::stoul(argv[1])) : 1669484;
    const Corpus corpus = MakeCorpus(count);
    std::vector<size_t> arrival(count);
    for (size_t i = 0; i < count; ++i) {
        arrival[i] = i;
    }
    std::shuffle(arrival.begin(), arrival.end(), std::mt19937(99));
    std::printf("corpus: %zu names, %.1f MB UTF-8, %zu zone markers\n", count,
        static_cast<double>(corpus.utf8Bytes) / 1048576.0, corpus.markers.size());

    // MFT order is record order, not tree order: parents arrive after children.
    // W2: build (both sizing strategies), median of 5.
    for (const bool reserve : {false, true}) {
        std::vector<double> total;
        Built last;
        for (int run = 0; run < 5; ++run) {
            Built built;
            const auto t = Clock::now();
            Build(corpus, arrival, reserve, built);
            built.index.ShrinkToFit();
            total.push_back(Ms(t));
            if (run == 4) {
                std::printf("build reserve=%d: ingest %.1f / finish %.1f / zones %.1f ms", reserve ? 1 : 0,
                    built.ingestMs, built.finishMs, built.zonesMs);
                std::printf(", memory %.1f MB\n", static_cast<double>(built.index.MemoryBytes()) / 1048576.0);
            }
        }
        std::sort(total.begin(), total.end());
        std::printf("build reserve=%d total: median %.1f ms (min %.1f, max %.1f)\n", reserve ? 1 : 0, total[2],
            total.front(), total.back());
    }

    Built built;
    Build(corpus, arrival, false, built);
    built.index.ShrinkToFit();
    FileIndex& index = built.index;

    // W1: queries.
    const unsigned hw = std::max(1U, std::thread::hardware_concurrency());
    const unsigned threads = std::clamp(hw / 2, 1U, 4U);
    const char* queries[] = {"a", "e", "re", "mail", "report", "chrome.exe", "visual studio", "zzqx", "outlook 42"};
    std::printf("%-14s %8s %9s %9s %9s %9s\n", "query", "matches", "1T p50", "1T p95", "NT p50", "NT p95");
    for (const char* q : queries) {
        SearchState state;
        static_cast<void>(index.Search(q, 50, &state, 1));
        const Stat one = Time([&] { static_cast<void>(index.Search(q, 50, nullptr, 1)); });
        const Stat many = Time([&] { static_cast<void>(index.Search(q, 50, nullptr, threads)); });
        std::printf("%-14s %8zu %9.2f %9.2f %9.2f %9.2f\n", q, state.lastMatchCount, one.median, one.p95, many.median,
            many.p95);
    }
    for (const char* word : {"invoice", "powershell"}) {
        const Stat typing = Time([&] {
            SearchState state;
            std::string q;
            for (const char c : std::string(word)) {
                q.push_back(c);
                static_cast<void>(index.Search(q, 50, &state, threads));
            }
        });
        std::printf("type '%s' (%u threads): total p50 %.2f ms, max %.2f ms\n", word, threads, typing.median,
            typing.maximum);
    }

    // W3: journal-style updates (create + rename + delete), 100k operations.
    {
        const auto t = Clock::now();
        for (uint64_t i = 0; i < 100000; ++i) {
            const uint64_t id = kFirstId + count + i;
            index.Upsert(0, id, corpus.parents[i % count], 0, "journal_new_file.txt");
        }
        for (uint64_t i = 0; i < 100000; ++i) {
            index.Upsert(0, kFirstId + count + i, corpus.parents[(i * 7) % count], 0, "journal_renamed.txt");
        }
        for (uint64_t i = 0; i < 100000; ++i) {
            index.Remove(0, kFirstId + count + i);
        }
        index.CompactIfWasteful();
        std::printf("journal: 300k updates %.1f ms (%.0f ns/op)\n", Ms(t), Ms(t) * 1e6 / 300000.0);
    }
    std::printf("threads used for NT: %u\npeak RSS: %.1f MB\n", threads, PeakRssMb());
    return 0;
}
