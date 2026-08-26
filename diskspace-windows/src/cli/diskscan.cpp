// diskscan — the scan engine on the command line, for scripting and for
// measuring. Everything the app does to a tree, this does without a window.

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "../core/file_kind.h"
#include "../core/tree_scanner.h"
#include "../core/win_util.h"

using namespace diskscan;

namespace {

std::string humanBytes(int64_t n) {
    static const char* units[] = {"B", "kB", "MB", "GB", "TB", "PB"};
    double v = double(n);
    int u = 0;
    while (v >= 1000 && u < 5) {
        v /= 1000;
        ++u;
    }
    char buf[64];
    if (u == 0) {
        snprintf(buf, sizeof(buf), "%lld B", (long long)n);
    } else {
        snprintf(buf, sizeof(buf), "%.1f %s", v, units[u]);
    }
    return buf;
}

std::string grouped(int64_t n) {
    std::string digits = std::to_string(n < 0 ? -n : n);
    std::string out;
    int count = 0;
    for (size_t i = digits.size(); i-- > 0;) {
        out.push_back(digits[i]);
        if (++count % 3 == 0 && i > 0) out.push_back(',');
    }
    if (n < 0) out.push_back('-');
    std::reverse(out.begin(), out.end());
    return out;
}

void printKindStatistics(const TreeScanner& scanner, size_t limit) {
    auto rows = scanner.kindTotals();
    std::sort(rows.begin(), rows.end(),
              [](const TreeScanner::KindTotal& a, const TreeScanner::KindTotal& b) {
                  return a.bytes > b.bytes;
              });
    printf("\n  %-24s %14s %14s\n", "Kind", "Size", "Files");
    printf("  %-24s %14s %14s\n", "------------------------", "--------------", "--------------");
    size_t shown = 0;
    for (const auto& r : rows) {
        if (shown++ >= limit) break;
        printf("  %-24s %14s %14s\n", toUtf8(FileKind::names()[size_t(r.kind)]).c_str(),
               humanBytes(r.bytes).c_str(), grouped(r.count).c_str());
    }
}

void printLargest(const NodeArena& arena, size_t limit, SizeMetric metric) {
    size_t count = arena.count();
    std::vector<size_t> best;
    best.reserve(count);
    for (size_t i = 1; i < count; ++i) {
        if (!arena.isDirectory(i)) best.push_back(i);
    }
    size_t take = std::min(limit, best.size());
    std::partial_sort(best.begin(), best.begin() + ptrdiff_t(take), best.end(),
                      [&](size_t a, size_t b) { return arena.size(metric, a) > arena.size(metric, b); });
    printf("\n  Largest files\n");
    for (size_t i = 0; i < take; ++i) {
        printf("  %12s  %s\n", humanBytes(arena.size(metric, best[i])).c_str(),
               toUtf8(arena.path(best[i])).c_str());
    }
}

/// Checks the invariant the whole tree view depends on: a directory's children
/// occupy one contiguous run, and every node's parent points back at a node whose
/// run contains it.
std::string verifyChildRanges(const NodeArena& arena) {
    size_t count = arena.count();
    std::vector<uint8_t> claimed(count, 0);
    int64_t problems = 0;
    for (size_t i = 0; i < count; ++i) {
        auto range = arena.children(i);
        for (size_t c = range.begin; c < range.end; ++c) {
            if (claimed[c]) ++problems;
            claimed[c] = 1;
            if (size_t(uint32_t(arena.parent[c])) != i) ++problems;
        }
    }
    for (size_t i = 1; i < count; ++i) {
        if (!claimed[i]) ++problems;
    }
    char buf[128];
    snprintf(buf, sizeof(buf), problems == 0 ? "contiguous children: ok" : "contiguous children: %lld PROBLEMS",
             (long long)problems);
    return buf;
}

/// Independent recount straight out of the arena, so the rolled-up totals are
/// checked against a sum that shares none of their code.
///
/// Folders can legitimately hold bytes of their own: on the MFT backend a large
/// directory's index allocation is real space on the disk. That difference is
/// reported rather than treated as an error — what would be an error is the flat
/// recount coming out *above* the rolled-up total, which would mean bytes counted
/// in a leaf that never reached the root.
void verifyTotals(const NodeArena& arena) {
    size_t count = arena.count();
    int64_t allocated = 0, logical = 0, files = 0;
    for (size_t i = 1; i < count; ++i) {
        if (arena.isDirectory(i)) continue;
        allocated += arena.allocated[i];
        logical += arena.logical[i];
        files += arena.fileCount[i];
    }
    int64_t folderBytes = arena.allocated[0] - allocated;
    bool ok = folderBytes >= 0 && logical == arena.logical[0] && files == arena.fileCount[0];
    printf("  rolled-up totals: %s\n", ok ? "consistent with a flat recount"
                                          : "INCONSISTENT with a flat recount");
    printf("    files            %s allocated, %s logical, %s files\n", humanBytes(allocated).c_str(),
           humanBytes(logical).c_str(), grouped(files).c_str());
    printf("    folders' own     %s\n", humanBytes(folderBytes).c_str());
    printf("    rolled up        %s allocated, %s logical, %s files\n",
           humanBytes(arena.allocated[0]).c_str(), humanBytes(arena.logical[0]).c_str(),
           grouped(arena.fileCount[0]).c_str());

    // Every folder must hold at least what its children hold. This is the check
    // that actually catches a roll-up bug, and it is independent of where a
    // folder's own bytes came from.
    int64_t inversions = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!arena.isDirectory(i)) continue;
        auto range = arena.children(i);
        int64_t sum = 0;
        for (size_t c = range.begin; c < range.end; ++c) sum += arena.allocated[c];
        if (sum > arena.allocated[i]) ++inversions;
    }
    printf("  parent >= children: %s\n",
           inversions == 0 ? "ok" : (std::to_string(inversions) + " PROBLEMS").c_str());
}

