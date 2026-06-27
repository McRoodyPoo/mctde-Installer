#include "DisplayConfig.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace mctde {

// One setting we enforce: [section] key = value.
struct IniEdit { const char* section; const char* key; const char* value; };

// The display tweaks DSFix needs. Order is the order they're written when the
// ini has to be created from scratch.
static const IniEdit kEdits[] = {
    { "DisplaySetting",       "WindowMode",   "1" },  // windowed (DSFix requires it)
    { "DisplaySettingFilter", "Antialiasing", "0" },  // DSFix supplies its own AA
};

// %LOCALAPPDATA% as a wide path, so a non-ASCII Windows username still resolves.
static std::wstring localAppData() {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p)) && p)
        out = p;
    if (p) CoTaskMemFree(p);
    return out;
}

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]))
            return false;
    return true;
}

// Split into lines, each retaining its own line terminator (so a mixed/odd file
// is rewritten byte-for-byte except where we touch it).
static std::vector<std::string> splitKeepEol(const std::string& s) {
    std::vector<std::string> lines;
    size_t i = 0;
    while (i < s.size()) {
        size_t nl = s.find('\n', i);
        if (nl == std::string::npos) { lines.push_back(s.substr(i)); break; }
        lines.push_back(s.substr(i, nl - i + 1));
        i = nl + 1;
    }
    return lines;
}

// The terminator on a line ("\r\n", "\n", or `fallback` if the line has none,
// e.g. a final line with no trailing newline).
static std::string eolOf(const std::string& line, const std::string& fallback) {
    if (line.size() >= 2 && line[line.size() - 2] == '\r' && line.back() == '\n')
        return "\r\n";
    if (!line.empty() && line.back() == '\n') return "\n";
    return fallback;
}

// Drop the trailing terminator from a line for inspection.
static std::string stripEol(const std::string& line) {
    std::string c = line;
    while (!c.empty() && (c.back() == '\n' || c.back() == '\r')) c.pop_back();
    return c;
}

static bool isSectionHeader(const std::string& content, std::string& name) {
    std::string t = trim(content);
    if (t.size() >= 2 && t.front() == '[' && t.back() == ']') { name = t; return true; }
    return false;
}

static std::string join(const std::vector<std::string>& v) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) { if (i) s += "; "; s += v[i]; }
    return s;
}

// Force `[section] key = value` in `lines`, preserving everything else and the
// file's line-ending style. Returns whether a write is needed; `note` describes
// the action for the install log. Cases: key present-but-wrong (rewrite line),
// key missing (insert after the section header), section missing (append both).
static bool applyEdit(std::vector<std::string>& lines, const IniEdit& e, std::string& note) {
    const std::string section = std::string("[") + e.section + "]";
    const std::string kv = std::string(e.key) + " = " + e.value;

    // Locate the section exactly (so [DisplaySetting] != [DisplaySettingFilter]).
    int headerIdx = -1, sectionEnd = (int)lines.size();
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string name;
        if (isSectionHeader(stripEol(lines[i]), name) && iequals(name, section)) {
            headerIdx = (int)i;
            for (size_t j = i + 1; j < lines.size(); ++j) {
                std::string n2;
                if (isSectionHeader(stripEol(lines[j]), n2)) { sectionEnd = (int)j; break; }
            }
            break;
        }
    }

    if (headerIdx < 0) {
        std::string eol = lines.empty() ? "\r\n" : eolOf(lines.back(), "\r\n");
        if (!lines.empty()) {                       // finish a dangling last line first
            std::string& last = lines.back();
            if (last.empty() || last.back() != '\n') last += eol;
        }
        lines.push_back(section + eol);
        lines.push_back(kv + eol);
        note = "added " + section + " " + kv;
        return true;
    }

    for (int i = headerIdx + 1; i < sectionEnd; ++i) {
        std::string content = stripEol(lines[i]);
        size_t eq = content.find('=');
        if (eq == std::string::npos) continue;
        if (!iequals(trim(content.substr(0, eq)), e.key)) continue;
        if (trim(content.substr(eq + 1)) == e.value) {
            note = std::string(e.key) + " already = " + e.value;
            return false;
        }
        lines[i] = kv + eolOf(lines[i], "\r\n");
        note = "set " + kv;
        return true;
    }

    // Section exists but the key is absent, so insert right after the header.
    lines.insert(lines.begin() + headerIdx + 1, kv + eolOf(lines[headerIdx], "\r\n"));
    note = "added " + kv + " to " + section;
    return true;
}

static bool writeAtomic(const fs::path& target, const std::string& bytes, std::string& message) {
    fs::path tmp = target;
    tmp += ".mctde-tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) { message = "cannot write " + tmp.string(); return false; }
        out.write(bytes.data(), (std::streamsize)bytes.size());
        if (!out) { message = "write failed for " + tmp.string(); return false; }
    }
    std::error_code ec;
    fs::remove(target, ec);                 // rename won't overwrite on Windows
    fs::rename(tmp, target, ec);
    if (ec) {
        fs::remove(tmp, ec);
        message = "could not replace " + target.string() + ": " + ec.message();
        return false;
    }
    return true;
}

bool ensureDisplayConfig(std::string& message) {
    std::wstring lad = localAppData();
    if (lad.empty()) { message = "could not resolve %LOCALAPPDATA%"; return false; }

    fs::path ini = fs::path(lad) / L"NBGI" / L"DarkSouls" / L"DarkSouls.ini";
    std::error_code ec;

    std::vector<std::string> lines;
    bool existed = fs::exists(ini, ec);
    if (existed) {
        std::ifstream in(ini, std::ios::binary);
        if (!in) { message = "cannot read " + ini.string(); return false; }
        std::ostringstream ss; ss << in.rdbuf();
        lines = splitKeepEol(ss.str());
    } else {
        // Game never launched -> no ini yet. We build a minimal one; the game
        // fills in the rest (keybinds, resolution) on first run, matched to the
        // player's own setup, and reads our values at startup.
        fs::create_directories(ini.parent_path(), ec);
        if (ec) { message = "could not create " + ini.parent_path().string(); return false; }
    }

    bool changed = false;
    std::vector<std::string> notes;
    for (const IniEdit& e : kEdits) {
        std::string note;
        changed |= applyEdit(lines, e, note);
        notes.push_back(note);
    }

    if (existed && !changed) {
        message = join(notes);          // all already correct; nothing to write
        return true;
    }

    std::string out;
    for (const auto& l : lines) out += l;
    if (!writeAtomic(ini, out, message)) return false;

    message = (existed ? "" : "created DarkSouls.ini; ") + join(notes);
    return true;
}

} // namespace mctde
