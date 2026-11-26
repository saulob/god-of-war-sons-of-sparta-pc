// SOS-6Y8NVY installer regression on the real file system, synthetic data only (CUSA99999). Built against the
// current installer and, with SOS_INSTALL_BASELINE, against v0.1.0, which must fail as reported.
//   install_tests <test folder> <launcher/pkg_extract.cpp> <repository root, never touched>
//   install_tests --manifest   (linked with src/sons_of_sparta.rc)

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#else
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/rsa.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <string>
#include <string_view>
#include <vector>

#include "crypto.h"
#include "game_files.h"
#include "game_install.h"
#include "pkg_extract.h"
#include "utf8.h"
#ifndef SOS_INSTALL_BASELINE
#include "install_fs.h"
#include "log_scrubber.h"
#include "trg/platform.h"
#endif

namespace fs = std::filesystem;

namespace {

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const std::string& name) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
    }
    std::printf("  [%s] %s\n", ok ? "ok" : "fail", name.c_str());
    std::fflush(stdout);
}

void Step(const char* name) {
    std::printf("== %s\n", name);
    std::fflush(stdout);
}

void Note(const std::string& text) {
    std::printf("     %s\n", text.c_str());
    std::fflush(stdout);
}

// The file of the report and its neighbour: the two longest paths of CUSA57767 1.00 (120 characters each).
constexpr char kBundle[] =
    "Media/StreamingAssets/aa/PS4/rewardassets_assets_onenemyhitorbs_normaldmg_spirit_2301b2731ef4afeb9fccafb7fd438410.bundle";
constexpr char kBundle2[] =
    "Media/StreamingAssets/aa/PS4/rewardassets_assets_onenemyhitorbs_spiritdmg_health_f1872c4b3fc75f31627fb85287399895.bundle";
constexpr char kBundleName[] = "rewardassets_assets_onenemyhitorbs_normaldmg_spirit_2301b2731ef4afeb9fccafb7fd438410.bundle";
[[maybe_unused]] constexpr char kStagingSuffix[] = ".install-temp";

size_t Units(const fs::path& path) { return path.native().size(); }

// ------------------------------------------------------------------------------------------------------ sandbox ---
// Fail closed: an empty path once resolved to the repository root and remove_all deleted part of .git.

fs::path g_sandbox;   // canonical
fs::path g_protected; // canonical repository root

[[noreturn]] void Abort(const std::string& why) {
    std::fprintf(stderr, "install_tests: stopped: %s\n", why.c_str());
    std::fflush(stderr);
    std::_Exit(70);
}

fs::path Raw(const fs::path& path);

std::vector<fs::path::string_type> Parts(const fs::path& path) {
    std::vector<fs::path::string_type> parts;
    for (const fs::path& part : path) {
        fs::path::string_type text = part.native();
#ifdef _WIN32
        if (text == L"/") text = L"\\";
        for (wchar_t& c : text) c = wchar_t(std::towlower(c));
#endif
        if (!text.empty()) parts.push_back(text);
    }
    return parts;
}

bool Within(const fs::path& child, const fs::path& parent, bool or_same) {
    const auto c = Parts(child), p = Parts(parent);
    if (c.size() < p.size() || (c.size() == p.size() && !or_same)) return false;
    return std::equal(p.begin(), p.end(), c.begin());
}

fs::path Canonical(const fs::path& path, const char* what) {
    if (path.empty() || !path.is_absolute()) Abort(std::string(what) + ": not an absolute path");
    for (const fs::path& part : path) {
        if (part == "." || part == "..") Abort(std::string(what) + ": '.' or '..' in " + path.string());
    }
    std::error_code ec;
    fs::path canonical = fs::weakly_canonical(Raw(path), ec);
    if (ec) Abort(std::string(what) + ": cannot resolve " + path.string() + ": " + ec.message());
#ifdef _WIN32
    const std::wstring& text = canonical.native();
    if (text.starts_with(L"\\\\?\\UNC\\")) canonical = L"\\\\" + text.substr(8);
    else if (text.starts_with(L"\\\\?\\")) canonical = text.substr(4);
#endif
    return canonical.lexically_normal();
}

// Inside the test folder and nowhere near the repository; `may_be_sandbox` allows the test folder itself as a base.
fs::path Guarded(const fs::path& path, const char* what, bool may_be_sandbox = false) {
    if (g_sandbox.empty() || g_protected.empty()) Abort(std::string(what) + ": no test folder");
    const fs::path canonical = Canonical(path, what);
    for (const auto& part : Parts(canonical)) {
#ifdef _WIN32
        if (part == L".git" || part == L".claude") Abort(std::string(what) + ": .git or .claude in " + path.string());
#else
        if (part == ".git" || part == ".claude") Abort(std::string(what) + ": .git or .claude in " + path.string());
#endif
    }
    if (Within(g_protected, canonical, true) || Within(canonical, g_protected, true)) {
        Abort(std::string(what) + ": the repository, inside it or one of its ancestors: " + path.string());
    }
    if (!Within(canonical, g_sandbox, may_be_sandbox)) {
        Abort(std::string(what) + " outside the test folder: " + path.string());
    }
    return path;
}

// ------------------------------------------------------------------------------------------------ raw file access ---

bool ProcessLongPaths() {
#ifdef _WIN32
    using Query = BOOLEAN(NTAPI*)();
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto query = ntdll ? reinterpret_cast<Query>(GetProcAddress(ntdll, "RtlAreLongPathsEnabled")) : nullptr;
    return query && query();
#else
    return true;
#endif
}

fs::path Raw(const fs::path& path) {
#ifdef _WIN32
    std::wstring text = fs::absolute(path).lexically_normal().wstring();
    std::replace(text.begin(), text.end(), L'/', L'\\');
    if (text.starts_with(L"\\\\?\\")) {
        return text;
    }
    return fs::path(L"\\\\?\\" + text);
#else
    return path;
#endif
}

bool RawWrite(const fs::path& path, const std::string& data) {
    Guarded(path, "a test file");
    std::error_code ec;
    fs::create_directories(Raw(path).parent_path(), ec);
    std::ofstream out(Raw(path), std::ios::binary | std::ios::trunc);
    out.write(data.data(), std::streamsize(data.size()));
    return bool(out.flush());
}

std::string RawRead(const fs::path& path) {
    std::ifstream in(Raw(path), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool RawExists(const fs::path& path) {
    std::error_code ec;
    return fs::exists(Raw(path), ec);
}

void RawRemoveAll(const fs::path& path) {
    Guarded(path, "a folder to remove");
    std::error_code ec;
    fs::remove_all(Raw(path), ec);
}

[[maybe_unused]] void RawRemoveFile(const fs::path& path) {
    Guarded(path, "a file to remove");
    std::error_code ec;
    fs::remove(Raw(path), ec);
}

void RemoveSandbox() {
    if (g_sandbox.empty() || g_protected.empty() || Parts(g_sandbox).size() < 4 || Within(g_protected, g_sandbox, true) ||
        Within(g_sandbox, g_protected, true) || Canonical(g_sandbox, "the test folder") != g_sandbox) {
        Abort("refusing to remove the test folder " + g_sandbox.string());
    }
    std::error_code ec;
    fs::current_path(g_sandbox.parent_path(), ec);
    fs::remove_all(Raw(g_sandbox), ec);
}

std::vector<std::string> InstallerLeftovers(const fs::path& dir) {
    std::vector<std::string> found;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(Raw(dir), ec)) {
        const std::string name = sos::Utf8(entry.path().filename());
        for (const char* suffix : {".install-temp", ".install-backup", ".install-update", ".removing"}) {
            if (name.ends_with(suffix)) {
                found.push_back(name);
            }
        }
    }
    return found;
}

// `count` characters of padding as folders of at most 200 characters (NTFS allows 255 per name), joined by separators
// that count too; the last folder is at least 2 characters.
fs::path Padding(size_t count, char fill, const std::string& prefix = {}) {
    fs::path out;
    std::string first = prefix;
    while (count > 0) {
        const size_t room = 200 - std::min<size_t>(200, first.size());
        size_t take = std::min(count, room);
        if (count > take && count - take < 3) take = count - 3; // leave room for a separator and a 2-character folder
        out /= sos::PathFromUtf8(first + std::string(take, fill));
        first.clear();
        count -= take;
        if (count > 0) --count; // the next separator
    }
    return out;
}

// `base`\<padding>\`tail`, the padding chosen so the whole path is `total` characters (UTF-16 units on Windows).
[[maybe_unused]] fs::path WithLength(const fs::path& base, const fs::path& tail, size_t total, char fill = 'p') {
    const size_t used = Units(Guarded(base, "a base folder", true)) + 1 + Units(tail) + 1;
    if (total <= used + 2) {
        Abort("the test folder is too long for a " + std::to_string(total) + "-character path");
    }
    return Guarded(base / Padding(total - used, fill) / tail, "a measured path");
}

// A folder whose own absolute path is `total` characters: `base`\<prefix><padding>[\<padding>...].
fs::path FolderOfLength(const fs::path& base, const std::string& prefix, size_t total) {
    const size_t used = Units(Guarded(base, "a base folder", true)) + 1 + Units(sos::PathFromUtf8(prefix));
    if (total <= used + 2) {
        Abort("the test folder is too long for a " + std::to_string(total) + "-character folder");
    }
    return Guarded(base / Padding(total - used, 'r', prefix), "a measured folder");
}

std::string Filled(const std::string& seed, size_t size) {
    std::string out;
    while (out.size() < size) {
        out += seed;
    }
    out.resize(size);
    return out;
}

std::string Hex(const uint8_t* data, size_t size) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < size; ++i) {
        out += digits[data[i] >> 4];
        out += digits[data[i] & 15];
    }
    return out;
}

std::string Sha256Of(const std::string& data) {
    uint8_t digest[32];
    sos::crypto::Sha256(reinterpret_cast<const uint8_t*>(data.data()), data.size(), digest);
    return Hex(digest, 32);
}

// ----------------------------------------------------------------------------------------------- synthetic build ---
// CUSA99999 1.00 and its 1.01 update: the same folder name length as CUSA57767's, so the paths measure the same.

const std::string kBaseEboot = "SOS-6Y8NVY synthetic eboot.bin 1.00 (not a PS4 executable)";
const std::string kUpdateEboot = "SOS-6Y8NVY synthetic eboot.bin 1.01 (not a PS4 executable)";
std::string g_base_sha, g_update_sha;

constexpr sos::PackageLayer kBaseLayers[] = {{"01.00", "01.00", false}};
constexpr sos::PackageLayer kUpdateLayers[] = {{"01.00", "01.00", false}, {"01.01", "01.01", true}};
constexpr const char* kUpdateExtra[] = {"Media/update_only.bin"};
constexpr char kContentId[] = "UP9999-CUSA99999_00-SOS6Y8NVYTEST00";

std::vector<sos::GameBuild> g_builds;

