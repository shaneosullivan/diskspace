#pragma once

#include <string>

#include "name_string.h"

namespace app {

/// The four things the desktop used to be asked to do through GTK.
///
/// Opening and revealing go out to xdg-open, which is the portable way to ask
/// "whatever handles this, handle it" and is present on every desktop. The trash
/// is implemented here against the freedesktop specification, because the only
/// library that offered it was GIO.
namespace shell {

/// Open a file or folder with whatever the desktop uses for it.
bool openPath(const diskscan::NameString& path);

/// Show a file in its containing folder. Falls back to opening the folder, which
/// is what most file managers do with the request anyway.
bool revealPath(const diskscan::NameString& path);

/// Move `path` to the trash, per the freedesktop trash specification.
///
/// Returns true when the file is gone from where it was. `error` is filled in
/// with something worth showing a person when it is not.
bool moveToTrash(const diskscan::NameString& path, std::string* error);

}  // namespace shell
}  // namespace app
