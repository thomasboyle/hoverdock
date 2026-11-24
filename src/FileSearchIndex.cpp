#include "FileSearchIndex.h"

#include <algorithm>
#include <atomic>
#include <climits>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

#if defined(_M_X64) || defined(_M_AMD64) || defined(__SSE2__)
#define FILESEARCH_SSE2 1
#include <emmintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#elif defined(__ARM_NEON) || defined(_M_ARM64)
#define FILESEARCH_NEON 1
#include <arm_neon.h>
#endif

#if defined(_MSC_VER) && !defined(__clang__)
#define FILESEARCH_PREFETCH(address) _mm_prefetch(reinterpret_cast<const char*>(address), _MM_HINT_T0)
#elif defined(__GNUC__) || defined(__clang__)
#define FILESEARCH_PREFETCH(address) __builtin_prefetch(address)
#else
#define FILESEARCH_PREFETCH(address) static_cast<void>(address)
#endif

namespace filesearch {
namespace {

// Trailing zero bytes kept after the last name so 16-byte SIMD loads at
// (position + anchor offset) never read past the allocation.
constexpr size_t kPad = 64;
// Second anchor byte is chosen within this many bytes of the token start.
constexpr size_t kMaxAnchorOffset = 32;
// NTFS metadata files ($MFT, $LogFile, $Extend, ...) live in records < 24.
constexpr uint64_t kFirstUserRecord = 24;
constexpr uint8_t kFlagMeta = 1U << 5;
constexpr uint8_t kFlagOrphan = 1U << 6;

struct Decoded {
    uint32_t cp;
    uint32_t length;
};

// Invalid sequences decode as (0x80000000 | byte, 1) so they only ever match
// the identical byte.
Decoded DecodeUtf8(const unsigned char* s) noexcept {
    const unsigned char b0 = s[0];
    if (b0 < 0x80U) {
        return {b0, 1};
    }
    if (b0 >= 0xC2U && b0 <= 0xDFU && (s[1] & 0xC0U) == 0x80U) {
        return {((b0 & 0x1FU) << 6) | (s[1] & 0x3FU), 2};
    }
    if (b0 >= 0xE0U && b0 <= 0xEFU && (s[1] & 0xC0U) == 0x80U && (s[2] & 0xC0U) == 0x80U) {
        return {((b0 & 0x0FU) << 12) | ((s[1] & 0x3FU) << 6) | (s[2] & 0x3FU), 3};
    }
    if (b0 >= 0xF0U && b0 <= 0xF4U && (s[1] & 0xC0U) == 0x80U && (s[2] & 0xC0U) == 0x80U &&
        (s[3] & 0xC0U) == 0x80U) {
        return {((b0 & 0x07U) << 18) | ((s[1] & 0x3FU) << 12) | ((s[2] & 0x3FU) << 6) | (s[3] & 0x3FU), 4};
    }
    return {0x80000000U | b0, 1};
}

void EncodeUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80U) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800U) {
        out.push_back(static_cast<char>(0xC0U | (cp >> 6)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    } else if (cp < 0x10000U) {
        out.push_back(static_cast<char>(0xE0U | (cp >> 12)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 6) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    } else {
        out.push_back(static_cast<char>(0xF0U | (cp >> 18)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 12) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 6) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    }
}

// True when some other code point folds to/from `cp` (its bytes cannot be
// compared exactly). Covers every range FoldCodePoint touches.
bool IsCaseVariant(uint32_t cp) noexcept {
    return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || (cp >= 0xC0U && cp <= 0x24FU) ||
        (cp >= 0x370U && cp <= 0x4FFU);
}

inline unsigned char AsciiFold(unsigned char b) noexcept {
    return (b >= 'A' && b <= 'Z') ? static_cast<unsigned char>(b | 0x20U) : b;
}

// Case-insensitive match of folded `token` at `s`. `s` is always followed by
// a '\0' terminator, which never equals a token byte, so no bound is needed.
inline bool MatchAt(const unsigned char* s, const unsigned char* token, size_t length) noexcept {
    size_t i = 0;
    while (i < length) {
        const unsigned char t = token[i];
        if (t < 0x80U) {
            if (AsciiFold(s[i]) != t) {
                return false;
            }
            ++i;
            continue;
        }
        const Decoded want = DecodeUtf8(token + i);
        const Decoded have = DecodeUtf8(s + i);
        if (want.length != have.length) {
            return false;
        }
        const uint32_t folded = (have.cp & 0x80000000U) != 0 ? have.cp : FoldCodePoint(have.cp);
        if (folded != want.cp) {
            return false;
        }
        i += want.length;
    }
    return true;
}

struct Token {
    const unsigned char* bytes = nullptr;
    size_t length = 0;
    // SIMD prefilter: (byte | mask) == value at two offsets in the token.
    bool anchored = false;
    size_t offset1 = 0;
    size_t offset2 = 0;
    unsigned char value1 = 0;
    unsigned char value2 = 0;
    unsigned char mask1 = 0;
    unsigned char mask2 = 0;
};

Token MakeToken(std::string_view folded) {
    Token token;
    token.bytes = reinterpret_cast<const unsigned char*>(folded.data());
    token.length = folded.size();
    bool haveFirst = false;
    size_t i = 0;
    while (i < folded.size()) {
        const Decoded d = DecodeUtf8(token.bytes + i);
        const bool letter = d.cp < 0x80U && d.cp >= 'a' && d.cp <= 'z';
        const bool exact = (d.cp & 0x80000000U) != 0 || !IsCaseVariant(d.cp);
        if ((letter || exact) && i <= kMaxAnchorOffset) {
            // Any byte of an exact code point works; use its first byte.
            const unsigned char value = token.bytes[i];
            const unsigned char mask = letter ? 0x20U : 0x00U;
            if (!haveFirst) {
                token.offset1 = i;
                token.value1 = value;
                token.mask1 = mask;
                haveFirst = true;
            }
            token.offset2 = i;
            token.value2 = value;
            token.mask2 = mask;
        }
        i += d.length;
    }
    token.anchored = haveFirst;
    return token;
}

// Lowest position of `token` in `name`, starting at `from`; npos when absent.
size_t FindIn(std::string_view name, const Token& token, size_t from = 0) noexcept {
    if (token.length > name.size()) {
        return std::string_view::npos;
    }
    const auto* s = reinterpret_cast<const unsigned char*>(name.data());
    const size_t last = name.size() - token.length;
    if (token.anchored) {
        const size_t o1 = token.offset1;
        const size_t o2 = token.offset2;
        const unsigned char m1 = token.mask1;
        const unsigned char m2 = token.mask2;
        const unsigned char v1 = token.value1;
        const unsigned char v2 = token.value2;
        for (size_t i = from; i <= last; ++i) {
            if (((s[i + o1] | m1) == v1) && ((s[i + o2] | m2) == v2) &&
                MatchAt(s + i, token.bytes, token.length)) {
                return i;
            }
        }
        return std::string_view::npos;
    }
    for (size_t i = from; i <= last; ++i) {
        if (MatchAt(s + i, token.bytes, token.length)) {
            return i;
        }
    }
    return std::string_view::npos;
}

bool IsWordStart(std::string_view name, size_t pos) noexcept {
    if (pos == 0) {
        return true;
    }
    const auto prev = static_cast<unsigned char>(name[pos - 1]);
    const auto cur = static_cast<unsigned char>(name[pos]);
    switch (prev) {
    case ' ': case '-': case '_': case '.': case '(': case ')': case '[': case ']':
    case '+': case ',': case '&': case '\'': case '@': case '#': case '~':
        return true;
    default:
        break;
    }
    // camelCase / PascalCase boundary and letter <-> digit boundary.
    if (prev >= 'a' && prev <= 'z' && cur >= 'A' && cur <= 'Z') {
        return true;
    }
    const bool prevDigit = prev >= '0' && prev <= '9';
    const bool curDigit = cur >= '0' && cur <= '9';
    return prevDigit != curDigit && prev < 0x80U && cur < 0x80U &&
        (((prev | 0x20U) >= 'a' && (prev | 0x20U) <= 'z') || ((cur | 0x20U) >= 'a' && (cur | 0x20U) <= 'z'));
}

bool EqualsFolded(std::string_view name, std::string_view folded) noexcept {
    if (name.size() != folded.size()) {
        return false;
    }
    return MatchAt(reinterpret_cast<const unsigned char*>(name.data()),
        reinterpret_cast<const unsigned char*>(folded.data()), folded.size());
}

// 16-bit hash of the case-folded UTF-8 bytes of text[0, length).
uint16_t FoldedKey(std::string_view text) noexcept {
    uint32_t hash = 2166136261U;
    const auto* s = reinterpret_cast<const unsigned char*>(text.data());
    size_t i = 0;
    while (i < text.size()) {
        if (s[i] < 0x80U) {
            hash = (hash ^ AsciiFold(s[i])) * 16777619U;
            ++i;
            continue;
        }
        unsigned char bytes[4];
        size_t count = 0;
        const size_t left = text.size() - i;
        const Decoded d = left >= 4 ? DecodeUtf8(s + i) : Decoded{0x80000000U | s[i], 1};
        if ((d.cp & 0x80000000U) != 0 || d.length > left) {
            bytes[count++] = s[i];
            i += 1;
        } else {
            std::string folded;
            EncodeUtf8(folded, FoldCodePoint(d.cp));
            for (const char c : folded) {
                bytes[count++] = static_cast<unsigned char>(c);
            }
            i += d.length;
        }
        for (size_t b = 0; b < count; ++b) {
            hash = (hash ^ bytes[b]) * 16777619U;
        }
    }
    return static_cast<uint16_t>(hash ^ (hash >> 16));
}

// Key of the part of a name that can earn the stem bonus: up to the last '.'
// (or the whole name when it has none, which also covers whole-name matches
// of dot-free queries).
uint16_t StemKey(std::string_view name) noexcept {
    const size_t dot = name.rfind('.');
    const std::string_view stem = dot != std::string_view::npos && dot > 0 ? name.substr(0, dot) : name;
    uint32_t hash = 2166136261U;
    const auto* s = reinterpret_cast<const unsigned char*>(stem.data());
    const size_t n = stem.size();
    size_t i = 0;
    while (i < n) {
        const unsigned char b = s[i];
        if (b < 0x80U) {
            hash = (hash ^ AsciiFold(b)) * 16777619U;
            ++i;
            continue;
        }
        const size_t left = n - i;
        const Decoded d = left >= 4 ? DecodeUtf8(s + i) : Decoded{0x80000000U | b, 1};
        if ((d.cp & 0x80000000U) != 0 || d.length > left) {
            hash = (hash ^ b) * 16777619U;
            ++i;
        } else {
            const uint32_t folded = FoldCodePoint(d.cp);
            if (folded < 0x80U) {
                hash = (hash ^ folded) * 16777619U;
            } else if (folded < 0x800U) {
                hash = (hash ^ (0xC0U | (folded >> 6))) * 16777619U;
                hash = (hash ^ (0x80U | (folded & 0x3FU))) * 16777619U;
            } else if (folded < 0x10000U) {
                hash = (hash ^ (0xE0U | (folded >> 12))) * 16777619U;
                hash = (hash ^ (0x80U | ((folded >> 6) & 0x3FU))) * 16777619U;
                hash = (hash ^ (0x80U | (folded & 0x3FU))) * 16777619U;
            } else {
                hash = (hash ^ (0xF0U | (folded >> 18))) * 16777619U;
                hash = (hash ^ (0x80U | ((folded >> 12) & 0x3FU))) * 16777619U;
                hash = (hash ^ (0x80U | ((folded >> 6) & 0x3FU))) * 16777619U;
                hash = (hash ^ (0x80U | (folded & 0x3FU))) * 16777619U;
            }
            i += d.length;
        }
    }
    return static_cast<uint16_t>(hash ^ (hash >> 16));
}

// Static (query-independent) score sentinel: dead or excluded slot.
constexpr int16_t kNoBase = INT16_MIN;
// Best possible query-dependent part per token (match at position 0).
constexpr int kTokenCeiling = 600;
// Best possible whole-name bonus.
constexpr int kWholeNameCeiling = 600;

inline int KindBonus(std::string_view name, uint8_t flags) noexcept {
    if ((flags & kFlagDirectory) != 0) {
        return 40;
    }
    const size_t n = name.size();
    if (n > 4 && name[n - 4] == '.') {
        const unsigned char c1 = AsciiFold(static_cast<unsigned char>(name[n - 3]));
        const unsigned char c2 = AsciiFold(static_cast<unsigned char>(name[n - 2]));
        const unsigned char c3 = AsciiFold(static_cast<unsigned char>(name[n - 1]));
        if (c1 == 'l' && c2 == 'n' && c3 == 'k') {
            return 120;
        }
        if (c1 == 'e' && c2 == 'x' && c3 == 'e') {
            return 60;
        }
    }
    return 0;
}

int ZoneBonus(Zone zone) noexcept {
    switch (zone) {
    case Zone::UserFolder: return 260;
    case Zone::UserHome: return 150;
    case Zone::Normal: return 0;
    case Zone::ProgramFiles: return -60;
    case Zone::AppData: return -260;
    case Zone::Windows: return -380;
    case Zone::Hidden: return -420;
    case Zone::Excluded: return -100000;
    }
    return 0;
}

struct Query {
    std::string folded;            // tokens joined by one space
    std::vector<Token> tokens;
    size_t anchor = 0;             // index of the longest token (scanned)
};

bool ParseQuery(std::string_view text, Query& query) {
    const std::string folded = FoldUtf8(text);
    query.folded.clear();
    query.tokens.clear();
    query.anchor = 0;
    size_t i = 0;
    while (i < folded.size()) {
        while (i < folded.size() && (folded[i] == ' ' || folded[i] == '\t')) {
            ++i;
        }
        const size_t start = i;
        while (i < folded.size() && folded[i] != ' ' && folded[i] != '\t') {
            ++i;
        }
        if (i > start) {
            if (!query.folded.empty()) {
                query.folded.push_back(' ');
            }
            query.folded.append(folded, start, i - start);
        }
    }
    // Views into query.folded (stable from here on).
    size_t pos = 0;
    while (pos < query.folded.size()) {
        size_t end = query.folded.find(' ', pos);
        if (end == std::string::npos) {
            end = query.folded.size();
        }
        const std::string_view part(query.folded.data() + pos, end - pos);
        query.tokens.push_back(MakeToken(part));
        if (part.size() > query.tokens[query.anchor].length) {
            query.anchor = query.tokens.size() - 1;
        }
        pos = end + 1;
    }
    return !query.tokens.empty();
}

bool Better(const Hit& a, const Hit& b) noexcept {
    if (a.score != b.score) {
        return a.score > b.score;
    }
    return a.entry < b.entry;
}

// Bounded best-N collector (min-heap on Better).
class TopN {
public:
    explicit TopN(size_t limit) : m_limit(limit) { m_heap.reserve(limit + 1); }
    void Offer(const Hit& hit) {
        if (m_limit == 0) {
            return;
        }
        if (m_heap.size() < m_limit) {
            m_heap.push_back(hit);
            std::push_heap(m_heap.begin(), m_heap.end(), Better);
        } else if (Better(hit, m_heap.front())) {
            std::pop_heap(m_heap.begin(), m_heap.end(), Better);
            m_heap.back() = hit;
            std::push_heap(m_heap.begin(), m_heap.end(), Better);
        }
    }
    [[nodiscard]] int32_t Threshold() const noexcept {
        return m_heap.size() < m_limit ? INT32_MIN : m_heap.front().score;
    }
    std::vector<Hit>& Items() noexcept { return m_heap; }

private:
    size_t m_limit;
    std::vector<Hit> m_heap;
};

}  // namespace

uint32_t FoldCodePoint(uint32_t cp) noexcept {
    if (cp < 0x80U) {
        return (cp >= 'A' && cp <= 'Z') ? cp + 32U : cp;
    }
    if (cp >= 0xC0U && cp <= 0xDEU && cp != 0xD7U) {
        return cp + 32U;
    }
    if (cp >= 0x100U && cp <= 0x17FU) {
        if (cp <= 0x137U && cp != 0x130U && cp != 0x131U && (cp & 1U) == 0) {
            return cp + 1U;
        }
        if (cp >= 0x139U && cp <= 0x148U && (cp & 1U) == 1) {
            return cp + 1U;
        }
        if (cp >= 0x14AU && cp <= 0x177U && (cp & 1U) == 0) {
            return cp + 1U;
        }
        if (cp == 0x178U) {
            return 0xFFU;
        }
        if (cp >= 0x179U && cp <= 0x17EU && (cp & 1U) == 1) {
            return cp + 1U;
        }
        return cp;
    }
    if (cp >= 0x391U && cp <= 0x3A9U && cp != 0x3A2U) {
        return cp + 32U;
    }
    if (cp == 0x386U) {
        return 0x3ACU;
    }
    if (cp >= 0x388U && cp <= 0x38AU) {
        return cp + 37U;
    }
    if (cp == 0x38CU) {
        return 0x3CCU;
    }
    if (cp == 0x38EU || cp == 0x38FU) {
        return cp + 63U;
    }
    if (cp >= 0x410U && cp <= 0x42FU) {
        return cp + 32U;
    }
    if (cp >= 0x400U && cp <= 0x40FU) {
        return cp + 80U;
    }
    return cp;
}

std::string FoldUtf8(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    std::string padded(text);
    padded.append(4, '\0');
    const auto* s = reinterpret_cast<const unsigned char*>(padded.data());
    size_t i = 0;
    while (i < text.size()) {
        if (s[i] < 0x80U) {
            out.push_back(static_cast<char>(AsciiFold(s[i])));
            ++i;
            continue;
        }
        const Decoded d = DecodeUtf8(s + i);
        if ((d.cp & 0x80000000U) != 0 || i + d.length > text.size()) {
            out.push_back(static_cast<char>(s[i]));
            ++i;
            continue;
        }
        EncodeUtf8(out, FoldCodePoint(d.cp));
        i += d.length;
    }
    return out;
}

void AppendUtf8(std::string& out, const char16_t* text, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        uint32_t c = text[i];
        if (c < 0x80U) {
            out.push_back(static_cast<char>(c));
            continue;
        }
        if (c >= 0xD800U && c <= 0xDBFFU && i + 1 < length && text[i + 1] >= 0xDC00U &&
            text[i + 1] <= 0xDFFFU) {
            c = 0x10000U + ((c - 0xD800U) << 10) + (text[i + 1] - 0xDC00U);
            ++i;
        } else if (c >= 0xD800U && c <= 0xDFFFU) {
            c = 0xFFFDU;
        }
        EncodeUtf8(out, c);
    }
}

