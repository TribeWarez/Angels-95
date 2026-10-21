#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <unordered_map>
#include "../miniz/miniz.h"

// ============================================================================
// OzPackage — universal asset container format for Angels95 / AngelTech
//
// Package types (magic):
//   OZPK  — generic asset package (.ozpak) — models, mtl, scripts
//   OZTX  — texture package     (.oztex) — .png textures
//   OZSD  — sound package       (.ozsnd) — .wav, .mp3, .ogg
//   OZMX  — music package       (.ozmux) — .wav, .mp3
//   OZWN  — world package       (.ozone) — .ozone world files
//
// Format:
//   [HEADER]  32 bytes
//   [DATA_0]  file data (8-byte aligned start)
//   [DATA_1]
//   ...
//   [INDEX]   array of OzPackageEntry
//
// Compression (zlib/deflate via bundled miniz):
//   Header flags bit 0 = package may contain compressed entries.
//   Per-entry compression flag lives in OzPackageEntry.reserved bit 0.
//   Readers produced before this flag ignore both fields — the format
//   stays backwards compatible (they simply read sizeRaw bytes raw).
// ============================================================================

constexpr uint32_t OZ_PACKAGE_MAGIC_PK  = 0x4B5A504F; // "OZPK"
constexpr uint32_t OZ_PACKAGE_MAGIC_TX  = 0x58545A4F; // "OZTX"
constexpr uint32_t OZ_PACKAGE_MAGIC_SD  = 0x44535A4F; // "OZSD"
constexpr uint32_t OZ_PACKAGE_MAGIC_MX  = 0x584D5A4F; // "OZMX"
constexpr uint32_t OZ_PACKAGE_MAGIC_WN  = 0x4E575A4F; // "OZWN"

constexpr uint32_t OZ_PACKAGE_VERSION   = 1;
constexpr size_t   OZ_FILENAME_MAX      = 128;

// Header flag: package may contain deflate-compressed entries (per-entry flag
// in OzPackageEntry.reserved bit 0).
constexpr uint16_t OZ_PACKAGE_FLAG_COMPRESSED = 0x0001;
// Minimum raw size worth attempting to compress.
constexpr uint32_t OZ_PACKAGE_COMPRESS_MIN = 512;

#pragma pack(push, 1)
struct OzPackageHeader {
    uint32_t magic;           // OZPK/OZTX/OZSD/OZMX/OZWN
    uint16_t version;         // 1
    uint16_t flags;           // bit 0: zlib-compressed
    uint32_t entryCount;      // number of files
    uint32_t indexOffset;     // byte offset from start to index array
    uint32_t indexSize;       // byte size of index array
    uint32_t reserved[2];     // future use (8 bytes)
    // total: 32 bytes
};

struct OzPackageEntry {
    char     filename[OZ_FILENAME_MAX]; // null-terminated, forward slashes
    uint64_t offset;          // byte offset from start of file to data
    uint64_t sizeRaw;         // uncompressed size
    uint64_t sizePacked;      // compressed size (= sizeRaw if no compression)
    uint32_t reserved;        // future use
    // total: 156 bytes
};
#pragma pack(pop)

// ============================================================================
// Writer — builds a package from a list of (filename, data) pairs
// ============================================================================
struct OzPackageFile {
    std::string filename;
    std::vector<uint8_t> data;
};

class OzPackageWriter {
public:
    OzPackageWriter(uint32_t magic) : m_magic(magic) {}

    void AddFile(const char* name, const void* data, size_t size) {
        OzPackageFile f;
        f.filename = name;
        const uint8_t* p = (const uint8_t*)data;
        f.data.assign(p, p + size);
        m_files.push_back(std::move(f));
    }

