import Darwin

/// Extension-based file kind classification.
///
/// Deliberately *not* UTType / Launch Services: those cost an IPC round trip per
/// file and would dominate the scan. An extension is packed into a `UInt64` (up
/// to 8 lowercased ASCII bytes) and looked up in a prebuilt table, so
/// classification is allocation-free and effectively a hash probe per file.
public enum FileKind {
    public static let unknown: Int16 = 0
    public static let directory: Int16 = 1
    public static let symlink: Int16 = 2
    public static let noExtension: Int16 = 3

    /// Index is the kind ID; value is the display name.
    public private(set) static var names: [String] = []
    private static var table: [UInt64: Int16] = [:]

    private static let groups: [(String, [String])] = [
        ("Document", ["doc", "docx", "pages", "rtf", "odt", "txt", "md", "tex", "epub"]),
        ("PDF document", ["pdf"]),
        ("JPEG image", ["jpg", "jpeg", "jpe"]),
        ("PNG image", ["png"]),
        ("HEIF image", ["heic", "heif"]),
        ("TIFF image", ["tif", "tiff"]),
        ("GIF image", ["gif"]),
        ("WebP image", ["webp"]),
        ("RAW image", ["raw", "cr2", "cr3", "nef", "arw", "dng", "orf", "rw2"]),
        ("SVG image", ["svg"]),
        ("QuickTime movie", ["mov", "qt"]),
        ("MPEG-4 movie", ["mp4", "m4v"]),
        ("AVI movie", ["avi"]),
        ("Matroska video", ["mkv", "webm"]),
        ("MPEG-2 stream", ["ts", "mpg", "mpeg", "m2ts", "vob"]),
        ("MP3 audio", ["mp3"]),
        ("AAC audio", ["m4a", "aac"]),
        ("Waveform audio", ["wav", "aiff", "aif", "caf", "flac"]),
        ("Disk image", ["dmg", "iso", "sparsebundle", "sparseimage", "img", "vmdk", "qcow2"]),
        ("Zip archive", ["zip", "zipx"]),
        ("GZip archive", ["gz", "tgz"]),
        ("Bzip2 archive", ["bz2", "tbz", "tbz2"]),
        ("xz archive", ["xz", "txz", "zst", "lz4", "lzma"]),
        ("Tar archive", ["tar"]),
        ("7-Zip archive", ["7z", "rar"]),
        ("Java archive", ["jar", "war", "class"]),
        ("Apple archive", ["aar", "pkg", "mpkg", "xip"]),
        ("JavaScript", ["js", "mjs", "cjs", "jsx"]),
        ("TypeScript", ["ts_", "tsx", "mts", "cts"]),
        ("JSON", ["json", "jsonl", "ndjson", "map"]),
        ("XML text", ["xml", "xsd", "xsl", "storyboard", "xib"]),
        ("YAML text", ["yml", "yaml", "toml"]),
        ("HTML text", ["html", "htm", "xhtml"]),
        ("CSS text", ["css", "scss", "sass", "less"]),
        ("C source code", ["c"]),
        ("C header code", ["h"]),
        ("C++ source code", ["cpp", "cc", "cxx", "mm"]),
        ("C++ header code", ["hpp", "hh", "hxx"]),
        ("Objective-C source", ["m"]),
        ("Swift source", ["swift"]),
        ("Rust source", ["rs"]),
        ("Go source", ["go"]),
        ("Python script", ["py", "pyi", "pyw"]),
        ("Python bytecode", ["pyc", "pyo"]),
        ("Ruby script", ["rb", "gemspec"]),
        ("Shell script", ["sh", "bash", "zsh", "fish", "command"]),
        ("Java source", ["java", "kt", "kts", "scala", "groovy"]),
        ("PHP script", ["php"]),
        ("Object code", ["o", "obj", "a", "lo", "bc"]),
        ("Dynamic library", ["dylib", "so", "dll"]),
        ("Static archive", ["lib"]),
        ("Debug symbols", ["dsym", "pdb", "dwarf"]),
        ("SQLite database", ["sqlite", "sqlite3", "db", "db3", "realm"]),
        ("Database log", ["wal", "shm", "journal"]),
        ("Property list", ["plist", "strings", "stringsdict"]),
        ("Log file", ["log", "logarchive", "diag", "ips", "crash", "panic"]),
        ("Index file", ["idx", "index", "cache", "pack"]),
        ("Font", ["ttf", "otf", "ttc", "woff", "woff2", "dfont"]),
        ("Spreadsheet", ["xls", "xlsx", "numbers", "csv", "tsv"]),
        ("Presentation", ["ppt", "pptx", "key"]),
        ("Bundle", ["app", "framework", "bundle", "kext", "appex", "xcarchive"]),
        ("Data file", ["dat", "bin", "data", "blob", "asset", "car", "nib"]),
        ("Backup", ["bak", "backup", "old", "orig", "tmp", "temp"]),
    ]

    /// Builds the lookup table once at startup.
    public static func warmUp() {
        guard names.isEmpty else { return }
        names = ["Unknown", "Folder", "Symbolic link", "No extension"]
        for (display, exts) in groups {
            let id = Int16(names.count)
            names.append(display)
            for ext in exts {
                // Some entries carry a trailing "_" to disambiguate duplicate
                // extensions across groups; strip it before packing.
                let clean = ext.hasSuffix("_") ? String(ext.dropLast()) : ext
                if let key = pack(Array(clean.utf8)) {
                    if table[key] == nil { table[key] = id }
                }
            }
        }
        names.append("Other")
        otherID = Int16(names.count - 1)
    }

    public private(set) static var otherID: Int16 = 0

    @inline(__always)
    private static func pack(_ bytes: [UInt8]) -> UInt64? {
        guard !bytes.isEmpty, bytes.count <= 8 else { return nil }
        var key: UInt64 = 0
        for b in bytes {
            let lower = (b >= 65 && b <= 90) ? b + 32 : b
            key = (key << 8) | UInt64(lower)
        }
        return key
    }

    /// Classifies by the extension found in `name` (raw UTF-8 bytes, no NUL).
    @inline(__always)
    public static func classify(name: UnsafeRawPointer, length: Int) -> Int16 {
        let p = name.assumingMemoryBound(to: UInt8.self)
        var dot = -1
        var i = length - 1
        // Scan back for the last dot; bail out on long extensions early.
        let limit = max(0, length - 12)
        while i > limit {
            if p[i] == 0x2E { dot = i; break }
            i -= 1
        }
        // A leading dot means a hidden file, not an extension.
        guard dot > 0, dot < length - 1 else { return noExtension }
        var key: UInt64 = 0
        let extLen = length - dot - 1
        guard extLen <= 8 else { return otherID }
        for j in (dot + 1)..<length {
            let b = p[j]
            let lower = (b >= 65 && b <= 90) ? b + 32 : b
            key = (key << 8) | UInt64(lower)
        }
        return table[key] ?? otherID
    }
}
