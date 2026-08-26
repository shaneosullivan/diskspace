#include "preferences.h"

#include <windows.h>

namespace app {
namespace {

constexpr const wchar_t* kKeyPath = L"Software\\DiskSpace";

bool readDword(const wchar_t* name, DWORD* out) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kKeyPath, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD size = sizeof(DWORD), type = 0;
    bool ok = RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(out), &size) ==
                  ERROR_SUCCESS &&
              type == REG_DWORD;
    RegCloseKey(key);
    return ok;
}

void writeDword(const wchar_t* name, DWORD value) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kKeyPath, 0, nullptr, 0, KEY_WRITE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS) {
        return;
    }
    RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
}

}  // namespace

Preferences& Preferences::shared() {
    static Preferences instance;
    return instance;
}

bool Preferences::flag(const wchar_t* key, bool fallback) const {
    DWORD value = 0;
    // A setting that has never been written keeps its default, which is how the
    // app behaved before it had settings at all.
    return readDword(key, &value) ? value != 0 : fallback;
}

void Preferences::set(const wchar_t* key, bool value) {
    writeDword(key, value ? 1 : 0);
    notify();
}

void Preferences::resetAll() {
    RegDeleteKeyW(HKEY_CURRENT_USER, kKeyPath);
    notify();
}

void Preferences::addObserver(std::function<void()> observer) {
    observers_.push_back(std::move(observer));
}

void Preferences::notify() {
    for (auto& observer : observers_) observer();
}

}  // namespace app
