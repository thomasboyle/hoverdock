// Unit tests for the portable file-name index (FileSearchIndex.cpp).
// Build: cmake -DDOCK_BUILD_TESTS=ON (Windows) or, anywhere:
//   c++ -std=c++20 -O2 -I src tests/FileSearchIndexTests.cpp src/FileSearchIndex.cpp
#include "FileSearchIndex.h"

#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

using namespace filesearch;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

constexpr uint64_t kRoot = 5;

std::vector<std::string> Names(const FileIndex& index, const std::vector<Hit>& hits) {
    std::vector<std::string> out;
    for (const Hit& hit : hits) {
        out.emplace_back(index.Name(hit.entry));
    }
    return out;
}

bool Contains(const std::vector<std::string>& names, const std::string& name) {
    for (const auto& n : names) {
        if (n == name) {
            return true;
        }
    }
    return false;
}

// Small tree:
//   C:\Users(100)\tom(101)\Documents(102)\Report Final.docx(200)
//   C:\Users\tom\AppData(103, hidden)\cache(104)\report.tmp(201)
//   C:\Windows(110)\System32(111)\reporter.dll(202)
//   C:\$Recycle.Bin(120)\report.docx(203)
//   C:\Program Files(130)\Mail(131)\Mail.exe(204)
//   C:\Users\tom\Desktop(105)\Mail.lnk(205)
//   orphan (parent 999) mailbox.txt(206)
FileIndex BuildSample() {
    FileIndex index;
    const uint8_t v = index.AddVolume("C:", kRoot);
    index.Add(v, 200, 102, 0, "Report Final.docx");
    index.Add(v, 100, kRoot, kFlagDirectory, "Users");
    index.Add(v, 101, 100, kFlagDirectory, "tom");
    index.Add(v, 102, 101, kFlagDirectory, "Documents");
    index.Add(v, 103, 101, kFlagDirectory | kFlagHidden, "AppData");
    index.Add(v, 104, 103, kFlagDirectory, "cache");
    index.Add(v, 201, 104, 0, "report.tmp");
    index.Add(v, 110, kRoot, kFlagDirectory, "Windows");
    index.Add(v, 111, 110, kFlagDirectory, "System32");
    index.Add(v, 202, 111, 0, "reporter.dll");
    index.Add(v, 120, kRoot, kFlagDirectory | kFlagHidden | kFlagSystem, "$Recycle.Bin");
    index.Add(v, 203, 120, 0, "report.docx");
    index.Add(v, 130, kRoot, kFlagDirectory, "Program Files");
    index.Add(v, 131, 130, kFlagDirectory, "Mail");
    index.Add(v, 204, 131, 0, "Mail.exe");
    index.Add(v, 105, 101, kFlagDirectory, "Desktop");
    index.Add(v, 205, 105, 0, "Mail.lnk");
    index.Add(v, 206, 999, 0, "mailbox.txt");
    index.Add(v, 3, kRoot, 0, "$Volume");
    index.FinishVolume(v);
    index.SetZoneMarker(v, 101, Zone::UserHome);
    index.SetZoneMarker(v, 102, Zone::UserFolder);
    index.SetZoneMarker(v, 105, Zone::UserFolder);
    index.SetZoneMarker(v, 103, Zone::AppData);
    index.SetZoneMarker(v, 110, Zone::Windows);
    index.SetZoneMarker(v, 130, Zone::ProgramFiles);
    index.RecomputeZones();
    return index;
}

void TestFolding() {
    CHECK(FoldUtf8("HeLLo") == "hello");
    CHECK(FoldUtf8("\xC3\x89T\xC3\x89") == "\xC3\xA9t\xC3\xA9");             // ÉTÉ -> été
    CHECK(FoldUtf8("\xD0\x9C\xD0\x98\xD0\xA0") == "\xD0\xBC\xD0\xB8\xD1\x80");  // МИР -> мир
    CHECK(FoldUtf8("\xCE\xA3") == "\xCF\x83");                               // Σ -> σ
    CHECK(FoldUtf8("\xE6\x97\xA5\xE6\x9C\xAC") == "\xE6\x97\xA5\xE6\x9C\xAC");  // 日本 unchanged
    std::string utf8;
    const char16_t text[] = u"R\u00E9sum\u00E9 \U0001F600";
    AppendUtf8(utf8, text, sizeof(text) / sizeof(char16_t) - 1);
    CHECK(utf8 == "R\xC3\xA9sum\xC3\xA9 \xF0\x9F\x98\x80");
}

