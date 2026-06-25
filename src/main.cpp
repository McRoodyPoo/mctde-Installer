//
// mctde-installer — a C++ take on UDSFM: PTDE unpacker / installer.
//
// This early build exposes the core decoders as a probe CLI so they can be
// validated against real files before the full install flow is wired up.
//
//   mctde-installer bhd5 <file.bhd5>     parse a BHD5 header, list records
//   mctde-installer dcx  <file>          detect + decompress a DCX container
//
#include "Bhd5.h"
#include "Bnd3.h"
#include "Detect.h"
#include "Dcx.h"
#include "Download.h"
#include "Extract.h"
#include "ExePatch.h"
#include "Installer.h"
#include "NameHash.h"
#include "Unpacker.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <cstdlib>
#include <string>
#include <unordered_set>
#include <vector>

static std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    f.seekg(0, std::ios::end);
    std::streamoff n = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(n > 0 ? size_t(n) : 0);
    if (n > 0) f.read(reinterpret_cast<char*>(buf.data()), n);
    return buf;
}

int main(int argc, char** argv) {
    using namespace mctde;

    if (argc < 2) {
        std::printf("usage:\n");
        std::printf("  mctde-installer detect\n");
        std::printf("  mctde-installer install <dataDir> [namelist]\n");
        std::printf("  mctde-installer unpack  <dataDir> <out> [namelist]\n");
        std::printf("  mctde-installer patchexe <inExe> <outExe>\n");
        return 1;
    }

    std::string cmd = argv[1];
    try {
        if (cmd == "bhd5") {
            auto data = readFile(argv[2]);
            Bhd5 bhd = parseBhd5(data);
            std::printf("records: %zu\n", bhd.records.size());
            for (size_t i = 0; i < bhd.records.size() && i < 8; ++i) {
                const Bhd5Record& r = bhd.records[i];
                std::printf("  [%zu] hash=%08X size=%-10u offset=%llu\n",
                            i, r.nameHash, r.size,
                            static_cast<unsigned long long>(r.offset));
            }
            return 0;
        }

        if (cmd == "bnd3") {
            auto data = readFile(argv[2]);
            Bnd3 b = parseBnd3(data);
            std::printf("split=%d entries=%zu\n", int(b.split), b.entries.size());
            for (size_t i = 0; i < b.entries.size() && i < 8; ++i) {
                const Bnd3Entry& e = b.entries[i];
                std::printf("  [%zu] off=%08X size=%-8u id=%u\n      name=%s\n      raw =%s\n",
                            i, e.offset, e.size, e.id, e.name.c_str(), e.rawName.c_str());
            }
            return 0;
        }

        if (cmd == "download" || cmd == "gdrive") {
            if (argc < 4) { std::printf("usage: %s <urlOrId> <out> [maxBytes]\n", cmd.c_str()); return 1; }
            uint64_t maxBytes = (argc >= 5) ? strtoull(argv[4], nullptr, 10) : 0;
            ProgressFn prog = [maxBytes](uint64_t got, uint64_t) -> bool {
                return !(maxBytes && got >= maxBytes);  // cancel once we have enough
            };
            std::string err;
            bool ok = (cmd == "download")
                          ? downloadUrl(argv[2], argv[3], err, prog)
                          : downloadGoogleDrive(argv[2], argv[3], err, prog);
            std::printf("result: %s  err=%s\n", ok ? "ok" : "fail", err.c_str());
            std::ifstream f(argv[3], std::ios::binary);
            if (f) {
                f.seekg(0, std::ios::end);
                long long sz = f.tellg();
                f.seekg(0);
                unsigned char h[8] = {0};
                f.read(reinterpret_cast<char*>(h), 8);
                std::printf("file: %lld bytes, first8:", sz);
                for (int i = 0; i < 8; ++i) std::printf(" %02X", h[i]);
                std::printf("  '");
                for (int i = 0; i < 8; ++i) std::printf("%c", (h[i] >= 32 && h[i] < 127) ? h[i] : '.');
                std::printf("'\n");
            }
            return ok ? 0 : 2;
        }

        if (cmd == "extractzip") {
            if (argc < 4) { std::printf("usage: extractzip <zip> <outDir>\n"); return 1; }
            std::string err;
            ExtractStats st = extractZip(argv[2], argv[3], err);
            if (!err.empty()) { std::printf("error: %s\n", err.c_str()); return 2; }
            std::printf("extracted %zu files (%zu errors)\n", st.files, st.errors);
            return 0;
        }

        if (cmd == "detectall") {
            int n = 0;
            findAllDataDirs([&](const GameInstall& gi) {
                const char* st = gi.state == GameState::Packed ? "packed"
                               : gi.state == GameState::Unpacked ? "unpacked" : "unknown";
                std::printf("  [%-9s] %-9s %s\n", gi.steam ? "Steam" : "non-Steam", st,
                            gi.dataDir.c_str());
                ++n;
            });
            std::printf("found %d install(s)\n", n);
            return 0;
        }

        if (cmd == "detect") {
            std::string data = detectSteamDataDir();
            if (data.empty()) { std::printf("no PTDE install found via Steam\n"); return 0; }
            const char* s = "unknown";
            switch (detectGameState(data)) {
                case GameState::Packed:   s = "packed";   break;
                case GameState::Unpacked: s = "unpacked"; break;
                case GameState::Unknown:  s = "unknown";  break;
            }
            std::printf("found: %s\nstate: %s\n", data.c_str(), s);
            return 0;
        }

        if (cmd == "fullinstall") {
            // fullinstall <dataDir> [namelist] [--backup-packed] [--backup-unpacked]
            if (argc < 3) {
                std::printf("usage: fullinstall <dataDir> [namelist] [--backup-packed] [--backup-unpacked]\n");
                return 1;
            }
            std::string namelist;
            bool bPacked = false, bUnpacked = false;
            for (int i = 3; i < argc; ++i) {
                std::string a = argv[i];
                if (a == "--backup-packed")        bPacked = true;
                else if (a == "--backup-unpacked") bUnpacked = true;
                else if (namelist.empty())         namelist = a;  // first non-flag = namelist
            }
            std::string msg;
            InstallResult r = fullInstall(argv[2], namelist, msg,
                [](const std::string& s, int pct) { std::printf("[%4d%%] %s\n", pct, s.c_str()); },
                bPacked, bUnpacked);
            std::printf("%s\n", msg.c_str());
            return r == InstallResult::Failed ? 2 : 0;
        }

        if (cmd == "install") {
            // install <dataDir> [namelist]
            if (argc < 3) { std::printf("usage: install <dataDir> [namelist]\n"); return 1; }
            std::string namelist = argc >= 4 ? argv[3] : "";  // "" -> embedded namelist
            std::string msg;
            InstallResult r = installFlow(argv[2], namelist, msg);
            std::printf("%s\n", msg.c_str());
            return r == InstallResult::Failed ? 2 : 0;
        }

        if (cmd == "backups") {
            // backups <dataDir> : list restorable backup folders next to DATA
            if (argc < 3) { std::printf("usage: backups <dataDir>\n"); return 1; }
            auto bk = findBackups(argv[2]);
            if (bk.empty()) { std::printf("no backups found next to %s\n", argv[2]); return 0; }
            for (const auto& b : bk) {
                const char* st = b.state == GameState::Packed ? "packed"
                               : b.state == GameState::Unpacked ? "unpacked" : "unknown";
                std::printf("  [%-8s] %s\n", st, b.path.c_str());
            }
            return 0;
        }

        if (cmd == "restore") {
            // restore <backupDir> <dataDir> : copy a backup back over DATA
            if (argc < 4) { std::printf("usage: restore <backupDir> <dataDir>\n"); return 1; }
            std::string msg;
            InstallResult r = restoreFromBackup(argv[2], argv[3], msg,
                [](const std::string& s, int pct) { std::printf("[%4d%%] %s\n", pct, s.c_str()); });
            std::printf("%s\n", msg.c_str());
            return r == InstallResult::Failed ? 2 : 0;
        }

        if (cmd == "patchexe") {
            // patchexe <inExe> <outExe>
            if (argc < 4) { std::printf("usage: patchexe <inExe> <outExe>\n"); return 1; }
            std::string msg;
            PatchResult r = patchExe(argv[2], argv[3], msg);
            std::printf("%s\n", msg.c_str());
            return (r == PatchResult::Patched || r == PatchResult::AlreadyPatched) ? 0 : 2;
        }

        if (cmd == "diff") {
            // diff <fileA> <fileB> : list differing byte offsets (old -> new)
            auto a = readFile(argv[2]);
            auto b = readFile(argv[3]);
            std::printf("A=%zu bytes, B=%zu bytes\n", a.size(), b.size());
            size_t n = a.size() < b.size() ? a.size() : b.size();
            size_t diffs = 0;
            for (size_t i = 0; i < n; ++i) {
                if (a[i] != b[i]) {
                    ++diffs;
                    if (diffs <= 200)
                        std::printf("  %08zX: %02X -> %02X\n", i, a[i], b[i]);
                }
            }
            std::printf("total differing bytes: %zu%s\n", diffs,
                        a.size() != b.size() ? " (+ size mismatch)" : "");
            return 0;
        }

        if (cmd == "extract") {
            // extract <dataDir> <virtualPath> <outFile>
            if (argc < 5) { std::printf("usage: extract <dataDir> <vpath> <out>\n"); return 1; }
            auto data = extractOne(argv[2], argv[3]);
            std::ofstream o(argv[4], std::ios::binary | std::ios::trunc);
            if (!o) throw std::runtime_error(std::string("cannot write ") + argv[4]);
            if (!data.empty()) o.write(reinterpret_cast<const char*>(data.data()),
                                       std::streamsize(data.size()));
            char m[5] = {0};
            for (int k = 0; k < 4 && k < int(data.size()); ++k) {
                unsigned char c = data[k];
                m[k] = (c >= 32 && c < 127) ? char(c) : '.';
            }
            std::printf("extracted %zu bytes, magic='%s' (%02X %02X %02X %02X)\n",
                        data.size(), m,
                        data.size() > 0 ? data[0] : 0, data.size() > 1 ? data[1] : 0,
                        data.size() > 2 ? data[2] : 0, data.size() > 3 ? data[3] : 0);
            return 0;
        }

        if (cmd == "unpack") {
            // unpack <dataDir> <outDir> [namelist]
            if (argc < 4) { std::printf("usage: unpack <dataDir> <outDir> [namelist]\n"); return 1; }
            std::string namelist = argc >= 5 ? argv[4] : "";  // "" -> embedded namelist
            UnpackStats st = unpackAll(argv[2], argv[3], namelist);
            std::printf("files=%zu  decompressed=%zu  unknown=%zu  errors=%zu\n",
                        st.files, st.decompressed, st.unknown, st.errors);
            return 0;
        }

        if (cmd == "nested") {
            // nested <outDir> : run the inner-archive pass on an existing tree
            if (argc < 3) { std::printf("usage: nested <outDir>\n"); return 1; }
            UnpackStats st;
            unpackNested(argv[2], st);
            std::printf("files=%zu  decompressed=%zu  errors=%zu\n",
                        st.files, st.decompressed, st.errors);
            return 0;
        }

        if (cmd == "cover") {
            // cover <namelist> <bhd5...> : how many records does the namelist name?
            NameMap names;
            size_t paths = names.load(argv[2]);
            std::printf("namelist: %zu paths, %zu unique hashes\n", paths, names.size());

            size_t total = 0, matched = 0;
            std::vector<uint32_t> miss;
            for (int a = 3; a < argc; ++a) {
                auto data = readFile(argv[a]);
                Bhd5 bhd = parseBhd5(data);
                for (const Bhd5Record& r : bhd.records) {
                    ++total;
                    if (names.contains(r.nameHash)) ++matched;
                    else if (miss.size() < 20) miss.push_back(r.nameHash);
                }
            }
            std::printf("records: %zu  matched: %zu (%.2f%%)  unmatched: %zu\n",
                        total, matched,
                        total ? 100.0 * double(matched) / double(total) : 0.0,
                        total - matched);
            for (uint32_t h : miss) std::printf("  unmatched %08X\n", h);
            return 0;
        }

        if (cmd == "hashstr") {
            std::printf("%08X\n", hashPath(argv[2]));
            return 0;
        }

        if (cmd == "hashes") {
            auto data = readFile(argv[2]);
            Bhd5 bhd = parseBhd5(data);
            for (const Bhd5Record& r : bhd.records)
                std::printf("%08X\n", r.nameHash);
            return 0;
        }

        if (cmd == "dcx") {
            auto data = readFile(argv[2]);
            std::printf("input: %zu bytes, isDcx=%d\n",
                        data.size(), int(isDcx(data)));
            auto out = dcxDecompress(data);
            std::printf("output: %zu bytes\n", out.size());
            return 0;
        }

        std::printf("unknown command: %s\n", cmd.c_str());
        return 1;
    } catch (const std::exception& e) {
        std::printf("error: %s\n", e.what());
        return 2;
    }
}
