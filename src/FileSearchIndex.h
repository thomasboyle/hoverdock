#pragma once

// Portable in-memory file-name index behind dock Search (no Windows headers,
// so it is unit-tested and benchmarked off-target). FileSearch.cpp feeds it
// from the NTFS MFT (FSCTL_ENUM_USN_DATA) and keeps it live from the USN
// change journal. Not thread-safe: the owner serialises writers vs readers.
//
// Layout (struct-of-arrays, ~10 B/entry + ~8 B/slot + UTF-8 names):
//   entries  stable ids; parent links, flags, zone, current name slot
//   slots    names in one contiguous '\0'-separated UTF-8 buffer, in
//            insertion order, so a query is one linear SIMD scan
//   renames append a fresh slot; dead slots are compacted lazily.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace filesearch {

enum EntryFlag : uint8_t {
    kFlagDirectory = 1U << 0,
    kFlagHidden = 1U << 1,
    kFlagSystem = 1U << 2,
    kFlagDeleted = 1U << 3,
    kFlagReparse = 1U << 4,
};

// Coarse location class used for ranking. Inherited by every descendant
// unless a descendant carries its own marker (see SetZoneMarker).
enum class Zone : uint8_t {
    Normal = 0,
    UserFolder,    // Desktop, Documents, Downloads, Pictures, Music, Videos
    UserHome,      // %USERPROFILE% (outside the folders above / AppData)
    ProgramFiles,
    AppData,
    Windows,
    Hidden,        // hidden or system directory (or below one)
    Excluded,      // NTFS metadata, $Recycle.Bin, System Volume Information, orphans
};

struct Hit {
    uint32_t entry = 0;
    int32_t score = 0;
};

// Folds one code point for case-insensitive matching. Simple (length
// preserving in UTF-8) folding: ASCII, Latin-1, Latin Extended-A, Greek,
// Cyrillic. Everything else maps to itself.
[[nodiscard]] uint32_t FoldCodePoint(uint32_t cp) noexcept;
// Lower-cases (FoldCodePoint) a UTF-8 string; invalid bytes pass through.
[[nodiscard]] std::string FoldUtf8(std::string_view text);
// UTF-16 -> UTF-8 append (unpaired surrogates become U+FFFD).
void AppendUtf8(std::string& out, const char16_t* text, size_t length);

class FileIndex;

// Per-session query cache: when a query extends the previous one (typing),
// only the previous matches are re-checked instead of rescanning all names.
struct SearchState {
    std::string folded;
    uint64_t generation = ~0ULL;
    std::vector<uint32_t> slots;   // every slot that matched `folded`
    bool valid = false;
    // Diagnostics from the last Search call.
    bool lastWasIncremental = false;
    size_t lastMatchCount = 0;
};

class FileIndex {
public:
    static constexpr uint32_t kNone = 0xFFFFFFFFU;

    // ---- building ----------------------------------------------------------
    // Registers a volume and its root directory entry ("C:"). `rootId` is the
    // file id of the root directory (NTFS: record 5).
    uint8_t AddVolume(std::string_view rootName, uint64_t rootId);
    void Reserve(size_t entries, size_t nameBytes);
    // Adds one record. Parents may arrive after children (MFT order); links
    // are resolved by FinishVolume. Ids are 64-bit file reference numbers.
    void Add(uint8_t volume, uint64_t id, uint64_t parentId, uint8_t flags, std::string_view nameUtf8);
    void AddUtf16(uint8_t volume, uint64_t id, uint64_t parentId, uint8_t flags, const char16_t* name,
        size_t length);
    // Resolves parent links of everything added since AddVolume. Records whose
    // parent never appeared are orphans and become Zone::Excluded.
    void FinishVolume(uint8_t volume);
    // Extra name for an already indexed record under another directory (NTFS
    // hard link: MFT enumeration reports one name per record). Aliases are not
    // tracked by the journal; they are rebuilt with the index. Returns false
    // when the parent is not indexed or the record already sits there.
    bool AddAlias(uint8_t volume, uint64_t id, uint64_t parentId, uint8_t flags, std::string_view nameUtf8);

