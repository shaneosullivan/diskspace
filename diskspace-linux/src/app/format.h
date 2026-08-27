#pragma once

#include <cstdint>
#include <string>

namespace app {

/// Bytes in the units a person reads, at three significant figures.
///
/// Powers of 1000 with SI prefixes, matching what the rest of a GNOME desktop
/// says about the same file and what the Windows and macOS builds report.
std::string humanBytes(int64_t bytes);

/// A count with thousands separators: 1,234,567.
std::string grouped(int64_t value);

/// A UTF-8 string safe to hand to Pango, which rejects invalid sequences.
///
/// A Linux filename is bytes and need not be valid UTF-8. The scanner keeps
/// those bytes exactly, because they are what has to go back to open() - but a
/// label is the one place being lossy is right, since the alternative is a
/// warning and a blank row where a file ought to be.
std::string displayName(const char* bytes, size_t length);

}  // namespace app
