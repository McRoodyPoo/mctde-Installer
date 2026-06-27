#include "Unpacker.h"
#include "Bhd5.h"
#include "Bnd3.h"
#include "C4110Header.h"
#include "Dcx.h"
#include "NameHash.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <unordered_set>

namespace fs = std::filesystem;

namespace mctde {

static std::vector<uint8_t> readWhole(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    f.seekg(0, std::ios::end);
    std::streamoff n = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(n > 0 ? size_t(n) : 0);
    if (n > 0) f.read(reinterpret_cast<char*>(buf.data()), n);
    return buf;
}

// Read one record's raw bytes from an open .bdt stream.
static bool readRecord(std::ifstream& bdt, const Bhd5Record& r, std::vector<uint8_t>& out) {
    out.assign(r.size, 0);
    bdt.seekg(static_cast<std::streamoff>(r.offset), std::ios::beg);
    if (r.size) bdt.read(reinterpret_cast<char*>(out.data()), r.size);
    return bdt.good() || bdt.gcount() == static_cast<std::streamsize>(r.size);
}

static std::string hexHash(uint32_t h) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08X", h);
    return std::string(buf);
}

// Convert a namelist virtual path (e.g. "/chr/c0000.anibnd.dcx") to the loose
// on-disk path. The DCX-disable exe patch makes the engine request the file
// without the ".dcx" suffix, so the unpacked file drops it (content is already
// decompressed). Leading slash is stripped too.
static std::string outputRel(const std::string& vpath) {
    std::string rel = vpath;
    if (!rel.empty() && rel.front() == '/') rel.erase(0, 1);
    const std::string dcx = ".dcx";
    if (rel.size() >= dcx.size() &&
        rel.compare(rel.size() - dcx.size(), dcx.size(), dcx) == 0)
        rel.erase(rel.size() - dcx.size());
    return rel;
}

static void writeFile(const fs::path& path, const std::vector<uint8_t>& data) {
    if (path.has_parent_path()) fs::create_directories(path.parent_path());
    std::ofstream o(path, std::ios::binary | std::ios::trunc);
    if (!o) throw std::runtime_error("cannot write " + path.string());
    if (!data.empty()) o.write(reinterpret_cast<const char*>(data.data()),
                               static_cast<std::streamsize>(data.size()));
}

static std::string stripDcxSuffix(std::string s) {
    const std::string dcx = ".dcx";
    if (s.size() >= dcx.size() &&
        s.compare(s.size() - dcx.size(), dcx.size(), dcx) == 0)
        s.erase(s.size() - dcx.size());
    return s;
}

// Unpack one split (BHF3 header + BDF3 data) archive into targetBase.
// When `flatten` is set the entry's directory part is dropped (textures pool
// flat into map/tx); otherwise the entry's internal subpath is preserved.
static void unpackSplitArchive(const fs::path& bhdPath, const fs::path& bdtPath,
                               const fs::path& targetBase, bool flatten,
                               UnpackStats& st, const UnpackProgress& progress, int basePct) {
    Bnd3 b = parseBnd3(readWhole(bhdPath.string()));
    std::ifstream bdt(bdtPath, std::ios::binary);
    if (!bdt) throw std::runtime_error("cannot open " + bdtPath.string());

    std::vector<uint8_t> data;
    for (const Bnd3Entry& e : b.entries) {
        data.assign(e.size, 0);
        bdt.clear();
        bdt.seekg(static_cast<std::streamoff>(e.offset), std::ios::beg);
        if (e.size) bdt.read(reinterpret_cast<char*>(data.data()), e.size);
        if (bdt.gcount() != static_cast<std::streamsize>(e.size)) { ++st.errors; continue; }

        if (isDcx(data)) {
            try { data = dcxDecompress(data); ++st.decompressed; }
            catch (const std::exception&) { ++st.errors; }
        }
        std::string name = stripDcxSuffix(e.name);
        if (flatten) {
            size_t slash = name.find_last_of('/');
            if (slash != std::string::npos) name = name.substr(slash + 1);
        }
        writeFile(targetBase / fs::path(name), data);
        ++st.files;
        if (progress) progress(name, basePct);
    }
}

static std::string leafName(const std::string& name) {
    size_t slash = name.find_last_of('/');
    return slash == std::string::npos ? name : name.substr(slash + 1);
}