// ---- building ---------------------------------------------------------------

uint8_t FileIndex::AddVolume(std::string_view rootName, uint64_t rootId) {
    if (m_names.size() < kPad) {
        m_names.assign(kPad, '\0');
        m_slotStart.assign(1, 0);
    }
    Volume volume;
    volume.rootName = std::string(rootName);
    volume.rootRecord = Record(rootId);
    m_volumes.push_back(std::move(volume));
    const auto index = static_cast<uint8_t>(m_volumes.size() - 1);
    const uint32_t root = NewEntry(kNone, kFlagDirectory, rootName);
    m_parent[root] = root;
    m_volumes[index].rootEntry = root;
    MapRecord(m_volumes[index], m_volumes[index].rootRecord, root);
    m_volumes[index].firstPending = static_cast<uint32_t>(m_parent.size());
    m_pendingParent.clear();
    ++m_generation;
    return index;
}

void FileIndex::Reserve(size_t entries, size_t nameBytes) {
    const size_t total = m_parent.size() + entries;
    m_parent.reserve(total);
    m_slot.reserve(total);
    m_flags.reserve(total);
    m_zone.reserve(total);
    m_slotStart.reserve(m_slotStart.size() + entries);
    m_slotEntry.reserve(m_slotEntry.size() + entries);
    m_slotBase.reserve(m_slotBase.size() + entries);
    m_slotKey.reserve(m_slotKey.size() + entries);
    m_pendingParent.reserve(entries);
    m_names.reserve(m_names.size() + nameBytes + entries);
}

