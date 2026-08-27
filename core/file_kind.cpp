#include "file_kind.h"

#include <unordered_map>

#include "name_string.h"

namespace diskscan {
namespace FileKind {
namespace {

struct Group {
    const NameChar* display;
    const NameChar* extensions;  // space separated
};

/// Ordered roughly by how often a kind turns out to be what filled a Windows
/// disk. Extensions appear once: the first group claiming one wins.
const Group kGroups[] = {
    {DS_T("Document"), DS_T("doc docx rtf odt txt md tex epub")},
    {DS_T("PDF document"), DS_T("pdf")},
    {DS_T("Spreadsheet"), DS_T("xls xlsx xlsm csv tsv ods")},
    {DS_T("Presentation"), DS_T("ppt pptx odp")},
    {DS_T("JPEG image"), DS_T("jpg jpeg jpe")},
    {DS_T("PNG image"), DS_T("png")},
    {DS_T("HEIF image"), DS_T("heic heif avif")},
    {DS_T("TIFF image"), DS_T("tif tiff")},
    {DS_T("GIF image"), DS_T("gif")},
    {DS_T("WebP image"), DS_T("webp")},
    {DS_T("Bitmap image"), DS_T("bmp dib ico cur")},
    {DS_T("RAW image"), DS_T("raw cr2 cr3 nef arw dng orf rw2")},
    {DS_T("SVG image"), DS_T("svg")},
    {DS_T("Photoshop image"), DS_T("psd psb ai")},
    {DS_T("MPEG-4 movie"), DS_T("mp4 m4v")},
    {DS_T("Windows video"), DS_T("wmv asf avi")},
    {DS_T("QuickTime movie"), DS_T("mov qt")},
    {DS_T("Matroska video"), DS_T("mkv webm")},
    {DS_T("MPEG-2 stream"), DS_T("ts mpg mpeg m2ts vob")},
    {DS_T("MP3 audio"), DS_T("mp3")},
    {DS_T("AAC audio"), DS_T("m4a aac")},
    {DS_T("Windows audio"), DS_T("wma")},
    {DS_T("Waveform audio"), DS_T("wav aiff aif flac ogg opus")},
    {DS_T("Disk image"), DS_T("iso vhd vhdx vmdk qcow2 img wim esd")},
    {DS_T("Zip archive"), DS_T("zip zipx")},
    {DS_T("7-Zip archive"), DS_T("7z rar")},
    {DS_T("GZip archive"), DS_T("gz tgz")},
    {DS_T("Bzip2 archive"), DS_T("bz2 tbz tbz2")},
    {DS_T("xz archive"), DS_T("xz txz zst lz4 lzma")},
    {DS_T("Tar archive"), DS_T("tar")},
    {DS_T("Cabinet archive"), DS_T("cab msu")},
    {DS_T("Java archive"), DS_T("jar war class")},
    {DS_T("Executable"), DS_T("exe com scr")},
    {DS_T("Installer"), DS_T("msi msix appx appxbundle msixbundle msp")},
    {DS_T("Dynamic library"), DS_T("dll ocx so dylib")},
    {DS_T("Driver"), DS_T("sys inf cat")},
    {DS_T("Object code"), DS_T("o obj lo bc res")},
    {DS_T("Static archive"), DS_T("lib a")},
    {DS_T("Debug symbols"), DS_T("pdb ilk map dbg")},
    {DS_T("JavaScript"), DS_T("js mjs cjs jsx")},
    {DS_T("TypeScript"), DS_T("tsx mts cts")},
    {DS_T("JSON"), DS_T("json jsonl ndjson")},
    {DS_T("XML text"), DS_T("xml xsd xsl xaml resx csproj vcxproj sln")},
    {DS_T("YAML text"), DS_T("yml yaml toml")},
    {DS_T("HTML text"), DS_T("html htm xhtml")},
    {DS_T("CSS text"), DS_T("css scss sass less")},
    {DS_T("C source code"), DS_T("c")},
    {DS_T("C header code"), DS_T("h")},
    {DS_T("C++ source code"), DS_T("cpp cc cxx")},
    {DS_T("C++ header code"), DS_T("hpp hh hxx")},
    {DS_T("C# source code"), DS_T("cs")},
    {DS_T("Rust source"), DS_T("rs")},
    {DS_T("Go source"), DS_T("go")},
    {DS_T("Swift source"), DS_T("swift")},
    {DS_T("Python script"), DS_T("py pyi pyw")},
    {DS_T("Python bytecode"), DS_T("pyc pyo")},
    {DS_T("Ruby script"), DS_T("rb gemspec")},
    {DS_T("Shell script"), DS_T("sh bash zsh")},
    {DS_T("Windows script"), DS_T("bat cmd ps1 psm1 psd1 vbs wsf")},
    {DS_T("Java source"), DS_T("java kt kts scala groovy")},
    {DS_T("PHP script"), DS_T("php")},
    {DS_T("SQLite database"), DS_T("sqlite sqlite3 db db3 realm mdf ldf")},
    {DS_T("Database log"), DS_T("wal shm journal")},
    {DS_T("Registry hive"), DS_T("hiv reg dat_")},
    {DS_T("Log file"), DS_T("log etl evtx dmp trace")},
    {DS_T("Index file"), DS_T("idx index cache pack")},
    {DS_T("Font"), DS_T("ttf otf ttc woff woff2 fon")},
    {DS_T("Shortcut"), DS_T("lnk url")},
    {DS_T("Configuration"), DS_T("ini cfg conf config settings properties")},
    {DS_T("Data file"), DS_T("dat bin data blob asset pak bundle")},
    {DS_T("Backup"), DS_T("bak backup old orig tmp temp partial crdownload")},
};

std::vector<NameString> gNames;
std::unordered_map<uint64_t, int16_t> gTable;
int16_t gOtherId = 0;

/// Packs up to 8 lowercased ASCII characters into a single integer key.
/// Returns false for anything that cannot be a plain extension.
inline bool pack(const NameChar* p, size_t len, uint64_t* out) {
    if (len == 0 || len > 8) return false;
    uint64_t key = 0;
    for (size_t i = 0; i < len; ++i) {
        NameChar c = p[i];
        if (c > 127) return false;
        if (c >= DS_T('A') && c <= DS_T('Z')) c = NameChar(c + 32);
        key = (key << 8) | uint64_t(uint8_t(c));
    }
    *out = key;
    return true;
}

}  // namespace

void warmUp() {
    if (!gNames.empty()) return;
    gNames = {DS_T("Unknown"), DS_T("Folder"), DS_T("Symbolic link"), DS_T("No extension")};
    for (const Group& g : kGroups) {
        int16_t id = int16_t(gNames.size());
        gNames.push_back(g.display);
        const NameChar* p = g.extensions;
        while (*p) {
            const NameChar* start = p;
            while (*p && *p != DS_T(' ')) ++p;
            size_t len = size_t(p - start);
            // A trailing "_" disambiguates an extension that would otherwise
            // collide with an earlier group; strip it before packing.
            if (len > 1 && start[len - 1] == DS_T('_')) --len;
            uint64_t key;
            if (pack(start, len, &key) && gTable.find(key) == gTable.end()) gTable[key] = id;
            while (*p == DS_T(' ')) ++p;
        }
    }
    gNames.push_back(DS_T("Other"));
    gOtherId = int16_t(gNames.size() - 1);
}

const std::vector<NameString>& names() { return gNames; }
int16_t otherId() { return gOtherId; }
int16_t count() { return int16_t(gNames.size()); }

int16_t classify(const NameChar* name, size_t length) {
    // Scan back for the last dot, bailing out early on long extensions.
    size_t limit = length > 12 ? length - 12 : 0;
    size_t dot = size_t(-1);
    for (size_t i = length; i-- > limit;) {
        if (name[i] == DS_T('.')) {
            dot = i;
            break;
        }
    }
    // A leading dot means a hidden file, not an extension.
    if (dot == size_t(-1) || dot == 0 || dot + 1 >= length) return NoExtension;
    uint64_t key;
    if (!pack(name + dot + 1, length - dot - 1, &key)) return gOtherId;
    auto it = gTable.find(key);
    return it == gTable.end() ? gOtherId : it->second;
}

}  // namespace FileKind
}  // namespace diskscan
