// OzPack — Angels95 / AngelTech package packer/unpacker
// Usage:
//   OzPack pack <magic> <input_dir> <output.ozpak>
//   OzPack unpack <input.ozpak> <output_dir>
//   OzPack list <input.ozpak>
//   OzPack dir <input_dir> <output.ozpak>   -- auto-detect magic from content
//
// Magic values: OZPK (models), OZTX (textures), OZSD (sounds), OZMX (music), OZWN (worlds)

#include "Package/OzPackage.hpp"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <filesystem>
#include <stdexcept>
namespace fs = std::filesystem;

static uint32_t ParseMagic(const char* s) {
    if (strcmp(s, "OZPK") == 0) return OZ_PACKAGE_MAGIC_PK;
    if (strcmp(s, "OZTX") == 0) return OZ_PACKAGE_MAGIC_TX;
    if (strcmp(s, "OZSD") == 0) return OZ_PACKAGE_MAGIC_SD;
    if (strcmp(s, "OZMX") == 0) return OZ_PACKAGE_MAGIC_MX;
    if (strcmp(s, "OZWN") == 0) return OZ_PACKAGE_MAGIC_WN;
    return 0;
}

static const char* MagicLabel(uint32_t m) {
    switch (m) {
        case OZ_PACKAGE_MAGIC_PK: return "OZPK";
        case OZ_PACKAGE_MAGIC_TX: return "OZTX";
        case OZ_PACKAGE_MAGIC_SD: return "OZSD";
        case OZ_PACKAGE_MAGIC_MX: return "OZMX";
        case OZ_PACKAGE_MAGIC_WN: return "OZWN";
        default: return "????";
    }
}

static uint32_t AutoDetectMagic(const fs::path& dir) {
    // Check contents of directory to determine best magic
    bool hasObj = false, hasPng = false, hasWav = false, hasMp3 = false, hasOzone = false;
    try {
        for (auto& p : fs::recursive_directory_iterator(dir)) {
            auto ext = p.path().extension().string();
            if (ext == ".obj" || ext == ".mtl" || ext == ".ps") hasObj = true;
            if (ext == ".png" || ext == ".jpg" || ext == ".bmp") hasPng = true;
            if (ext == ".wav") hasWav = true;
            if (ext == ".mp3" || ext == ".ogg") hasMp3 = true;
            if (ext == ".ozone") hasOzone = true;
        }
    } catch (const fs::filesystem_error&) {
        // Fall through to return default
    }
    if (hasOzone) return OZ_PACKAGE_MAGIC_WN;
    if (hasObj)   return OZ_PACKAGE_MAGIC_PK;
    if (hasWav || hasMp3) return hasMp3 ? OZ_PACKAGE_MAGIC_MX : OZ_PACKAGE_MAGIC_SD;
    if (hasPng)   return OZ_PACKAGE_MAGIC_TX;
    return OZ_PACKAGE_MAGIC_PK; // default
}

static bool CmdPack(int argc, char** argv) {
    if (argc < 5) {
        fprintf(stderr, "Usage: OzPack pack <magic|auto> <input_dir> <output.ozpak>\n");
        return false;
    }

    uint32_t magic;
    if (strcmp(argv[2], "auto") == 0)
        magic = AutoDetectMagic(argv[3]);
    else
        magic = ParseMagic(argv[2]);
    if (magic == 0) {
        fprintf(stderr, "Unknown magic '%s'. Use OZPK, OZTX, OZSD, OZMX, OZWN, or auto\n", argv[2]);
        return false;
    }

    const char* inputDir = argv[3];
    const char* outputPath = argv[4];

    fs::path inPath(inputDir);
    if (!fs::is_directory(inPath)) {
        fprintf(stderr, "Not a directory: %s\n", inputDir);
        return false;
    }

    OzPackageWriter writer(magic);
    size_t totalBytes = 0;
    int fileCount = 0;

    // Walk directory recursively, collecting all files
    try {
        for (auto& p : fs::recursive_directory_iterator(inPath)) {
            if (!p.is_regular_file()) continue;
            std::string name;
            try {
                auto relPath = fs::relative(p.path(), inPath);
                name = relPath.string();
            } catch (const fs::filesystem_error& e) {
                fprintf(stderr, "Warning: path resolution error for %s: %s\n",
                        p.path().string().c_str(), e.what());
                continue;
            }
            // Normalize to forward slashes
            for (auto& c : name) if (c == '\\') c = '/';

            std::vector<uint8_t> fileData;
            FILE* f = fopen(p.path().string().c_str(), "rb");
            if (!f) { fprintf(stderr, "Warning: could not open %s\n", p.path().string().c_str()); continue; }
            fseek(f, 0, SEEK_END);
            size_t sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            fileData.resize(sz);
            if (fread(fileData.data(), 1, sz, f) != sz) {
                fprintf(stderr, "Warning: read error %s\n", p.path().string().c_str());
                fclose(f);
                continue;
            }
            fclose(f);

            writer.AddFile(name.c_str(), fileData.data(), fileData.size());
            totalBytes += sz;
            fileCount++;
        }
    } catch (const fs::filesystem_error& e) {
        fprintf(stderr, "Filesystem error: %s\n", e.what());
        return false;
    } catch (const std::exception& e) {
        fprintf(stderr, "Unexpected error: %s\n", e.what());
        return false;
    }

    if (fileCount == 0) {
        fprintf(stderr, "No files found in %s\n", inputDir);
        return false;
    }

    if (!writer.WriteToFile(outputPath)) {
        fprintf(stderr, "Failed to write %s\n", outputPath);
        return false;
    }

    fprintf(stdout, "Packed %d files (%zu bytes) -> %s  [%s]\n",
            fileCount, totalBytes, outputPath, MagicLabel(magic));
    return true;
}