uint32_t FileIndex::AppendSlot(uint32_t entry, std::string_view name) {
    // m_names = [names...][kPad zeros]; slot sentinel = logical end. Grow in
    // place: the old pad (already zero) becomes the name, and only the new
    // tail is zero-filled (no per-record pad rewrite).
    const size_t logical = m_names.size() - kPad;
    m_names.resize(m_names.size() + name.size() + 1);
    std::memcpy(m_names.data() + logical, name.data(), name.size());
    m_names[logical + name.size()] = '\0';
    const auto slot = static_cast<uint32_t>(m_slotEntry.size());
    m_slotEntry.push_back(entry);
    m_slotBase.push_back(kNoBase);  // set once the zone is known
    m_slotKey.push_back(StemKey(name));
    m_slotStart.back() = static_cast<uint32_t>(logical);
    m_slotStart.push_back(static_cast<uint32_t>(m_names.size() - kPad));
    return slot;
}

uint32_t FileIndex::NewEntry(uint32_t parent, uint8_t flags, std::string_view name) {
    const auto entry = static_cast<uint32_t>(m_parent.size());
    m_parent.push_back(parent);
    m_flags.push_back(flags);
    m_zone.push_back(static_cast<uint8_t>(Zone::Normal));
    m_slot.push_back(AppendSlot(entry, name));
    ++m_liveEntries;
    return entry;
}

void FileIndex::MapRecord(Volume& volume, uint64_t record, uint32_t entry) {
    if (record >= (1ULL << 32)) {
        return;  // Not an MFT record number; never seen on NTFS.
    }
    if (record >= volume.byRecord.size()) {
        size_t grow = std::max<size_t>(static_cast<size_t>(record) + 1, volume.byRecord.size() * 3 / 2);
        volume.byRecord.resize(grow, kNone);
    }
    volume.byRecord[static_cast<size_t>(record)] = entry;
}

uint32_t FileIndex::Find(uint8_t volume, uint64_t id) const noexcept {
    if (volume >= m_volumes.size()) {
        return kNone;
    }
    const uint64_t record = Record(id);
    const std::vector<uint32_t>& map = m_volumes[volume].byRecord;
    return record < map.size() ? map[static_cast<size_t>(record)] : kNone;
}

void FileIndex::Add(uint8_t volume, uint64_t id, uint64_t parentId, uint8_t flags, std::string_view nameUtf8) {
    const uint64_t record = Record(id);
    if (record == m_volumes[volume].rootRecord) {
        return;
    }
    if (record < kFirstUserRecord) {
        flags |= kFlagMeta;
    }
    const uint32_t entry = NewEntry(kNone, flags, nameUtf8);
    m_pendingParent.push_back(parentId);
    MapRecord(m_volumes[volume], record, entry);
}

void FileIndex::AddUtf16(uint8_t volume, uint64_t id, uint64_t parentId, uint8_t flags, const char16_t* name,
    size_t length) {
    const uint64_t record = Record(id);
    Volume& vol = m_volumes[volume];
    if (record == vol.rootRecord) {
        return;
    }
    if (record < kFirstUserRecord) {
        flags |= kFlagMeta;
    }
    // Single-pass ASCII fast path: narrow UTF-16 directly into m_names while
    // computing the FNV-1a stem hash, growing m_names geometrically so we avoid
    // 1.67M per-record resize/zero-fill calls during bulk MFT ingest.
    const size_t logical = m_slotStart.back();
    const size_t needed = logical + length + 1 + kPad;
    if (needed > m_names.size()) {
        const size_t grown = std::max(needed, m_names.size() + (m_names.size() >> 1) + 4096);
        m_names.resize(grown, '\0');
    }
    char* dst = m_names.data() + logical;
    uint32_t hash = 2166136261U;
    uint32_t hashAtLastDot = 0;
    bool sawDot = false;
    size_t i = 0;
    for (; i < length; ++i) {
        const char16_t ch = name[i];
        if (ch >= 0x80U) {
            break;
        }
        const auto b = static_cast<unsigned char>(ch);
        dst[i] = static_cast<char>(b);
        if (b == '.' && i > 0) {
            hashAtLastDot = hash;
            sawDot = true;
        }
        hash = (hash ^ AsciiFold(b)) * 16777619U;
    }
    if (i == length) {
        dst[length] = '\0';
        const auto entry = static_cast<uint32_t>(m_parent.size());
        const auto slot = static_cast<uint32_t>(m_slotEntry.size());
        const uint32_t stem = sawDot ? hashAtLastDot : hash;
        m_parent.push_back(kNone);
        m_flags.push_back(flags);
        m_zone.push_back(static_cast<uint8_t>(Zone::Normal));
        m_slot.push_back(slot);
        m_slotEntry.push_back(entry);
        m_slotBase.push_back(kNoBase);
        m_slotKey.push_back(static_cast<uint16_t>(stem ^ (stem >> 16)));
        m_slotStart.back() = static_cast<uint32_t>(logical);
        m_slotStart.push_back(static_cast<uint32_t>(logical + length + 1));
        ++m_liveEntries;
        m_pendingParent.push_back(parentId);
        MapRecord(vol, record, entry);
        return;
    }
    // Non-ASCII fallback: restore m_names size and zero the overwritten bytes.
    m_names.resize(logical + kPad);
    std::memset(m_names.data() + logical, 0, std::min(i, kPad));
    std::string utf8;
    utf8.reserve(length * 3);
    AppendUtf8(utf8, name, length);
    Add(volume, id, parentId, flags, utf8);
}

void FileIndex::FinishVolume(uint8_t volume) {
    if (!m_slotStart.empty()) {
        m_names.resize(static_cast<size_t>(m_slotStart.back()) + kPad, '\0');
    }
    Volume& v = m_volumes[volume];
    const uint32_t first = v.firstPending;
    for (size_t i = 0; i < m_pendingParent.size(); ++i) {
        const auto entry = static_cast<uint32_t>(first + i);
        const uint64_t parentRecord = Record(m_pendingParent[i]);
        uint32_t parent = parentRecord < v.byRecord.size() ? v.byRecord[static_cast<size_t>(parentRecord)] : kNone;
        if (parent == kNone || parent == entry) {
            parent = v.rootEntry;
            m_flags[entry] |= kFlagOrphan;
        }
        m_parent[entry] = parent;
    }
    std::vector<uint64_t>().swap(m_pendingParent);
    v.firstPending = static_cast<uint32_t>(m_parent.size());
    ++m_generation;
}

bool FileIndex::AddAlias(uint8_t volume, uint64_t id, uint64_t parentId, uint8_t flags, std::string_view nameUtf8) {
    if (volume >= m_volumes.size()) {
        return false;
    }
    const uint32_t parent = Find(volume, parentId);
    const uint32_t existing = Find(volume, id);
    if (parent == kNone || (existing != kNone && m_parent[existing] == parent)) {
        return false;
    }
    static_cast<void>(NewEntry(parent, flags, nameUtf8));
    ++m_generation;
    return true;
}

// ---- updates ----------------------------------------------------------------

void FileIndex::Upsert(uint8_t volume, uint64_t id, uint64_t parentId, uint8_t flags, std::string_view nameUtf8) {
    if (volume >= m_volumes.size()) {
        return;
    }
    Volume& v = m_volumes[volume];
    const uint64_t record = Record(id);
    if (record == v.rootRecord) {
        return;
    }
    if (record < kFirstUserRecord) {
        flags |= kFlagMeta;
    }
    const uint32_t parent = Find(volume, parentId);
    uint32_t entry = Find(volume, id);
    if (parent == kNone) {
        // Outside what is indexed (or moved out of it): never store orphans,
        // so unrelated disk churn cannot grow the index.
        if (entry != kNone) {
            Remove(volume, id);
        }
        return;
    }
    if (entry == kNone) {
        entry = NewEntry(parent, flags, nameUtf8);
        MapRecord(v, record, entry);
    } else {
        if ((m_flags[entry] & kFlagDeleted) != 0) {
            ++m_liveEntries;
        }
        m_parent[entry] = parent;
        m_flags[entry] = flags;
    }
    m_zone[entry] = ZoneForNew(parent, m_flags[entry], nameUtf8, parent == v.rootEntry);
    const int16_t base = BaseScore(entry, nameUtf8, Depth(entry));
    uint32_t slot = m_slot[entry];
    // A new slot is needed for a new name, a revived entry (its old slot is
    // dead), or a changed static score inside the sorted region (the scan's
    // early exit relies on descending order there).
    const bool live = slot != kNone && m_slotEntry[slot] == entry;
    if (!live || Name(entry) != nameUtf8 || (slot < m_sortedEnd && m_slotBase[slot] != base)) {
        if (live) {
            m_slotEntry[slot] = kNone;
            m_deadNameBytes += m_slotStart[slot + 1] - m_slotStart[slot];
        }
        slot = AppendSlot(entry, nameUtf8);
        m_slot[entry] = slot;
    }
    m_slotBase[slot] = base;
    ++m_generation;
}

