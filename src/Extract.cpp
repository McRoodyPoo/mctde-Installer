#include "Extract.h"
#include "miniz.h"

#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace fs = std::filesystem;

namespace mctde {

// Reject unsafe zip entry paths (absolute, drive-qualified, or containing ".."),
// returning a normalized forward-slash relative path on success (zip-slip guard).
static bool safeRelPath(const char* raw, std::string& rel) {
    std::string n = raw ? raw : "";
    for (char& c : n) if (c == '\\') c = '/';
    if (n.empty() || n[0] == '/') return false;
    if (n.size() >= 2 && n[1] == ':') return false;
    size_t i = 0;
    while (i < n.size()) {
        size_t j = n.find('/', i);
        std::string comp = n.substr(i, j == std::string::npos ? std::string::npos : j - i);
        if (comp == "..") return false;
        if (j == std::string::npos) break;
        i = j + 1;
    }
    rel = n;
    return true;
}

#ifndef _WIN32
// On a case-sensitive filesystem, map each component of `rel` onto an entry
// that already exists under `base` with different case ("Chr" -> "chr"), so the
// mod's files land in the unpacked game's folders instead of creating a second,
// differently-cased copy the game (via Wine's case-insensitive lookup) might
// not pick. Components that don't exist yet are kept as given.
static fs::path resolveCaseInsensitive(const fs::path& base, const std::string& rel) {
    fs::path cur = base;
    size_t i = 0;
    while (i <= rel.size()) {
        size_t j = rel.find('/', i);
        std::string comp = rel.substr(i, j == std::string::npos ? std::string::npos : j - i);
        if (!comp.empty()) {
            std::error_code ec;
            if (!fs::exists(cur / comp, ec)) {
                std::string want = comp;
                for (char& c : want) c = char(std::tolower((unsigned char)c));
                for (fs::directory_iterator it(cur, ec), end; !ec && it != end; it.increment(ec)) {
                    std::string have = it->path().filename().string();
                    std::string low = have;
                    for (char& c : low) c = char(std::tolower((unsigned char)c));
                    if (low == want) { comp = have; break; }
                }
            }
            cur /= comp;
        }
        if (j == std::string::npos) break;
        i = j + 1;
    }
    return cur;
}
#endif

// If every entry lives under one shared top-level folder (e.g. a zip that wraps
// its contents in "mctde 0.88 (STABLE)/"), return that prefix to strip so the
// contents land directly in outDir. "" if there's no single common root.
static std::string commonRoot(mz_zip_archive& zip, mz_uint count) {
    std::string root;
    bool have = false;
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat s;
        if (!mz_zip_reader_file_stat(&zip, i, &s)) continue;
        std::string n = s.m_filename;
        for (char& c : n) if (c == '\\') c = '/';
        size_t slash = n.find('/');
        if (slash == std::string::npos) return "";  // a root-level file -> can't strip
        std::string top = n.substr(0, slash);
        if (!have) { root = top; have = true; }
        else if (top != root) return "";
    }
    return have ? root + "/" : "";
}

ExtractStats extractZip(const std::string& zipPath, const std::string& outDir,
                        std::string& err) {
    ExtractStats st;
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, zipPath.c_str(), 0)) {
        err = "cannot open zip: " + zipPath;
        return st;
    }

    mz_uint count = mz_zip_reader_get_num_files(&zip);
    std::string strip = commonRoot(zip, count);

    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat s;
        if (!mz_zip_reader_file_stat(&zip, i, &s)) { ++st.errors; continue; }

        std::string rel;
        if (!safeRelPath(s.m_filename, rel)) { ++st.errors; continue; }
        if (!strip.empty() && rel.rfind(strip, 0) == 0) rel.erase(0, strip.size());
        if (rel.empty()) continue;  // the stripped root directory entry itself
#ifdef _WIN32
        fs::path out = fs::path(outDir) / fs::path(rel);
#else
        fs::path out = resolveCaseInsensitive(fs::path(outDir), rel);
#endif

        std::error_code ec;
        if (mz_zip_reader_is_file_a_directory(&zip, i)) {
            fs::create_directories(out, ec);
            continue;
        }
        if (out.has_parent_path()) fs::create_directories(out.parent_path(), ec);

        // Preserve an existing dsfix.ini. Never clobber the user's DSFix settings.
        std::string base = out.filename().string();
        for (char& c : base) c = char(std::tolower((unsigned char)c));
        if (base == "dsfix.ini" && fs::exists(out, ec)) { ++st.files; continue; }

        if (!mz_zip_reader_extract_to_file(&zip, i, out.string().c_str(), 0)) {
            ++st.errors;
            continue;
        }
        ++st.files;
    }

    mz_zip_reader_end(&zip);
    return st;
}

// Paths are UTF-8. The zip is read and the entry written through std::filesystem
// (wide APIs on Windows) rather than miniz's fopen, so a non-ASCII Windows user
// folder still works.
bool extractZipEntry(const std::string& zipPath, const std::string& baseName,
                     const std::string& outPath, std::string& err) {
    std::vector<char> bytes;
    {
        std::ifstream f(fs::u8path(zipPath), std::ios::binary);
        if (!f) { err = "cannot open zip: " + zipPath; return false; }
        bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    if (bytes.empty() || !mz_zip_reader_init_mem(&zip, bytes.data(), bytes.size(), 0)) {
        err = "not a valid zip: " + zipPath;
        return false;
    }
    auto lower = [](std::string s) {
        for (char& c : s) c = char(std::tolower((unsigned char)c));
        return s;
    };
    const std::string want = lower(baseName);
    bool ok = false, found = false;
    mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count && !found; ++i) {
        mz_zip_archive_file_stat s;
        if (!mz_zip_reader_file_stat(&zip, i, &s) || mz_zip_reader_is_file_a_directory(&zip, i))
            continue;
        std::string n = s.m_filename;
        for (char& c : n) if (c == '\\') c = '/';
        size_t slash = n.find_last_of('/');
        if (lower(slash == std::string::npos ? n : n.substr(slash + 1)) != want) continue;
        found = true;
        size_t size = 0;
        void* data = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);   // CRC-checked
        if (data) {
            std::ofstream o(fs::u8path(outPath), std::ios::binary | std::ios::trunc);
            o.write(static_cast<const char*>(data), (std::streamsize)size);
            ok = o.good();
            mz_free(data);
        }
        if (!ok) err = "could not extract " + baseName + " from " + zipPath;
    }
    mz_zip_reader_end(&zip);
    if (!found) err = baseName + " is not in " + zipPath;
    return ok;
}

} // namespace mctde