static bool CmdUnpack(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "Usage: OzPack unpack <input.ozpak> <output_dir>\n");
        return false;
    }

    const char* inputPath = argv[2];
    const char* outputDir = argv[3];

    OzPackageReader reader;
    if (!reader.Open(inputPath)) {
        fprintf(stderr, "Failed to open %s\n", inputPath);
        return false;
    }

    fs::create_directories(outputDir);

    int fileCount = 0;
    size_t totalBytes = 0;
    for (auto& e : reader.Entries()) {
        std::string outPath = (fs::path(outputDir) / e.filename).string();
        fs::create_directories(fs::path(outPath).parent_path());

        size_t sz;
        const uint8_t* data = reader.GetData(e.filename, sz);
        if (!data) continue;

        FILE* f = fopen(outPath.c_str(), "wb");
        if (!f) { fprintf(stderr, "Warning: could not write %s\n", outPath.c_str()); continue; }
        fwrite(data, 1, sz, f);
        fclose(f);
        fileCount++;
        totalBytes += sz;
    }

    fprintf(stdout, "Unpacked %d files (%zu bytes) <- %s\n", fileCount, totalBytes, inputPath);
    return true;
}

static bool CmdList(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: OzPack list <input.ozpak>\n");
        return false;
    }

    OzPackageReader reader;
    if (!reader.Open(argv[2])) {
        fprintf(stderr, "Failed to open %s\n", argv[2]);
        return false;
    }

    printf("Package: %s\n", argv[2]);
    printf("Format:  %s v%u\n", MagicLabel(reader.Magic()), (unsigned)OZ_PACKAGE_VERSION);
    printf("Entries: %u\n\n", (unsigned)reader.EntryCount());

    size_t total = 0;
    for (auto& e : reader.Entries()) {
        printf("  %-8llu  %-8llu  %s\n",
               (unsigned long long)e.sizeRaw,
               (unsigned long long)e.offset,
               e.filename);
        total += (size_t)e.sizeRaw;
    }
    printf("\nTotal: %zu bytes\n", total);
    return true;
}

static bool CmdDir(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "Usage: OzPack dir <input_dir> <output.ozpak>\n");
        return false;
    }
    // Rebuild argv for CmdPack with auto-detect
    const char* fakeArgs[] = {"OzPack", "pack", "auto", argv[2], argv[3]};
    return CmdPack(5, (char**)fakeArgs);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Angels95 OzPack v1 — Package Packer/Unpacker\n");
        fprintf(stderr, "Usage:\n");
        fprintf(stderr, "  OzPack pack <magic|auto> <dir> <out.ozpak>\n");
        fprintf(stderr, "  OzPack unpack <in.ozpak> <out_dir>\n");
        fprintf(stderr, "  OzPack list <in.ozpak>\n");
        fprintf(stderr, "  OzPack dir <dir> <out.ozpak>\n");
        return 1;
    }

    if (strcmp(argv[1], "pack") == 0)   return CmdPack(argc, argv) ? 0 : 1;
    if (strcmp(argv[1], "unpack") == 0) return CmdUnpack(argc, argv) ? 0 : 1;
    if (strcmp(argv[1], "list") == 0)   return CmdList(argc, argv) ? 0 : 1;
    if (strcmp(argv[1], "dir") == 0)    return CmdDir(argc, argv) ? 0 : 1;

    fprintf(stderr, "Unknown command: %s\n", argv[1]);
    return 1;
}