    // enableCompression: deflate entries >= OZ_PACKAGE_COMPRESS_MIN when the
    // packed form is actually smaller (stored raw otherwise).
    bool WriteToFile(const char* path, bool enableCompression = true) {
        // Ensure parent directory exists
        std::string pathStr(path);
        auto slash = pathStr.find_last_of("/\\");
        if (slash != std::string::npos) {
            std::string dir = pathStr.substr(0, slash);
            std::filesystem::create_directories(dir);
        }
        FILE* f = fopen(path, "wb");
        if (!f) return false;

        // Calculate offsets
        OzPackageHeader hdr;
        memset(&hdr, 0, sizeof(hdr));
        hdr.magic = m_magic;
        hdr.version = OZ_PACKAGE_VERSION;
        hdr.flags = 0;
        hdr.entryCount = (uint32_t)m_files.size();

        uint64_t dataOff = sizeof(OzPackageHeader);

        // Sort files alphabetically for deterministic output
        std::sort(m_files.begin(), m_files.end(),
            [](const OzPackageFile& a, const OzPackageFile& b) {
                return a.filename < b.filename;
            });

        // Build entries array (compressing eligible files)
        struct OutBlob { const uint8_t* data; size_t size; };
        std::vector<OutBlob> blobs(m_files.size());
        std::vector<std::vector<uint8_t>> packed(m_files.size());

        for (size_t i = 0; i < m_files.size(); i++) {
            const auto& raw = m_files[i].data;
            if (enableCompression && raw.size() >= OZ_PACKAGE_COMPRESS_MIN) {
                mz_ulong packedCap = mz_compressBound((mz_ulong)raw.size());
                packed[i].resize((size_t)packedCap);
                mz_ulong packedSize = packedCap;
                if (mz_compress(packed[i].data(), &packedSize, raw.data(), (mz_ulong)raw.size()) == MZ_OK &&
                    packedSize > 0 && packedSize < (mz_ulong)raw.size()) {
                    packed[i].resize((size_t)packedSize);
                } else {
                    packed[i].clear(); // incompressible — store raw
                }
            }
            blobs[i].data = packed[i].empty() ? raw.data() : packed[i].data();
            blobs[i].size = packed[i].empty() ? raw.size() : packed[i].size();
        }

        std::vector<OzPackageEntry> entries;
        for (size_t i = 0; i < m_files.size(); i++) {
            OzPackageEntry e;
            memset(&e, 0, sizeof(e));
            strncpy(e.filename, m_files[i].filename.c_str(), OZ_FILENAME_MAX - 1);
            e.offset = dataOff;
            e.sizeRaw = m_files[i].data.size();
            e.sizePacked = blobs[i].size;
            e.reserved = packed[i].empty() ? 0 : OZ_PACKAGE_FLAG_COMPRESSED;
            entries.push_back(e);
            dataOff += blobs[i].size;
            if (!packed[i].empty()) hdr.flags |= OZ_PACKAGE_FLAG_COMPRESSED;
        }

        hdr.indexOffset = (uint32_t)dataOff;
        hdr.indexSize = (uint32_t)(entries.size() * sizeof(OzPackageEntry));

        // Write header
        fwrite(&hdr, sizeof(hdr), 1, f);

        // Write file data
        for (size_t i = 0; i < m_files.size(); i++) {
            fwrite(blobs[i].data, 1, blobs[i].size, f);
        }

        // Write index
        fwrite(entries.data(), 1, entries.size() * sizeof(OzPackageEntry), f);

        fclose(f);
        return true;
    }

private:
    uint32_t m_magic;
    std::vector<OzPackageFile> m_files;
};

// ============================================================================
// Reader — opens a package and provides random access by filename
// ============================================================================
class OzPackageReader {
public:
    OzPackageReader() : m_data(nullptr), m_size(0), m_owned(false), m_magic(0), m_version(0) {}

    ~OzPackageReader() { Close(); }

    OzPackageReader(const OzPackageReader&) = delete;
    OzPackageReader& operator=(const OzPackageReader&) = delete;

