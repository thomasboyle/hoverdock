#include "FileSearchIndex.h"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstring>
#include <thread>
#include <utility>

#if defined(_M_X64) || defined(_M_AMD64) || defined(__SSE2__)
#define FILESEARCH_SSE2 1
#include <emmintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
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

bool ExtensionIs(std::string_view name, std::string_view lowerExt) noexcept {
    if (name.size() <= lowerExt.size()) {
        return false;
    }
    const std::string_view tail = name.substr(name.size() - lowerExt.size());
    for (size_t i = 0; i < tail.size(); ++i) {
        if (AsciiFold(static_cast<unsigned char>(tail[i])) != static_cast<unsigned char>(lowerExt[i])) {
            return false;
        }
    }
    return true;
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
    std::vector<std::string_view> parts;
    std::vector<Token> tokens;
    size_t anchor = 0;             // index of the longest token (scanned)
};

bool ParseQuery(std::string_view text, Query& query) {
    const std::string folded = FoldUtf8(text);
    query.folded.clear();
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
        query.parts.emplace_back(query.folded.data() + pos, end - pos);
        pos = end + 1;
    }
    for (const std::string_view part : query.parts) {
        query.tokens.push_back(MakeToken(part));
        if (part.size() > query.parts[query.anchor].size()) {
            query.anchor = query.tokens.size() - 1;
        }
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
    const uint32_t root = NewEntry(index, kNone, kFlagDirectory, rootName);
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
    m_volume.reserve(total);
    m_slotStart.reserve(m_slotStart.size() + entries);
    m_slotEntry.reserve(m_slotEntry.size() + entries);
    m_pendingParent.reserve(entries);
    m_names.reserve(m_names.size() + nameBytes + entries);
}

uint32_t FileIndex::AppendSlot(uint32_t entry, std::string_view name) {
    // m_names = [names...][kPad zeros]; slot sentinel = logical end.
    const size_t logical = m_names.size() - kPad;
    m_names.resize(logical);
    m_names.append(name);
    m_names.push_back('\0');
    m_names.append(kPad, '\0');
    const auto slot = static_cast<uint32_t>(m_slotEntry.size());
    m_slotEntry.push_back(entry);
    m_slotStart.back() = static_cast<uint32_t>(logical);
    m_slotStart.push_back(static_cast<uint32_t>(m_names.size() - kPad));
    return slot;
}

uint32_t FileIndex::NewEntry(uint8_t volume, uint32_t parent, uint8_t flags, std::string_view name) {
    const auto entry = static_cast<uint32_t>(m_parent.size());
    m_parent.push_back(parent);
    m_flags.push_back(flags);
    m_zone.push_back(static_cast<uint8_t>(Zone::Normal));
    m_volume.push_back(volume);
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
    const uint32_t entry = NewEntry(volume, kNone, flags, nameUtf8);
    m_pendingParent.push_back(parentId);
    MapRecord(m_volumes[volume], record, entry);
}

void FileIndex::AddUtf16(uint8_t volume, uint64_t id, uint64_t parentId, uint8_t flags, const char16_t* name,
    size_t length) {
    // Fast path: pure ASCII names (the vast majority) skip the scratch string.
    char ascii[256];
    if (length <= sizeof(ascii)) {
        size_t i = 0;
        for (; i < length && name[i] < 0x80U; ++i) {
            ascii[i] = static_cast<char>(name[i]);
        }
        if (i == length) {
            Add(volume, id, parentId, flags, std::string_view(ascii, length));
            return;
        }
    }
    std::string utf8;
    utf8.reserve(length * 3);
    AppendUtf8(utf8, name, length);
    Add(volume, id, parentId, flags, utf8);
}