/// Exact figures, one per line, for a script that has to compare numbers rather
/// than read them. The human report rounds to one decimal place, which is fine to
/// look at and useless to assert on.
void printRaw(const TreeScanner& scanner, const ScanResult& r) {
    const NodeArena& arena = scanner.arena();
    printf("root=%s\n", toUtf8(scanner.rootPath()).c_str());
    printf("backend=%s\n", toUtf8(scanBackendName(r.backend)).c_str());
    printf("allocated=%lld\n", (long long)arena.allocated[0]);
    printf("logical=%lld\n", (long long)arena.logical[0]);
    printf("files=%lld\n", (long long)r.files);
    printf("directories=%lld\n", (long long)r.directories);
    printf("nodes=%lld\n", (long long)arena.count());
    printf("hardlink_duplicates=%lld\n", (long long)r.hardlinkDuplicates);
    printf("hardlink_duplicate_bytes=%lld\n", (long long)r.hardlinkDuplicateBytes);
    printf("errors=%lld\n", (long long)r.errors);
    printf("elapsed=%.4f\n", r.elapsed);
}

int runScan(const std::wstring& path, const ScanOptions& base, bool quiet, bool verify,
            bool raw) {
    ScanOptions options = base;
    if (raw) quiet = true;
    if (!quiet) {
        options.progress = [](const ScanProgress& p) {
            fprintf(stderr, "\r  %s files, %s folders, %s   ", grouped(p.files).c_str(),
                    grouped(p.directories).c_str(), humanBytes(p.allocatedBytes).c_str());
        };
    }
    options.applyScanHistory(canonicalPath(path));

    TreeScanner scanner(options);
    ScanResult r = scanner.scan(path);
    if (!quiet) fprintf(stderr, "\r%60s\r", "");

    const NodeArena& arena = scanner.arena();
    if (r.errors > 0 && arena.count() <= 1) {
        fprintf(stderr, "diskscan: cannot read %s\n", toUtf8(path).c_str());
        return 1;
    }

    if (raw) {
        printRaw(scanner, r);
        return 0;
    }

    printf("%s\n", toUtf8(scanner.rootPath()).c_str());
    printf("  backend            %s%s\n", toUtf8(scanBackendName(r.backend)).c_str(),
           r.mftUnavailable ? (" (mft unavailable: " + toUtf8(r.mftUnavailableReason) + ")").c_str()
                            : "");
    if (r.backend != ScanBackend::Mft) printf("  workers            %d\n", options.resolvedWorkers());
    printf("  elapsed            %.2fs\n", r.elapsed);
    printf("  entries            %s (%s files, %s folders)\n", grouped(int64_t(arena.count())).c_str(),
           grouped(r.files).c_str(), grouped(r.directories).c_str());
    printf("  throughput         %s entries/sec\n",
           grouped(r.elapsed > 0 ? int64_t(double(arena.count()) / r.elapsed) : 0).c_str());
    printf("  allocated          %s\n", humanBytes(arena.allocated[0]).c_str());
    printf("  logical            %s\n", humanBytes(arena.logical[0]).c_str());
    if (r.hardlinkDuplicates > 0) {
        printf("  hard links         %s extra links, %s counted once\n",
               grouped(r.hardlinkDuplicates).c_str(), humanBytes(r.hardlinkDuplicateBytes).c_str());
    }
    printf("  unreadable         %s\n", grouped(r.errors).c_str());
    printf("  with timestamps    %s\n", grouped(r.entriesWithTimestamp).c_str());
    printf("  memory             %s\n", humanBytes(int64_t(arena.approximateMemoryBytes())).c_str());
    printf("  syscalls           %s\n", grouped(r.syscalls).c_str());
    if (r.backend != ScanBackend::Mft) {
        printf("  applier queue      depth %llu, blocked pushes %llu\n",
               (unsigned long long)r.queueMaxDepth, (unsigned long long)r.queueBlockedPushes);
        printf("  worker time        build path %.2fs, open %.2fs, read %.2fs, push %.2fs (of %.2fs)\n",
               r.secBuildPath, r.secOpen, r.secRead, r.secPush, r.secWorkerTotal);
        printf("  applier time       %.2fs busy of %.2fs\n", r.secApplier, r.secApplierTotal);
    }

    if (verify) {
        printf("\n  %s\n", verifyChildRanges(arena).c_str());
        verifyTotals(arena);
    }

    printKindStatistics(scanner, 20);
    printLargest(arena, 15, SizeMetric::Total);
    return 0;
}