void FileIndex::Remove(uint8_t volume, uint64_t id) {
    if (volume >= m_volumes.size()) {
        return;
    }
    const uint32_t entry = Find(volume, id);
    if (entry == kNone || entry == m_volumes[volume].rootEntry || (m_flags[entry] & kFlagDeleted) != 0) {
        return;
    }
    m_flags[entry] |= kFlagDeleted;
    const uint32_t slot = m_slot[entry];
    if (slot != kNone && m_slotEntry[slot] == entry) {
        // The base stays: the sorted region must remain monotonic.
        m_slotEntry[slot] = kNone;
        m_deadNameBytes += m_slotStart[slot + 1] - m_slotStart[slot];
    }
    m_volumes[volume].byRecord[static_cast<size_t>(Record(id))] = kNone;
    --m_liveEntries;
    ++m_generation;
}

void FileIndex::CompactIfWasteful() {
    const size_t logical = m_names.size() - kPad;
    if (m_deadNameBytes < (4U << 20) || m_deadNameBytes * 4 < logical) {
        return;
    }
    Reorder();
}

int16_t FileIndex::BaseScore(uint32_t entry, std::string_view name, int depth) const noexcept {
    const auto zone = static_cast<Zone>(m_zone[entry]);
    if (zone == Zone::Excluded || (m_flags[entry] & kFlagDeleted) != 0) {
        return kNoBase;
    }
    // Prefer shallow paths among equal names (C:\\Tools\\x over deep copies).
    const int base = ZoneBonus(zone) + KindBonus(name, m_flags[entry]) -
        static_cast<int>(std::min<size_t>(name.size(), 120)) * 2 - std::max(0, depth - 3) * 12;
    return static_cast<int16_t>(std::clamp(base, -32000, 32000));
}

// Rewrites the name buffer with live slots ordered by static score (best
// first; stable), dropping dead slots. Search stops scanning once no later
// name can reach the current top-N.
void FileIndex::Reorder() {
    // Counting sort over the clamped base range [-32000, 32000]: descending
    // buckets, then one trailing bucket for kNoBase.
    constexpr int kMax = 32000;
    std::vector<uint32_t> counts(static_cast<size_t>(kMax) * 2 + 2, 0);
    const auto bucketOf = [&counts](int16_t base) {
        return base == kNoBase ? counts.size() - 1 : static_cast<size_t>(kMax - base);
    };
    size_t live = 0;
    size_t liveBytes = 0;
    for (size_t slot = 0; slot < m_slotEntry.size(); ++slot) {
        if (m_slotEntry[slot] == kNone) {
            continue;
        }
        ++counts[bucketOf(m_slotBase[slot])];
        ++live;
        liveBytes += m_slotStart[slot + 1] - m_slotStart[slot];
    }
    uint32_t running = 0;
    for (uint32_t& count : counts) {
        const uint32_t c = count;
        count = running;
        running += c;
    }
    std::vector<uint32_t> order(live);
    for (size_t slot = 0; slot < m_slotEntry.size(); ++slot) {
        if (m_slotEntry[slot] == kNone) {
            continue;
        }
        order[counts[bucketOf(m_slotBase[slot])]++] = static_cast<uint32_t>(slot);
    }
    std::string names(liveBytes + kPad, '\0');
    std::vector<uint32_t> starts(live + 1);
    std::vector<uint32_t> owners(live);
    std::vector<int16_t> bases(live);
    std::vector<uint16_t> keys(live);
    std::fill(m_slot.begin(), m_slot.end(), kNone);
    // Two-stage prefetch: first prefetch the random slot metadata, then the
    // name bytes and entry slot back-pointer, and write via raw pointers.
    const char* srcNames = m_names.data();
    const uint32_t* srcStart = m_slotStart.data();
    const uint32_t* srcEntry = m_slotEntry.data();
    const int16_t* srcBase = m_slotBase.data();
    const uint16_t* srcKey = m_slotKey.data();
    uint32_t* slotOfEntry = m_slot.data();
    char* dstNames = names.data();
    uint32_t* dstStart = starts.data();
    uint32_t* dstEntry = owners.data();
    int16_t* dstBase = bases.data();
    uint16_t* dstKey = keys.data();
    const uint32_t* orderData = order.data();
    const size_t orderSize = order.size();
    uint32_t cursor = 0;
    constexpr size_t kAhead1 = 32;
    constexpr size_t kAhead2 = 16;
    for (size_t k = 0; k < orderSize; ++k) {
        if (k + kAhead1 < orderSize) {
            const uint32_t a1 = orderData[k + kAhead1];
            FILESEARCH_PREFETCH(srcStart + a1);
            FILESEARCH_PREFETCH(srcEntry + a1);
        }
        if (k + kAhead2 < orderSize) {
            const uint32_t a2 = orderData[k + kAhead2];
            FILESEARCH_PREFETCH(srcNames + srcStart[a2]);
            FILESEARCH_PREFETCH(slotOfEntry + srcEntry[a2]);
        }
        const uint32_t slot = orderData[k];
        const uint32_t entry = srcEntry[slot];
        const uint32_t s0 = srcStart[slot];
        const uint32_t len = srcStart[slot + 1] - s0;
        dstStart[k] = cursor;
        std::memcpy(dstNames + cursor, srcNames + s0, len);
        cursor += len;
        slotOfEntry[entry] = static_cast<uint32_t>(k);
        dstEntry[k] = entry;
        dstBase[k] = srcBase[slot];
        dstKey[k] = srcKey[slot];
    }
    dstStart[orderSize] = cursor;
    m_names = std::move(names);
    m_slotStart = std::move(starts);
    m_slotEntry = std::move(owners);
    m_slotBase = std::move(bases);
    m_slotKey = std::move(keys);
    m_sortedEnd = static_cast<uint32_t>(m_slotEntry.size());
    m_deadNameBytes = 0;
    ++m_generation;
}

void FileIndex::ShrinkToFit() {
    m_parent.shrink_to_fit();
    m_slot.shrink_to_fit();
    m_flags.shrink_to_fit();
    m_zone.shrink_to_fit();
    m_slotStart.shrink_to_fit();
    m_slotEntry.shrink_to_fit();
    m_slotBase.shrink_to_fit();
    m_slotKey.shrink_to_fit();
    m_names.shrink_to_fit();
    for (Volume& v : m_volumes) {
        v.byRecord.shrink_to_fit();
    }
}

int FileIndex::Depth(uint32_t entry) const noexcept {
    int depth = 0;
    while (m_parent[entry] != entry && depth < 64) {
        entry = m_parent[entry];
        ++depth;
    }
    return depth;
}

// ---- zones --------------------------------------------------------------------

void FileIndex::SetZoneMarker(uint8_t volume, uint64_t id, Zone zone) {
    m_zoneMarkers[(static_cast<uint64_t>(volume) << 56) | Record(id)] = static_cast<uint8_t>(zone);
}

uint8_t FileIndex::ZoneForNew(uint32_t parent, uint8_t flags, std::string_view name, bool parentIsRoot) const noexcept {
    if ((flags & (kFlagMeta | kFlagOrphan)) != 0) {
        return static_cast<uint8_t>(Zone::Excluded);
    }
    if (parentIsRoot) {
        if ((!name.empty() && name[0] == '$') || EqualsFolded(name, "system volume information") ||
            EqualsFolded(name, "config.msi") || EqualsFolded(name, "recovery")) {
            return static_cast<uint8_t>(Zone::Excluded);
        }
    }
    const auto inherited = static_cast<Zone>(m_zone[parent]);
    if (inherited == Zone::Excluded) {
        return static_cast<uint8_t>(Zone::Excluded);
    }
    if ((flags & (kFlagHidden | kFlagSystem)) != 0) {
        return static_cast<uint8_t>(Zone::Hidden);
    }
    return static_cast<uint8_t>(inherited);
}

void FileIndex::RecomputeZones() {
    const size_t entryCount = m_parent.size();
    // State per entry: 0 = unvisited, 1 = visited, >=2 = marked with Zone(state - 2).
    std::vector<uint8_t> state(entryCount, 0);
    for (const auto& [key, zone] : m_zoneMarkers) {
        const auto volume = static_cast<uint8_t>(key >> 56);
        const uint32_t entry = Find(volume, key & 0x00FFFFFFFFFFFFFFULL);
        if (entry != kNone) {
            state[entry] = static_cast<uint8_t>(zone + 2);
        }
    }
    std::vector<uint16_t> depth(entryCount, 0);
    std::vector<uint32_t> chain;
    chain.reserve(64);
    const uint32_t* parentArr = m_parent.data();
    const uint8_t* flagsArr = m_flags.data();
    uint8_t* zoneArr = m_zone.data();
    uint8_t* stateArr = state.data();
    uint16_t* depthArr = depth.data();
    for (uint32_t start = 0; start < entryCount; ++start) {
        if (stateArr[start] == 1) {
            continue;
        }
        chain.clear();
        uint32_t x = start;
        while (stateArr[x] != 1 && chain.size() < 4096) {
            chain.push_back(x);
            if (parentArr[x] == x) {
                break;
            }
            x = parentArr[x];
        }
        while (!chain.empty()) {
            const uint32_t e = chain.back();
            chain.pop_back();
            const uint32_t parent = parentArr[e];
            const uint8_t st = stateArr[e];
            depthArr[e] = (parent == e || stateArr[parent] != 1)
                ? uint16_t{0}
                : static_cast<uint16_t>(std::min<int>(depthArr[parent] + 1, 1024));
            uint8_t zone = 0;
            if (st >= 2) {
                zone = static_cast<uint8_t>(st - 2);
            } else if (parent == e) {
                zone = static_cast<uint8_t>(Zone::Normal);
            } else if (stateArr[parent] != 1) {
                zone = static_cast<uint8_t>(Zone::Excluded);  // cycle guard
            } else {
                const bool parentIsRoot = parentArr[parent] == parent;
                zone = ZoneForNew(parent, flagsArr[e], parentIsRoot ? Name(e) : std::string_view(), parentIsRoot);
            }
            zoneArr[e] = zone;
            stateArr[e] = 1;
        }
    }
    const size_t slotCount = m_slotEntry.size();
    const uint32_t* slotEntryArr = m_slotEntry.data();
    const uint32_t* slotStartArr = m_slotStart.data();
    int16_t* slotBaseArr = m_slotBase.data();
    const char* namesData = m_names.data();
    for (size_t slot = 0; slot < slotCount; ++slot) {
        const uint32_t entry = slotEntryArr[slot];
        if (entry != kNone) {
            const uint32_t start = slotStartArr[slot];
            slotBaseArr[slot] = BaseScore(entry,
                std::string_view(namesData + start, slotStartArr[slot + 1] - start - 1), depthArr[entry]);
        }
    }
    Reorder();
}