void FileIndex::FinishVolume(uint8_t volume) {
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
    static_cast<void>(NewEntry(volume, parent, flags, nameUtf8));
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
        entry = NewEntry(volume, parent, flags, nameUtf8);
        MapRecord(v, record, entry);
    } else {
        if ((m_flags[entry] & kFlagDeleted) != 0) {
            ++m_liveEntries;
        }
        m_parent[entry] = parent;
        m_flags[entry] = flags;
        if (Name(entry) != nameUtf8) {
            const uint32_t old = m_slot[entry];
            if (old != kNone) {
                m_slotEntry[old] = kNone;
                m_deadNameBytes += m_slotStart[old + 1] - m_slotStart[old];
            }
            m_slot[entry] = AppendSlot(entry, nameUtf8);
        }
    }
    m_zone[entry] = ZoneForNew(parent, m_flags[entry], nameUtf8, parent == v.rootEntry);
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
    if (slot != kNone) {
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
    std::string names;
    names.reserve(logical - m_deadNameBytes + kPad);
    std::vector<uint32_t> starts;
    std::vector<uint32_t> owners;
    starts.reserve(m_liveEntries + 1);
    owners.reserve(m_liveEntries);
    for (uint32_t& slot : m_slot) {
        slot = kNone;
    }
    for (size_t s = 0; s < m_slotEntry.size(); ++s) {
        const uint32_t entry = m_slotEntry[s];
        if (entry == kNone) {
            continue;
        }
        starts.push_back(static_cast<uint32_t>(names.size()));
        names.append(m_names, m_slotStart[s], m_slotStart[s + 1] - m_slotStart[s]);
        m_slot[entry] = static_cast<uint32_t>(owners.size());
        owners.push_back(entry);
    }
    starts.push_back(static_cast<uint32_t>(names.size()));
    names.append(kPad, '\0');
    m_names = std::move(names);
    m_slotStart = std::move(starts);
    m_slotEntry = std::move(owners);
    m_deadNameBytes = 0;
    ++m_generation;
}

