#include "file_kind.h"

#include <unordered_map>

namespace diskscan {
namespace FileKind {
namespace {

struct Group {
    const wchar_t* display;
    const wchar_t* extensions;  // space separated
};

/// Ordered roughly by how often a kind turns out to be what filled a Windows
/// disk. Extensions appear once: the first group claiming one wins.
const Group kGroups[] = {
    {L"Document", L"doc docx rtf odt txt md tex epub"},
    {L"PDF document", L"pdf"},
    {L"Spreadsheet", L"xls xlsx xlsm csv tsv ods"},
    {L"Presentation", L"ppt pptx odp"},
    {L"JPEG image", L"jpg jpeg jpe"},
    {L"PNG image", L"png"},
    {L"HEIF image", L"heic heif avif"},
    {L"TIFF image", L"tif tiff"},
    {L"GIF image", L"gif"},
    {L"WebP image", L"webp"},
    {L"Bitmap image", L"bmp dib ico cur"},
    {L"RAW image", L"raw cr2 cr3 nef arw dng orf rw2"},
    {L"SVG image", L"svg"},
    {L"Photoshop image", L"psd psb ai"},
    {L"MPEG-4 movie", L"mp4 m4v"},
    {L"Windows video", L"wmv asf avi"},
    {L"QuickTime movie", L"mov qt"},
    {L"Matroska video", L"mkv webm"},
    {L"MPEG-2 stream", L"ts mpg mpeg m2ts vob"},
    {L"MP3 audio", L"mp3"},
    {L"AAC audio", L"m4a aac"},
    {L"Windows audio", L"wma"},
    {L"Waveform audio", L"wav aiff aif flac ogg opus"},
    {L"Disk image", L"iso vhd vhdx vmdk qcow2 img wim esd"},
    {L"Zip archive", L"zip zipx"},
    {L"7-Zip archive", L"7z rar"},
    {L"GZip archive", L"gz tgz"},
    {L"Bzip2 archive", L"bz2 tbz tbz2"},
    {L"xz archive", L"xz txz zst lz4 lzma"},
    {L"Tar archive", L"tar"},
    {L"Cabinet archive", L"cab msu"},
    {L"Java archive", L"jar war class"},
    {L"Executable", L"exe com scr"},
    {L"Installer", L"msi msix appx appxbundle msixbundle msp"},
    {L"Dynamic library", L"dll ocx so dylib"},
    {L"Driver", L"sys inf cat"},
    {L"Object code", L"o obj lo bc res"},
    {L"Static archive", L"lib a"},
    {L"Debug symbols", L"pdb ilk map dbg"},
    {L"JavaScript", L"js mjs cjs jsx"},
    {L"TypeScript", L"tsx mts cts"},
    {L"JSON", L"json jsonl ndjson"},
    {L"XML text", L"xml xsd xsl xaml resx csproj vcxproj sln"},
    {L"YAML text", L"yml yaml toml"},
    {L"HTML text", L"html htm xhtml"},
    {L"CSS text", L"css scss sass less"},
    {L"C source code", L"c"},
    {L"C header code", L"h"},
    {L"C++ source code", L"cpp cc cxx"},
    {L"C++ header code", L"hpp hh hxx"},
    {L"C# source code", L"cs"},
    {L"Rust source", L"rs"},
    {L"Go source", L"go"},
    {L"Swift source", L"swift"},
    {L"Python script", L"py pyi pyw"},
    {L"Python bytecode", L"pyc pyo"},
    {L"Ruby script", L"rb gemspec"},
    {L"Shell script", L"sh bash zsh"},
    {L"Windows script", L"bat cmd ps1 psm1 psd1 vbs wsf"},
    {L"Java source", L"java kt kts scala groovy"},
    {L"PHP script", L"php"},
    {L"SQLite database", L"sqlite sqlite3 db db3 realm mdf ldf"},
    {L"Database log", L"wal shm journal"},
    {L"Registry hive", L"hiv reg dat_"},
    {L"Log file", L"log etl evtx dmp trace"},
    {L"Index file", L"idx index cache pack"},
    {L"Font", L"ttf otf ttc woff woff2 fon"},
    {L"Shortcut", L"lnk url"},
    {L"Configuration", L"ini cfg conf config settings properties"},
    {L"Data file", L"dat bin data blob asset pak bundle"},
    {L"Backup", L"bak backup old orig tmp temp partial crdownload"},
};

std::vector<std::wstring> gNames;
std::unordered_map<uint64_t, int16_t> gTable;
int16_t gOtherId = 0;

/// Packs up to 8 lowercased ASCII characters into a single integer key.
/// Returns false for anything that cannot be a plain extension.
inline bool pack(const wchar_t* p, size_t len, uint64_t* out) {
    if (len == 0 || len > 8) return false;
    uint64_t key = 0;
    for (size_t i = 0; i < len; ++i) {
        wchar_t c = p[i];
        if (c > 127) return false;
        if (c >= L'A' && c <= L'Z') c = wchar_t(c + 32);
        key = (key << 8) | uint64_t(uint8_t(c));
    }
    *out = key;
    return true;
}

}  // namespace

void warmUp() {
    if (!gNames.empty()) return;
    gNames = {L"Unknown", L"Folder", L"Symbolic link", L"No extension"};
    for (const Group& g : kGroups) {
        int16_t id = int16_t(gNames.size());
        gNames.push_back(g.display);
        const wchar_t* p = g.extensions;
        while (*p) {
            const wchar_t* start = p;
            while (*p && *p != L' ') ++p;
            size_t len = size_t(p - start);
            // A trailing "_" disambiguates an extension that would otherwise
            // collide with an earlier group; strip it before packing.
            if (len > 1 && start[len - 1] == L'_') --len;
            uint64_t key;
            if (pack(start, len, &key) && gTable.find(key) == gTable.end()) gTable[key] = id;
            while (*p == L' ') ++p;
        }
    }
    gNames.push_back(L"Other");
    gOtherId = int16_t(gNames.size() - 1);
}

const std::vector<std::wstring>& names() { return gNames; }
int16_t otherId() { return gOtherId; }
int16_t count() { return int16_t(gNames.size()); }

int16_t classify(const wchar_t* name, size_t length) {
    // Scan back for the last dot, bailing out early on long extensions.
    size_t limit = length > 12 ? length - 12 : 0;
    size_t dot = size_t(-1);
    for (size_t i = length; i-- > limit;) {
        if (name[i] == L'.') {
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
