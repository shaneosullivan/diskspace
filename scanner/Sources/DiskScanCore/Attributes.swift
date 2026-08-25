import Darwin

// MARK: - vnode types (sys/vnode.h, ABI-stable)

enum VType {
    static let non: UInt32 = 0
    static let reg: UInt32 = 1
    static let dir: UInt32 = 2
    static let blk: UInt32 = 3
    static let chr: UInt32 = 4
    static let lnk: UInt32 = 5
    static let sock: UInt32 = 6
    static let fifo: UInt32 = 7
}

let DIR_MNTSTATUS_MNTPOINT: UInt32 = 0x0000_0001

/// The `ATTR_*` macros import with a mix of signednesses; normalise them.
@inline(__always) func ag<T: BinaryInteger>(_ v: T) -> attrgroup_t {
    attrgroup_t(truncatingIfNeeded: v)
}

/// One decoded directory entry as returned by `getattrlistbulk`.
struct BulkEntry {
    var nameStart: UnsafeRawPointer
    var nameLen: Int          // excludes the trailing NUL
    var objType: UInt32
    var fileID: UInt64
    var logicalSize: Int64
    var allocSize: Int64
    var linkCount: UInt32
    var isMountPoint: Bool
    var error: UInt32
    /// Seconds since the epoch. `addedTime` is when the entry was linked into this
    /// directory; `createTime` is the file's birth time. They usually agree, but a
    /// file moved between folders keeps its birth time and gets a new added time —
    /// which is why "added to the folder" prefers the former.
    var createTime: Int64
    var addedTime: Int64
}

/// Attribute set we ask the kernel for. Keep this in sync with `decodeEntry`.
///
/// Note we deliberately do *not* pass `FSOPT_PACK_INVAL_ATTRS`: the decoder is
/// driven by each entry's `ATTR_CMN_RETURNED_ATTRS` mask, which is correct in
/// both packing modes and costs only a handful of predictable branches per entry
/// against a syscall that already dominates.
/// Which attributes to request, for measuring what each one costs.
public enum AttributeProfile: String, CaseIterable {
    /// Everything the app uses today.
    case full
    /// Drops the two timestamps, to price the recency feature.
    case noTimes
    /// Drops timestamps and directory sizes: the minimum for a size total.
    case minimal
}

func makeAttrList(_ profile: AttributeProfile = .full) -> attrlist {
    var al = attrlist()
    al.bitmapcount = u_short(ATTR_BIT_MAP_COUNT)
    var common =
        ag(ATTR_CMN_RETURNED_ATTRS) | ag(ATTR_CMN_NAME) | ag(ATTR_CMN_OBJTYPE)
        | ag(ATTR_CMN_FILEID) | ag(ATTR_CMN_ERROR)
    if profile == .full {
        common |= ag(ATTR_CMN_CRTIME) | ag(ATTR_CMN_ADDEDTIME)
    }
    al.commonattr = common
    al.dirattr =
        profile == .minimal
        ? ag(ATTR_DIR_MOUNTSTATUS)
        : ag(ATTR_DIR_MOUNTSTATUS) | ag(ATTR_DIR_ALLOCSIZE) | ag(ATTR_DIR_DATALENGTH)
    al.fileattr = ag(ATTR_FILE_LINKCOUNT) | ag(ATTR_FILE_TOTALSIZE) | ag(ATTR_FILE_ALLOCSIZE)
    return al
}

private let attrSetSize = MemoryLayout<attribute_set_t>.size  // 20

/// Decodes one variable-length record. Returns the entry and its byte length.
///
/// Fields arrive in a fixed order: the group order of `attrlist` (common, vol,
/// dir, file, fork) and, within a group, ascending bit order. Only attributes
/// flagged in `returned` occupy space. Everything is read unaligned because the
/// kernel packs records without padding.
@inline(__always)
func decodeEntry(_ p: UnsafeRawPointer) -> (entry: BulkEntry, length: Int) {
    let entryLength = Int(p.loadUnaligned(as: UInt32.self))
    var f = p + 4

    let returned = f.loadUnaligned(as: attribute_set_t.self)
    f += attrSetSize

    var e = BulkEntry(
        nameStart: p, nameLen: 0, objType: VType.non, fileID: 0,
        logicalSize: 0, allocSize: 0, linkCount: 1,
        isMountPoint: false, error: 0, createTime: 0, addedTime: 0)

    let common = returned.commonattr
    if common & ag(ATTR_CMN_NAME) != 0 {
        let ref = f.loadUnaligned(as: attrreference_t.self)
        e.nameStart = f + Int(ref.attr_dataoffset)
        e.nameLen = max(0, Int(ref.attr_length) - 1)  // attr_length includes the NUL
        f += MemoryLayout<attrreference_t>.size
    }
    if common & ag(ATTR_CMN_OBJTYPE) != 0 {
        e.objType = f.loadUnaligned(as: UInt32.self)
        f += 4
    }
    if common & ag(ATTR_CMN_CRTIME) != 0 {
        e.createTime = Int64(f.loadUnaligned(as: timespec.self).tv_sec)
        f += MemoryLayout<timespec>.size
    }
    if common & ag(ATTR_CMN_FILEID) != 0 {
        e.fileID = f.loadUnaligned(as: UInt64.self)
        f += 8
    }
    if common & ag(ATTR_CMN_ADDEDTIME) != 0 {
        e.addedTime = Int64(f.loadUnaligned(as: timespec.self).tv_sec)
        f += MemoryLayout<timespec>.size
    }
    if common & ag(ATTR_CMN_ERROR) != 0 {
        e.error = f.loadUnaligned(as: UInt32.self)
        f += 4
    }

    let dir = returned.dirattr
    if dir & ag(ATTR_DIR_MOUNTSTATUS) != 0 {
        e.isMountPoint = (f.loadUnaligned(as: UInt32.self) & DIR_MNTSTATUS_MNTPOINT) != 0
        f += 4
    }
    if dir & ag(ATTR_DIR_ALLOCSIZE) != 0 {
        e.allocSize = f.loadUnaligned(as: Int64.self)
        f += 8
    }
    if dir & ag(ATTR_DIR_DATALENGTH) != 0 {
        e.logicalSize = f.loadUnaligned(as: Int64.self)
        f += 8
    }

    let file = returned.fileattr
    if file & ag(ATTR_FILE_LINKCOUNT) != 0 {
        e.linkCount = f.loadUnaligned(as: UInt32.self)
        f += 4
    }
    if file & ag(ATTR_FILE_TOTALSIZE) != 0 {
        e.logicalSize = f.loadUnaligned(as: Int64.self)
        f += 8
    }
    if file & ag(ATTR_FILE_ALLOCSIZE) != 0 {
        e.allocSize = f.loadUnaligned(as: Int64.self)
        f += 8
    }

    return (e, entryLength)
}