// chr textures: the BHF3 header lives *inside* the loose chrbnd as a
// ".chrtpfbhd" entry; the data is the external chrtpfbdt. Unpack to chr/cXXXX/,
// keep the chrbnd, consume the chrtpfbdt.
// baseDone/A position the chr archives within the nested progress range
// (75-100%): chr archive j reports pct = 75 + (baseDone + j) * 25 / A.
static void unpackChrTextures(const fs::path& root, UnpackStats& st,
                              const UnpackProgress& progress, size_t baseDone, size_t A) {
    std::vector<fs::path> bdts;
    fs::path chrDir = root / "chr";
    if (!fs::exists(chrDir)) return;
    for (const auto& de : fs::recursive_directory_iterator(chrDir)) {
        if (de.is_regular_file() && de.path().extension() == ".chrtpfbdt")
            bdts.push_back(de.path());
    }

    for (size_t j = 0; j < bdts.size(); ++j) {
      const fs::path& bdtPath = bdts[j];
      int basePct = A ? 75 + (int)((baseDone + j) * 25 / A) : 99;
      try {
        fs::path chrbnd = bdtPath; chrbnd.replace_extension(".chrbnd");
        if (!fs::exists(chrbnd)) { ++st.errors; continue; }

        std::vector<uint8_t> bndData = readWhole(chrbnd.string());
        Bnd3 bnd = parseBnd3(bndData);

        std::string stem = bdtPath.stem().string();  // cXXXX

        // Find the inner texture header entry inside the chrbnd.
        const Bnd3Entry* hdrEntry = nullptr;
        for (const Bnd3Entry& e : bnd.entries) {
            const std::string& n = e.name;
            if (n.size() >= 10 && n.compare(n.size() - 10, 10, ".chrtpfbhd") == 0) {
                hdrEntry = &e; break;
            }
        }

        std::vector<uint8_t> hdr;
        if (hdrEntry) {
            if (size_t(hdrEntry->offset) + hdrEntry->size > bndData.size()) { ++st.errors; continue; }
            hdr.assign(bndData.begin() + hdrEntry->offset,
                       bndData.begin() + hdrEntry->offset + hdrEntry->size);
            if (isDcx(hdr)) hdr = dcxDecompress(hdr);
        } else if (stem == "c4110") {
            // c4110's header is absent from the game; use the reconstruction.
            hdr.assign(c4110_chrtpfbhd, c4110_chrtpfbhd + c4110_chrtpfbhd_len);
        } else {
            ++st.errors; continue;
        }

        Bnd3 tpf = parseBnd3(hdr);
        std::ifstream bdt(bdtPath, std::ios::binary);
        if (!bdt) { ++st.errors; continue; }

        fs::path targetDir = root / "chr" / stem;
        std::vector<uint8_t> data;
        for (const Bnd3Entry& e : tpf.entries) {
            data.assign(e.size, 0);
            bdt.clear();
            bdt.seekg(static_cast<std::streamoff>(e.offset), std::ios::beg);
            if (e.size) bdt.read(reinterpret_cast<char*>(data.data()), e.size);
            if (bdt.gcount() != static_cast<std::streamsize>(e.size)) { ++st.errors; continue; }
            if (isDcx(data)) {
                try { data = dcxDecompress(data); ++st.decompressed; }
                catch (const std::exception&) { ++st.errors; }
            }
            std::string leaf = leafName(stripDcxSuffix(e.name));
            writeFile(targetDir / fs::path(leaf), data);
            ++st.files;
            if (progress) progress("chr/" + stem + "/" + leaf, basePct);
        }
        bdt.close();
        fs::remove(bdtPath);  // consume the data; keep the chrbnd
      } catch (const std::exception& e) {
        ++st.errors;
        std::fprintf(stderr, "  [chrtpf] %s: %s\n", bdtPath.string().c_str(), e.what());
      }
    }
}

void unpackNested(const std::string& outDir, UnpackStats& st,
                  const UnpackProgress& progress) {
    fs::path root(outDir);
    std::vector<fs::path> tpfbhd, hkxbhd;
    for (const auto& de : fs::recursive_directory_iterator(root)) {
        if (!de.is_regular_file()) continue;
        std::string ext = de.path().extension().string();
        if (ext == ".tpfbhd") tpfbhd.push_back(de.path());
        else if (ext == ".hkxbhd") hkxbhd.push_back(de.path());
    }
    // Count chr texture archives too, so the nested progress range (75-100%) is
    // spread across every second-level archive.
    size_t chrCount = 0;
    fs::path chrDir = root / "chr";
    if (fs::exists(chrDir))
        for (const auto& de : fs::recursive_directory_iterator(chrDir))
            if (de.is_regular_file() && de.path().extension() == ".chrtpfbdt") ++chrCount;

    size_t A = tpfbhd.size() + hkxbhd.size() + chrCount, done = 0;
    auto basePct = [&]() -> int { return A ? 75 + (int)(done * 25 / A) : 99; };

    // Map textures collapse into a single map/tx pool (per the tpfbnd:->map:/tx
    // engine remap); collision archives unpack alongside themselves.
    fs::path txDir = root / "map" / "tx";
    for (const fs::path& bhd : tpfbhd) {
        fs::path bdt = bhd; bdt.replace_extension(".tpfbdt");
        if (!fs::exists(bdt)) { ++st.errors; ++done; continue; }
        try {
            unpackSplitArchive(bhd, bdt, txDir, /*flatten=*/true, st, progress, basePct());
            fs::remove(bhd); fs::remove(bdt);
        } catch (const std::exception& e) {
            ++st.errors;
            std::fprintf(stderr, "  [tpf] %s: %s\n", bhd.string().c_str(), e.what());
        }
        ++done;
    }
    // hkx entry names already carry their map-relative subpath (e.g.
    // m10_00_00_00/h0000B0A10.hkx), so write them under map/ unflattened.
    fs::path mapDir = root / "map";
    for (const fs::path& bhd : hkxbhd) {
        fs::path bdt = bhd; bdt.replace_extension(".hkxbdt");
        if (!fs::exists(bdt)) { ++st.errors; ++done; continue; }
        try {
            unpackSplitArchive(bhd, bdt, mapDir, /*flatten=*/false, st, progress, basePct());
            fs::remove(bhd); fs::remove(bdt);
        } catch (const std::exception& e) {
            ++st.errors;
            std::fprintf(stderr, "  [hkx] %s: %s\n", bhd.string().c_str(), e.what());
        }
        ++done;
    }

    unpackChrTextures(root, st, progress, done, A);
}