void MakeBuilds() {
    g_base_sha = Sha256Of(kBaseEboot);
    g_update_sha = Sha256Of(kUpdateEboot);
    sos::GameBuild base{};
    base.title_id = "CUSA99999";
    base.content_id = kContentId;
    base.version = "1.00";
    base.folder = "CUSA99999-1.00";
    base.eboot_sha256 = g_base_sha.c_str();
    base.regional_archive = nullptr;
    base.label = "CUSA99999 1.00 TEST";
    base.region = "TEST";
    base.layers = kBaseLayers;
    sos::GameBuild update = base;
    update.version = "1.01";
    update.folder = "CUSA99999-1.01";
    update.eboot_sha256 = g_update_sha.c_str();
    update.extra_files = kUpdateExtra;
    update.label = "CUSA99999 1.01 TEST";
    update.layers = kUpdateLayers;
    g_builds = {base, update};
}

// A PSF (param.sfo) with UTF-8 string values.
std::string MakeSfo(const std::vector<std::pair<std::string, std::string>>& values) {
    std::string keys, data, table;
    const auto le16 = [](std::string& out, uint16_t v) { out += char(v & 0xFF); out += char(v >> 8); };
    const auto le32 = [](std::string& out, uint32_t v) {
        for (int i = 0; i < 4; ++i) out += char((v >> (8 * i)) & 0xFF);
    };
    for (const auto& [key, value] : values) {
        const uint32_t length = uint32_t(value.size() + 1);
        const uint32_t max = (length + 3) & ~3u;
        le16(table, uint16_t(keys.size()));
        le16(table, 0x0204);
        le32(table, length);
        le32(table, max);
        le32(table, uint32_t(data.size()));
        keys += key + '\0';
        data += value;
        data.append(max - value.size(), '\0');
    }
    while (keys.size() % 4) keys += '\0';
    std::string sfo("\0PSF", 4);
    le32(sfo, 0x101);
    le32(sfo, uint32_t(20 + table.size()));
    le32(sfo, uint32_t(20 + table.size() + keys.size()));
    le32(sfo, uint32_t(values.size()));
    return sfo + table + keys + data;
}

std::string BaseSfo() {
    return MakeSfo({{"APP_VER", "01.00"}, {"CATEGORY", "gd"}, {"CONTENT_ID", kContentId}, {"TITLE_ID", "CUSA99999"},
                    {"VERSION", "01.00"}});
}

std::string UpdateSfo() {
    return MakeSfo({{"APP_VER", "01.01"}, {"CATEGORY", "gp"}, {"CONTENT_ID", kContentId}, {"TITLE_ID", "CUSA99999"},
                    {"VERSION", "01.01"}});
}

using Tree = std::map<std::string, std::string>; // relative path ('/') -> bytes

// The base game: the files the readiness check needs, the two long bundles and files sorted around them whose sizes put
// the report's bundle at 43% of the bytes, as in CUSA57767 1.00 (4120309346 of 9508144635).
Tree BaseTree(const std::string& version_tag) {
    Tree tree;
    tree["eboot.bin"] = kBaseEboot;
    tree["sce_discmap.plt"] = Filled("discmap ", 100);
    tree["sce_module/libc.prx"] = Filled("libc ", 100);
    tree["sce_module/libSceFios2.prx"] = Filled("fios ", 100);
    tree["Media/Modules/PS4Util.prx"] = Filled("util ", 1000);
    tree["Media/Modules/Il2cppUserAssemblies.prx"] = Filled("il2cpp ", 1000);
    tree["sce_sys/param.sfo"] = BaseSfo();
    tree["Media/StreamingAssets/aa/PS4/aaa_before.bundle"] = Filled("before ", 41300);
    tree[kBundle] = Filled("SOS-6Y8NVY bundle " + version_tag + " ", 3000);
    tree[kBundle2] = Filled("neighbour " + version_tag + " ", 3000);
    tree["Media/StreamingAssets/aa/PS4/zzz_after.bundle"] = Filled("after ", 50000);
    return tree;
}

Tree UpdateTree() {
    Tree tree;
    tree["eboot.bin"] = kUpdateEboot;
    tree["sce_sys/param.sfo"] = UpdateSfo();
    tree["Media/update_only.bin"] = Filled("update ", 2000);
    return tree;
}

uint64_t BytesBefore(const Tree& tree, const std::string& path) {
    uint64_t bytes = 0;
    for (const auto& [name, data] : tree) {
        if (name < path) bytes += data.size();
    }
    return bytes;
}

uint64_t TotalBytes(const Tree& tree) {
    uint64_t bytes = 0;
    for (const auto& [name, data] : tree) bytes += data.size();
    return bytes;
}

// ---------------------------------------------------------------------------------------------- synthetic fake PKG ---

std::vector<uint8_t> ModulusFrom(const std::string& source, const std::string& name) {
    std::vector<uint8_t> bytes;
    const size_t at = source.find(name + "[] = {");
    if (at == std::string::npos) return bytes;
    const size_t end = source.find("};", at);
    for (size_t i = source.find("0x", at); i != std::string::npos && i < end; i = source.find("0x", i + 2)) {
        bytes.push_back(uint8_t(std::stoul(source.substr(i + 2, 2), nullptr, 16)));
    }
    return bytes;
}

bool RsaEncrypt(const std::vector<uint8_t>& modulus, const uint8_t* message, size_t size, uint8_t (&out)[256]) {
#ifdef _WIN32
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    std::vector<uint8_t> blob(sizeof(BCRYPT_RSAKEY_BLOB) + 3 + 256);
    BCRYPT_RSAKEY_BLOB header{};
    header.Magic = BCRYPT_RSAPUBLIC_MAGIC;
    header.BitLength = 2048;
    header.cbPublicExp = 3;
    header.cbModulus = 256;
    std::memcpy(blob.data(), &header, sizeof(header));
    const uint8_t exponent[3] = {1, 0, 1};
    std::memcpy(blob.data() + sizeof(header), exponent, 3);
    std::memcpy(blob.data() + sizeof(header) + 3, modulus.data(), 256);
    ULONG written = 0;
    bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_RSA_ALGORITHM, nullptr, 0) == 0 &&
              BCryptImportKeyPair(alg, nullptr, BCRYPT_RSAPUBLIC_BLOB, &key, blob.data(), ULONG(blob.size()), 0) == 0 &&
              BCryptEncrypt(key, const_cast<PUCHAR>(message), ULONG(size), nullptr, nullptr, 0, out, 256, &written,
                            BCRYPT_PAD_PKCS1) == 0 &&
              written == 256;
    if (key) BCryptDestroyKey(key);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
#else
    BIGNUM* n = BN_bin2bn(modulus.data(), int(modulus.size()), nullptr);
    BIGNUM* e = BN_new();
    BN_set_word(e, 65537);
    OSSL_PARAM_BLD* build = OSSL_PARAM_BLD_new();
    OSSL_PARAM_BLD_push_BN(build, OSSL_PKEY_PARAM_RSA_N, n);
    OSSL_PARAM_BLD_push_BN(build, OSSL_PKEY_PARAM_RSA_E, e);
    OSSL_PARAM* params = OSSL_PARAM_BLD_to_param(build);
    EVP_PKEY_CTX* from = EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr);
    EVP_PKEY* pkey = nullptr;
    bool ok = from && EVP_PKEY_fromdata_init(from) > 0 && EVP_PKEY_fromdata(from, &pkey, EVP_PKEY_PUBLIC_KEY, params) > 0;
    EVP_PKEY_CTX* enc = ok ? EVP_PKEY_CTX_new(pkey, nullptr) : nullptr;
    size_t length = 256;
    ok = ok && enc && EVP_PKEY_encrypt_init(enc) > 0 && EVP_PKEY_CTX_set_rsa_padding(enc, RSA_PKCS1_PADDING) > 0 &&
         EVP_PKEY_encrypt(enc, out, &length, message, size) > 0 && length == 256;
    EVP_PKEY_CTX_free(enc);
    EVP_PKEY_free(pkey);
    EVP_PKEY_CTX_free(from);
    OSSL_PARAM_free(params);
    OSSL_PARAM_BLD_free(build);
    BN_free(n);
    BN_free(e);
    return ok;
#endif
}

void PutBe32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + i] = uint8_t(v >> (24 - 8 * i));
}
void PutBe64(std::vector<uint8_t>& b, size_t at, uint64_t v) {
    PutBe32(b, at, uint32_t(v >> 32));
    PutBe32(b, at + 4, uint32_t(v));
}
template <class T>
void PutLe(std::vector<uint8_t>& b, size_t at, T v) {
    std::memcpy(&b[at], &v, sizeof(v));
}

constexpr uint64_t kBlock = 0x10000;

std::vector<uint8_t> InnerPfs(const Tree& tree) {
    struct Node {
        std::string name;
        bool dir;
        std::vector<size_t> children;
        const std::string* data = nullptr;
        uint32_t loc = 0, blocks = 0;
    };
    std::vector<Node> nodes{{"", true, {}}, {"uroot", true, {}}}; // 0: super-root, 1: uroot
    nodes[0].children.push_back(1);
    std::map<std::string, size_t> dirs{{"", 1}};
    const auto dir_of = [&](const std::string& path, auto&& self) -> size_t {
        if (const auto it = dirs.find(path); it != dirs.end()) return it->second;
        const size_t slash = path.rfind('/');
        const size_t parent = self(slash == std::string::npos ? "" : path.substr(0, slash), self);
        nodes.push_back({slash == std::string::npos ? path : path.substr(slash + 1), true, {}});
        const size_t index = nodes.size() - 1;
        nodes[parent].children.push_back(index);
        dirs[path] = index;
        return index;
    };
    for (const auto& [path, data] : tree) {
        const size_t slash = path.rfind('/');
        const size_t parent = dir_of(slash == std::string::npos ? "" : path.substr(0, slash), dir_of);
        nodes.push_back({slash == std::string::npos ? path : path.substr(slash + 1), false, {}, &data});
        nodes[parent].children.push_back(nodes.size() - 1);
    }
    uint32_t next = 2;
    for (Node& node : nodes) {
        if (node.dir) {
            node.loc = next++;
            node.blocks = 1;
        }
    }
    for (Node& node : nodes) {
        if (!node.dir) {
            node.blocks = uint32_t((node.data->size() + kBlock - 1) / kBlock);
            node.loc = next;
            next += node.blocks;
        }
    }
    std::vector<uint8_t> image(size_t(next) * kBlock, 0);
    // PfsHeader: version, magic, id, fmode.., mode, unk1, block_size, n_backup, n_block, dinode_count, nd_block,
    // dinode_block_count, superroot_ino.
    PutLe<int64_t>(image, 0x00, 1);
    PutLe<int64_t>(image, 0x08, 20130315);
    PutLe<int32_t>(image, 0x20, int32_t(kBlock)); // after fmode..rsv (0x18), mode (0x1C, 0: unsigned 32-bit) and unk1
    PutLe<int64_t>(image, 0x28, next);
    PutLe<int64_t>(image, 0x30, int64_t(nodes.size()));
    PutLe<int64_t>(image, 0x38, next);
    PutLe<int64_t>(image, 0x40, 1);
    PutLe<int64_t>(image, 0x48, 0);
    for (size_t i = 0; i < nodes.size(); ++i) {
        const size_t at = kBlock + i * 0xA8;
        PutLe<uint16_t>(image, at + 0, nodes[i].dir ? 0x41ED : 0x81A4);
        PutLe<uint16_t>(image, at + 2, 1);
        PutLe<int64_t>(image, at + 8, nodes[i].dir ? int64_t(kBlock) : int64_t(nodes[i].data->size()));
        PutLe<int64_t>(image, at + 16, nodes[i].dir ? int64_t(kBlock) : int64_t(nodes[i].data->size()));
        PutLe<uint32_t>(image, at + 96, nodes[i].blocks);
        PutLe<uint32_t>(image, at + 100, nodes[i].loc);
    }
    for (const Node& node : nodes) {
        if (node.dir) {
            size_t at = size_t(node.loc) * kBlock;
            for (const size_t child : node.children) {
                const std::string& name = nodes[child].name;
                const int32_t entsize = int32_t((16 + name.size() + 7) & ~size_t(7));
                PutLe<int32_t>(image, at, int32_t(child));
                PutLe<int32_t>(image, at + 4, nodes[child].dir ? 3 : 2);
                PutLe<int32_t>(image, at + 8, int32_t(name.size()));
                PutLe<int32_t>(image, at + 12, entsize);
                std::memcpy(&image[at + 16], name.data(), name.size());
                at += size_t(entsize);
            }
        } else {
            std::memcpy(&image[size_t(node.loc) * kBlock], node.data->data(), node.data->size());
        }
    }
    return image;
}

