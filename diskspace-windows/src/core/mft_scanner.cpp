// The NTFS master-file-table backend.
//
// Directory enumeration costs one open and at least one read per directory. The
// MFT holds a record for every file and folder on the volume in one stream, so
// reading it start to finish replaces millions of small metadata reads with a few
// thousand large sequential ones — which is where the order-of-magnitude
// difference comes from. The price is raw volume access, so administrator rights,
// and NTFS.
//
// It also sees things directory enumeration cannot: the space a folder's own
// index occupies, alternate data streams, and the volume's metadata files. Those
// are real bytes on the disk, so they are counted, and the total for a volume
// scanned this way is legitimately larger than the same volume walked directory
// by directory.

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "file_kind.h"
#include "tree_scanner.h"
#include "win_util.h"

namespace diskscan {
namespace {

// ---- On-disk structures (NTFS, stable since 3.0) ----

constexpr uint32_t kAttrStandardInformation = 0x10;
constexpr uint32_t kAttrAttributeList = 0x20;
constexpr uint32_t kAttrFileName = 0x30;
constexpr uint32_t kAttrData = 0x80;
constexpr uint32_t kAttrIndexAllocation = 0xA0;
constexpr uint32_t kAttrEnd = 0xFFFFFFFF;

constexpr uint16_t kRecordInUse = 0x0001;
constexpr uint16_t kRecordIsDirectory = 0x0002;

/// The volume's own root directory is always record 5.
constexpr uint32_t kRootRecord = 5;

/// 8.3 short names duplicate a name we already have, so they are skipped unless
/// they are all a record offers.
constexpr uint8_t kNameSpaceDos = 2;

#pragma pack(push, 1)
struct FileRecordHeader {
    uint32_t magic;  // 'FILE'
    uint16_t updateSequenceOffset;
    uint16_t updateSequenceCount;
    uint64_t logSequenceNumber;
    uint16_t sequenceNumber;
    uint16_t hardLinkCount;
    uint16_t attributesOffset;
    uint16_t flags;
    uint32_t bytesInUse;
    uint32_t bytesAllocated;
    uint64_t baseFileRecord;
    uint16_t nextAttributeId;
};

struct AttributeHeader {
    uint32_t type;
    uint32_t length;
    uint8_t nonResident;
    uint8_t nameLength;
    uint16_t nameOffset;
    uint16_t flags;
    uint16_t attributeId;
};

struct ResidentAttribute {
    AttributeHeader header;
    uint32_t valueLength;
    uint16_t valueOffset;
    uint8_t indexedFlag;
    uint8_t padding;
};

struct NonResidentAttribute {
    AttributeHeader header;
    uint64_t lowestVcn;
    uint64_t highestVcn;
    uint16_t runListOffset;
    uint16_t compressionUnit;
    uint32_t padding;
    uint64_t allocatedSize;
    uint64_t dataSize;
    uint64_t initializedSize;
};

struct FileNameAttribute {
    uint64_t parentDirectory;
    uint64_t creationTime;
    uint64_t lastModified;
    uint64_t lastMftChange;
    uint64_t lastAccess;
    uint64_t allocatedSize;
    uint64_t realSize;
    uint32_t flags;
    uint32_t reparseOrEa;
    uint8_t nameLength;  // in characters
    uint8_t nameSpace;
    // wchar_t name[nameLength] follows
};

struct StandardInformation {
    uint64_t creationTime;
    uint64_t lastModified;
    uint64_t lastMftChange;
    uint64_t lastAccess;
    uint32_t fileAttributes;
};
#pragma pack(pop)

/// The low 48 bits of a file reference are the record number; the top 16 are a
/// sequence number that guards against a reused record, and are not an address.
inline uint64_t recordNumberOf(uint64_t reference) { return reference & 0x0000FFFFFFFFFFFFULL; }

/// Rewrites the last two bytes of every sector from the update sequence array.
///
/// NTFS stamps a single value into the tail of each sector of a multi-sector
/// structure and keeps the real bytes aside, so a torn write can be detected.
/// Undoing that is mandatory before the record can be read: without it, an
/// attribute that happens to straddle a sector boundary is silently corrupt.
bool applyFixups(uint8_t* record, size_t recordSize, uint32_t bytesPerSector) {
    auto* header = reinterpret_cast<FileRecordHeader*>(record);
    size_t offset = header->updateSequenceOffset;
    size_t count = header->updateSequenceCount;
    if (count == 0 || offset + count * 2 > recordSize) return false;
    const uint16_t* usa = reinterpret_cast<const uint16_t*>(record + offset);
    uint16_t stamp = usa[0];
    for (size_t i = 1; i < count; ++i) {
        size_t tail = i * bytesPerSector - 2;
        if (tail + 2 > recordSize) return false;
        uint16_t* slot = reinterpret_cast<uint16_t*>(record + tail);
        if (*slot != stamp) return false;  // torn or not a record we can trust
        *slot = usa[i];
    }
    return true;
}

struct Run {
    int64_t lcn;
    int64_t clusters;
};

/// Decodes a run list into absolute cluster runs. Offsets are stored as signed
/// deltas from the previous run's start, which is what makes this stateful.
bool decodeRunList(const uint8_t* p, const uint8_t* end, std::vector<Run>* runs) {
    int64_t lcn = 0;
    while (p < end && *p != 0) {
        uint8_t lengthSize = *p & 0x0F;
        uint8_t offsetSize = (*p >> 4) & 0x0F;
        ++p;
        if (lengthSize == 0 || lengthSize > 8 || offsetSize > 8) return false;
        if (p + lengthSize + offsetSize > end) return false;

        int64_t length = 0;
        for (int i = lengthSize - 1; i >= 0; --i) length = (length << 8) | p[i];
        p += lengthSize;

        if (offsetSize == 0) {
            // A sparse run: no clusters on disk, so nothing to read or count.
            p += offsetSize;
            continue;
        }
        int64_t delta = int8_t(p[offsetSize - 1]);
        for (int i = offsetSize - 2; i >= 0; --i) delta = (delta << 8) | p[i];
        p += offsetSize;

        lcn += delta;
        if (length <= 0) return false;
        runs->push_back({lcn, length});
    }
    return true;
}

/// Sequential reader over a non-resident stream described by a run list.
class RunStream {
public:
    RunStream(HANDLE volume, int64_t bytesPerCluster, std::vector<Run> runs)
        : volume_(volume), bytesPerCluster_(bytesPerCluster), runs_(std::move(runs)) {
        for (const Run& r : runs_) totalBytes_ += r.clusters * bytesPerCluster_;
    }