UnpackStats unpackAll(const std::string& dataDir,
                      const std::string& outDir,
                      const std::string& namelistFile,
                      const UnpackProgress& progress) {
    NameMap names;
    if (namelistFile.empty()) names.loadEmbedded();
    else names.load(namelistFile);

    // Parse all four headers first so we know the total record count (D) up
    // front, so the dvdbnd pass can report a real 0-75% percentage.
    struct Src { Bhd5 bhd; std::string bdt; };
    std::vector<Src> srcs;
    size_t D = 0;
    for (int i = 0; i < 4; ++i) {
        std::string base = dataDir + "/dvdbnd" + std::to_string(i);
        std::string bhdPath = base + ".bhd5";
        std::string bdtPath = base + ".bdt";
        if (!fs::exists(bhdPath) || !fs::exists(bdtPath)) continue;
        srcs.push_back({ parseBhd5(readWhole(bhdPath)), bdtPath });
        D += srcs.back().bhd.records.size();
    }

    UnpackStats st;
    std::vector<uint8_t> data;

    // A few assets ship in the dvdbnd as BOTH a compressed "<name>.dcx" record and
    // a stale raw "<name>" record. They hash to different names but collapse to the
    // same loose output once ".dcx" is stripped (e.g. menu/menu.drb). The patched
    // engine loads the decompressed .dcx form, so when both exist the .dcx must
    // win. Otherwise the raw copy (often an older, smaller build) overwrites it.
    // That is exactly what truncated menu.drb and stripped the title-screen and
    // in-game menu entries. Collect every output a .dcx record will produce so the
    // colliding raw records can be skipped below.
    auto endsWithDcx = [](const std::string& s) {
        return s.size() >= 4 && s.compare(s.size() - 4, 4, ".dcx") == 0;
    };
    std::unordered_set<std::string> dcxOutputs;
    for (const Src& src : srcs)
        for (const Bhd5Record& r : src.bhd.records)
            if (const std::string* p = names.find(r.nameHash))
                if (endsWithDcx(*p))
                    dcxOutputs.insert(outputRel(*p));

    for (const Src& src : srcs) {
        std::ifstream bdt(src.bdt, std::ios::binary);
        if (!bdt) throw std::runtime_error("cannot open " + src.bdt);

        for (const Bhd5Record& r : src.bhd.records) {
            const std::string* p = names.find(r.nameHash);

            // Drop a raw record when a .dcx record owns the same output path.
            if (p && !endsWithDcx(*p) && dcxOutputs.count(outputRel(*p))) {
                ++st.skippedDup;
                continue;
            }

            if (!readRecord(bdt, r, data)) { ++st.errors; continue; }

            fs::path rel;
            if (p) {
                rel = fs::path(outputRel(*p));
            } else {
                rel = fs::path("_unknown") / (hexHash(r.nameHash) + ".bin");
                ++st.unknown;
            }

            if (isDcx(data)) {
                try {
                    data = dcxDecompress(data);
                    ++st.decompressed;
                } catch (const std::exception&) {
                    ++st.errors;  // leave raw, still written
                }
            }

            writeFile(fs::path(outDir) / rel, data);
            ++st.files;
            if (progress) progress(rel.string(), D ? (int)(st.files * 75 / D) : -1);
        }
    }

    // Second level: unpack the inner split archives the dvdbnd just produced.
    unpackNested(outDir, st, progress);
    return st;
}

std::vector<uint8_t> extractOne(const std::string& dataDir,
                                const std::string& virtualPath) {
    uint32_t want = hashPath(virtualPath);
    std::vector<uint8_t> data;
    for (int i = 0; i < 4; ++i) {
        std::string base = dataDir + "/dvdbnd" + std::to_string(i);
        std::string bhdPath = base + ".bhd5";
        std::string bdtPath = base + ".bdt";
        if (!fs::exists(bhdPath) || !fs::exists(bdtPath)) continue;

        Bhd5 bhd = parseBhd5(readWhole(bhdPath));
        for (const Bhd5Record& r : bhd.records) {
            if (r.nameHash != want) continue;
            std::ifstream bdt(bdtPath, std::ios::binary);
            if (!bdt) throw std::runtime_error("cannot open " + bdtPath);
            if (!readRecord(bdt, r, data))
                throw std::runtime_error("short read for " + virtualPath);
            return isDcx(data) ? dcxDecompress(data) : data;
        }
    }
    throw std::runtime_error("path not found in any dvdbnd: " + virtualPath);
}

} // namespace mctde