void FileIndex::ShrinkToFit() {
    m_parent.shrink_to_fit();
    m_slot.shrink_to_fit();
    m_flags.shrink_to_fit();
    m_zone.shrink_to_fit();
    m_volume.shrink_to_fit();
    m_slotStart.shrink_to_fit();
    m_slotEntry.shrink_to_fit();
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
        const std::string folded = FoldUtf8(name);
        if ((!folded.empty() && folded[0] == '$') || folded == "system volume information" ||
            folded == "config.msi" || folded == "recovery") {
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
    std::unordered_map<uint32_t, uint8_t> markers;
    for (const auto& [key, zone] : m_zoneMarkers) {
        const auto volume = static_cast<uint8_t>(key >> 56);
        const uint32_t entry = Find(volume, key & 0x00FFFFFFFFFFFFFFULL);
        if (entry != kNone) {
            markers[entry] = zone;
        }
    }
    std::vector<uint8_t> done(m_parent.size(), 0);
    std::vector<uint32_t> chain;
    for (uint32_t start = 0; start < m_parent.size(); ++start) {
        if (done[start] != 0) {
            continue;
        }
        chain.clear();
        uint32_t x = start;
        while (done[x] == 0 && chain.size() < 4096) {
            chain.push_back(x);
            if (m_parent[x] == x) {
                break;
            }
            x = m_parent[x];
        }
        while (!chain.empty()) {
            const uint32_t e = chain.back();
            chain.pop_back();
            const uint32_t parent = m_parent[e];
            uint8_t zone = 0;
            if (const auto marker = markers.find(e); marker != markers.end()) {
                zone = marker->second;
            } else if (parent == e) {
                zone = static_cast<uint8_t>(Zone::Normal);
            } else if (done[parent] == 0) {
                zone = static_cast<uint8_t>(Zone::Excluded);  // cycle guard
            } else {
                zone = ZoneForNew(parent, m_flags[e], Name(e), m_parent[parent] == parent);
            }
            m_zone[e] = zone;
            done[e] = 1;
        }
    }
    ++m_generation;
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
    size_t bytes = m_parent.capacity() * 4 + m_slot.capacity() * 4 + m_flags.capacity() + m_zone.capacity() +
        m_volume.capacity() + m_slotStart.capacity() * 4 + m_slotEntry.capacity() * 4 + m_names.capacity() +
        m_pendingParent.capacity() * 8;
    for (const Volume& v : m_volumes) {
        bytes += v.byRecord.capacity() * 4;
    }
    return bytes;
}

// ---- search -------------------------------------------------------------------

namespace {

struct ScanContext {
    const FileIndex* index = nullptr;
    const Query* query = nullptr;
    const unsigned char* names = nullptr;
    const uint32_t* slotStart = nullptr;
    const uint32_t* slotEntry = nullptr;
    size_t slotCount = 0;
};

}  // namespace

enum class SlotResult { NoMatch, Matched, Scored };

int KindBonus(std::string_view name, uint8_t flags) noexcept {
    if ((flags & kFlagDirectory) != 0) {
        return 40;
    }
    if (ExtensionIs(name, ".lnk")) {
        return 120;
    }
    return ExtensionIs(name, ".exe") ? 60 : 0;
}

// Upper bound of ScoreSlot: every token at position 0, plus the whole-name
// bonus only when the length allows it (exact name, or stem ending in '.').
inline int ScoreCeiling(size_t tokens, std::string_view name, size_t queryLength, int kindBonus,
    Zone zone) noexcept {
    int ceiling = static_cast<int>(tokens) * 600 + kindBonus + ZoneBonus(zone) -
        static_cast<int>(std::min<size_t>(name.size(), 120)) * 2;
    if (name.size() == queryLength) {
        ceiling += 600;
    } else if (name.size() > queryLength && name[queryLength] == '.') {
        ceiling += 400;
    }
    return ceiling;
}

// Checks slot `slot` against every token except `skipToken` (already matched
// by the scan). Matching slots that cannot beat `threshold` are reported as
// Matched without paying for the full score (the common case for broad
// queries such as "a", where nearly every name matches).
static SlotResult ScoreSlot(const ScanContext& ctx, const FileIndex& index, uint32_t slot, size_t skipToken,
    int32_t threshold, Hit& hit) {
    const uint32_t entry = ctx.slotEntry[slot];
    if (entry == FileIndex::kNone) {
        return SlotResult::NoMatch;
    }
    const Zone zone = index.ZoneOf(entry);
    if (zone == Zone::Excluded) {
        return SlotResult::NoMatch;
    }
    const std::string_view name(reinterpret_cast<const char*>(ctx.names) + ctx.slotStart[slot],
        ctx.slotStart[slot + 1] - ctx.slotStart[slot] - 1);
    const std::vector<Token>& tokens = ctx.query->tokens;
    for (size_t t = 0; t < tokens.size(); ++t) {
        if (t != skipToken && FindIn(name, tokens[t]) == std::string_view::npos) {
            return SlotResult::NoMatch;
        }
    }
    const int kindBonus = KindBonus(name, index.Flags(entry));
    if (threshold != INT32_MIN &&
        ScoreCeiling(tokens.size(), name, ctx.query->folded.size(), kindBonus, zone) < threshold) {
        return SlotResult::Matched;
    }
    int score = 0;
    for (const Token& token : tokens) {
        size_t pos = FindIn(name, token);
        int best = 100;
        while (pos != std::string_view::npos) {
            if (pos == 0) {
                best = 600;
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
        score += 600;
    } else if (const size_t dot = name.rfind('.'); dot != std::string_view::npos && dot > 0 &&
        EqualsFolded(name.substr(0, dot), folded)) {
        score += 400;
    }
    score -= static_cast<int>(std::min<size_t>(name.size(), 120)) * 2;
    // Prefer shallow paths among equal names (C:\\Tools\\x over deep copies).
    score -= std::max(0, index.Depth(entry) - 3) * 12;
    score += kindBonus + ZoneBonus(zone);
    hit.entry = entry;
    hit.score = score;
    return SlotResult::Scored;
}

// Linear scan of slots [s0, s1) for the anchor token. Every matching slot is
// appended to `matches` and offered to `top`.
static void ScanSlots(const ScanContext& ctx, const FileIndex& index, uint32_t s0, uint32_t s1,
    std::vector<uint32_t>& matches, size_t& matchCount, TopN& top) {
    if (s0 >= s1) {
        return;
    }
    const Token& token = ctx.query->tokens[ctx.query->anchor];
    const unsigned char* names = ctx.names;
    const uint32_t* starts = ctx.slotStart;
    // Narrowing only pays for small match sets (see Search), so stop
    // recording once this part exceeds that share.
    const size_t recordLimit = (s1 - s0) / 8 + 1;
    const size_t begin = starts[s0];
    const size_t end = starts[s1];
    uint32_t slot = s0;

    // Returns the position to resume scanning from (start of the next slot).
    auto onCandidate = [&](size_t pos) -> size_t {
        if (starts[slot + 1] <= pos) {
            if (starts[slot + 2] > pos) {
                ++slot;
            } else {
                slot = static_cast<uint32_t>(
                    std::upper_bound(starts + slot + 1, starts + s1 + 1, static_cast<uint32_t>(pos)) - starts - 1);
            }
        }
        Hit hit;
        const SlotResult result = ScoreSlot(ctx, index, slot, ctx.query->anchor, top.Threshold(), hit);
        if (result != SlotResult::NoMatch) {
            if (matches.size() <= recordLimit) {
                matches.push_back(slot);
            }
            ++matchCount;
            if (result == SlotResult::Scored) {
                top.Offer(hit);
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
        while (pos <= lastStart) {
            const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p1 + pos));
            const __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p2 + pos));
            const __m128i eq = _mm_and_si128(_mm_cmpeq_epi8(_mm_or_si128(a, m1), v1),
                _mm_cmpeq_epi8(_mm_or_si128(b, m2), v2));
            unsigned mask = static_cast<unsigned>(_mm_movemask_epi8(eq));
            size_t next = pos + 16;
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

std::vector<Hit> FileIndex::Search(std::string_view queryUtf8, size_t limit, SearchState* state,
    unsigned threads) const {
    Query query;
    if (!ParseQuery(queryUtf8, query) || m_slotEntry.empty()) {
        if (state != nullptr) {
            state->valid = false;
        }
        return {};
    }
    ScanContext ctx;
    ctx.index = this;
    ctx.query = &query;
    ctx.names = reinterpret_cast<const unsigned char*>(m_names.data());
    ctx.slotStart = m_slotStart.data();
    ctx.slotEntry = m_slotEntry.data();
    ctx.slotCount = m_slotEntry.size();

    std::vector<uint32_t> matches;
    size_t matchCount = 0;
    TopN top(limit);
    // Narrow the previous result when this query extends it, unless that
    // result is so large a SIMD rescan is cheaper than per-name checks.
    const bool incremental = state != nullptr && state->valid && state->generation == m_generation &&
        !state->folded.empty() && query.folded.size() >= state->folded.size() &&
        query.folded.compare(0, state->folded.size(), state->folded) == 0 &&
        state->slots.size() < ctx.slotCount / 8;
    if (incremental) {
        matches.reserve(state->slots.size());
        for (const uint32_t slot : state->slots) {
            Hit hit;
            const SlotResult result = ScoreSlot(ctx, *this, slot, SIZE_MAX, top.Threshold(), hit);
            if (result != SlotResult::NoMatch) {
                matches.push_back(slot);
                ++matchCount;
                if (result == SlotResult::Scored) {
                    top.Offer(hit);
                }
            }
        }
    } else {
        const auto slotCount = static_cast<uint32_t>(ctx.slotCount);
        unsigned workers = std::max(1U, threads);
        if (slotCount < 200000U) {
            workers = 1;
        }
        if (workers == 1) {
            ScanSlots(ctx, *this, 0, slotCount, matches, matchCount, top);
        } else {
            std::vector<std::vector<uint32_t>> partMatches(workers);
            std::vector<size_t> partCounts(workers, 0);
            std::vector<TopN> partTops(workers, TopN(limit));
            std::vector<std::thread> pool;
            pool.reserve(workers - 1);
            auto run = [&](unsigned w) {
                const uint32_t s0 = static_cast<uint32_t>(static_cast<uint64_t>(slotCount) * w / workers);
                const uint32_t s1 = static_cast<uint32_t>(static_cast<uint64_t>(slotCount) * (w + 1) / workers);
                ScanSlots(ctx, *this, s0, s1, partMatches[w], partCounts[w], partTops[w]);
            };
            for (unsigned w = 1; w < workers; ++w) {
                pool.emplace_back(run, w);
            }
            run(0);
            for (std::thread& t : pool) {
                t.join();
            }
            size_t total = 0;
            for (unsigned w = 0; w < workers; ++w) {
                total += partMatches[w].size();
                matchCount += partCounts[w];
            }
            matches.reserve(total);
            for (unsigned w = 0; w < workers; ++w) {
                matches.insert(matches.end(), partMatches[w].begin(), partMatches[w].end());
                for (const Hit& hit : partTops[w].Items()) {
                    top.Offer(hit);
                }
            }
        }
    }

    std::vector<Hit> result = std::move(top.Items());
    std::sort(result.begin(), result.end(), Better);
    if (state != nullptr) {
        state->lastWasIncremental = incremental;
        state->lastMatchCount = matchCount;
        state->folded = query.folded;
        state->generation = m_generation;
        // Truncated lists cannot be narrowed; the next query rescans.
        state->valid = matches.size() == matchCount;
        state->slots = std::move(matches);
    }
    return result;
}

}  // namespace filesearch
