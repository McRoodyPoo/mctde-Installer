#include "Extract.h"
#include "miniz.h"

#include <cctype>
#include <cstring>
#include <filesystem>

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
        fs::path out = fs::path(outDir) / fs::path(rel);

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

} // namespace mctde
