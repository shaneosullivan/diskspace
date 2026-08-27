#pragma once

#include <string>

namespace diskscan {

/// The character type a file name is stored in, which is whatever the operating
/// system actually hands over.
///
/// The temptation is to normalise both platforms onto UTF-8 and be done with it.
/// That loses data on each of them, in opposite directions:
///
///  * NTFS names are UTF-16 and may contain unpaired surrogates. Those are not
///    representable in UTF-8, so a name containing one cannot survive the round
///    trip back to the call that has to open the file.
///  * Linux names are bytes. The kernel enforces no encoding at all, so a name
///    is not required to be valid UTF-8 and frequently is not - a file copied
///    from an old archive or a Latin-1 system is an ordinary thing to meet.
///
/// Storing what the system gave keeps both exact, and keeps both hot paths a
/// memcpy: the scanner rebuilds each directory path out of the arena before
/// opening it, so a stored form that needed converting would pay for that
/// conversion once per directory on the way in and again on the way out. The
/// conversion happens instead at the display boundary, where a toolkit is going
/// to demand its own encoding regardless, and where being lossy is harmless.
///
/// This is the same arrangement Rust reaches for with `OsString`, for the same
/// reasons.
#ifdef _WIN32
using NameChar = wchar_t;
#else
using NameChar = char;
#endif

using NameString = std::basic_string<NameChar>;

/// Literal in whichever encoding `NameChar` is. `DS_T("Image")` is `L"Image"` on
/// Windows and `"Image"` everywhere else.
#ifdef _WIN32
#define DS_T(x) L##x
#else
#define DS_T(x) x
#endif

/// The separator between path components: backslash on Windows, slash elsewhere.
#ifdef _WIN32
inline constexpr NameChar kPathSeparator = L'\\';
#else
inline constexpr NameChar kPathSeparator = '/';
#endif

/// UTF-8 for anything that has to leave the process as bytes - the command line
/// tool's output, a log line, a path written to a file. Lossy by necessity on
/// both platforms, and only ever used where that is acceptable.
std::string toUtf8(const NameString& s);

/// The inverse, for a name arriving as UTF-8 - a command line argument, a saved
/// path. Also lossy in the cases above, which is why the scanner never uses it
/// on a name it read from disk.
NameString fromUtf8(const std::string& s);

}  // namespace diskscan