std::vector<uint8_t> MakePackage(const Tree& tree, const std::string& sfo, const std::string& pkg_extract_source) {
    const std::vector<uint8_t> fake_n = ModulusFrom(pkg_extract_source, "kFakeKeysetModulus");
    const std::vector<uint8_t> dk3_n = ModulusFrom(pkg_extract_source, "kPkgDerivedKey3KeysetModulus");
    if (fake_n.size() != 256 || dk3_n.size() != 256) return {};

    // The PFS: 0x20000 bytes the reader never decrypts (the seed at 0x370), then the PFSC image, XTS-encrypted.
    const std::vector<uint8_t> inner = InnerPfs(tree);
    const uint64_t blocks = inner.size() / kBlock;
    const uint64_t pfsc_at = 0x20000;
    std::vector<uint8_t> pfs(size_t(pfsc_at + kBlock + inner.size()), 0);
    uint8_t seed[16];
    for (int i = 0; i < 16; ++i) seed[i] = uint8_t(0x5A ^ i);
    std::memcpy(&pfs[0x370], seed, 16);
    PutLe<int32_t>(pfs, pfsc_at + 0x00, 0x43534650);
    PutLe<int32_t>(pfs, pfsc_at + 0x0C, int32_t(kBlock));
    PutLe<int64_t>(pfs, pfsc_at + 0x10, int64_t(kBlock));
    PutLe<int64_t>(pfs, pfsc_at + 0x18, 0x400);
    PutLe<uint64_t>(pfs, pfsc_at + 0x20, kBlock);
    PutLe<int64_t>(pfs, pfsc_at + 0x28, int64_t(inner.size()));
    for (uint64_t b = 0; b <= blocks; ++b) PutLe<uint64_t>(pfs, pfsc_at + 0x400 + b * 8, kBlock + b * kBlock);
    std::memcpy(&pfs[pfsc_at + kBlock], inner.data(), inner.size());

    uint8_t ekpfs[32], dk3[32];
    for (int i = 0; i < 32; ++i) {
        ekpfs[i] = uint8_t(0xA0 + i);
        dk3[i] = uint8_t(0x30 + i);
    }
    uint8_t input[20], digest[32];
    const uint32_t one = 1;
    std::memcpy(input, &one, 4);
    std::memcpy(input + 4, seed, 16);
    sos::crypto::AesKey tweak_key, data_key;
    if (!sos::crypto::HmacSha256(ekpfs, 32, input, 20, digest) || !tweak_key.Init(sos::crypto::AesMode::kEcb, digest) ||
        !data_key.Init(sos::crypto::AesMode::kEcb, digest + 16)) {
        return {};
    }
    for (uint64_t start = pfsc_at; start < pfs.size(); start += 0x1000) {
        uint8_t tweak[16] = {};
        const uint64_t sector = start / 0x1000;
        std::memcpy(tweak, &sector, 8);
        tweak_key.Encrypt(tweak, 16);
        uint64_t low, high;
        std::memcpy(&low, tweak, 8);
        std::memcpy(&high, tweak + 8, 8);
        std::vector<uint8_t> mask(0x1000);
        for (size_t block = 0; block < 0x1000; block += 16) {
            std::memcpy(&mask[block], &low, 8);
            std::memcpy(&mask[block + 8], &high, 8);
            const uint64_t carry = high >> 63;
            high = high << 1 | low >> 63;
            low = low << 1 ^ (carry ? 0x87 : 0);
        }
        uint8_t* sector_data = &pfs[size_t(start)];
        for (size_t i = 0; i < 0x1000; ++i) sector_data[i] ^= mask[i];
        data_key.Encrypt(sector_data, 0x1000);
        for (size_t i = 0; i < 0x1000; ++i) sector_data[i] ^= mask[i];
    }

    const uint32_t table_at = 0x2000, keys_at = 0x3000, keys_size = 32 + 7 * 32 + 7 * 256, image_key_at = 0x4000;
    const uint32_t sfo_at = 0x5000;
    const uint64_t pfs_offset = (sfo_at + sfo.size() + kBlock - 1) & ~(kBlock - 1);
    std::vector<uint8_t> pkg(size_t(pfs_offset + pfs.size()), 0);
    PutBe32(pkg, 0x00, 0x7F434E54);
    PutBe32(pkg, 0x10, 3);
    PutBe32(pkg, 0x18, table_at);
    PutBe64(pkg, 0x30, pfs_offset);
    PutBe64(pkg, 0x38, pfs.size());
    std::memcpy(&pkg[0x40], kContentId, std::strlen(kContentId));
    PutBe64(pkg, 0x410, pfs_offset);
    PutBe64(pkg, 0x418, pfs.size());
    PutBe64(pkg, 0x430, pkg.size());
    PutBe32(pkg, 0x43C, uint32_t(pfs.size()));
    const auto entry = [&](size_t index, uint32_t id, uint32_t flags1, uint32_t flags2, uint32_t offset, uint32_t size) {
        const size_t at = table_at + index * 32;
        PutBe32(pkg, at, id);
        PutBe32(pkg, at + 8, flags1);
        PutBe32(pkg, at + 12, flags2);
        PutBe32(pkg, at + 16, offset);
        PutBe32(pkg, at + 20, size);
    };
    entry(0, 0x10, 0, 0, keys_at, keys_size);
    entry(1, 0x20, 0x80000000u, 3u << 12, image_key_at, 256);
    entry(2, 0x1000, 0, 0, sfo_at, uint32_t(sfo.size()));
    uint8_t cipher[256];
    if (!RsaEncrypt(dk3_n, dk3, 32, cipher)) return {};
    std::memcpy(&pkg[keys_at + 32 + 7 * 32 + 3 * 256], cipher, 256);
    if (!RsaEncrypt(fake_n, ekpfs, 32, cipher)) return {};
    uint8_t ivkey_input[64], ivkey[32];
    std::memcpy(ivkey_input, &pkg[table_at + 32], 32);
    std::memcpy(ivkey_input + 32, dk3, 32);
    sos::crypto::Sha256(ivkey_input, 64, ivkey);
    sos::crypto::AesKey cbc;
    uint8_t iv[16];
    std::memcpy(iv, ivkey, 16);
    if (!cbc.Init(sos::crypto::AesMode::kCbc, ivkey + 16) || !cbc.Encrypt(cipher, 256, iv)) return {};
    std::memcpy(&pkg[image_key_at], cipher, 256);
    std::memcpy(&pkg[sfo_at], sfo.data(), sfo.size());
    std::memcpy(&pkg[size_t(pfs_offset)], pfs.data(), pfs.size());
    return pkg;
}

fs::path WritePackage(const fs::path& path, const Tree& tree, const std::string& sfo, const std::string& source) {
    const std::vector<uint8_t> bytes = MakePackage(tree, sfo, source);
    if (bytes.empty() || !RawWrite(path, std::string(bytes.begin(), bytes.end()))) return {};
    return path;
}

// ------------------------------------------------------------------------------------------ installation helpers ---

struct Recorder {
    std::vector<std::string> phases;
    uint64_t done = 0, total = 0;
    bool monotonic = true;
    std::function<bool(uint64_t, uint64_t)> cancel; // true cancels
};

#ifndef SOS_INSTALL_BASELINE
std::string FakeBootImage(const fs::path& app0, const fs::path& image) {
    const std::string sfo = RawRead(app0 / "sce_sys" / "param.sfo");
    const std::string sha = Sha256Of(RawRead(app0 / "eboot.bin"));
    const std::string report = "{\n  \"title_id\": \"" + sos::SfoString(sfo, "TITLE_ID") + "\",\n  \"app_ver\": \"" +
                               sos::SfoString(sfo, "APP_VER") + "\",\n  \"eboot_sha256\": \"" + sha + "\"\n}\n";
    if (!RawWrite(image / "boot.bin", "SOSBOOT1" + std::string(56, '\1')) ||
        !RawWrite(image / "content.bin", "SOSCONT1" + std::string(20, '\1')) || !RawWrite(image / "image.json", report)) {
        return "the test boot image could not be written";
    }
    return {};
}
#endif

sos::InstallHooks HooksFor(Recorder& recorder, bool replace = false) {
    sos::InstallHooks hooks;
    hooks.replace_active = replace;
    hooks.phase = [&recorder](const char* text) { recorder.phases.push_back(text); };
    hooks.progress = [&recorder](uint64_t done, uint64_t total) {
        if (total == recorder.total && done < recorder.done) recorder.monotonic = false;
        recorder.done = done;
        recorder.total = total;
        return !(recorder.cancel && recorder.cancel(done, total));
    };
#ifndef SOS_INSTALL_BASELINE
    hooks.build_image = FakeBootImage;
#endif
    return hooks;
}

sos::GameRoots RootsUnder(const fs::path& root) { return {root / "game" / "prepared", root / "game" / "image"}; }

fs::path RootForApp0(const fs::path& base, const std::string& prefix, size_t app0_length) {
    const size_t tail = Units(fs::path("game") / "prepared" / "CUSA99999-1.00") + 1;
    return FolderOfLength(base, prefix, app0_length - tail);
}

bool Has(const std::string& text, std::string_view part) { return text.find(part) != std::string::npos; }

std::string StatusText(sos::InstallStatus status) {
#ifndef SOS_INSTALL_BASELINE
    return sos::InstallStatusKey(status);
#else
    return std::to_string(int(status));
#endif
}

// ===================================================================================== the v0.1.0 baseline only ===
#ifdef SOS_INSTALL_BASELINE