    int64_t totalBytes() const { return totalBytes_; }

    /// Reads `bytes` starting at `offset` within the stream. Both are cluster
    /// aligned by construction, which is also what the volume handle requires.
    bool read(int64_t offset, void* destination, size_t bytes) {
        auto* out = static_cast<uint8_t*>(destination);
        int64_t consumed = 0;
        for (const Run& run : runs_) {
            int64_t runBytes = run.clusters * bytesPerCluster_;
            if (offset >= consumed + runBytes) {
                consumed += runBytes;
                continue;
            }
            while (bytes > 0) {
                if (offset >= consumed + runBytes) break;
                int64_t within = offset - consumed;
                int64_t available = runBytes - within;
                size_t take = size_t(std::min<int64_t>(available, int64_t(bytes)));
                LARGE_INTEGER position;
                position.QuadPart = run.lcn * bytesPerCluster_ + within;
                if (!SetFilePointerEx(volume_, position, nullptr, FILE_BEGIN)) return false;
                DWORD got = 0;
                if (!ReadFile(volume_, out, DWORD(take), &got, nullptr) || got != take) {
                    return false;
                }
                out += take;
                bytes -= take;
                offset += int64_t(take);
            }
            if (bytes == 0) return true;
            consumed += runBytes;
        }
        return bytes == 0;
    }

private:
    HANDLE volume_;
    int64_t bytesPerCluster_;
    std::vector<Run> runs_;
    int64_t totalBytes_ = 0;
};

/// What one pass over the MFT extracts for each record. Struct-of-arrays for the
/// same reason the node arena is: at several million records the padding of an
/// array of structs is measured in tens of megabytes.
struct RecordTable {
    std::vector<uint32_t> parent;
    std::vector<uint32_t> nameOffset;
    std::vector<uint16_t> nameLength;
    std::vector<uint8_t> flags;  // bit 0 usable, bit 1 directory
    std::vector<int64_t> allocated;
    std::vector<int64_t> logical;
    std::vector<uint32_t> created;
    std::vector<wchar_t> names;