void TestPathsAndZones() {
    FileIndex index = BuildSample();
    const uint32_t report = index.Find(0, 200);
    CHECK(report != FileIndex::kNone);
    CHECK(index.Path(report) == "C:\\Users\\tom\\Documents\\Report Final.docx");
    CHECK(index.ZoneOf(report) == Zone::UserFolder);
    CHECK(index.ZoneOf(index.Find(0, 201)) == Zone::AppData);
    CHECK(index.ZoneOf(index.Find(0, 202)) == Zone::Windows);
    CHECK(index.ZoneOf(index.Find(0, 203)) == Zone::Excluded);
    CHECK(index.ZoneOf(index.Find(0, 206)) == Zone::Excluded);  // orphan
    CHECK(index.ZoneOf(index.Find(0, 3)) == Zone::Excluded);    // NTFS metafile
}

void TestSearchBasics() {
    FileIndex index = BuildSample();
    auto names = Names(index, index.Search("report", 10));
    CHECK(Contains(names, "Report Final.docx"));
    CHECK(Contains(names, "report.tmp"));
    CHECK(Contains(names, "reporter.dll"));
    CHECK(!Contains(names, "report.docx"));  // recycle bin excluded
    CHECK(!names.empty() && names[0] == "Report Final.docx");  // user folder ranks first

    names = Names(index, index.Search("REPORT final", 10));
    CHECK(names.size() == 1 && names[0] == "Report Final.docx");
    names = Names(index, index.Search("final report", 10));
    CHECK(names.size() == 1);
    names = Names(index, index.Search("  rep   fin  ", 10));
    CHECK(names.size() == 1);
    names = Names(index, index.Search("nomatch", 10));
    CHECK(names.empty());
    names = Names(index, index.Search("", 10));
    CHECK(names.empty());

    // Exact app name: shortcut beats exe in Program Files, directory "Mail" also present.
    names = Names(index, index.Search("mail", 10));
    CHECK(Contains(names, "Mail.lnk") && Contains(names, "Mail.exe") && Contains(names, "Mail"));
    CHECK(!Contains(names, "mailbox.txt"));
    CHECK(!names.empty() && names[0] == "Mail.lnk");

    // Limit honoured.
    CHECK(index.Search("e", 2).size() == 2);
}

void TestUpdates() {
    FileIndex index = BuildSample();
    index.Upsert(0, 300, 102, 0, "Budget 2026.xlsx");
    auto names = Names(index, index.Search("budget", 10));
    CHECK(names.size() == 1);
    CHECK(index.Path(index.Find(0, 300)) == "C:\\Users\\tom\\Documents\\Budget 2026.xlsx");
    CHECK(index.ZoneOf(index.Find(0, 300)) == Zone::UserFolder);

    // Rename + move.
    index.Upsert(0, 300, 105, 0, "Forecast.xlsx");
    CHECK(Names(index, index.Search("budget", 10)).empty());
    names = Names(index, index.Search("forecast", 10));
    CHECK(names.size() == 1);
    CHECK(index.Path(index.Find(0, 300)) == "C:\\Users\\tom\\Desktop\\Forecast.xlsx");

    // Delete.
    index.Remove(0, 300);
    CHECK(Names(index, index.Search("forecast", 10)).empty());
    CHECK(index.Find(0, 300) == FileIndex::kNone);

    // Recreate same record number.
    index.Upsert(0, 300, 102, 0, "Again.txt");
    CHECK(Names(index, index.Search("again", 10)).size() == 1);

    // Hard-link alias: second name under another directory.
    CHECK(index.AddAlias(0, 204, 111, 0, "Mail.exe"));       // Program Files\Mail\Mail.exe also in System32
    CHECK(!index.AddAlias(0, 204, 131, 0, "Mail.exe"));      // same directory as the record: no alias
    CHECK(!index.AddAlias(0, 204, 55555, 0, "Mail.exe"));    // unknown parent
    index.RecomputeZones();
    bool sawAlias = false;
    for (const Hit& hit : index.Search("mail.exe", 10)) {
        sawAlias = sawAlias || index.Path(hit.entry) == "C:\\Windows\\System32\\Mail.exe";
    }
    CHECK(sawAlias);

    // Unknown parent: never indexed; moving out of scope removes.
    const size_t before = index.EntryCount();
    index.Upsert(0, 400, 77777, 0, "elsewhere.txt");
    CHECK(index.EntryCount() == before);
    CHECK(index.Search("elsewhere", 10).empty());
    index.Upsert(0, 300, 77777, 0, "Again.txt");
    CHECK(index.Search("again", 10).empty());
}