int runBench(const std::wstring& path, int repeats, int maxWorkers) {
    printf("%-10s %-10s %10s %18s\n", "backend", "workers", "seconds", "entries/sec");
    printf("%-10s %-10s %10s %18s\n", "-------", "-------", "-------", "-----------");
    for (int workers = 1; workers <= maxWorkers; workers *= 2) {
        double best = 1e18;
        int64_t entries = 0;
        for (int i = 0; i < repeats; ++i) {
            ScanOptions options;
            options.workers = workers;
            options.backend = ScanBackend::Enum;
            TreeScanner scanner(options);
            ScanResult r = scanner.scan(path);
            best = std::min(best, r.elapsed);
            entries = int64_t(scanner.arena().count());
        }
        printf("%-10s %-10d %10.2f %18s\n", "enum", workers, best,
               grouped(int64_t(double(entries) / best)).c_str());
    }
    return 0;
}

/// Prices the two things a worker does per directory that are not the read
/// itself: rebuilding the path, and opening the handle.
int runIoBench(const std::wstring& path, int workers) {
    for (size_t buffer : {size_t(64) << 10, size_t(256) << 10, size_t(1) << 20}) {
        ScanOptions options;
        options.workers = workers;
        options.backend = ScanBackend::Enum;
        options.bulkBufferSize = buffer;
        TreeScanner scanner(options);
        ScanResult r = scanner.scan(path);
        printf("  buffer %5llu kB   %6.2fs   %s syscalls\n", (unsigned long long)(buffer >> 10),
               r.elapsed, grouped(r.syscalls).c_str());
    }
    for (bool resolve : {true, false}) {
        ScanOptions options;
        options.workers = workers;
        options.backend = ScanBackend::Enum;
        options.resolveCompressedSizes = resolve;
        TreeScanner scanner(options);
        ScanResult r = scanner.scan(path);
        printf("  compressed sizes %-3s %6.2fs   %s allocated\n", resolve ? "on" : "off", r.elapsed,
               humanBytes(scanner.arena().allocated[0]).c_str());
    }
    for (bool dedupe : {true, false}) {
        ScanOptions options;
        options.workers = workers;
        options.backend = ScanBackend::Enum;
        options.countHardlinksOnce = dedupe;
        TreeScanner scanner(options);
        ScanResult r = scanner.scan(path);
        printf("  hard-link dedupe %-3s %6.2fs   %s allocated\n", dedupe ? "on" : "off", r.elapsed,
               humanBytes(scanner.arena().allocated[0]).c_str());
    }
    return 0;
}