    OzPackageReader(OzPackageReader&& other) noexcept
        : m_data(other.m_data), m_size(other.m_size), m_owned(other.m_owned),
          m_magic(other.m_magic), m_version(other.m_version),
          m_headerFlags(other.m_headerFlags), m_entries(std::move(other.m_entries)),
          m_decompressed(std::move(other.m_decompressed)) {
        other.m_data = nullptr;
        other.m_size = 0;
        other.m_owned = false;
        other.m_headerFlags = 0;
    }

    OzPackageReader& operator=(OzPackageReader&& other) noexcept {
        if (this != &other) {
            Close();
            m_data = other.m_data;
            m_size = other.m_size;
            m_owned = other.m_owned;
            m_magic = other.m_magic;
            m_version = other.m_version;
            m_headerFlags = other.m_headerFlags;
            m_entries = std::move(other.m_entries);
            m_decompressed = std::move(other.m_decompressed);
            other.m_data = nullptr;
            other.m_size = 0;
            other.m_owned = false;
            other.m_headerFlags = 0;
        }
        return *this;
    }

    bool Open(const char* path) {
        FILE* f = fopen(path, "rb");
        if (!f) return false;

        OzPackageHeader hdr;
        if (fread(&hdr, sizeof(hdr), 1, f) != 1) { fclose(f); return false; }
        if (!is_valid_magic(hdr.magic) ||
            hdr.version != OZ_PACKAGE_VERSION) { fclose(f); return false; }
        m_headerFlags = hdr.flags;

        m_magic = hdr.magic;
        m_version = hdr.version;

        // Read all data into memory
        fseek(f, 0, SEEK_END);
        m_size = (size_t)ftell(f);
        fseek(f, 0, SEEK_SET);

        m_data = new uint8_t[m_size];
        if (fread(m_data, 1, m_size, f) != m_size) { fclose(f); Close(); return false; }
        fclose(f);
        m_owned = true;

        return ParseIndex();
    }

    bool OpenMem(const void* data, size_t size) {
        OzPackageHeader hdr;
        if (size < sizeof(hdr)) return false;
        memcpy(&hdr, data, sizeof(hdr));
        if (!is_valid_magic(hdr.magic) ||
            hdr.version != OZ_PACKAGE_VERSION) return false;
        m_headerFlags = hdr.flags;
        m_magic = hdr.magic;
        m_version = hdr.version;
        m_data = (uint8_t*)data;
        m_size = size;
        m_owned = false;
        return ParseIndex();
    }

    void Close() {
        if (m_owned && m_data) { delete[] m_data; }
        m_data = nullptr;
        m_size = 0;
        m_entries.clear();
        m_decompressed.clear();
        m_headerFlags = 0;
        m_owned = false;
    }

    uint32_t Magic() const { return m_magic; }
    uint32_t EntryCount() const { return (uint32_t)m_entries.size(); }

    // Entry access by index. For loaders that ENUMERATE a package instead of
    // naming one file - the pawn .cfg defs are the case that needed it.
    const OzPackageEntry* EntryAt(uint32_t i) const {
        return i < m_entries.size() ? &m_entries[i] : nullptr;
    }

    // Find entry by exact filename (forward slashes)
    const OzPackageEntry* Find(const char* name) const {
        for (auto& e : m_entries) {
            if (strcmp(e.filename, name) == 0)
                return &e;
        }
        return nullptr;
    }

    // Find entry by basename only (ignores path components)
    const OzPackageEntry* FindBasename(const char* name) const {
        for (auto& e : m_entries) {
            const char* base = strrchr(e.filename, '/');
            if (!base) base = strrchr(e.filename, '\\');
            if (base) base++; else base = e.filename;
            if (strcmp(base, name) == 0)
                return &e;
        }
        return nullptr;
    }

    // Read file data into a buffer (decompressing if needed).
    // Returns bytes read, 0 if not found / corrupt.
    size_t Read(const char* name, std::vector<uint8_t>& out) const {
        const OzPackageEntry* e = Find(name);
        if (!e) return 0;
        return ReadEntry(*e, out);
    }