void TestIncremental() {
    FileIndex index;
    const uint8_t v = index.AddVolume("C:", kRoot);
    std::mt19937 rng(42);
    const char* words[] = {"alpha", "beta", "gamma", "delta", "report", "mail", "photo", "invoice"};
    for (uint64_t i = 0; i < 20000; ++i) {
        std::string name = std::string(words[rng() % 8]) + "_" + words[rng() % 8] + std::to_string(i) + ".txt";
        index.Add(v, 1000 + i, kRoot, 0, name);
    }
    index.FinishVolume(v);
    index.RecomputeZones();
    SearchState state;
    const char* sequence[] = {"r", "re", "rep", "repo", "repor", "report", "report m", "report ma", "report mai"};
    for (const char* q : sequence) {
        const auto viaState = index.Search(q, 1000000, &state);
        const auto fresh = index.Search(q, 1000000);
        bool same = viaState.size() == fresh.size();
        for (size_t i = 0; same && i < fresh.size(); ++i) {
            same = viaState[i].entry == fresh[i].entry && viaState[i].score == fresh[i].score;
        }
        CHECK(same);
    }
    CHECK(state.lastWasIncremental);
    // Backspace -> full rescan path.
    static_cast<void>(index.Search("repo", 10, &state));
    CHECK(!state.lastWasIncremental);
    // Mutation invalidates.
    index.Upsert(v, 99999, kRoot, 0, "report_new_mail.txt");
    const auto after = Names(index, index.Search("report_new", 10, &state));
    CHECK(after.size() == 1);
    CHECK(!state.lastWasIncremental);
}

std::vector<std::string> Split(const std::string& folded) {
    std::vector<std::string> parts;
    size_t p = 0;
    while (p < folded.size()) {
        size_t e = folded.find(' ', p);
        if (e == std::string::npos) {
            e = folded.size();
        }
        if (e > p) {
            parts.push_back(folded.substr(p, e - p));
        }
        p = e + 1;
    }
    return parts;
}

void TestParallelMatchesSerial() {
    FileIndex index;
    const uint8_t v = index.AddVolume("C:", kRoot);
    std::mt19937 rng(7);
    constexpr uint64_t kCount = 300000;
    for (uint64_t i = 0; i < kCount; ++i) {
        std::string name;
        const int len = 3 + static_cast<int>(rng() % 20);
        for (int c = 0; c < len; ++c) {
            name.push_back("abcdeFGHIJ .-_xyzQ0123"[rng() % 22]);
        }
        index.Add(v, 1000 + i, kRoot, (i % 5) == 0 ? kFlagDirectory : 0, name);
    }
    index.FinishVolume(v);
    index.RecomputeZones();
    for (const char* q : {"a", "ab", "fgh", "x-", "q0", "abc de", "J 1", "Fghij"}) {
        const auto serial = index.Search(q, 200, nullptr, 1);
        const auto parallel = index.Search(q, 200, nullptr, 4);
        bool same = serial.size() == parallel.size();
        for (size_t i = 0; same && i < serial.size(); ++i) {
            same = serial[i].entry == parallel[i].entry && serial[i].score == parallel[i].score;
        }
        CHECK(same);
        // Cross-check the SIMD scan against a brute-force case-insensitive find.
        SearchState state;
        static_cast<void>(index.Search(q, 1, &state));
        size_t brute = 0;
        const std::vector<std::string> parts = Split(FoldUtf8(q));
        for (uint64_t i = 0; i < kCount; ++i) {
            const std::string name = FoldUtf8(index.Name(index.Find(v, 1000 + i)));
            bool all = true;
            for (const auto& part : parts) {
                all = all && name.find(part) != std::string::npos;
            }
            brute += all ? 1 : 0;
        }
        CHECK(state.lastMatchCount == brute);
        if (state.lastMatchCount != brute) {
            std::printf("  query '%s': index=%zu brute=%zu\n", q, state.lastMatchCount, brute);
        }
    }
}

void TestUnicodeMatching() {
    FileIndex index;
    const uint8_t v = index.AddVolume("C:", kRoot);
    index.Add(v, 30, kRoot, 0, "\xC3\x89t\xC3\xA9 Photos.jpg");  // Été Photos.jpg
    index.Add(v, 31, kRoot, 0, "\xD0\x9E\xD1\x82\xD1\x87\xD1\x91\xD1\x82.pdf");  // Отчёт.pdf
    index.Add(v, 32, kRoot, 0, "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E.txt");  // 日本語.txt
    index.FinishVolume(v);
    index.RecomputeZones();
    CHECK(index.Search("\xC3\xA9t\xC3\xA9", 10).size() == 1);   // été
    CHECK(index.Search("\xC3\x89T\xC3\x89", 10).size() == 1);   // ÉTÉ
    CHECK(index.Search("\xD0\xBE\xD1\x82\xD1\x87", 10).size() == 1);  // отч
    CHECK(index.Search("\xD0\x9E\xD0\xA2\xD0\xA7", 10).size() == 1);  // ОТЧ
    CHECK(index.Search("\xE6\x9C\xAC\xE8\xAA\x9E", 10).size() == 1);  // 本語
    CHECK(index.Search("photos", 10).size() == 1);
}