// ---- accessors ----------------------------------------------------------------

std::string_view FileIndex::Name(uint32_t entry) const noexcept {
    const uint32_t slot = m_slot[entry];
    if (slot == kNone) {
        return {};
    }
    const uint32_t start = m_slotStart[slot];
    return {m_names.data() + start, static_cast<size_t>(m_slotStart[slot + 1] - start - 1)};
}

std::string FileIndex::Path(uint32_t entry) const {
    std::vector<std::string_view> parts;
    uint32_t x = entry;
    for (int depth = 0; depth < 1024; ++depth) {
        parts.push_back(Name(x));
        if (m_parent[x] == x) {
            break;
        }
        x = m_parent[x];
    }
    std::string path;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        path.append(*it);
        if (it + 1 != parts.rend()) {
            path.push_back('\\');
        }
    }
    return path;
}

size_t FileIndex::MemoryBytes() const noexcept {
    size_t bytes = m_parent.capacity() * 4 + m_slot.capacity() * 4 + m_slotBase.capacity() * 2 +
        m_slotKey.capacity() * 2 + m_flags.capacity() + m_zone.capacity() +
        m_slotStart.capacity() * 4 + m_slotEntry.capacity() * 4 + m_names.capacity() +
        m_pendingParent.capacity() * 8;
    for (const Volume& v : m_volumes) {
        bytes += v.byRecord.capacity() * 4;
    }
    return bytes;
}

// ---- binary persistence -------------------------------------------------------

namespace {

constexpr uint32_t kIndexCacheMagic = 0x31495346U;  // "FSI1"

struct BinaryArchive {
    std::vector<uint8_t>* out = nullptr;
    const uint8_t* p = nullptr;
    size_t left = 0;
    bool ok = true;

    template <typename T>
    void Pod(T& v) {
        if (out != nullptr) {
            const auto* b = reinterpret_cast<const uint8_t*>(&v);
            out->insert(out->end(), b, b + sizeof(T));
        } else if (left >= sizeof(T)) {
            std::memcpy(&v, p, sizeof(T));
            p += sizeof(T);
            left -= sizeof(T);
        } else {
            ok = false;
        }
    }

    template <typename T>
    void Vec(std::vector<T>& vec, size_t maxElements = (1ULL << 27)) {
        auto count = static_cast<uint64_t>(vec.size());
        Pod(count);
        if (!ok) {
            return;
        }
        if (out != nullptr) {
            const auto* b = reinterpret_cast<const uint8_t*>(vec.data());
            out->insert(out->end(), b, b + vec.size() * sizeof(T));
        } else if (count <= maxElements && count <= left / sizeof(T)) {
            vec.resize(static_cast<size_t>(count));
            if (count > 0) {
                std::memcpy(vec.data(), p, static_cast<size_t>(count) * sizeof(T));
                p += static_cast<size_t>(count) * sizeof(T);
                left -= static_cast<size_t>(count) * sizeof(T);
            }
        } else {
            ok = false;
        }
    }

    void Str(std::string& s, size_t maxBytes = (1ULL << 29)) {
        auto count = static_cast<uint64_t>(s.size());
        Pod(count);
        if (!ok) {
            return;
        }
        if (out != nullptr) {
            const auto* b = reinterpret_cast<const uint8_t*>(s.data());
            out->insert(out->end(), b, b + s.size());
        } else if (count <= maxBytes && count <= left) {
            s.resize(static_cast<size_t>(count));
            if (count > 0) {
                std::memcpy(s.data(), p, static_cast<size_t>(count));
                p += static_cast<size_t>(count);
                left -= static_cast<size_t>(count);
            }
        } else {
            ok = false;
        }
    }
};

}  // namespace

void FileIndex::Serialize(std::vector<uint8_t>& out) const {
    auto& self = const_cast<FileIndex&>(*this);
    BinaryArchive ar{&out, nullptr, 0, true};
    uint32_t magic = kIndexCacheMagic;
    auto live = static_cast<uint64_t>(m_liveEntries);
    auto dead = static_cast<uint64_t>(m_deadNameBytes);
    ar.Pod(magic);
    ar.Pod(live);
    ar.Pod(dead);
    ar.Pod(self.m_sortedEnd);
    ar.Vec(self.m_parent);
    ar.Vec(self.m_slot);
    ar.Vec(self.m_flags);
    ar.Vec(self.m_zone);
    ar.Vec(self.m_slotStart);
    ar.Vec(self.m_slotEntry);
    ar.Vec(self.m_slotBase);
    ar.Vec(self.m_slotKey);
    ar.Str(self.m_names);
    auto volumeCount = static_cast<uint64_t>(m_volumes.size());
    ar.Pod(volumeCount);
    for (Volume& v : self.m_volumes) {
        ar.Str(v.rootName);
        ar.Pod(v.rootEntry);
        ar.Pod(v.rootRecord);
        ar.Pod(v.firstPending);
        ar.Vec(v.byRecord);
    }
    auto markerCount = static_cast<uint64_t>(m_zoneMarkers.size());
    ar.Pod(markerCount);
    for (const auto& [k, z] : m_zoneMarkers) {
        uint64_t key = k;
        uint8_t zone = z;
        ar.Pod(key);
        ar.Pod(zone);
    }
}

bool FileIndex::Deserialize(const uint8_t* data, size_t size) {
    FileIndex tmp;
    BinaryArchive ar{nullptr, data, size, true};
    uint32_t magic = 0;
    uint64_t live = 0;
    uint64_t dead = 0;
    ar.Pod(magic);
    ar.Pod(live);
    ar.Pod(dead);
    ar.Pod(tmp.m_sortedEnd);
    if (!ar.ok || magic != kIndexCacheMagic) {
        return false;
    }
    ar.Vec(tmp.m_parent);
    ar.Vec(tmp.m_slot);
    ar.Vec(tmp.m_flags);
    ar.Vec(tmp.m_zone);
    ar.Vec(tmp.m_slotStart);
    ar.Vec(tmp.m_slotEntry);
    ar.Vec(tmp.m_slotBase);
    ar.Vec(tmp.m_slotKey);
    ar.Str(tmp.m_names);

    uint64_t volumeCount = 0;
    ar.Pod(volumeCount);
    if (!ar.ok || volumeCount > 64) {
        return false;
    }
    tmp.m_volumes.resize(static_cast<size_t>(volumeCount));
    for (Volume& v : tmp.m_volumes) {
        ar.Str(v.rootName, 64);
        ar.Pod(v.rootEntry);
        ar.Pod(v.rootRecord);
        ar.Pod(v.firstPending);
        ar.Vec(v.byRecord);
    }

    uint64_t markerCount = 0;
    ar.Pod(markerCount);
    if (!ar.ok || markerCount > (1ULL << 16)) {
        return false;
    }
    tmp.m_zoneMarkers.reserve(static_cast<size_t>(markerCount));
    for (uint64_t i = 0; i < markerCount && ar.ok; ++i) {
        uint64_t key = 0;
        uint8_t zone = 0;
        ar.Pod(key);
        ar.Pod(zone);
        tmp.m_zoneMarkers[key] = zone;
    }

    const size_t entries = tmp.m_parent.size();
    const size_t slots = tmp.m_slotEntry.size();
    if (!ar.ok || ar.left != 0 || tmp.m_slot.size() != entries || tmp.m_flags.size() != entries ||
        tmp.m_zone.size() != entries || tmp.m_slotBase.size() != slots || tmp.m_slotKey.size() != slots ||
        tmp.m_slotStart.size() != slots + 1 || tmp.m_sortedEnd > slots || tmp.m_names.size() < kPad ||
        live > entries) {
        return false;
    }
    const size_t logical = tmp.m_names.size() - kPad;
    if (tmp.m_slotStart.front() != 0 || tmp.m_slotStart.back() != logical) {
        return false;
    }
    for (size_t i = logical; i < tmp.m_names.size(); ++i) {
        if (tmp.m_names[i] != '\0') {
            return false;
        }
    }
    for (size_t i = 0; i < entries; ++i) {
        if (tmp.m_parent[i] >= entries || (tmp.m_slot[i] != kNone && tmp.m_slot[i] >= slots)) {
            return false;
        }
    }
    for (size_t s = 0; s < slots; ++s) {
        const uint32_t s0 = tmp.m_slotStart[s];
        const uint32_t s1 = tmp.m_slotStart[s + 1];
        if (s1 <= s0 || s1 > logical || tmp.m_names[s1 - 1] != '\0' ||
            (tmp.m_slotEntry[s] != kNone && tmp.m_slotEntry[s] >= entries)) {
            return false;
        }
    }
    for (const Volume& v : tmp.m_volumes) {
        if (v.rootEntry >= entries || v.firstPending > entries) {
            return false;
        }
        for (const uint32_t e : v.byRecord) {
            if (e != kNone && e >= entries) {
                return false;
            }
        }
    }

    tmp.m_liveEntries = static_cast<size_t>(live);
    tmp.m_deadNameBytes = static_cast<size_t>(dead);
    tmp.m_generation = m_generation + 1;
    *this = std::move(tmp);
    return true;
}