    static constexpr uint8_t kUsable = 1 << 0;
    static constexpr uint8_t kDirectory = 1 << 1;

    void resize(size_t n) {
        parent.assign(n, 0);
        nameOffset.assign(n, 0);
        nameLength.assign(n, 0);
        flags.assign(n, 0);
        allocated.assign(n, 0);
        logical.assign(n, 0);
        created.assign(n, 0);
        names.reserve(n * 12);
    }

    void release() {
        RecordTable empty;
        parent.swap(empty.parent);
        nameOffset.swap(empty.nameOffset);
        nameLength.swap(empty.nameLength);
        flags.swap(empty.flags);
        allocated.swap(empty.allocated);
        logical.swap(empty.logical);
        created.swap(empty.created);
        names.swap(empty.names);
    }
};

/// Clusters a run list actually occupies, skipping its holes.
int64_t occupiedClusters(const uint8_t* p, const uint8_t* end) {
    std::vector<Run> runs;
    if (!decodeRunList(p, end, &runs)) return 0;
    int64_t clusters = 0;
    for (const Run& r : runs) clusters += r.clusters;
    return clusters;
}

/// Pulls everything the tree needs out of one file record.
void parseRecord(uint8_t* record, size_t recordSize, uint32_t recordNumber, RecordTable& table,
                 int64_t bytesPerCluster, bool* isExtension, uint32_t* baseRecord) {
    auto* header = reinterpret_cast<FileRecordHeader*>(record);
    *isExtension = false;
    *baseRecord = 0;

    if (header->magic != 0x454C4946) return;  // 'FILE'
    if (!(header->flags & kRecordInUse)) return;
    if (header->attributesOffset >= recordSize) return;

    if (header->baseFileRecord != 0) {
        // An extension record: its attributes belong to the base record, which is
        // somewhere else in this same pass. Its sizes are folded into the base
        // rather than followed, so a fragmented file needs no random reads.
        *isExtension = true;
        *baseRecord = uint32_t(recordNumberOf(header->baseFileRecord));
    }

    const bool isDirectory = (header->flags & kRecordIsDirectory) != 0;
    const uint32_t target = *isExtension ? *baseRecord : recordNumber;
    if (target >= table.flags.size()) return;

    int64_t allocated = 0;
    int64_t logical = 0;
    bool haveLogical = false;
    uint32_t created = 0;
    uint64_t parentReference = 0;
    const wchar_t* name = nullptr;
    size_t nameLength = 0;
    uint8_t nameSpace = 0xFF;

    size_t offset = header->attributesOffset;
    const size_t limit = std::min<size_t>(recordSize, header->bytesInUse);
    while (offset + sizeof(AttributeHeader) <= limit) {
        auto* attribute = reinterpret_cast<AttributeHeader*>(record + offset);
        if (attribute->type == kAttrEnd) break;
        if (attribute->length < sizeof(AttributeHeader) || offset + attribute->length > limit) {
            break;
        }

        const bool unnamed = attribute->nameLength == 0;
        if (!attribute->nonResident) {
            auto* resident = reinterpret_cast<ResidentAttribute*>(attribute);
            uint8_t* value = record + offset + resident->valueOffset;
            size_t valueLength = resident->valueLength;
            if (size_t(resident->valueOffset) + valueLength > attribute->length) {
                offset += attribute->length;
                continue;
            }
            if (attribute->type == kAttrStandardInformation &&
                valueLength >= sizeof(StandardInformation)) {
                auto* info = reinterpret_cast<StandardInformation*>(value);
                created = epochSecondsFromFileTime(int64_t(info->creationTime));
            } else if (attribute->type == kAttrFileName &&
                       valueLength >= sizeof(FileNameAttribute)) {
                auto* fileName = reinterpret_cast<FileNameAttribute*>(value);
                size_t candidateLength = fileName->nameLength;
                if (sizeof(FileNameAttribute) + candidateLength * 2 <= valueLength) {
                    // A record may carry several names: one per hard link, plus an
                    // 8.3 alias. The first non-DOS name wins, so the file is
                    // counted and shown once, at one of the paths it really has.
                    bool better = (nameSpace == 0xFF) ||
                                  (nameSpace == kNameSpaceDos && fileName->nameSpace != kNameSpaceDos);
                    if (better) {
                        nameSpace = fileName->nameSpace;
                        parentReference = fileName->parentDirectory;
                        name = reinterpret_cast<const wchar_t*>(value + sizeof(FileNameAttribute));
                        nameLength = candidateLength;
                    }
                }
            } else if (attribute->type == kAttrData) {
                // Small files live inside the record itself and occupy no cluster
                // of their own, which is exactly what `allocated` should say.
                if (unnamed && !haveLogical) {
                    logical = int64_t(valueLength);
                    haveLogical = true;
                }
            }
            offset += attribute->length;
            continue;
        }

        auto* nonResident = reinterpret_cast<NonResidentAttribute*>(attribute);
        // Named $DATA streams are alternate data streams. They occupy real
        // clusters, so they count towards what the file holds, while the length
        // shown for the file stays the length of its main stream. A folder's
        // $INDEX_ALLOCATION is a real allocation too — one that walking the
        // directory tree cannot see, and that is free to count here.
        const bool counts = attribute->type == kAttrData ||
                            (attribute->type == kAttrIndexAllocation && isDirectory);
        if (counts) {
            // Measured from the run list rather than read out of the header, and
            // on every extent rather than only the first.
            //
            // The header's allocated size is what the attribute would occupy if
            // it were laid out flat, which is simply wrong for anything sparse.
            // `$BadClus:$Bad` is the extreme case: a hole spanning the entire
            // volume, whose header therefore claims the whole capacity of the
            // disk. The sparse flag is not a reliable way to spot those either —
            // `$BadClus` does not set it. Counting the runs sidesteps the
            // question: a hole has no runs, so it contributes nothing, and a
            // compressed attribute's runs are the compressed ones, so it reports
            // what it really holds.
            const uint8_t* runs = record + offset + nonResident->runListOffset;
            const uint8_t* runsEnd = record + offset + attribute->length;
            if (runs < runsEnd && runsEnd <= record + recordSize) {
                allocated += occupiedClusters(runs, runsEnd) * bytesPerCluster;
            }
        }
        if (attribute->type == kAttrData && unnamed && nonResident->lowestVcn == 0 &&
            !haveLogical) {
            logical = int64_t(nonResident->dataSize);
            haveLogical = true;
        }
        offset += attribute->length;
    }

    table.allocated[target] += allocated;
    if (haveLogical && !*isExtension) table.logical[target] += logical;

    if (*isExtension) return;

    if (name && nameLength > 0) {
        uint32_t parentRecord = uint32_t(recordNumberOf(parentReference));
        if (parentRecord < table.flags.size()) {
            table.parent[recordNumber] = parentRecord;
            table.nameOffset[recordNumber] = uint32_t(table.names.size());
            table.nameLength[recordNumber] = uint16_t(std::min<size_t>(nameLength, 0xFFFF));
            table.names.insert(table.names.end(), name, name + nameLength);
            table.created[recordNumber] = created;
            table.flags[recordNumber] =
                RecordTable::kUsable | (isDirectory ? RecordTable::kDirectory : 0);
        }
    }
}

int compareNoCase(const wchar_t* a, size_t aLen, const std::wstring& b) {
    if (aLen != b.size()) return 1;
    return CompareStringOrdinal(a, int(aLen), b.c_str(), int(b.size()), TRUE) == CSTR_EQUAL ? 0 : 1;
}

}  // namespace

// ---- The backend proper ----

ScanResult TreeScanner::runMft(const std::wstring& root, int64_t startTicks, bool* handled) {
    *handled = false;
    fastScanUnavailable_ = false;
    fastScanUnavailableReason_.clear();

    const bool asked = options_.backend == ScanBackend::Mft;
    if (options_.backend == ScanBackend::Enum) return ScanResult();

    VolumeInfo volume;
    if (!volumeInfoForPath(root, &volume)) {
        if (asked) {
            fastScanUnavailable_ = true;
            fastScanUnavailableReason_ = L"the volume could not be identified";
        }
        return ScanResult();
    }
    if (!volume.supportsFastScan) {
        if (asked) {
            fastScanUnavailable_ = true;
            fastScanUnavailableReason_ = L"the volume is " + volume.fileSystem + L", not NTFS";
        }
        return ScanResult();
    }
    if (!isElevated()) {
        // Worth saying even when the backend was not explicitly asked for: it is
        // what the app offers to relaunch for.
        fastScanUnavailable_ = true;
        fastScanUnavailableReason_ = L"reading the master file table needs administrator rights";
        return ScanResult();
    }
    // Left to the caller to ask for on a subfolder. Reading the whole table to
    // show one folder is slower than just walking that folder, and the automatic
    // choice should never be the slower one.
    const bool wholeVolume = isVolumeRoot(root);
    if (!asked && !wholeVolume) return ScanResult();

    std::wstring devicePath = L"\\\\.\\" + volume.mountPoint.substr(0, 2);
    HANDLE device = CreateFileW(devicePath.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                                nullptr);
    if (device == INVALID_HANDLE_VALUE) {
        fastScanUnavailable_ = true;
        fastScanUnavailableReason_ = L"the volume could not be opened for raw reading";
        return ScanResult();
    }

    NTFS_VOLUME_DATA_BUFFER volumeData{};
    DWORD returned = 0;
    if (!DeviceIoControl(device, FSCTL_GET_NTFS_VOLUME_DATA, nullptr, 0, &volumeData,
                         sizeof(volumeData), &returned, nullptr)) {
        CloseHandle(device);
        fastScanUnavailable_ = true;
        fastScanUnavailableReason_ = L"the volume did not report its master file table";
        return ScanResult();
    }

    const int64_t bytesPerCluster = volumeData.BytesPerCluster;
    const uint32_t bytesPerSector = volumeData.BytesPerSector;
    const size_t recordSize = size_t(volumeData.BytesPerFileRecordSegment);
    if (bytesPerCluster <= 0 || bytesPerSector == 0 || recordSize < sizeof(FileRecordHeader)) {
        CloseHandle(device);
        fastScanUnavailable_ = true;
        fastScanUnavailableReason_ = L"the volume reported an unusable geometry";
        return ScanResult();
    }

    // Record 0 describes the master file table itself, and its run list is what
    // makes the rest of it reachable.
    std::vector<uint8_t> firstRecord(recordSize);
    LARGE_INTEGER position;
    position.QuadPart = volumeData.MftStartLcn.QuadPart * bytesPerCluster;
    DWORD got = 0;
    if (!SetFilePointerEx(device, position, nullptr, FILE_BEGIN) ||
        !ReadFile(device, firstRecord.data(), DWORD(recordSize), &got, nullptr) ||
        got != recordSize || !applyFixups(firstRecord.data(), recordSize, bytesPerSector)) {
        CloseHandle(device);
        fastScanUnavailable_ = true;
        fastScanUnavailableReason_ = L"the first master file table record could not be read";
        return ScanResult();
    }

    std::vector<Run> mftRuns;
    {
        auto* header = reinterpret_cast<FileRecordHeader*>(firstRecord.data());
        size_t offset = header->attributesOffset;
        const size_t limit = std::min<size_t>(recordSize, header->bytesInUse);
        while (offset + sizeof(AttributeHeader) <= limit) {
            auto* attribute = reinterpret_cast<AttributeHeader*>(firstRecord.data() + offset);
            if (attribute->type == kAttrEnd || attribute->length < sizeof(AttributeHeader)) break;
            if (attribute->type == kAttrData && attribute->nonResident &&
                attribute->nameLength == 0) {
                auto* nonResident = reinterpret_cast<NonResidentAttribute*>(attribute);
                const uint8_t* runs = firstRecord.data() + offset + nonResident->runListOffset;
                const uint8_t* end = firstRecord.data() + offset + attribute->length;
                decodeRunList(runs, end, &mftRuns);
                break;
            }
            offset += attribute->length;
        }
    }
    if (mftRuns.empty()) {
        CloseHandle(device);
        fastScanUnavailable_ = true;
        fastScanUnavailableReason_ = L"the master file table's extents could not be decoded";
        return ScanResult();
    }

    backendUsed_ = ScanBackend::Mft;
    *handled = true;

    RunStream stream(device, bytesPerCluster, std::move(mftRuns));
    const int64_t validBytes = std::min<int64_t>(volumeData.MftValidDataLength.QuadPart,
                                                 stream.totalBytes());
    const size_t recordCount = size_t(validBytes / int64_t(recordSize));

    RecordTable table;
    table.resize(recordCount);

    // Read in large sequential chunks. The whole point of this backend is that
    // the disk sees one long streaming read instead of millions of small ones.
    size_t chunkRecords = std::max<size_t>(1, (4u << 20) / recordSize);
    std::vector<uint8_t> chunk(chunkRecords * recordSize);
    int64_t parsedRecords = 0;
    int64_t filesSeen = 0;

    for (size_t base = 0; base < recordCount; base += chunkRecords) {
        {
            std::lock_guard<std::mutex> lock(stopMutex_);
            if (stopRequested_) break;
        }
        size_t take = std::min(chunkRecords, recordCount - base);
        if (!stream.read(int64_t(base) * int64_t(recordSize), chunk.data(), take * recordSize)) {
            errorTotal_ += int64_t(take);
            continue;
        }
        for (size_t i = 0; i < take; ++i) {
            uint8_t* record = chunk.data() + i * recordSize;
            if (!applyFixups(record, recordSize, bytesPerSector)) continue;
            bool isExtension = false;
            uint32_t baseRecord = 0;
            parseRecord(record, recordSize, uint32_t(base + i), table, bytesPerCluster,
                        &isExtension, &baseRecord);
        }
        parsedRecords += int64_t(take);
        if (options_.progress) {
            ScanProgress p;
            p.nodes = parsedRecords;
            p.files = filesSeen;
            options_.progress(p);
        }
    }
    CloseHandle(device);

    // ---- Bucket children by parent ----
    //
    // A counting sort rather than a map: every record's parent is already a dense
    // integer, so children can be grouped in two linear passes with no hashing and
    // no per-directory allocation.
    std::vector<uint32_t> childStart(recordCount + 1, 0);
    for (size_t i = 0; i < recordCount; ++i) {
        if (!(table.flags[i] & RecordTable::kUsable)) continue;
        if (i == kRootRecord) continue;
        ++childStart[table.parent[i] + 1];
    }
    for (size_t i = 0; i < recordCount; ++i) childStart[i + 1] += childStart[i];
    std::vector<uint32_t> childList(childStart[recordCount]);
    {
        std::vector<uint32_t> cursor(childStart.begin(), childStart.end() - 1);
        for (size_t i = 0; i < recordCount; ++i) {
            if (!(table.flags[i] & RecordTable::kUsable)) continue;
            if (i == kRootRecord) continue;
            childList[cursor[table.parent[i]]++] = uint32_t(i);
        }
    }

    // ---- Locate the scan root inside the table ----
    uint32_t rootRecord = kRootRecord;
    {
        std::wstring relative = root.substr(volume.mountPoint.size() > 0
                                                ? volume.mountPoint.size() - 1
                                                : 0);
        size_t at = 0;
        while (at < relative.size() && rootRecord != UINT32_MAX) {
            while (at < relative.size() && relative[at] == L'\\') ++at;
            if (at >= relative.size()) break;
            size_t end = relative.find(L'\\', at);
            if (end == std::wstring::npos) end = relative.size();
            std::wstring component = relative.substr(at, end - at);
            uint32_t found = UINT32_MAX;
            for (uint32_t c = childStart[rootRecord]; c < childStart[rootRecord + 1]; ++c) {
                uint32_t child = childList[c];
                if (!(table.flags[child] & RecordTable::kDirectory)) continue;
                if (compareNoCase(table.names.data() + table.nameOffset[child],
                                  table.nameLength[child], component) == 0) {
                    found = child;
                    break;
                }
            }
            rootRecord = found;
            at = end;
        }
    }
    if (rootRecord == UINT32_MAX) {
        fastScanUnavailable_ = true;
        fastScanUnavailableReason_ = L"the folder was not found in the master file table";
        *handled = false;
        return ScanResult();
    }

    // ---- Build the arena, breadth first ----
    //
    // Breadth first is what makes a directory's children one contiguous run, and
    // it also guarantees a parent's index is below every one of its children's —
    // which is what lets the totals roll up in a single reverse sweep.
    struct Pending {
        uint32_t record;
        uint32_t node;  // where that record landed in the arena
    };
    std::vector<Pending> queue{{rootRecord, 0}};
    size_t head = 0;
    while (head < queue.size()) {
        uint32_t record = queue[head].record;
        size_t parentNode = queue[head].node;
        ++head;
        uint32_t begin = childStart[record];
        uint32_t end = childStart[record + 1];
        if (begin == end) continue;

        size_t base = arena_.reserveNodes(size_t(end - begin));
        arena_.childStart[parentNode] = int32_t(base);
        arena_.childCount[parentNode] = int32_t(end - begin);

        for (uint32_t c = begin; c < end; ++c) {
            uint32_t child = childList[c];
            size_t idx = base + (c - begin);
            const bool isDirectory = (table.flags[child] & RecordTable::kDirectory) != 0;
            size_t nameLength = table.nameLength[child];
            const wchar_t* name = table.names.data() + table.nameOffset[child];

            size_t nameSlot = arena_.allocateName(nameLength);
            arena_.writeName(name, nameLength, nameSlot);
            arena_.parent[idx] = int32_t(parentNode);
            arena_.nameOffset[idx] = int32_t(nameSlot);
            arena_.nameLength[idx] = uint16_t(nameLength);
            arena_.kind[idx] =
                isDirectory ? FileKind::Directory : FileKind::classify(name, nameLength);
            arena_.flags[idx] = isDirectory ? NodeFlags::IsDir : 0;
            arena_.allocated[idx] = table.allocated[child];
            arena_.logical[idx] = isDirectory ? 0 : table.logical[child];
            arena_.fileCount[idx] = isDirectory ? 0 : 1;
            arena_.childStart[idx] = 0;
            arena_.childCount[idx] = 0;
            arena_.addedTime[idx] = table.created[child];
            arena_.addedLastMonth[idx] = 0;
            arena_.addedLastSixMonths[idx] = 0;
            arena_.addedSinceLastScan[idx] = 0;

            if (isDirectory) {
                queue.push_back({child, uint32_t(idx)});
                ++directoryTotal_;
            } else {
                ++fileTotal_;
                ++filesSeen;
            }
        }
        arena_.publish();
    }

    // ---- Own figures, then one reverse sweep to roll them up ----
    const size_t nodeCount = arena_.uncheckedCount();
    for (size_t i = 1; i < nodeCount; ++i) {
        int64_t bytes = arena_.allocated[i];
        uint32_t created = arena_.addedTime[i];
        if (created > 0) {
            ++timestampedTotal_;
            if (created >= options_.cutoffLastSixMonths) {
                arena_.addedLastSixMonths[i] = bytes;
                if (created >= options_.cutoffLastMonth) arena_.addedLastMonth[i] = bytes;
            }
            if (options_.hasSinceLastScan && created >= options_.cutoffSinceLastScan) {
                arena_.addedSinceLastScan[i] = bytes;
            }
        }
        if (!arena_.isDirectory(i)) {
            size_t k = size_t(arena_.kind[i]);
            kindBytes_[k] += bytes;
            kindLogical_[k] += arena_.logical[i];
            kindCount_[k] += 1;
            kindMonth_[k] += arena_.addedLastMonth[i];
            kindSix_[k] += arena_.addedLastSixMonths[i];
            kindScan_[k] += arena_.addedSinceLastScan[i];
        }
    }
    for (size_t i = nodeCount; i-- > 1;) {
        size_t p = size_t(uint32_t(arena_.parent[i]));
        arena_.allocated[p] += arena_.allocated[i];
        arena_.logical[p] += arena_.logical[i];
        arena_.fileCount[p] += arena_.fileCount[i];
        arena_.addedLastMonth[p] += arena_.addedLastMonth[i];
        arena_.addedLastSixMonths[p] += arena_.addedLastSixMonths[i];
        arena_.addedSinceLastScan[p] += arena_.addedSinceLastScan[i];
    }
    arena_.publish();

    table.release();

    ScanResult result = makeResult(startTicks);
    result.backend = ScanBackend::Mft;
    result.files = fileTotal_;
    result.directories = directoryTotal_;
    result.syscalls = int64_t((recordCount + chunkRecords - 1) / chunkRecords);
    return result;
}

}  // namespace diskscan