void TestCompaction() {
    FileIndex index;
    const uint8_t v = index.AddVolume("C:", kRoot);
    for (uint64_t i = 0; i < 50000; ++i) {
        index.Add(v, 100 + i, kRoot, 0, std::string(200, 'a') + std::to_string(i));
    }
    index.FinishVolume(v);
    index.RecomputeZones();
    for (uint64_t i = 0; i < 50000; i += 2) {
        index.Remove(v, 100 + i);
    }
    const size_t before = index.NameBytes();
    index.CompactIfWasteful();
    CHECK(index.NameBytes() < before);
    CHECK(index.Search("a49999", 10).size() == 1);
    CHECK(index.Search("a49998", 10).empty());
    CHECK(index.Name(index.Find(v, 100 + 49999)) == std::string(200, 'a') + "49999");
    index.Upsert(v, 100 + 49999, kRoot, 0, "renamed.txt");
    CHECK(index.Search("renamed", 10).size() == 1);
}

// Characterization: the bounded top-N must equal the head of the complete
// ranking (same entries, scores and order), for every limit and thread count,
// on a tree with zones, hidden folders and varied depth.
void TestTopNIsPrefixOfFullRanking() {
    FileIndex index;
    const uint8_t v = index.AddVolume("C:", kRoot);
    std::mt19937 rng(2024);
    std::vector<uint64_t> directories{kRoot};
    const char* words[] = {"Alpha", "beta", "report", "Mail", "photo", "data", "a", "re", "notes", "x"};
    const char* exts[] = {".txt", ".exe", ".lnk", ".dll", "", ".docx"};
    constexpr uint64_t kCount = 120000;
    for (uint64_t i = 0; i < kCount; ++i) {
        const bool directory = i % 9 == 0;
        std::string name = std::string(words[rng() % 10]) + (rng() % 2 ? " " : "_") + words[rng() % 10];
        if (rng() % 3 == 0) {
            name += std::to_string(rng() % 100);
        }
        if (!directory) {
            name += exts[rng() % 6];
        }
        const uint64_t parent = directories[rng() % directories.size()];
        const uint8_t flags = (directory ? kFlagDirectory : 0) | (rng() % 40 == 0 ? kFlagHidden : 0);
        index.Add(v, 1000 + i, parent, flags, name);
        if (directory) {
            directories.push_back(1000 + i);
        }
    }
    index.FinishVolume(v);
    const Zone zones[] = {Zone::UserFolder, Zone::UserHome, Zone::ProgramFiles, Zone::AppData, Zone::Windows,
        Zone::Hidden};
    for (size_t d = 1; d < directories.size(); d += 37) {
        index.SetZoneMarker(v, directories[d], zones[d % 6]);
    }
    index.RecomputeZones();
    // A few journal-style changes so appended (renamed) slots are covered.
    for (uint64_t i = 0; i < 2000; ++i) {
        index.Upsert(v, 1000 + (i * 53) % kCount, directories[i % directories.size()],
            (i % 9 == 0) ? kFlagDirectory : 0, std::string("renamed report a ") + std::to_string(i));
    }
    for (const char* q : {"a", "re", "report", "mail a", "e", "x", "alpha beta", "zz"}) {
        const std::vector<Hit> full = index.Search(q, 10000000, nullptr, 1);
        for (const size_t limit : {1U, 7U, 50U, 400U}) {
            for (const unsigned threads : {1U, 4U}) {
                const std::vector<Hit> top = index.Search(q, limit, nullptr, threads);
                bool same = top.size() == std::min<size_t>(limit, full.size());
                for (size_t i = 0; same && i < top.size(); ++i) {
                    same = top[i].entry == full[i].entry && top[i].score == full[i].score;
                }
                CHECK(same);
                if (!same) {
                    std::printf("  query '%s' limit %zu threads %u differs\n", q, limit, threads);
                }
            }
        }
        // Typing with the narrowing cache gives the same head.
        SearchState state;
        std::string typed;
        std::vector<Hit> last;
        for (const char c : std::string(q)) {
            typed.push_back(c);
            last = index.Search(typed, 50, &state, 4);
        }
        bool same = last.size() == std::min<size_t>(50, full.size());
        for (size_t i = 0; same && i < last.size(); ++i) {
            same = last[i].entry == full[i].entry && last[i].score == full[i].score;
        }
        CHECK(same);
    }
}

}  // namespace

int main() {
    TestFolding();
    TestPathsAndZones();
    TestSearchBasics();
    TestUpdates();
    TestIncremental();
    TestParallelMatchesSerial();
    TestUnicodeMatching();
    TestCompaction();
    TestTopNIsPrefixOfFullRanking();
    if (g_failures == 0) {
        std::printf("FileSearchIndex tests: all passed\n");
        return 0;
    }
    std::printf("FileSearchIndex tests: %d failure(s)\n", g_failures);
    return 1;
}
