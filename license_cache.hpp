#pragma once
// FEATURE-V11-KEY: Cache ONLY after successful online authentication. DPAPI is
// scoped to the current Windows user (never CRYPTPROTECT_LOCAL_MACHINE).
// This is convenience storage, NOT an authentication token or offline bypass.
#include <windows.h>
#include <wincrypt.h>
#include <cstring>
#include <string>
#include <vector>
#include "app_paths.hpp"
#include "atomic_file.hpp"
#include "license_key.hpp"
#pragma comment(lib, "crypt32.lib")
namespace albion {
inline std::wstring LicenseCachePath() { return AlbionUserDirectory() + L"\\license.dpapi"; }
struct LocalBlob {
    LocalBlob() = default;
    LocalBlob(const LocalBlob&) = delete;
    LocalBlob& operator=(const LocalBlob&) = delete;
    DATA_BLOB blob{};
    ~LocalBlob() { if (blob.pbData) { SecureZeroMemory(blob.pbData, blob.cbData); LocalFree(blob.pbData); } }
};
inline bool EncryptLicense(const std::string& key, std::string& encrypted) {
    if (key.empty() || key.size() > 136) return false;
    DATA_BLOB input{DWORD(key.size()), reinterpret_cast<BYTE*>(const_cast<char*>(key.data()))};
    LocalBlob result;
    if (!CryptProtectData(&input, L"AlbionAI license cache v1", nullptr, nullptr,
        nullptr, CRYPTPROTECT_UI_FORBIDDEN, &result.blob)) return false;
    encrypted.assign("ALBKEY1\0", 8);
    encrypted.append(reinterpret_cast<const char*>(result.blob.pbData), result.blob.cbData);
    return true;
}
inline bool DecryptLicense(const std::string& encrypted, std::string& key) {
    key.clear();
    if (encrypted.size() <= 8 || encrypted.size() > 16384 || std::memcmp(encrypted.data(), "ALBKEY1\0", 8)) return false;
    DATA_BLOB input{DWORD(encrypted.size() - 8), reinterpret_cast<BYTE*>(const_cast<char*>(encrypted.data() + 8))};
    LocalBlob result;
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
        CRYPTPROTECT_UI_FORBIDDEN, &result.blob) || !result.blob.cbData || result.blob.cbData > 136) return false;
    std::string raw(reinterpret_cast<const char*>(result.blob.pbData), result.blob.cbData);
    const bool ok = NormalizeAlbionLicense(raw, key);
    if (!raw.empty()) SecureZeroMemory(raw.data(), raw.size());
    return ok;
}
inline bool LoadLicenseCache(std::string& key) noexcept {
    key.clear();
    try {
        Handle file(CreateFileW(LicenseCachePath().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!file.Valid()) return false;
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file.Get(), &size) || size.QuadPart <= 8 || size.QuadPart > 16384) return false;
        std::string encrypted(size_t(size.QuadPart), '\0'); DWORD n = 0;
        return ReadFile(file.Get(), encrypted.data(), DWORD(encrypted.size()), &n, nullptr) &&
            n == encrypted.size() && DecryptLicense(encrypted, key);
    } catch (...) { key.clear(); return false; }
}
inline bool SaveLicenseCache(const std::string& key) noexcept {
    try { std::string encrypted; return EncryptLicense(key, encrypted) && WriteAtomic(LicenseCachePath(), encrypted); }
    catch (...) { return false; }
}
inline bool ForgetLicenseCache() noexcept {
    try {
        const auto path = LicenseCachePath();
        Handle lock(OpenFileLock(path));
        if (!lock.Valid()) return false;
        return DeleteFileW(path.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND;
    } catch (...) { return false; }
}
} // namespace albion
