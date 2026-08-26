#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace diskscan {

/// Extension-based file kind classification.
///
/// Deliberately *not* the shell's file-type association: `SHGetFileInfo` costs a
/// registry lookup and often a COM round trip per file, which would dominate the
/// scan by orders of magnitude. An extension is packed into a `uint64_t` (up to 8
/// lowercased ASCII characters) and looked up in a table built once at startup,
/// so classification is allocation-free and effectively a hash probe per file.
namespace FileKind {

inline constexpr int16_t Unknown = 0;
inline constexpr int16_t Directory = 1;
inline constexpr int16_t Symlink = 2;
inline constexpr int16_t NoExtension = 3;
/// First slot that carries a real extension group. Below this the four entries
/// above are reserved.
inline constexpr int16_t ReservedSlots = 4;

/// Builds the lookup table once. Safe to call repeatedly.
void warmUp();

/// Index is the kind id; value is the display name.
const std::vector<std::wstring>& names();
int16_t otherId();
int16_t count();

/// Classifies by the extension found in `name` (UTF-16, no terminator).
int16_t classify(const wchar_t* name, size_t length);

}  // namespace FileKind
}  // namespace diskscan