    // ---- incremental updates (USN journal) -----------------------------------
    // Create, rename or move. A record whose parent is not indexed is ignored
    // (and removed when it was indexed: moved out of the indexed scope).
    void Upsert(uint8_t volume, uint64_t id, uint64_t parentId, uint8_t flags, std::string_view nameUtf8);
    void Remove(uint8_t volume, uint64_t id);
    // Rebuilds the name buffer when dead slots waste > 25% (and > 4 MB).
    void CompactIfWasteful();
    // Releases build-time over-allocation (vector growth slack).
    void ShrinkToFit();

    // ---- zones -----------------------------------------------------------------
    // Marks a directory (by file id) with an explicit zone; RecomputeZones
    // propagates markers to every descendant.
    void SetZoneMarker(uint8_t volume, uint64_t id, Zone zone);
    void RecomputeZones();

    // ---- queries ------------------------------------------------------------------
    // Case-insensitive AND of whitespace separated terms against file names.
    // Returns the best `limit` hits, best first. `state` (optional) enables
    // narrowing when the query extends the previous one.
    [[nodiscard]] std::vector<Hit> Search(std::string_view queryUtf8, size_t limit,
        SearchState* state = nullptr, unsigned threads = 1) const;

    [[nodiscard]] size_t EntryCount() const noexcept { return m_parent.size(); }
    [[nodiscard]] size_t LiveEntryCount() const noexcept { return m_liveEntries; }
    [[nodiscard]] size_t NameBytes() const noexcept { return m_names.size(); }
    [[nodiscard]] size_t MemoryBytes() const noexcept;
    [[nodiscard]] uint64_t Generation() const noexcept { return m_generation; }

    [[nodiscard]] std::string_view Name(uint32_t entry) const noexcept;
    [[nodiscard]] std::string Path(uint32_t entry) const;
    [[nodiscard]] uint32_t Parent(uint32_t entry) const noexcept { return m_parent[entry]; }
    [[nodiscard]] uint8_t Flags(uint32_t entry) const noexcept { return m_flags[entry]; }
    [[nodiscard]] Zone ZoneOf(uint32_t entry) const noexcept { return static_cast<Zone>(m_zone[entry]); }
    [[nodiscard]] uint32_t Find(uint8_t volume, uint64_t id) const noexcept;
    // Directory levels below the volume root (capped).
    [[nodiscard]] int Depth(uint32_t entry) const noexcept;

private:
    struct Volume {
        std::string rootName;
        uint32_t rootEntry = kNone;
        uint64_t rootRecord = 0;
        // NTFS record number (low 48 bits of the id) -> entry. Dense: MFT
        // record numbers are compact, so this beats a hash map ~10x in memory.
        std::vector<uint32_t> byRecord;
        uint32_t firstPending = 0;  // entries added since AddVolume
    };

    static uint64_t Record(uint64_t id) noexcept { return id & 0x0000FFFFFFFFFFFFULL; }
    uint32_t NewEntry(uint8_t volume, uint32_t parent, uint8_t flags, std::string_view name);
    uint32_t AppendSlot(uint32_t entry, std::string_view name);
    void MapRecord(Volume& volume, uint64_t record, uint32_t entry);
    [[nodiscard]] uint8_t ZoneForNew(uint32_t parent, uint8_t flags, std::string_view name,
        bool parentIsRoot) const noexcept;

    // entries
    std::vector<uint32_t> m_parent;
    std::vector<uint32_t> m_slot;
    std::vector<uint8_t> m_flags;
    std::vector<uint8_t> m_zone;
    std::vector<uint8_t> m_volume;
    // build-time only: unresolved parent record per pending entry
    std::vector<uint64_t> m_pendingParent;
    // slots
    std::vector<uint32_t> m_slotStart;  // size = slots + 1 (sentinel = m_names.size())
    std::vector<uint32_t> m_slotEntry;  // kNone when dead
    std::string m_names;                // name bytes + '\0' per slot
    size_t m_deadNameBytes = 0;

    std::vector<Volume> m_volumes;
    std::unordered_map<uint64_t, uint8_t> m_zoneMarkers;  // (volume << 56 | record) -> Zone
    size_t m_liveEntries = 0;
    uint64_t m_generation = 0;
};

}  // namespace filesearch