/// Why a scan would pick the backend it is going to pick.
///
/// This exists because the choice is made from three conditions that are each
/// easy to get quietly wrong, and a wrong answer costs speed without ever
/// reporting an error. Printing the inputs makes it testable without needing
/// administrator rights to observe the outcome.
int runCheck(const std::wstring& path) {
    std::wstring canonical = canonicalPath(path);
    if (canonical.empty()) canonical = path;
    VolumeInfo volume;
    bool haveVolume = volumeInfoForPath(canonical, &volume);
    bool volumeRoot = isVolumeRoot(canonical);
    bool elevated = isElevated();
    bool ntfs = haveVolume && volume.isNtfs;

    printf("path=%s\n", toUtf8(canonical).c_str());
    printf("volume=%s\n", haveVolume ? toUtf8(volume.mountPoint).c_str() : "");
    printf("file_system=%s\n", haveVolume ? toUtf8(volume.fileSystem).c_str() : "");
    printf("volume_root=%d\n", volumeRoot ? 1 : 0);
    printf("ntfs=%d\n", ntfs ? 1 : 0);
    printf("elevated=%d\n", elevated ? 1 : 0);
    // Exactly the conditions runMft applies for ScanBackend::Auto.
    bool wouldUseMft = ntfs && elevated && volumeRoot;
    printf("auto_backend=%s\n", wouldUseMft ? "mft" : "enum");
    if (!wouldUseMft) {
        const char* reason = !ntfs         ? "not an NTFS volume"
                             : !volumeRoot ? "not a whole volume"
                                           : "needs administrator rights";
        printf("auto_reason=%s\n", reason);
    }
    return 0;
}

[[noreturn]] void usage() {
    fprintf(stderr,
            "usage:\n"
            "  diskscan scan <path> [--workers N] [--backend auto|enum|mft] [--quiet]\n"
            "                       [--verify] [--raw] [--no-hardlinks] [--no-compressed]\n"
            "  diskscan bench <path> [--repeat N] [--max-workers N]\n"
            "  diskscan iobench <path> [--workers N]\n"
            "  diskscan volumes\n");
    exit(2);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::wstring> args(argv + 1, argv + argc);
    if (args.empty()) usage();

    auto hasFlag = [&](const wchar_t* name) {
        return std::find(args.begin(), args.end(), name) != args.end();
    };
    auto flagValue = [&](const wchar_t* name) -> std::wstring {
        auto it = std::find(args.begin(), args.end(), name);
        if (it == args.end() || it + 1 == args.end()) return std::wstring();
        return *(it + 1);
    };

    std::wstring command = args[0];

    if (command == L"volumes") {
        for (const auto& v : mountedVolumes()) {
            printf("  %-6s %-8s %12s total  %12s free  %5u byte clusters\n",
                   toUtf8(volumeScanPath(v)).c_str(), toUtf8(v.fileSystem).c_str(),
                   humanBytes(v.totalBytes).c_str(), humanBytes(v.freeBytes).c_str(),
                   v.clusterBytes);
        }
        return 0;
    }

    // Positionals are everything that is neither a flag nor a flag's value.
    std::vector<std::wstring> positionals;
    for (size_t i = 1; i < args.size(); ++i) {
        if (!args[i].empty() && args[i][0] == L'-') {
            static const wchar_t* takesValue[] = {L"--workers", L"--backend", L"--repeat",
                                                  L"--max-workers"};
            for (const wchar_t* f : takesValue) {
                if (args[i] == f) {
                    ++i;
                    break;
                }
            }
            continue;
        }
        positionals.push_back(args[i]);
    }
    if (positionals.empty()) usage();
    const std::wstring& target = positionals[0];

    ScanOptions options;
    std::wstring workersFlag = flagValue(L"--workers");
    if (!workersFlag.empty()) options.workers = _wtoi(workersFlag.c_str());
    std::wstring backendFlag = flagValue(L"--backend");
    if (backendFlag == L"enum") options.backend = ScanBackend::Enum;
    else if (backendFlag == L"mft") options.backend = ScanBackend::Mft;
    if (hasFlag(L"--no-hardlinks")) options.countHardlinksOnce = false;
    if (hasFlag(L"--no-compressed")) options.resolveCompressedSizes = false;

    if (command == L"scan") {
        return runScan(target, options, hasFlag(L"--quiet"), hasFlag(L"--verify"),
                       hasFlag(L"--raw"));
    }
    if (command == L"bench") {
        std::wstring repeat = flagValue(L"--repeat");
        std::wstring maxWorkers = flagValue(L"--max-workers");
        return runBench(target, repeat.empty() ? 2 : _wtoi(repeat.c_str()),
                        maxWorkers.empty() ? 16 : _wtoi(maxWorkers.c_str()));
    }
    if (command == L"check") {
        return runCheck(target);
    }
    if (command == L"iobench") {
        return runIoBench(target, options.resolvedWorkers());
    }
    usage();
}