// ---- search -------------------------------------------------------------------

namespace {

struct ScanContext {
    const Query* query = nullptr;
    const unsigned char* names = nullptr;
    const uint32_t* slotStart = nullptr;
    const uint32_t* slotEntry = nullptr;
    const int16_t* slotBase = nullptr;
    const uint16_t* slotKey = nullptr;
    uint16_t queryKey = 0;
    bool queryHasDot = false;
    size_t slotCount = 0;
    // Narrowing only pays for small match sets (see NarrowingCap): stop
    // recording past it (one cap for the whole query, across scan phases).
    size_t recordCap = 0;
    // Upper bound of the query-dependent score part with all tokens at
    // position 0: without (tokenCeiling) and with (ceilingHead) the
    // whole-name bonus. A slot can never beat ceilingHead + its base.
    int tokenCeiling = 0;
    int ceilingHead = 0;
    // Best top-N threshold published by any worker (strict bound: a hit
    // scoring below it cannot enter the merged top-N).
    std::atomic<int32_t>* shared = nullptr;
};

enum class SlotResult { NoMatch, Matched, Scored };

// Re-scoring a recorded match (random access, full token checks) costs
// about as much as SIMD-scanning ~70 names (MEASURED: ~40 ns vs ~0.55 ns),
// so narrowing beats a fresh scan only below ~1/64 of the names.
size_t NarrowingCap(size_t slotCount) noexcept {
    return std::max<size_t>(slotCount / 64, 4096);
}

// Exact ceiling for one name: tokens at position 0, whole-name bonus only
// when the length allows it (exact name, or stem ending in '.').
inline int ScoreCeiling(size_t tokens, std::string_view name, size_t queryLength, int base) noexcept {
    int ceiling = static_cast<int>(tokens) * kTokenCeiling + base;
    if (name.size() == queryLength) {
        ceiling += kWholeNameCeiling;
    } else if (name.size() > queryLength && name[queryLength] == '.') {
        ceiling += 400;
    }
    return ceiling;
}

int32_t SharedThreshold(const ScanContext& ctx, const TopN& top) noexcept {
    return std::max(top.Threshold(), ctx.shared->load(std::memory_order_relaxed));
}

void Publish(const ScanContext& ctx, const TopN& top) noexcept {
    const int32_t mine = top.Threshold();
    int32_t current = ctx.shared->load(std::memory_order_relaxed);
    while (mine > current && !ctx.shared->compare_exchange_weak(current, mine, std::memory_order_relaxed)) {
    }
}

// Checks slot `slot` against every token except `skipToken` (already matched
// by the scan). Matching slots that cannot beat `threshold` are reported as
// Matched without paying for the full score.
SlotResult ScoreSlot(const ScanContext& ctx, uint32_t slot, size_t skipToken, int32_t threshold, Hit& hit) {
    const int base = ctx.slotBase[slot];
    if (base == kNoBase || ctx.slotEntry[slot] == FileIndex::kNone) {
        return SlotResult::NoMatch;  // excluded or dead
    }
    const std::string_view name(reinterpret_cast<const char*>(ctx.names) + ctx.slotStart[slot],
        ctx.slotStart[slot + 1] - ctx.slotStart[slot] - 1);
    const std::vector<Token>& tokens = ctx.query->tokens;
    for (size_t t = 0; t < tokens.size(); ++t) {
        if (t != skipToken && FindIn(name, tokens[t]) == std::string_view::npos) {
            return SlotResult::NoMatch;
        }
    }
    if (threshold != INT32_MIN && ScoreCeiling(tokens.size(), name, ctx.query->folded.size(), base) < threshold) {
        return SlotResult::Matched;
    }
    int score = base;
    for (const Token& token : tokens) {
        size_t pos = FindIn(name, token);
        int best = 100;
        while (pos != std::string_view::npos) {
            if (pos == 0) {
                best = kTokenCeiling;
                break;
            }
            if (IsWordStart(name, pos)) {
                best = 400;
                break;
            }
            pos = FindIn(name, token, pos + 1);
        }
        score += best;
    }
    const std::string_view folded = ctx.query->folded;
    if (EqualsFolded(name, folded)) {
        score += kWholeNameCeiling;
    } else if (const size_t dot = name.rfind('.'); dot != std::string_view::npos && dot > 0 &&
        EqualsFolded(name.substr(0, dot), folded)) {
        score += 400;
    }
    hit.entry = ctx.slotEntry[slot];
    hit.score = score;
    return SlotResult::Scored;
}

// Second phase of a sorted scan from `slot`: only names that could still earn
// the whole-name / stem bonus (length == query, or '.' right after the
// query-length prefix; folding preserves UTF-8 length) can still enter the
// top-N. One length check and one byte per slot instead of a full match.
void ScanWholeNameCandidates(const ScanContext& ctx, uint32_t slot, uint32_t s1, TopN& top) {
    const std::string_view folded = ctx.query->folded;
    const size_t queryLength = folded.size();
    const uint32_t* starts = ctx.slotStart;
    const char* names = reinterpret_cast<const char*>(ctx.names);
    // Bases are non-increasing here: from `stop` on nothing can enter even
    // with the bonus. Recomputed whenever an offer raises the threshold.
    auto stopFrom = [&](uint32_t from) {
        const int32_t threshold = SharedThreshold(ctx, top);
        return static_cast<uint32_t>(std::partition_point(ctx.slotBase + from, ctx.slotBase + s1,
            [&](int16_t base) { return ctx.ceilingHead + base >= threshold; }) - ctx.slotBase);
    };
    uint32_t stop = stopFrom(slot);
    // Exact bonus conditions (ScoreSlot): the whole name, or the stem up to
    // the last '.', equals the query. Others cannot reach the top-N.
    auto consider = [&](uint32_t candidate) {
        const uint32_t start = starts[candidate];
        const size_t length = starts[candidate + 1] - start - 1;
        const std::string_view name(names + start, length);
        const bool whole = length == queryLength && EqualsFolded(name, folded);
        const bool stem = !whole && length > queryLength && name.rfind('.') == queryLength &&
            EqualsFolded(name.substr(0, queryLength), folded);
        if (!whole && !stem) {
            return;
        }
        Hit hit;
        if (ScoreSlot(ctx, candidate, SIZE_MAX, SharedThreshold(ctx, top), hit) == SlotResult::Scored) {
            top.Offer(hit);
            Publish(ctx, top);
            stop = stopFrom(candidate + 1);
        }
    };
    if (ctx.queryHasDot) {
        // Only a dotted query can equal a dotted name as a whole: length filter.
        for (; slot < stop; ++slot) {
            const size_t length = starts[slot + 1] - starts[slot] - 1;
            if (length == queryLength || (length > queryLength && names[starts[slot] + queryLength] == '.')) {
                consider(slot);
            }
        }
        return;
    }
    // Dot-free query: the stem key (whole name when dot-free) must match.
    const uint16_t key = ctx.queryKey;
    const uint16_t* keys = ctx.slotKey;
    while (slot < stop) {
#if defined(FILESEARCH_SSE2)
        if (slot + 8 <= stop) {
            const __m128i lanes = _mm_cmpeq_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(keys + slot)),
                _mm_set1_epi16(static_cast<short>(key)));
            unsigned mask = static_cast<unsigned>(_mm_movemask_epi8(lanes));
            const uint32_t block = slot;
            slot += 8;
            while (mask != 0) {
#if defined(_MSC_VER)
                unsigned long bit = 0;
                _BitScanForward(&bit, mask);
#else
                const unsigned bit = static_cast<unsigned>(__builtin_ctz(mask));
#endif
                const uint32_t candidate = block + static_cast<uint32_t>(bit / 2);
                if (candidate < stop) {
                    consider(candidate);
                }
                mask &= ~(3U << (bit & ~1U));
            }
            continue;
        }
#elif defined(FILESEARCH_NEON)
        if (slot + 32 <= stop) {
            const uint16x8_t vkey = vdupq_n_u16(key);
            const uint16x8_t e0 = vceqq_u16(vld1q_u16(keys + slot), vkey);
            const uint16x8_t e1 = vceqq_u16(vld1q_u16(keys + slot + 8), vkey);
            const uint16x8_t e2 = vceqq_u16(vld1q_u16(keys + slot + 16), vkey);
            const uint16x8_t e3 = vceqq_u16(vld1q_u16(keys + slot + 24), vkey);
            const uint16x8_t any = vorrq_u16(vorrq_u16(e0, e1), vorrq_u16(e2, e3));
            if (vmaxvq_u32(vreinterpretq_u32_u16(any)) == 0) {
                slot += 32;
                continue;
            }
            const uint16x8_t eqs[4] = {e0, e1, e2, e3};
            for (uint32_t g = 0; g < 4; ++g) {
                if (vmaxvq_u32(vreinterpretq_u32_u16(eqs[g])) != 0) {
                    const uint32_t base = slot + g * 8;
                    for (uint32_t j = 0; j < 8; ++j) {
                        if (keys[base + j] == key && base + j < stop) {
                            consider(base + j);
                        }
                    }
                }
            }
            slot += 32;
            continue;
        }
#endif
        if (keys[slot] == key) {
            consider(slot);
        }
        ++slot;
    }
}