    // Like Read() but matches by basename (ignores directory components)
    size_t ReadBasename(const char* basename, std::vector<uint8_t>& out) const {
        const OzPackageEntry* e = FindBasename(basename);
        if (!e) return 0;
        return ReadEntry(*e, out);
    }

private:
    size_t ReadEntry(const OzPackageEntry& e, std::vector<uint8_t>& out) const {
        if (!entry_is_compressed(e)) {
            out.resize((size_t)e.sizeRaw);
            memcpy(out.data(), m_data + e.offset, (size_t)e.sizeRaw);
            return (size_t)e.sizeRaw;
        }
        out.resize((size_t)e.sizeRaw);
        mz_ulong outLen = (mz_ulong)e.sizeRaw;
        if (mz_uncompress(out.data(), &outLen,
                          m_data + e.offset, (mz_ulong)e.sizePacked) != MZ_OK) {
            out.clear();
            return 0;
        }
        return (size_t)outLen;
    }

public:

    // Get pointer to file data. Uncompressed entries point straight into the
    // mapped buffer; compressed entries are inflated once into an owned cache.
    const uint8_t* GetData(const char* name, size_t& outSize) const {
        const OzPackageEntry* e = Find(name);
        if (!e) { outSize = 0; return nullptr; }
        if (!entry_is_compressed(*e)) {
            outSize = (size_t)e->sizeRaw;
            return m_data + e->offset;
        }
        std::vector<uint8_t>& slot = m_decompressed[std::string(name)];
        if (slot.empty()) {
            slot.resize((size_t)e->sizeRaw);
            mz_ulong outLen = (mz_ulong)e->sizeRaw;
            if (mz_uncompress(slot.data(), &outLen,
                               m_data + e->offset, (mz_ulong)e->sizePacked) != MZ_OK) {
                slot.clear();
                outSize = 0;
                return nullptr;
            }
            slot.resize((size_t)outLen);
        }
        outSize = slot.size();
        return slot.data();
    }

    // List all filenames
    void List(std::vector<std::string>& names) const {
        names.clear();
        names.reserve(m_entries.size());
        for (auto& e : m_entries)
            names.push_back(e.filename);
    }

    const std::vector<OzPackageEntry>& Entries() const { return m_entries; }

private:
    static bool is_valid_magic(uint32_t magic) {
        return magic == OZ_PACKAGE_MAGIC_PK || magic == OZ_PACKAGE_MAGIC_TX ||
               magic == OZ_PACKAGE_MAGIC_SD || magic == OZ_PACKAGE_MAGIC_MX ||
               magic == OZ_PACKAGE_MAGIC_WN;
    }

    bool entry_is_compressed(const OzPackageEntry& e) const {
        return (m_headerFlags & OZ_PACKAGE_FLAG_COMPRESSED) &&
               (e.reserved & OZ_PACKAGE_FLAG_COMPRESSED) != 0;
    }

    bool ParseIndex() {
        OzPackageHeader* hdr = (OzPackageHeader*)m_data;
        if (hdr->version != OZ_PACKAGE_VERSION) return false;

        uint32_t count = hdr->entryCount;
        // Bounds sanity: index must live inside the mapped buffer.
        if ((size_t)hdr->indexOffset + (size_t)hdr->indexSize > m_size ||
            (size_t)hdr->indexSize != (size_t)count * sizeof(OzPackageEntry))
            return false;
        OzPackageEntry* idx = (OzPackageEntry*)(m_data + hdr->indexOffset);

        m_entries.clear();
        m_entries.reserve(count);
        for (uint32_t i = 0; i < count; i++) {
            m_entries.push_back(idx[i]);
        }
        return true;
    }

    uint8_t* m_data;
    size_t m_size;
    bool m_owned;
    uint32_t m_magic;
    uint32_t m_version;
    uint16_t m_headerFlags = 0;
    std::vector<OzPackageEntry> m_entries;
    mutable std::unordered_map<std::string, std::vector<uint8_t>> m_decompressed;
};