int RunBaseline(const fs::path& temp, const std::string& source) {
    Step("v0.1.0 installer: the SOS-6Y8NVY scenario");
    if (ProcessLongPaths()) {
        Note("this process may use long paths, so Windows' MAX_PATH limit does not apply here: not reproducible");
        return 3;
    }
    const Tree tree = BaseTree("v1");
    const fs::path package = WritePackage(temp / "packages" / "base.pkg", tree, BaseSfo(), source);
    Check(!package.empty(), "the synthetic base package was built");
    // The player's window: the build folder 130 characters, so its staging file is 264 and the installed one 251.
    const fs::path root = RootForApp0(temp, "player (DLPSGAME.COM) Jogos ", 130);
    const sos::GameRoots roots = RootsUnder(root);
    Recorder recorder;
    const sos::InstallResult result = sos::InstallFromPackage(package, roots, HooksFor(recorder), g_builds);
    const fs::path stage = roots.prepared / ("CUSA99999-1.00" + std::string(kStagingSuffix));
    // The package's own '/' after the staging folder's separator, exactly as in the player's log.
    const std::string expected = "cannot create " + sos::Utf8(stage / sos::PathFromUtf8(kBundle));
    Note("status " + StatusText(result.status) + ", detail: " + result.detail);
    Check(result.status == sos::InstallStatus::kWriteFailed, "the v0.1.0 installer fails with a write failure");
    Check(result.detail == expected,
          "with the player's message: cannot create <install>.install-temp\\Media/StreamingAssets/aa/PS4/"
          "rewardassets_..._2301b2731ef4afeb9fccafb7fd438410.bundle");
    Check(!Has(result.detail, ": "), "and no system error in it (the error code is lost)");
    const uint64_t before = BytesBefore(tree, kBundle), total = TotalBytes(tree);
    const double percent = recorder.total ? 100.0 * double(recorder.done) / double(recorder.total) : 0.0;
    char shown[32];
    std::snprintf(shown, sizeof(shown), "%.2f%%", percent);
    Note("progress when it stopped: " + std::to_string(recorder.done) + " of " + std::to_string(recorder.total) +
         " bytes (" + shown + ")");
    Check(recorder.done == before && recorder.total == total && percent > 43.0 && percent < 43.6,
          std::string("it stops at the report's file, at ") + shown + " of the bytes (43.3% in CUSA57767 1.00)");
    Check(!RawExists(stage), "its staging folder was removed");
    const fs::path staged_file = stage / sos::PathFromUtf8(kBundle).make_preferred();
    const fs::path final_file = roots.prepared / "CUSA99999-1.00" / sos::PathFromUtf8(kBundle).make_preferred();
    Note("staging file length " + std::to_string(Units(staged_file)) + ", installed file length " +
         std::to_string(Units(final_file)));
    Check(Units(staged_file) == 264 && Units(final_file) == 251,
          "only .install-temp pushes the file past the limit: staging 264, installed 251");
#ifdef _WIN32
    std::error_code ec;
    fs::create_directories(Raw(Guarded(staged_file, "the staging file")).parent_path(), ec);
    SetLastError(0);
    std::ofstream again(staged_file, std::ios::binary | std::ios::trunc);
    const DWORD last = GetLastError();
    Check(!again && last == ERROR_PATH_NOT_FOUND,
          "the same std::ofstream call on that path fails with ERROR_PATH_NOT_FOUND (" + std::to_string(last) + ")");
    again.close();
    fs::create_directories(Raw(Guarded(final_file, "the installed file")).parent_path(), ec);
    const std::string narrow = final_file.string();
    const HANDLE handle = CreateFileA(narrow.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(handle != INVALID_HANDLE_VALUE, "while the installed path stays usable by the game's file layer (CreateFileA)");
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    RawRemoveAll(root);
#endif

    Step("v0.1.0 installer: exactly at the limit");
    for (const size_t staged_length : {size_t(259), size_t(260)}) {
        const fs::path edge_root = RootForApp0(temp, "edge" + std::to_string(staged_length) + " ",
                                               staged_length - 1 - 120 - (sizeof(kStagingSuffix) - 1));
        Recorder edge;
        const sos::InstallResult edge_result =
            sos::InstallFromPackage(package, RootsUnder(edge_root), HooksFor(edge), g_builds);
        const bool write_failed = edge_result.status == sos::InstallStatus::kWriteFailed;
        Check(staged_length == 259 ? !write_failed : write_failed,
              "a staging file of " + std::to_string(staged_length) + " characters " +
                  (staged_length == 259 ? "is written (the install stops later, at the boot image of synthetic files)"
                                        : "cannot be created") +
                  " [" + StatusText(edge_result.status) + "]");
    }
    return 0;
}

#else
// ======================================================================================== the current installer ===

using Fault = std::function<std::error_code(std::string_view operation, const fs::path& path)>;

// Every file operation of the installer under test is checked against the test folder, faults or not.
void SetFault(Fault fault) {
    sos::installfs::SetFaultHookForTesting([fault = std::move(fault)](std::string_view operation, const fs::path& path) {
        Guarded(path, "an installer file operation");
        return fault ? fault(operation, path) : std::error_code();
    });
}

void TestWin32Limit([[maybe_unused]] const fs::path& temp) {
    Step("SOS-6Y8NVY: the Win32 limit the v0.1.0 installer ran into (std::ofstream, CreateFileA, std::filesystem)");
    if (ProcessLongPaths()) {
        Note("this process may use long paths: the MAX_PATH reproduction does not apply");
        return;
    }
    Note("this process cannot use long paths (no longPathAware manifest): MAX_PATH applies, as with LongPathsEnabled=0");
#ifdef _WIN32
    for (const size_t total : {size_t(259), size_t(260)}) {
        const fs::path file = WithLength(temp / "limit", sos::PathFromUtf8(kBundle).make_preferred(), total);
        std::error_code ec;
        fs::create_directories(Raw(file).parent_path(), ec);
        SetLastError(0);
        errno = 0;
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        const DWORD last = GetLastError();
        const int err = errno;
        const bool opened = bool(out);
        out.close();
        if (total == 259) {
            Check(opened, "std::ofstream opens a 259-character path");
        } else {
            Check(!opened, "std::ofstream cannot open a 260-character path");
            Check(last == ERROR_PATH_NOT_FOUND && err == ENOENT,
                  "the error Windows gives is ERROR_PATH_NOT_FOUND (3), errno ENOENT, not a length error (got " +
                      std::to_string(last) + ", errno " + std::to_string(err) + ")");
        }
        const std::string narrow = file.string();
        const HANDLE handle = CreateFileA(narrow.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                                          FILE_ATTRIBUTE_NORMAL, nullptr);
        Check((handle != INVALID_HANDLE_VALUE) == (total == 259),
              "CreateFileA (the game's own file layer) " + std::string(total == 259 ? "opens" : "cannot open") + " a " +
                  std::to_string(total) + "-character path");
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    }
    RawRemoveAll(temp / "limit");
    const fs::path stage = FolderOfLength(temp / "limit", "game.install-temp ", 143);
    const fs::path file = stage / sos::PathFromUtf8(kBundle);
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    Check(!ec, "create_directories makes the 172-character folder of the report's file");
    SetLastError(0);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    const DWORD last = GetLastError();
    Check(!out, "but the 264-character file in it cannot be created: the player's failure");
    Note("Win32 error " + std::to_string(last) + " for path length " + std::to_string(Units(file)));
    out.close();
    const fs::path deep = FolderOfLength(temp / "limit", "deep ", 249) / "x";
    fs::create_directories(deep, ec);
    Check(ec.value() == ERROR_FILENAME_EXCED_RANGE,
          "a folder of 248 characters or more cannot be made either: ERROR_FILENAME_EXCED_RANGE (206), got " +
              std::to_string(ec.value()));
    RawRemoveAll(temp / "limit");
#endif
}

void TestPathForms() {
    Step("installfs: extended-length paths");
    using sos::installfs::Extended;
    using sos::installfs::Plain;
#ifdef _WIN32
    Check(Extended(fs::path(L"C:/Games/Sons of Sparta/../SoS (1)/game")).native() == L"\\\\?\\C:\\Games\\SoS (1)\\game",
          "a drive path becomes \\\\?\\C:\\..., '/' made '\\' and '..' resolved");
    Check(Extended(fs::path(L"\\\\server\\share\\SoS\\game")).native() == L"\\\\?\\UNC\\server\\share\\SoS\\game",
          "a network path becomes \\\\?\\UNC\\server\\share\\...");
    Check(Extended(fs::path(L"\\\\?\\D:\\x\\y")).native() == L"\\\\?\\D:\\x\\y", "an extended path stays as it is");
    Check(Plain(fs::path(L"\\\\?\\UNC\\server\\share\\x")).native() == L"\\\\server\\share\\x" &&
              Plain(fs::path(L"\\\\?\\D:\\x")).native() == L"D:\\x",
          "Plain gives the path back without the prefix");
    Check(Extended(fs::path("relative\\dir")).native().starts_with(L"\\\\?\\") &&
              Plain(Extended(fs::path("relative\\dir"))) == fs::absolute("relative\\dir"),
          "a relative path is made absolute first");
    Check(Extended(fs::path(L"C:\\Games\\")).native() == L"\\\\?\\C:\\Games", "a trailing separator is dropped");
    Check(sos::installfs::PathLength(fs::path(L"C:\\A\u00e7\u00e3o\\\U0001F3AE")) == 10,
          "lengths count UTF-16 units, as MAX_PATH does (\u00e7 one, an emoji two)");
#else
    Check(Extended("/home/player/SoS/game") == fs::path("/home/player/SoS/game"), "paths are used as they are");
    Check(Plain("/x/y") == fs::path("/x/y"), "Plain leaves them alone");
#endif
}

void TestHelpers(const fs::path& temp) {
    namespace ifs = sos::installfs;
    Step("installfs: every operation of the installer on long, spaced, bracketed and Unicode paths");
    const fs::path rel = sos::PathFromUtf8(kBundle).make_preferred();
    struct Case {
        std::string name;
        size_t length;
        std::string fill_prefix;
    };
    for (const Case& c : {Case{"259 characters", 259, "a b "}, Case{"260 characters", 260, "(DLPSGAME.COM) "},
                          Case{"261 characters", 261, "[CyB1K] "}, Case{"300 characters", 300, "Jogos [PS4] (x) "},
                          Case{"400 characters", 400, "Meus Documentos (x) [y] "},
                          Case{"700 characters with Unicode", 700, "Jogos A\xC3\xA7\xC3\xA3o \xE6\xB8\xB8\xE6\x88\x8F [z] "}}) {
        const fs::path dir = FolderOfLength(temp / "helpers", c.fill_prefix, c.length - 1 - Units(rel));
        const fs::path file = dir / rel;
        Check(ifs::PathLength(file) == c.length, c.name + ": the path measures " + std::to_string(ifs::PathLength(file)));
        ifs::FsError error;
        ifs::OutputFile out;
        const bool written = ifs::CreateDirectories(file.parent_path(), error) && ifs::CreateDirectories(file.parent_path(), error) &&
                             out.Open(file, error) && out.Write("bundle data", 11, error) && out.Close(error);
        Check(written && RawRead(file) == "bundle data", c.name + ": create folders (twice), create, write and close " + error.Describe());
        uint64_t size = 0;
        Check(ifs::Exists(file) && ifs::IsRegularFile(file) && ifs::FileSize(file, size, error) && size == 11,
              c.name + ": exists and file size");
        std::vector<ifs::ListedFile> listed;
        Check(ifs::ListFiles(dir, listed, error) && listed.size() == 1 && listed[0].relative == rel && listed[0].size == 11,
              c.name + ": listing gives the relative path");
        const fs::path link = file.parent_path() / "linked.bundle", copy = file.parent_path() / "copied.bundle";
        uint64_t copied = 0;
        Check(ifs::HardLink(file, link, error) && RawRead(link) == "bundle data", c.name + ": hard link " + error.Describe());
        Check(ifs::CopyFileTo(file, copy, [&](uint64_t n) { copied = n; return true; }, error) == ifs::CopyOutcome::kCopied &&
                  RawRead(copy) == "bundle data" && copied == 11,
              c.name + ": copy with progress " + error.Describe());
        const fs::path moved = dir.parent_path() / sos::PathFromUtf8(sos::Utf8(dir.filename()) + "-moved");
        Check(ifs::Rename(dir, moved, error) && RawRead(moved / rel) == "bundle data" && !RawExists(dir),
              c.name + ": rename the folder " + error.Describe());
        Check(ifs::Remove(moved / rel.parent_path() / "linked.bundle", error) && !RawExists(moved / rel.parent_path() / "linked.bundle"),
              c.name + ": remove a file");
        Check(ifs::Remove(moved / "missing.bin", error), c.name + ": removing a missing file is not an error");
        Check(ifs::RemoveAll(moved, error) && !RawExists(moved), c.name + ": remove the whole folder " + error.Describe());
        Check(ifs::RemoveAll(moved, error), c.name + ": removing a missing folder is not an error");
    }
    std::vector<uint8_t> whole(70000, 7);
    ifs::FsError error;
    const fs::path whole_file = FolderOfLength(temp / "helpers", "whole ", 220) / rel;
    Check(ifs::WriteWholeFile(whole_file, whole.data(), whole.size(), error) && RawRead(whole_file).size() == whole.size(),
          "WriteWholeFile creates the folders and writes the file");
    uint64_t available = 0;
    Check(ifs::SpaceAvailable(whole_file.parent_path(), available, error) && available > 0, "free space of a long folder");
    std::ifstream in;
    Check(ifs::OpenForRead(whole_file, in, error) && in.get() == 7, "a long file is opened for reading");
    in.close();
    RawRemoveAll(temp / "helpers");
}

void TestErrors(const fs::path& temp) {
    namespace ifs = sos::installfs;
    Step("installfs: failures keep the operation, path, length and the system's error");
    const fs::path dir = temp / "errors (x)";
    std::error_code ec;
    fs::create_directories(Raw(dir), ec);

    ifs::FsError missing;
    ifs::OutputFile out;
    Check(!out.Open(dir / "no such folder" / "file.bin", missing), "a file in a missing folder is not created");
    const std::string line = missing.Describe();
    Note(line);
    Check(missing.operation == "create file" && missing.path == dir / "no such folder" / "file.bin" &&
              Has(line, "create file failed: path=") && Has(line, "path_length=" + std::to_string(missing.path_length())) &&
              Has(line, " error=") && Has(line, " category=") && Has(line, " message="),
          "the line names the operation, the plain path, its length, the error value, its category and message");
#ifdef _WIN32
    Check(missing.win32 == ERROR_PATH_NOT_FOUND && Has(line, "win32=3 ERROR_PATH_NOT_FOUND") &&
              missing.problem() == ifs::FsProblem::kNotFound,
          "with the Win32 error and its name");
    Check(!Has(line, "\\\\?\\"), "the log never shows the \\\\?\\ prefix");
#else
    Check(missing.code.value() == ENOENT && missing.problem() == ifs::FsProblem::kNotFound, "with errno ENOENT");
#endif

    fs::create_directories(Raw(dir / "taken.bin"), ec);
    ifs::FsError taken;
    ifs::OutputFile out2;
    Check(!out2.Open(dir / "taken.bin", taken), "a file cannot replace a folder of the same name");
#ifdef _WIN32
    Check(taken.win32 == ERROR_ACCESS_DENIED && taken.problem() == ifs::FsProblem::kAccessDenied,
          "Windows reports access denied (5) " + taken.Describe());
#endif
    RawWrite(dir / "blocker", "x");
    ifs::FsError blocked;
    Check(!ifs::CreateDirectories(dir / "blocker" / "child", blocked) && blocked.operation == "create directory" &&
              blocked.code,
          "a folder cannot be made through a file " + blocked.Describe());

#ifdef _WIN32
    RawWrite(dir / "readonly.bin", "x");
    SetFileAttributesW(Raw(dir / "readonly.bin").c_str(), FILE_ATTRIBUTE_READONLY);
    ifs::FsError readonly;
    ifs::OutputFile out3;
    Check(!out3.Open(dir / "readonly.bin", readonly) && readonly.win32 == ERROR_ACCESS_DENIED,
          "a read-only file is access denied " + readonly.Describe());
    SetFileAttributesW(Raw(dir / "readonly.bin").c_str(), FILE_ATTRIBUTE_NORMAL);
    RawWrite(dir / "held.bin", "x");
    const HANDLE held = CreateFileW(Raw(dir / "held.bin").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    ifs::FsError sharing;
    ifs::OutputFile out4;
    Check(!out4.Open(dir / "held.bin", sharing) && sharing.win32 == ERROR_SHARING_VIOLATION &&
              sharing.problem() == ifs::FsProblem::kInUse,
          "a file held open elsewhere is a sharing violation (32) " + sharing.Describe());
    ifs::FsError held_remove;
    Check(!ifs::RemoveAll(dir / "held.bin", held_remove) && held_remove.failed(), "and cannot be removed " + held_remove.Describe());
    CloseHandle(held);
    ifs::FsError invalid;
    ifs::OutputFile out5;
    Check(!out5.Open(dir / "bad<name>.bin", invalid) && invalid.win32 == ERROR_INVALID_NAME &&
              invalid.problem() == ifs::FsProblem::kInvalidName,
          "an invalid name is ERROR_INVALID_NAME (123) " + invalid.Describe());
#else
    if (geteuid() != 0) {
        RawWrite(dir / "readonly.bin", "x");
        chmod(Raw(dir / "readonly.bin").c_str(), 0444);
        ifs::FsError readonly;
        ifs::OutputFile out3;
        Check(!out3.Open(dir / "readonly.bin", readonly) && readonly.problem() == ifs::FsProblem::kAccessDenied,
              "a read-only file is permission denied " + readonly.Describe());
    } else {
        Note("running as root: the read-only check does not apply");
    }
#endif

    SetFault([](std::string_view operation, const fs::path& path) -> std::error_code {
        if (operation == "write file" && path.filename() == "full.bin") {
#ifdef _WIN32
            return {ERROR_DISK_FULL, std::system_category()};
#else
            return {ENOSPC, std::generic_category()};
#endif
        }
        if (operation == "create directory" && path.filename() == "denied") {
#ifdef _WIN32
            return {ERROR_ACCESS_DENIED, std::system_category()};
#else
            return {EACCES, std::generic_category()};
#endif
        }
        return {};
    });
    ifs::FsError full;
    ifs::OutputFile out6;
    Check(out6.Open(dir / "full.bin", full) && !out6.Write("x", 1, full) && full.operation == "write file" &&
              full.problem() == ifs::FsProblem::kDiskFull,
          "a full disk while writing is reported as one " + full.Describe());
    out6.Close(full);
    ifs::FsError denied;
    Check(!ifs::CreateDirectories(dir / "denied", denied) && denied.problem() == ifs::FsProblem::kAccessDenied,
          "a folder that cannot be made keeps its error " + denied.Describe());
    SetFault({});

    sos::PrivateFacts facts;
    facts.install_folders = {sos::Utf8(dir)};
    const std::string scrubbed = sos::ScrubText(line, facts);
    Check(Has(scrubbed, "<install>") && !Has(scrubbed, sos::Utf8(dir)), "the log scrubber hides the folder: " + scrubbed);
    RawRemoveAll(dir);
}

void TestExtract(const fs::path& temp, const std::string& source) {
    Step("Package::Extract (the function that failed) into the player's staging length");
    const Tree tree = BaseTree("v1");
    const fs::path package = WritePackage(temp / "packages" / "extract.pkg", tree, BaseSfo(), source);
    Check(!package.empty(), "the synthetic fake package was built");
    sos::Package pkg;
    const sos::PackageResult opened = pkg.Open(package);
    const sos::PackageResult loaded = pkg.LoadFiles();
    Check(opened.ok() && loaded.ok(), "it opens and its files load: " + opened.detail + loaded.detail);
    Check(pkg.total_bytes() == TotalBytes(tree) && pkg.file_count() == tree.size(), "every file is listed with its size");
    const fs::path stage = FolderOfLength(temp / "extract", "CUSA99999-1.00 (x).install-temp ", 143);
    uint64_t last = 0;
    bool monotonic = true;
    const sos::PackageResult extracted = pkg.Extract(stage, [&](uint64_t done, uint64_t total) {
        monotonic = monotonic && done >= last && total == TotalBytes(tree);
        last = done;
        return true;
    });
    Check(extracted.ok(), "the whole package unpacks into a 143-character staging folder " + extracted.detail + " " +
                              extracted.fs.Describe());
    Check(sos::installfs::PathLength(stage / sos::PathFromUtf8(kBundle)) == 264 &&
              RawRead(stage / sos::PathFromUtf8(kBundle)) == tree.at(kBundle),
          "the report's file is written at 264 characters with its content");
    bool all = true;
    for (const auto& [name, data] : tree) all = all && RawRead(stage / sos::PathFromUtf8(name)) == data;
    Check(all && RawRead(stage / "sce_sys" / "param.sfo") == BaseSfo(), "every file has its content");
    Check(monotonic && last == TotalBytes(tree), "progress counts bytes up to the total");

    SetFault([](std::string_view operation, const fs::path& path) -> std::error_code {
        return operation == "create file" && path.filename() == kBundleName
                   ? std::error_code(EACCES, std::generic_category())
                   : std::error_code();
    });
    sos::Package again;
    again.Open(package);
    again.LoadFiles();
    const fs::path second = FolderOfLength(temp / "extract", "second.install-temp ", 143);
    const sos::PackageResult failed = again.Extract(second, {});
    SetFault({});
    Check(failed.code == sos::PackageResult::kWriteFailed && failed.fs.operation == "create file" &&
              failed.fs.path == (second / sos::PathFromUtf8(kBundle)).make_preferred() &&
              Has(failed.detail, "cannot create ") && Has(failed.detail, ": "),
          "a failed file keeps the short detail with the system's message, and the whole error: " + failed.detail);
    RawRemoveAll(temp / "extract");
}

bool Installed(const sos::GameRoots& roots, const sos::GameBuild& build, const std::string& version_tag) {
    const sos::GamePaths paths = sos::BuildPaths(roots, build);
    const sos::GameCheck check = sos::CheckGameFiles(paths, g_builds);
    return check.state == sos::GameFileState::kValid && check.build == &build &&
           RawRead(paths.app0 / sos::PathFromUtf8(kBundle)) == BaseTree(version_tag).at(kBundle);
}

bool CleanupAfter(const std::vector<std::string>& phases, std::string_view step) {
    const auto failed = std::find(phases.begin(), phases.end(), step);
    return failed != phases.end() &&
           std::find(failed, phases.end(), std::string("Removing the incomplete installation...")) != phases.end();
}

std::vector<std::string> g_prefixed; // results that showed a \\?\ path

sos::InstallResult Install(const fs::path& package, const sos::GameRoots& roots, const sos::InstallHooks& hooks) {
    sos::InstallResult result = sos::InstallFromPackage(package, roots, hooks, g_builds);
    const sos::InstallMessage message = sos::DescribeInstall(result);
    std::vector<std::string> shown = {result.detail, result.recognition, message.title, message.text,
                                      result.fs_error.Describe()};
    shown.insert(shown.end(), result.leftovers.begin(), result.leftovers.end());
    for (const std::string& line : shown) {
        if (Has(line, "\\\\?\\") || Has(line, "//?/")) g_prefixed.push_back(line);
    }
    return result;
}

void PlaceSentinels(const sos::GameRoots& roots, const sos::GameBuild& build) {
    const sos::GamePaths paths = sos::BuildPaths(roots, build);
    RawWrite(paths.app0 / "sentinel-active.txt", "active");
    RawWrite(paths.image_dir / "sentinel-image.txt", "image");
    RawWrite(roots.prepared / "sentinel-root.txt", "root");
    RawWrite(roots.prepared.parent_path().parent_path() / "user" / "sentinel-user.txt", "user");
}

bool Untouched(const sos::GameRoots& roots, const sos::GameBuild& build, const std::string& version_tag) {
    const sos::GamePaths paths = sos::BuildPaths(roots, build);
    return Installed(roots, build, version_tag) && RawRead(paths.app0 / "sentinel-active.txt") == "active" &&
           RawRead(paths.image_dir / "sentinel-image.txt") == "image" &&
           RawRead(roots.prepared / "sentinel-root.txt") == "root" &&
           RawRead(roots.prepared.parent_path().parent_path() / "user" / "sentinel-user.txt") == "user" &&
           InstallerLeftovers(roots.prepared).empty() && InstallerLeftovers(roots.image).empty();
}

bool FullTree(const fs::path& app0, const Tree& tree, const std::string& sfo) {
    for (const auto& [name, data] : tree) {
        if (RawRead(app0 / sos::PathFromUtf8(name)) != data) return false;
    }
    return RawRead(app0 / "sce_sys" / "param.sfo") == sfo;
}

void TestTaskState(const fs::path& temp, const fs::path& package, const fs::path& next_package) {
    Step("the launcher's task state (trg::Task, as StartInstall drives it): failure, cleanup, usable again");
    const sos::GameRoots roots = RootsUnder(RootForApp0(temp, "task [x] ", 130));
    Recorder first;
    Check(Install(package, roots, HooksFor(first)).status == sos::InstallStatus::kInstalled, "a game is installed first");
    struct Seen {
        std::string label;
        float fraction;
    };
    std::mutex lock;
    std::vector<Seen> seen;
    auto outcome = std::make_shared<sos::InstallResult>();
    const auto job = [&, outcome](trg::Task& task) {
        sos::InstallHooks hooks;
        hooks.replace_active = true;
        hooks.build_image = FakeBootImage;
        hooks.phase = [&](const char* text) {
            task.SetLabel(text);
            task.Progress(0, 0);
            std::lock_guard guard(lock);
            seen.push_back({task.label(), task.fraction()});
        };
        hooks.progress = [&](uint64_t done, uint64_t total) {
            task.Progress(static_cast<long long>(done), static_cast<long long>(total));
            std::lock_guard guard(lock);
            seen.push_back({task.label(), task.fraction()});
            return !task.cancelled();
        };
        *outcome = sos::InstallGame(next_package, roots, hooks, g_builds);
        return outcome->ok() ? std::string() : std::string(sos::DescribeInstall(*outcome).title);
    };
    const auto run = [&](trg::Task& task, std::string& error) {
        seen.clear();
        if (!task.Start("Checking the package...", job)) return trg::Task::State::kRunning;
        for (int waited = 0; task.running() && waited < 1200; ++waited) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return task.Finish(&error);
    };
    trg::Task task;
    SetFault([](std::string_view operation, const fs::path& path) {
#ifdef _WIN32
        const std::error_code denied(ERROR_ACCESS_DENIED, std::system_category());
#else
        const std::error_code denied(EACCES, std::generic_category());
#endif
        return operation == "create file" && path.filename() == kBundleName ? denied : std::error_code();
    });
    std::string error;
    const trg::Task::State failed = run(task, error);
    SetFault({});
    float at_failure = -2.0f;
    size_t failure_index = 0, cleanup_index = 0;
    for (size_t i = 0; i < seen.size(); ++i) {
        if (seen[i].label == "Unpacking the game files..." && seen[i].fraction >= 0) {
            at_failure = seen[i].fraction;
            failure_index = i;
        }
        if (seen[i].label == "Removing the incomplete installation..." && !cleanup_index) cleanup_index = i;
    }
    char shown[32];
    std::snprintf(shown, sizeof(shown), "%.2f%%", double(at_failure) * 100.0);
    Check(failed == trg::Task::State::kFailed && error == "Could not install the game",
          "the task ends failed with the player's title: " + error);
    Check(at_failure > 0.430f && at_failure < 0.436f, std::string("the bar stood at ") + shown + " when it failed");
    Check(cleanup_index > failure_index && seen[cleanup_index].fraction < 0,
          "then it shows the cleanup of the incomplete installation, without a percentage");
    Check(!task.running() && task.state() == trg::Task::State::kIdle, "then the task is idle: the launcher is usable");
    Check(Installed(roots, g_builds[0], "v1"), "the installed game is unchanged");
    const trg::Task::State retried = run(task, error);
    float last = 0;
    for (const Seen& s : seen) {
        if (s.label == "Unpacking the game files..." && s.fraction >= 0) last = s.fraction;
    }
    Check(retried == trg::Task::State::kDone && last == 1.0f && Installed(roots, g_builds[0], "v3"),
          "a new attempt goes past 43% to 100% and installs");
}

void TestInstall(const fs::path& temp, const std::string& source) {
    const sos::GameBuild& base = g_builds[0];
    const sos::GameBuild& update = g_builds[1];
    const Tree v1 = BaseTree("v1"), v2 = BaseTree("v2"), v3 = BaseTree("v3");
    const fs::path pkg_v1 = WritePackage(temp / "packages" / "v1.pkg", v1, BaseSfo(), source);
    const fs::path pkg_v2 = WritePackage(temp / "packages" / "v2.pkg", v2, BaseSfo(), source);
    const fs::path pkg_v3 = WritePackage(temp / "packages" / "v3.pkg", v3, BaseSfo(), source);
    const fs::path pkg_update = WritePackage(temp / "packages" / "update.pkg", UpdateTree(), UpdateSfo(), source);
    Check(!pkg_v1.empty() && !pkg_v2.empty() && !pkg_v3.empty() && !pkg_update.empty(), "the synthetic packages were built");

    // The player's window: the build folder 130 characters, its staging file 264, the installed file 251.
    const fs::path root = RootForApp0(temp, "player (DLPSGAME.COM) Jogos ", 130);
    const sos::GameRoots roots = RootsUnder(root);
    const sos::GamePaths base_paths = sos::BuildPaths(roots, base);

    Step("an update needs its valid base");
    {
        const sos::GameRoots empty = RootsUnder(RootForApp0(temp, "no base ", 130));
        Recorder recorder;
        const sos::InstallResult result = Install(pkg_update, empty, HooksFor(recorder));
        Check(result.status == sos::InstallStatus::kUpdateNeedsBase && !RawExists(empty.prepared / "CUSA99999-1.01") &&
                  InstallerLeftovers(empty.prepared).empty(),
              "an update without its base is refused and writes nothing: " + result.detail);
        Recorder base_install;
        Install(pkg_v1, empty, HooksFor(base_install));
        RawRemoveFile(sos::BuildPaths(empty, base).app0 / "sce_module" / "libc.prx");
        Recorder broken;
        const sos::InstallResult refused = Install(pkg_update, empty, HooksFor(broken));
        Check(refused.status == sos::InstallStatus::kUpdateNeedsBase && !RawExists(empty.prepared / "CUSA99999-1.01"),
              "an update over an incomplete base is refused: " + refused.detail);
        RawRemoveAll(empty.prepared.parent_path().parent_path());
    }

    Step("SOS-6Y8NVY: the same scenario installs");
    {
        Recorder recorder;
        bool staged = false;
        size_t staged_length = 0;
        sos::InstallHooks hooks = HooksFor(recorder);
        hooks.progress = [&](uint64_t done, uint64_t total) {
            const fs::path stage = roots.prepared / "CUSA99999-1.00.install-temp";
            if (done > 0 && !staged && RawExists(stage)) {
                staged = true;
                staged_length = Units(stage / sos::PathFromUtf8(kBundle));
            }
            recorder.done = done;
            recorder.total = total;
            return true;
        };
        const sos::InstallResult result = Install(pkg_v1, roots, hooks);
        Check(result.status == sos::InstallStatus::kInstalled,
              "the base package installs: " + StatusText(result.status) + " " + result.detail + " " + result.fs_error.Describe());
        Check(Installed(roots, base, "v1"), "the installed game is complete, with the report's file in it");
        Check(InstallerLeftovers(roots.prepared).empty() && InstallerLeftovers(roots.image).empty(),
              "no staging, backup or update folder is left");
        Check(result.paths.target == 130 && result.paths.staging == 143 && result.paths.longest_final == 251 &&
                  result.paths.longest_staging == 264 && result.paths.longest_relative == sos::Utf8(sos::PathFromUtf8(kBundle).make_preferred()),
              "the measured lengths: folder 130, staging 143, longest installed file 251, longest staged file 264");
        Check(result.paths.long_paths == ProcessLongPaths(), "and whether this process may use long paths");
        Check(recorder.monotonic && recorder.total == TotalBytes(v1) && recorder.done == TotalBytes(v1),
              "progress goes past 43% to all of the package's bytes");
        Check(result.recognition == "package recognized: v1.pkg is CUSA99999 1.00 TEST (base, layer 1 of 1)",
              "the package is recognized as the base: " + result.recognition);
        Check(staged && staged_length == 264, "it was unpacked into the .install-temp staging folder (264 characters)");
        Check(FullTree(base_paths.app0, v1, BaseSfo()), "every file was extracted with its content");
    }

    Step("installing again over the installed game");
    {
        Recorder recorder;
        const sos::InstallResult same = Install(pkg_v1, roots, HooksFor(recorder));
        Check(same.status == sos::InstallStatus::kUpToDate, "the same build is already installed");
        const sos::InstallResult result = Install(pkg_v2, roots, HooksFor(recorder, true));
        Check(result.status == sos::InstallStatus::kInstalled && Installed(roots, base, "v2"),
              "a reinstall replaces it: " + StatusText(result.status) + " " + result.detail);
        Check(InstallerLeftovers(roots.prepared).empty(), "and leaves no staging or backup");
    }
    PlaceSentinels(roots, base);

    const auto with_fault = [](std::function<std::error_code(std::string_view, const fs::path&)> fault) {
        SetFault(std::move(fault));
    };
#ifdef _WIN32
    const std::error_code denied(ERROR_ACCESS_DENIED, std::system_category());
    const std::error_code full(ERROR_DISK_FULL, std::system_category());
#else
    const std::error_code denied(EACCES, std::generic_category());
    const std::error_code full(ENOSPC, std::generic_category());
#endif

    Step("a failed installation stops, says where and why, and changes nothing");
    {
        with_fault([&](std::string_view operation, const fs::path& path) {
            return operation == "create file" && path.filename() == kBundleName ? denied : std::error_code();
        });
        Recorder recorder;
        const sos::InstallResult result = Install(pkg_v3, roots, HooksFor(recorder, true));
        with_fault({});
        Check(result.status == sos::InstallStatus::kWriteFailed, "access denied on the report's file fails the install");
        Check(result.fs_error.operation == "create file" &&
                  result.fs_error.path == (roots.prepared / "CUSA99999-1.00.install-temp" / sos::PathFromUtf8(kBundle)).make_preferred() &&
                  result.fs_error.path_length() == 264,
              "the error names the operation and the 264-character file: " + result.fs_error.Describe());
        Check(result.stage == "Unpacking the game files..." && result.stage_done == BytesBefore(v3, kBundle) &&
                  result.stage_total == TotalBytes(v3),
              "the step and its progress when it stopped: " +
                  std::to_string(result.stage_total ? result.stage_done * 100 / result.stage_total : 0) +
                  "% of the bytes, as the bar showed");
        Check(CleanupAfter(recorder.phases, "Unpacking the game files..."),
              "the bar moves on to removing the incomplete installation instead of staying at the failed step");
        const sos::InstallMessage message = sos::DescribeInstall(result);
        Check(Has(message.text, "Antivirus") && message.title == "Could not install the game",
              "the player sees a short message that names the likely cause: " + message.title + ". " + message.text);
        Check(Untouched(roots, base, "v2"), "the installed game and its sentinels are untouched, nothing staged is active");
    }
    {
        with_fault([&](std::string_view operation, const fs::path& path) {
            return operation == "write file" && path.filename() == kBundleName ? full : std::error_code();
        });
        Recorder recorder;
        const sos::InstallResult result = Install(pkg_v3, roots, HooksFor(recorder, true));
        with_fault({});
        Check(result.status == sos::InstallStatus::kNoSpace && result.fs_error.problem() == sos::installfs::FsProblem::kDiskFull,
              "a full disk while writing is reported as not enough space " + result.fs_error.Describe());
        Check(Untouched(roots, base, "v2"), "the installed game and its sentinels are untouched");
    }
    {
        with_fault([&](std::string_view operation, const fs::path& path) {
            return operation == "create directory" && path.filename() == "PS4" ? denied : std::error_code();
        });
        Recorder recorder;
        const sos::InstallResult result = Install(pkg_v3, roots, HooksFor(recorder, true));
        with_fault({});
        Check(result.status == sos::InstallStatus::kWriteFailed && result.fs_error.operation == "create directory",
              "a folder that cannot be made fails the install with its error " + result.fs_error.Describe());
        Check(Untouched(roots, base, "v2"), "the installed game and its sentinels are untouched");
    }

#ifdef _WIN32
    const std::error_code sharing(ERROR_SHARING_VIOLATION, std::system_category());
    const std::error_code invalid(ERROR_INVALID_NAME, std::system_category());
#else
    const std::error_code sharing(EBUSY, std::generic_category());
    const std::error_code invalid(EILSEQ, std::generic_category());
#endif
    for (const auto& [code, wanted] : {std::pair{sharing, std::string("in use")}, std::pair{invalid, std::string("file names")}}) {
        with_fault([&, code = code](std::string_view operation, const fs::path& path) {
            return operation == "create file" && path.filename() == kBundleName ? code : std::error_code();
        });
        Recorder recorder;
        const sos::InstallResult result = Install(pkg_v3, roots, HooksFor(recorder, true));
        with_fault({});
        Check(result.status == sos::InstallStatus::kWriteFailed && Has(sos::DescribeInstall(result).text, wanted),
              "a create file failure (" + result.fs_error.code.message() + ") tells the player: " +
                  sos::DescribeInstall(result).text);
        Check(Untouched(roots, base, "v2"), "the installed game and its sentinels are untouched");
    }

    Step("cancelling");
    {
        Recorder recorder;
        recorder.cancel = [](uint64_t done, uint64_t total) { return total && done * 2 > total; };
        const sos::InstallResult result = Install(pkg_v3, roots, HooksFor(recorder, true));
        Check(result.status == sos::InstallStatus::kCancelled, "a cancelled install stops");
        Check(CleanupAfter(recorder.phases, "Unpacking the game files..."), "and says it removes what it staged");
        Check(Untouched(roots, base, "v2"), "the installed game and its sentinels are untouched, the staging is gone");
        Recorder retry;
        const sos::InstallResult again = Install(pkg_v2, roots, HooksFor(retry, true));
        Check(again.status == sos::InstallStatus::kInstalled && Installed(roots, base, "v2"),
              "a new attempt after cancelling installs " + again.detail);
        PlaceSentinels(roots, base);
    }

    Step("staging left by an earlier attempt");
    {
        const fs::path stale = roots.prepared / "CUSA99999-1.00.install-temp";
        RawWrite(stale / sos::PathFromUtf8(kBundle), "stale partial file");
        RawWrite(roots.image / "CUSA99999-1.00.install-temp" / "boot.bin", "stale");
        Recorder recorder;
        const sos::InstallResult result = Install(pkg_v3, roots, HooksFor(recorder, true));
        Check(result.status == sos::InstallStatus::kInstalled && Installed(roots, base, "v3"),
              "a stale staging folder with long paths is cleared and the install goes on " + result.detail);
        PlaceSentinels(roots, base);
    }
    std::string current = "v3"; // the version installed now
#ifdef _WIN32
    {
        const fs::path stale = roots.prepared / "CUSA99999-1.00.install-temp";
        RawWrite(stale / sos::PathFromUtf8(kBundle), "held");
        const HANDLE held = CreateFileW(Raw(stale / sos::PathFromUtf8(kBundle)).c_str(), GENERIC_READ, 0, nullptr,
                                        OPEN_EXISTING, 0, nullptr);
        Recorder recorder;
        const sos::InstallResult result = Install(pkg_v2, roots, HooksFor(recorder, true));
        Check(result.status == sos::InstallStatus::kWriteFailed && result.fs_error.failed() &&
                  Has(result.detail, "cannot clear"),
              "a staging folder another program holds stops the install with its error " + result.fs_error.Describe());
        Check(Installed(roots, base, "v3") && RawRead(base_paths.app0 / "sentinel-active.txt") == "active",
              "the installed game and its sentinels are untouched");
        CloseHandle(held);
        Recorder retry;
        const sos::InstallResult again = Install(pkg_v2, roots, HooksFor(retry, true));
        Check(again.status == sos::InstallStatus::kInstalled && Installed(roots, base, "v2"),
              "once it is released the next attempt installs " + again.detail);
        current = "v2";
        PlaceSentinels(roots, base);
    }
#endif
    {
        bool unpacking = false;
        Recorder recorder;
        sos::InstallHooks hooks = HooksFor(recorder, true);
        hooks.phase = [&](const char* text) {
            recorder.phases.push_back(text);
            unpacking = unpacking || std::string_view(text) == "Unpacking the game files...";
        };
        with_fault([&](std::string_view operation, const fs::path& path) {
            if (operation == "create file" && path.filename() == kBundleName) return denied;
            if (unpacking && operation == "remove folder" && sos::Utf8(path.filename()).ends_with(".install-temp")) return denied;
            return std::error_code();
        });
        const sos::InstallResult result = Install(pkg_v3, roots, hooks);
        with_fault({});
        Check(result.status == sos::InstallStatus::kWriteFailed && !result.leftovers.empty() &&
                  Has(result.leftovers.front(), "remove folder failed"),
              "a staging folder that cannot be removed is reported: " +
                  (result.leftovers.empty() ? std::string("none") : result.leftovers.front()));
        Check(Installed(roots, base, current) && RawRead(base_paths.app0 / "sentinel-active.txt") == "active",
              "the installed game and its sentinels are untouched");
        Recorder retry;
        const sos::InstallResult again = Install(pkg_v3, roots, HooksFor(retry, true));
        Check(again.status == sos::InstallStatus::kInstalled && Installed(roots, base, "v3") &&
                  InstallerLeftovers(roots.prepared).empty(),
              "the next attempt clears it and installs " + again.detail);
        current = "v3";
        PlaceSentinels(roots, base);
    }

    Step("a failed activation puts the installed game back");
    {
        with_fault([&](std::string_view operation, const fs::path& path) {
            return operation == "rename" && sos::Utf8(path.filename()) == "CUSA99999-1.00.install-temp" ? denied
                                                                                                        : std::error_code();
        });
        Recorder recorder;
        const sos::InstallResult result = Install(pkg_v1, roots, HooksFor(recorder, true));
        with_fault({});
        Check(result.status == sos::InstallStatus::kWriteFailed && Has(result.detail, "cannot activate") &&
                  result.fs_error.operation == "rename",
              "the staged game cannot be moved in: " + result.detail);
        Check(Untouched(roots, base, "v3"), "the previous game and its sentinels are back in place, no backup or staging left");
    }
    {
        with_fault([&](std::string_view operation, const fs::path& path) {
            const std::string name = sos::Utf8(path.filename());
            return operation == "rename" && (name == "CUSA99999-1.00.install-temp" || name == "CUSA99999-1.00.install-backup")
                       ? denied
                       : std::error_code();
        });
        Recorder recorder;
        const sos::InstallResult result = Install(pkg_v1, roots, HooksFor(recorder, true));
        with_fault({});
        const fs::path backup = roots.prepared / "CUSA99999-1.00.install-backup";
        const sos::GameCheck kept = sos::CheckGameFiles({backup, base_paths.image_dir}, g_builds);
        Check(result.status == sos::InstallStatus::kWriteFailed && !RawExists(base_paths.app0) &&
                  kept.state == sos::GameFileState::kValid && RawRead(backup / "sentinel-active.txt") == "active" &&
                  RawRead(backup / sos::PathFromUtf8(kBundle)) == v3.at(kBundle),
              "when the restore fails too, the previous game stays complete in its backup: " + result.detail);
        with_fault([&](std::string_view operation, const fs::path& path) {
            return operation == "rename" && sos::Utf8(path.filename()) == "CUSA99999-1.00.install-temp" ? denied
                                                                                                        : std::error_code();
        });
        Recorder failing;
        const sos::InstallResult failed_again = Install(pkg_v2, roots, HooksFor(failing, true));
        with_fault({});
        Check(failed_again.status == sos::InstallStatus::kWriteFailed && Untouched(roots, base, "v3"),
              "an attempt that fails again first puts the previous game back from its backup: " + failed_again.detail);
        Recorder retry;
        const sos::InstallResult again = Install(pkg_v2, roots, HooksFor(retry, true));
        Check(again.status == sos::InstallStatus::kInstalled && Installed(roots, base, "v2") &&
                  InstallerLeftovers(roots.prepared).empty(),
              "the next attempt installs " + again.detail);
        Recorder back;
        Check(Install(pkg_v3, roots, HooksFor(back, true)).status == sos::InstallStatus::kInstalled, "back to v3");
        PlaceSentinels(roots, base);
    }

    Step("the update over the base, then removing it");
    {
        Recorder recorder;
        const sos::InstallResult result = Install(pkg_update, roots, HooksFor(recorder));
        Check(result.status == sos::InstallStatus::kInstalled,
              "the update installs over the base: " + StatusText(result.status) + " " + result.detail + " " +
                  result.fs_error.Describe());
        const sos::GamePaths update_paths = sos::BuildPaths(roots, update);
        const sos::GameCheck check = sos::CheckGameFiles(update_paths, g_builds);
        Check(check.state == sos::GameFileState::kValid && check.build == &update &&
                  RawRead(update_paths.app0 / sos::PathFromUtf8(kBundle)) == v3.at(kBundle) &&
                  RawRead(update_paths.app0 / "Media" / "update_only.bin") == UpdateTree().at("Media/update_only.bin"),
              "the update holds the base's long files and its own");
        Check(result.paths.longest_final == 251 && result.paths.longest_relative == sos::Utf8(sos::PathFromUtf8(kBundle).make_preferred()),
              "its measured longest file is the base's bundle");
        Check(Installed(roots, base, "v3"), "the base stays installed");
        Check(sos::LastPreparedBuild(roots, g_builds) == &update, "the update is the active build");
        Check(InstallerLeftovers(roots.prepared).empty() && InstallerLeftovers(roots.image).empty(),
              "no staging or unpacked update is left");
        const sos::RemoveResult removed = sos::RemoveInstalledUpdate(roots, update, g_builds);
        Check(removed.ok() && removed.base_selected && !RawExists(update_paths.app0) && !RawExists(update_paths.image_dir),
              "removing the update works: " + removed.detail);
        Check(Installed(roots, base, "v3") && sos::LastPreparedBuild(roots, g_builds) == &base,
              "the base is installed and active again");
        Check(InstallerLeftovers(roots.prepared).empty(), "nothing is left aside");
    }

    Step("the installed game's own limit: Windows without long paths");
    for (const size_t final_length : {size_t(259), size_t(260), size_t(261), size_t(300)}) {
        const fs::path edge = RootForApp0(temp, "limit" + std::to_string(final_length) + " ", final_length - 1 - 120);
        const sos::GameRoots edge_roots = RootsUnder(edge);
        Recorder recorder;
        const sos::InstallResult result = Install(pkg_v1, edge_roots, HooksFor(recorder));
        const bool guarded = !ProcessLongPaths() && final_length >= 260;
        if (guarded) {
            Check(result.status == sos::InstallStatus::kPathTooLong && result.paths.longest_final == final_length,
                  "a game whose longest file would be " + std::to_string(final_length) +
                      " characters is refused before anything is written: " + result.detail);
            Check(!RawExists(edge_roots.prepared / "CUSA99999-1.00") && InstallerLeftovers(edge_roots.prepared).empty(),
                  "and nothing was written");
            Check(Has(sos::DescribeInstall(result).text, "shorter folder"), "the player is told to use a shorter folder");
        } else {
            Check(result.status == sos::InstallStatus::kInstalled && Installed(edge_roots, base, "v1"),
                  "a game whose longest file is " + std::to_string(final_length) + " characters installs (staging " +
                      std::to_string(result.paths.longest_staging) + ") " + StatusText(result.status) + " " + result.detail);
        }
        RawRemoveAll(edge);
    }

    Step("Unicode, spaces and brackets in the port's folder; a game folder as the source");
    {
        const fs::path unicode = RootForApp0(temp, "Jogos (DLPSGAME.COM) A\xC3\xA7\xC3\xA3o \xE6\xB8\xB8\xE6\x88\x8F ", 130);
        Recorder recorder;
        const sos::InstallResult result = Install(pkg_v1, RootsUnder(unicode), HooksFor(recorder));
        Check(result.status == sos::InstallStatus::kInstalled && Installed(RootsUnder(unicode), base, "v1"),
              "a package installs into a Unicode folder " + result.detail + " " + result.fs_error.Describe());
        const fs::path deep_package = FolderOfLength(temp / "sources", "(DLPSGAME.COM)-CUSA99999-CyB1K ", 250) /
                                      "UP9999-CUSA99999_00-SOS6Y8NVYTEST00-A0100-V0100-CyB1K.pkg";
        RawWrite(deep_package, RawRead(pkg_v2));
        Recorder deep;
        const sos::InstallResult from_deep =
            Install(deep_package, RootsUnder(unicode), HooksFor(deep, true));
        Check(from_deep.status == sos::InstallStatus::kInstalled && Installed(RootsUnder(unicode), base, "v2"),
              "a package whose own path is " + std::to_string(Units(deep_package)) + " characters installs " +
                  from_deep.detail + " " + from_deep.fs_error.Describe());
        RawRemoveAll(temp / "sources");
        const fs::path source_dir = FolderOfLength(temp / "sources", "Meus Documentos (x) unpacked ", 160);
        for (const auto& [name, data] : v2) RawWrite(source_dir / sos::PathFromUtf8(name), data);
        Recorder folder;
        const sos::InstallResult copied = sos::InstallFromFolder(source_dir, RootsUnder(unicode), HooksFor(folder, true), g_builds);
        Check(copied.status == sos::InstallStatus::kInstalled && Installed(RootsUnder(unicode), base, "v2") &&
                  copied.files == v2.size(),
              "a game folder whose files are 281 characters long installs " + copied.detail + " " + copied.fs_error.Describe());
        Check(InstallerLeftovers(RootsUnder(unicode).prepared).empty(), "no staging is left");
        RawRemoveAll(unicode);
        RawRemoveAll(temp / "sources");
    }
    Check(g_prefixed.empty(), "no result, message or log line shows a \\\\?\\ path" +
                                  (g_prefixed.empty() ? std::string() : ": " + g_prefixed.front()));
    TestTaskState(temp, pkg_v1, pkg_v3);
    RawRemoveAll(root);
}

#endif

} // namespace

#ifdef _WIN32
int ManifestCheck() {
    Step("the executable's manifest (src/sons_of_sparta.rc)");
    const HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(24));
    const HGLOBAL loaded = resource ? LoadResource(nullptr, resource) : nullptr;
    const char* data = loaded ? static_cast<const char*>(LockResource(loaded)) : nullptr;
    const std::string manifest = data ? std::string(data, SizeofResource(nullptr, resource)) : std::string();
    Check(Has(manifest, ">true</longPathAware>"), "it declares longPathAware");
    Check(Has(manifest, ">UTF-8</activeCodePage>"), "and the UTF-8 code page");
    Note(std::string("long paths in this process: ") +
         (ProcessLongPaths() ? "on (LongPathsEnabled is set on this machine)" : "off (LongPathsEnabled is not set)"));
    std::printf("manifest: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
#endif

bool RepositoryIntact(const fs::path& repository) {
    std::error_code ec;
    return fs::exists(repository / ".git" / "HEAD", ec) && fs::is_directory(repository / ".git" / "objects", ec);
}

bool FixtureInRepository(const fs::path& repository) {
    for (const char* sub : {"game", "game/prepared", "game/image", "game/extracted"}) {
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(repository / sub, ec)) {
            if (sos::Utf8(entry.path().filename()).find("CUSA99999") != std::string::npos) return true;
        }
    }
    return false;
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
#ifdef _WIN32
    if (argc == 2 && std::string_view(argv[1]) == "--manifest") return ManifestCheck();
#endif
    if (argc < 4) {
        std::fprintf(stderr, "usage: install_tests <test folder> <launcher/pkg_extract.cpp> <repository root>\n");
        return 64;
    }
    const fs::path given = sos::PathFromUtf8(argv[1]), repository = sos::PathFromUtf8(argv[3]);
    if (given.empty() || !given.is_absolute() || repository.empty() || !repository.is_absolute()) {
        Abort("the test folder and the repository root must be absolute paths");
    }
    if (!RepositoryIntact(repository)) Abort("the repository has no .git: check it by hand before testing");
    g_protected = Canonical(repository, "the repository root");
    if (FixtureInRepository(repository)) Abort("a CUSA99999 folder is already in the repository's game tree");

    const fs::path temp = given.lexically_normal();
    std::error_code ec;
    if (Units(temp) > 64 || Parts(temp).size() < 4) Abort("the test folder must be 2+ levels deep and at most 64 characters");
    if (fs::exists(Raw(temp), ec) || !fs::is_directory(Raw(temp.parent_path()), ec)) {
        Abort("the test folder must be new, in an existing folder: " + temp.string());
    }
    const fs::path planned = Canonical(temp.parent_path(), "the test folder's parent") / temp.filename();
    for (const auto& part : Parts(planned)) {
        if (part == Parts(fs::path(".git")).front() || part == Parts(fs::path(".claude")).front()) {
            Abort("the test folder is in .git or .claude");
        }
    }
    if (Within(planned, g_protected, true) || Within(g_protected, planned, true)) {
        Abort("the test folder must be outside the repository: " + temp.string());
    }
    if (!fs::create_directory(Raw(temp), ec) || ec) Abort("cannot create the test folder " + temp.string());
    g_sandbox = Canonical(temp, "the test folder");
    if (Units(g_sandbox) > 64) Abort("the resolved test folder is longer than 64 characters");
    fs::current_path(g_sandbox, ec);
    if (ec) Abort("cannot enter the test folder");

    std::ifstream source_file(sos::PathFromUtf8(argv[2]), std::ios::binary);
    const std::string source((std::istreambuf_iterator<char>(source_file)), std::istreambuf_iterator<char>());
    if (source.empty()) Abort(std::string("cannot read ") + argv[2]);
    MakeBuilds();
    std::printf("process long paths: %s\n", ProcessLongPaths() ? "on" : "off (MAX_PATH applies)");
#ifdef SOS_INSTALL_BASELINE
    int result = RunBaseline(g_sandbox, source);
#else
    SetFault({});
    TestWin32Limit(g_sandbox);
    TestPathForms();
    TestHelpers(g_sandbox);
    TestErrors(g_sandbox);
    TestExtract(g_sandbox, source);
    TestInstall(g_sandbox, source);
    sos::installfs::SetFaultHookForTesting({});
#endif
    RemoveSandbox();
    Step("the repository after the run");
    Check(RepositoryIntact(repository), "the repository's .git is still there");
    Check(!FixtureInRepository(repository), "no CUSA99999 folder was made in the repository's game tree");
#ifdef SOS_INSTALL_BASELINE
    if (result == 3 && g_failed == 0) return 3;
    std::printf("baseline: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
#else
    std::printf("install tests: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
#endif
}