#if defined(FILESEARCH_NEON)
inline unsigned NeonMoveMask16(uint8x16_t cmp) noexcept {
    static constexpr uint8_t kBitPos[16] = {1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128};
    const uint8x16_t bits = vandq_u8(cmp, vld1q_u8(kBitPos));
    return static_cast<unsigned>(vaddv_u8(vget_low_u8(bits))) |
        (static_cast<unsigned>(vaddv_u8(vget_high_u8(bits))) << 8);
}
#endif

// Linear scan of slots [s0, s1) for the anchor token. Every matching slot is
// recorded (up to a cap) and offered to `top`. With `sorted`, slots are in
// descending static-score order, so the scan stops (`cut`) as soon as no
// remaining slot could enter the top-N without a whole-name bonus, and the
// rest is checked by ScanWholeNameCandidates.
void ScanSlots(const ScanContext& ctx, uint32_t s0, uint32_t s1, bool sorted, std::vector<uint32_t>& matches,
    size_t& matchCount, TopN& top, bool& cut, uint32_t* cutAt = nullptr) {
    if (s0 >= s1) {
        return;
    }
    if (sorted) {
        // Whole range already below the cut (a later chunk): bonus-only check.
        const int32_t threshold = SharedThreshold(ctx, top);
        if (threshold != INT32_MIN && ctx.tokenCeiling + ctx.slotBase[s0] < threshold) {
            cut = true;
            if (cutAt != nullptr) {
                *cutAt = s0;
            }
            ScanWholeNameCandidates(ctx, s0, s1, top);
            return;
        }
    }
    const Token& token = ctx.query->tokens[ctx.query->anchor];
    const unsigned char* names = ctx.names;
    const uint32_t* starts = ctx.slotStart;
    const size_t begin = starts[s0];
    const size_t end = starts[s1];
    uint32_t slot = s0;

    // Returns the position to resume scanning from (start of the next slot,
    // or `end` to stop).
    auto onCandidate = [&](size_t pos) -> size_t {
        if (starts[slot + 1] <= pos) {
            if (starts[slot + 2] > pos) {
                ++slot;
            } else {
                slot = static_cast<uint32_t>(
                    std::upper_bound(starts + slot + 1, starts + s1 + 1, static_cast<uint32_t>(pos)) - starts - 1);
            }
        }
        if (ctx.slotEntry[slot] == FileIndex::kNone) {
            return starts[slot + 1];  // dead (renamed / deleted since the last reorder)
        }
        const int base = ctx.slotBase[slot];
        const int32_t threshold = SharedThreshold(ctx, top);
        if (sorted && threshold != INT32_MIN && ctx.tokenCeiling + base < threshold) {
            cut = true;
            if (cutAt != nullptr) {
                *cutAt = slot;
            }
            ScanWholeNameCandidates(ctx, slot, s1, top);
            return end;
        }
        Hit hit;
        const SlotResult result = ScoreSlot(ctx, slot, ctx.query->anchor, threshold, hit);
        if (result != SlotResult::NoMatch) {
            if (matches.size() <= ctx.recordCap) {
                matches.push_back(slot);
            }
            ++matchCount;
            if (result == SlotResult::Scored) {
                top.Offer(hit);
                Publish(ctx, top);
            }
        }
        return starts[slot + 1];
    };

    size_t pos = begin;
    const size_t k = token.length;
    if (k == 0 || end - begin < k) {
        return;
    }
    const size_t lastStart = end - k;
#if defined(FILESEARCH_SSE2)
    if (token.anchored) {
        const __m128i v1 = _mm_set1_epi8(static_cast<char>(token.value1));
        const __m128i v2 = _mm_set1_epi8(static_cast<char>(token.value2));
        const __m128i m1 = _mm_set1_epi8(static_cast<char>(token.mask1));
        const __m128i m2 = _mm_set1_epi8(static_cast<char>(token.mask2));
        const unsigned char* p1 = names + token.offset1;
        const unsigned char* p2 = names + token.offset2;
        const auto block = [&](size_t at) {
            const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p1 + at));
            const __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p2 + at));
            return static_cast<unsigned>(_mm_movemask_epi8(_mm_and_si128(_mm_cmpeq_epi8(_mm_or_si128(a, m1), v1),
                _mm_cmpeq_epi8(_mm_or_si128(b, m2), v2))));
        };
        while (pos <= lastStart) {
            // 64-byte skip while nothing can match (the zero padding plus the
            // range end bound keep these reads inside the buffer).
            if (pos + 64 <= lastStart &&
                (block(pos) | block(pos + 16) | block(pos + 32) | block(pos + 48)) == 0) {
                pos += 64;
                continue;
            }
            // 32 bytes per step, one branch for the common no-candidate case
            // (the buffer's zero padding keeps pos + 31 + offsets in bounds).
            const unsigned low = block(pos);
            const unsigned high = block(pos + 16);
            if ((low | high) == 0) {
                pos += 32;
                continue;
            }
            unsigned mask = low;
            size_t next = pos + 16;
            if (low == 0) {
                pos += 16;
                mask = high;
                next = pos + 16;
            }
            while (mask != 0) {
#if defined(_MSC_VER)
                unsigned long bit = 0;
                _BitScanForward(&bit, mask);
#else
                const unsigned bit = static_cast<unsigned>(__builtin_ctz(mask));
#endif
                const size_t candidate = pos + bit;
                if (candidate > lastStart) {
                    break;
                }
                if (MatchAt(names + candidate, token.bytes, k)) {
                    next = onCandidate(candidate);
                    break;
                }
                mask &= mask - 1;
            }
            pos = next;
        }
        return;
    }
#elif defined(FILESEARCH_NEON)
    if (token.anchored) {
        const uint8x16_t v1 = vdupq_n_u8(token.value1);
        const uint8x16_t v2 = vdupq_n_u8(token.value2);
        const uint8x16_t m1 = vdupq_n_u8(token.mask1);
        const uint8x16_t m2 = vdupq_n_u8(token.mask2);
        const unsigned char* p1 = names + token.offset1;
        const unsigned char* p2 = names + token.offset2;
        const auto blockVec = [&](size_t at) noexcept -> uint8x16_t {
            const uint8x16_t a = vld1q_u8(p1 + at);
            const uint8x16_t b = vld1q_u8(p2 + at);
            return vandq_u8(vceqq_u8(vorrq_u8(a, m1), v1), vceqq_u8(vorrq_u8(b, m2), v2));
        };
        while (pos <= lastStart) {
            if (pos + 64 <= lastStart) {
                const uint8x16_t c0 = blockVec(pos);
                const uint8x16_t c1 = blockVec(pos + 16);
                const uint8x16_t c2 = blockVec(pos + 32);
                const uint8x16_t c3 = blockVec(pos + 48);
                const uint8x16_t any = vorrq_u8(vorrq_u8(c0, c1), vorrq_u8(c2, c3));
                if (vmaxvq_u32(vreinterpretq_u32_u8(any)) == 0) {
                    pos += 64;
                    continue;
                }
                const uint8x16_t cs[4] = {c0, c1, c2, c3};
                size_t next = pos + 64;
                for (size_t g = 0; g < 4; ++g) {
                    if (vmaxvq_u32(vreinterpretq_u32_u8(cs[g])) == 0) {
                        continue;
                    }
                    unsigned mask = NeonMoveMask16(cs[g]);
                    const size_t base = pos + g * 16;
                    bool jumped = false;
                    while (mask != 0) {
                        const unsigned bit = static_cast<unsigned>(__builtin_ctz(mask));
                        const size_t candidate = base + bit;
                        if (candidate > lastStart) {
                            break;
                        }
                        if (MatchAt(names + candidate, token.bytes, k)) {
                            next = onCandidate(candidate);
                            jumped = true;
                            break;
                        }
                        mask &= mask - 1;
                    }
                    if (jumped) {
                        break;
                    }
                }
                pos = next;
                continue;
            }
            const uint8x16_t c0 = blockVec(pos);
            if (vmaxvq_u32(vreinterpretq_u32_u8(c0)) == 0) {
                pos += 16;
                continue;
            }
            unsigned mask = NeonMoveMask16(c0);
            size_t next = pos + 16;
            while (mask != 0) {
                const unsigned bit = static_cast<unsigned>(__builtin_ctz(mask));
                const size_t candidate = pos + bit;
                if (candidate > lastStart) {
                    break;
                }
                if (MatchAt(names + candidate, token.bytes, k)) {
                    next = onCandidate(candidate);
                    break;
                }
                mask &= mask - 1;
            }
            pos = next;
        }
        return;
    }
#endif
    while (pos <= lastStart) {
        if (token.anchored &&
            ((names[pos + token.offset1] | token.mask1) != token.value1 ||
                (names[pos + token.offset2] | token.mask2) != token.value2)) {
            ++pos;
            continue;
        }
        if (MatchAt(names + pos, token.bytes, k)) {
            pos = onCandidate(pos);
        } else {
            ++pos;
        }
    }
}

// Lightweight persistent worker pool so multi-threaded Search() avoids
// spawning and joining OS threads on every keystroke.
class SearchWorkerPool {
public:
    static SearchWorkerPool& Instance() {
        static SearchWorkerPool pool;
        return pool;
    }

    ~SearchWorkerPool() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stop = true;
            ++m_generation;
        }
        m_cv.notify_all();
        for (std::thread& t : m_threads) {
            if (t.joinable()) {
                t.join();
            }
        }
    }

    // Runs fn(0)..fn(workers - 1), with worker 0 on the calling thread.
    void Run(unsigned workers, const std::function<void(unsigned)>& fn) {
        if (workers <= 1) {
            fn(0);
            return;
        }
        std::unique_lock<std::mutex> callLock(m_callMutex, std::try_to_lock);
        if (!callLock.owns_lock()) {
            fn(0);
            return;
        }
        const unsigned helpers = workers - 1;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            while (m_threads.size() < helpers) {
                const unsigned id = static_cast<unsigned>(m_threads.size()) + 1;
                m_threads.emplace_back([this, id]() { WorkerLoop(id); });
            }
            m_job = &fn;
            m_activeWorkers = workers;
            m_remaining.store(helpers, std::memory_order_release);
            ++m_generation;
        }
        m_cv.notify_all();
        fn(0);
        while (m_remaining.load(std::memory_order_acquire) != 0) {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_doneCv.wait(lock, [&]() { return m_remaining.load(std::memory_order_acquire) == 0; });
        }
        m_job = nullptr;
    }

private:
    void WorkerLoop(unsigned id) {
        uint64_t seen = 0;
        for (;;) {
            const std::function<void(unsigned)>* job = nullptr;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [&]() { return m_generation != seen || m_stop; });
                if (m_stop) {
                    return;
                }
                seen = m_generation;
                if (id < m_activeWorkers) {
                    job = m_job;
                }
            }
            if (job != nullptr) {
                (*job)(id);
                if (m_remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_doneCv.notify_one();
                }
            }
        }
    }

    std::mutex m_callMutex;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::condition_variable m_doneCv;
    std::vector<std::thread> m_threads;
    const std::function<void(unsigned)>* m_job = nullptr;
    unsigned m_activeWorkers = 0;
    uint64_t m_generation = 0;
    std::atomic<unsigned> m_remaining{0};
    bool m_stop = false;
};

}  // namespace

std::vector<Hit> FileIndex::Search(std::string_view queryUtf8, size_t limit, SearchState* state,
    unsigned threads) const {
    Query query;
    if (!ParseQuery(queryUtf8, query) || m_slotEntry.empty()) {
        if (state != nullptr) {
            state->valid = false;
        }
        return {};
    }
    std::atomic<int32_t> shared{INT32_MIN};
    ScanContext ctx;
    ctx.query = &query;
    ctx.names = reinterpret_cast<const unsigned char*>(m_names.data());
    ctx.slotStart = m_slotStart.data();
    ctx.slotEntry = m_slotEntry.data();
    ctx.slotBase = m_slotBase.data();
    ctx.slotKey = m_slotKey.data();
    ctx.queryKey = FoldedKey(query.folded);
    ctx.queryHasDot = query.folded.find('.') != std::string::npos;
    ctx.slotCount = m_slotEntry.size();
    ctx.recordCap = NarrowingCap(ctx.slotCount);
    ctx.tokenCeiling = static_cast<int>(query.tokens.size()) * kTokenCeiling;
    ctx.ceilingHead = ctx.tokenCeiling + kWholeNameCeiling;
    ctx.shared = &shared;

    std::vector<uint32_t> matches;
    size_t matchCount = 0;
    bool cut = false;
    TopN top(limit);
    const auto slotCount = static_cast<uint32_t>(ctx.slotCount);
    const uint32_t sortedEnd = std::min(m_sortedEnd, slotCount);
    // `matches` holds every match in ranked slots [0, covered) plus the
    // unsorted tail; a later query that extends this one narrows them and
    // resumes the ranked scan at `covered` (its matches are a subset).
    uint32_t covered = sortedEnd;
    bool resumable = true;
    // Narrow the previous result when this query extends it, unless that
    // result is so large a SIMD rescan is cheaper than per-name checks.
    const bool incremental = state != nullptr && state->valid && state->generation == m_generation &&
        !state->folded.empty() && query.folded.size() >= state->folded.size() &&
        query.folded.compare(0, state->folded.size(), state->folded) == 0 &&
        state->slots.size() < NarrowingCap(ctx.slotCount) && state->resumeSlot <= sortedEnd;
    if (incremental) {
        matches.reserve(state->slots.size());
        for (const uint32_t slot : state->slots) {
            Hit hit;
            const SlotResult result = ScoreSlot(ctx, slot, SIZE_MAX, top.Threshold(), hit);
            if (result != SlotResult::NoMatch) {
                matches.push_back(slot);
                ++matchCount;
                if (result == SlotResult::Scored) {
                    top.Offer(hit);
                    Publish(ctx, top);
                }
            }
        }
    }
    // Ranked scan of [begin, sortedEnd): everything for a fresh query, the
    // part past the previous early exit for a narrowed one.
    const uint32_t begin = incremental ? state->resumeSlot : 0;
    if (begin < sortedEnd) {
        unsigned workers = std::max(1U, threads);
        if (sortedEnd - begin < 200000U) {
            workers = 1;
        }
        // Probe the best-ranked slots on this thread first: most queries
        // fill the top-N there and stop, so threads would only add start-up
        // cost. Fan out over the rest only when the probe did not finish.
        const uint32_t probeEnd = workers == 1 ? sortedEnd : begin + (sortedEnd - begin) / 64;
        uint32_t cutAt = sortedEnd;
        ScanSlots(ctx, begin, probeEnd, true, matches, matchCount, top, cut, &cutAt);
        if (cut) {
            // The probe's bonus-only phase stopped at probeEnd; the order is
            // monotonic across the whole sorted region, so continue it there.
            ScanWholeNameCandidates(ctx, probeEnd, sortedEnd, top);
            covered = cutAt;
        } else if (probeEnd < sortedEnd && [&] {
                       // Top-N already full: the threshold bounds where the
                       // scan must stop. Close by => stay on this thread.
                       const int32_t threshold = top.Threshold();
                       if (threshold == INT32_MIN) {
                           return false;
                       }
                       const int16_t* stop = std::partition_point(ctx.slotBase + probeEnd, ctx.slotBase + sortedEnd,
                           [&](int16_t base) { return ctx.tokenCeiling + base >= threshold; });
                       return static_cast<uint32_t>(stop - ctx.slotBase) - probeEnd <= sortedEnd / 8;
                   }()) {
            ScanSlots(ctx, probeEnd, sortedEnd, true, matches, matchCount, top, cut, &cutAt);
            covered = cut ? cutAt : sortedEnd;
        } else if (probeEnd < sortedEnd) {
            // Workers pull chunks in ranked order, so every thread works on the
            // best remaining names and the shared threshold rises early;
            // chunks past the cut reduce to the bonus-only check.
            const uint32_t span = sortedEnd - probeEnd;
            const uint32_t chunk = std::max<uint32_t>(4096, span / 256);
            std::atomic<uint32_t> nextChunk{0};
            std::vector<std::vector<uint32_t>> partMatches(workers);
            std::vector<size_t> partCounts(workers, 0);
            std::vector<TopN> partTops(workers, TopN(limit));
            std::vector<uint8_t> partCut(workers, 0);
            std::vector<uint32_t> partFirstCut(workers, sortedEnd);
            auto run = [&](unsigned w) {
                bool partWasCut = false;
                for (;;) {
                    const uint64_t first = static_cast<uint64_t>(nextChunk.fetch_add(1)) * chunk;
                    if (first >= span) {
                        break;
                    }
                    const auto s0 = static_cast<uint32_t>(probeEnd + first);
                    const auto s1 = static_cast<uint32_t>(std::min<uint64_t>(sortedEnd, s0 + uint64_t{chunk}));
                    bool chunkCut = false;
                    uint32_t chunkCutAt = s1;
                    ScanSlots(ctx, s0, s1, true, partMatches[w], partCounts[w], partTops[w], chunkCut, &chunkCutAt);
                    if (chunkCut) {
                        partWasCut = true;
                        partFirstCut[w] = std::min(partFirstCut[w], chunkCutAt);
                    }
                }
                partCut[w] = partWasCut ? 1 : 0;
            };
            SearchWorkerPool::Instance().Run(workers, run);
            for (unsigned w = 0; w < workers; ++w) {
                matches.insert(matches.end(), partMatches[w].begin(), partMatches[w].end());
                matchCount += partCounts[w];
                cut = cut || partCut[w] != 0;
                for (const Hit& hit : partTops[w].Items()) {
                    top.Offer(hit);
                }
            }
            if (cut) {
                // Every chunk before the earliest cut point was scanned in
                // full, so that point bounds the covered ranked prefix.
                // Matches recorded past it come from partial chunks: drop
                // them (the resumed scan revisits those slots), but keep the
                // matches from before probeEnd and the unsorted tail.
                covered = *std::min_element(partFirstCut.begin(), partFirstCut.end());
                const bool complete = matches.size() == matchCount;
                std::erase_if(matches, [&](uint32_t slot) { return slot >= covered && slot < sortedEnd; });
                matchCount = matches.size();
                resumable = complete;
            }
        }
    }
    if (!incremental) {
        // Names added since the last reorder (journal) are unsorted: scan all
        // (a narrowed query already re-checked their recorded matches).
        bool unused = false;
        ScanSlots(ctx, sortedEnd, slotCount, false, matches, matchCount, top, unused);
    }

    std::vector<Hit> result = std::move(top.Items());
    std::sort(result.begin(), result.end(), Better);
    if (state != nullptr) {
        state->lastWasIncremental = incremental;
        state->lastMatchCount = matchCount;
        state->lastMatchCountExact = !cut;
        state->folded = query.folded;
        state->generation = m_generation;
        // Truncated lists (or a non-prefix cut) cannot be narrowed.
        state->valid = resumable && matches.size() == matchCount;
        state->resumeSlot = covered;
        state->slots = std::move(matches);
    }
    return result;
}

}  // namespace filesearch
