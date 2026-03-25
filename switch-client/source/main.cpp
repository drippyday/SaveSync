#include <switch.h>

#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <strings.h>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <netdb.h>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

struct Config {
  std::string server_url = "http://10.0.0.151:8080";
  std::string api_key = "change-me";
  /** Legacy single-root (GBA): used when gba_save_dir / nds_save_dir / gb_save_dir are unset. */
  std::string save_dir = "sdmc:/mGBA";
  std::string rom_dir = "sdmc:/mGBA";
  std::string rom_extension = ".gba";
  /** ``normal``: multi-root below; ``vc``: Checkpoint-style GBA VC folder only. */
  std::string sync_mode = "normal";
  std::string vc_save_dir;
  std::string gba_save_dir;
  std::string nds_save_dir;
  std::string gb_save_dir;
  std::string gba_rom_dir;
  std::string nds_rom_dir;
  std::string gb_rom_dir;
  std::string gba_rom_extension = ".gba";
  std::string nds_rom_extension = ".nds";
  std::string gb_rom_extension = ".gb";
  /** When false, the NDS save root is omitted and .sav files that match an NDS ROM (see nds_rom_dir) or skip_save_patterns are ignored. */
  bool sync_nds_saves = true;
  /** Lowercased substrings; if any appear in the save filename stem, the file is skipped (any root). */
  std::vector<std::string> skip_save_patterns;
  std::set<std::string> locked_ids;
};

enum class SaveRootKind { Gba, Nds, Gb };

struct SaveRoot {
  SaveRootKind kind = SaveRootKind::Gba;
  std::string save_dir;
  std::string rom_dir;
  std::string rom_extension;
};

struct SaveMeta {
  std::string game_id;
  std::string last_modified_utc;
  std::string server_updated_at;
  std::string sha256;
  size_t size_bytes = 0;
  std::string filename_hint;
  std::string display_name;
  /** From GET /saves JSON array order (0 = first). */
  int list_order = 0;
};

struct LocalSave {
  std::string path;
  std::string name;
  std::string game_id;
  std::string last_modified_utc;
  std::string sha256;
  size_t size_bytes = 0;
  bool mtime_trusted = false;
  std::int64_t st_mtime_unix = -1;
};

struct BaselineRow {
  std::string game_id;
  std::string sha256;
};

struct IdMapRow {
  std::string save_stem;
  std::string game_id;
};

enum class SyncAction {
  Auto,
  UploadOnly,
  DownloadOnly,
  DropboxSync,
  SaveViewer,
};

struct SyncManualFilter {
  bool all = true;
  std::set<std::string> ids;
};

static std::string trim(const std::string& input) {
  const auto start = input.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) return "";
  const auto end = input.find_last_not_of(" \t\r\n");
  return input.substr(start, end - start + 1);
}

static std::string to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

static Config load_config(const std::string& path) {
  Config cfg;
  std::ifstream file(path);
  if (!file.good()) return cfg;

  std::string section;
  std::string line;
  while (std::getline(file, line)) {
    line = trim(line);
    if (line.empty() || line[0] == '#' || line[0] == ';') continue;
    if (line.front() == '[' && line.back() == ']') {
      section = line.substr(1, line.size() - 2);
      continue;
    }
    const auto split = line.find('=');
    if (split == std::string::npos) continue;
    const std::string key = trim(line.substr(0, split));
    const std::string value = trim(line.substr(split + 1));
    if (section == "server" && key == "url") cfg.server_url = value;
    if (section == "server" && key == "api_key") cfg.api_key = value;
    if (section == "sync" && key == "mode") cfg.sync_mode = value;
    if (section == "sync" && key == "save_dir") cfg.save_dir = value;
    if (section == "sync" && key == "vc_save_dir") cfg.vc_save_dir = value;
    if (section == "sync" && key == "gba_save_dir") cfg.gba_save_dir = value;
    if (section == "sync" && key == "nds_save_dir") cfg.nds_save_dir = value;
    if (section == "sync" && key == "sync_nds_saves") {
      const std::string v = to_lower(value);
      cfg.sync_nds_saves = !(v == "0" || v == "false" || v == "no" || v == "off");
    }
    if (section == "sync" && key == "skip_save_patterns") {
      std::stringstream ss(value);
      std::string tok;
      while (std::getline(ss, tok, ',')) {
        tok = trim(tok);
        if (!tok.empty()) cfg.skip_save_patterns.push_back(to_lower(tok));
      }
    }
    if (section == "sync" && key == "gb_save_dir") cfg.gb_save_dir = value;
    if (section == "sync" && key == "locked_ids") {
      std::stringstream ss(value);
      std::string tok;
      while (std::getline(ss, tok, ',')) {
        tok = trim(tok);
        if (!tok.empty()) cfg.locked_ids.insert(to_lower(tok));
      }
    }
    if (section == "rom" && key == "rom_dir") cfg.rom_dir = value;
    if (section == "rom" && key == "rom_extension") cfg.rom_extension = value;
    if (section == "rom" && key == "gba_rom_dir") cfg.gba_rom_dir = value;
    if (section == "rom" && key == "nds_rom_dir") cfg.nds_rom_dir = value;
    if (section == "rom" && key == "gb_rom_dir") cfg.gb_rom_dir = value;
    if (section == "rom" && key == "gba_rom_extension") cfg.gba_rom_extension = value;
    if (section == "rom" && key == "nds_rom_extension") cfg.nds_rom_extension = value;
    if (section == "rom" && key == "gb_rom_extension") cfg.gb_rom_extension = value;
  }
  return cfg;
}

static std::string sanitize_game_id(const std::string& stem) {
  std::string out;
  for (char c : to_lower(stem)) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.') {
      out.push_back(c);
    } else {
      out.push_back('-');
    }
  }
  while (!out.empty() && out.front() == '-') out.erase(out.begin());
  while (!out.empty() && out.back() == '-') out.pop_back();
  return out.empty() ? "unknown-game" : out;
}

/**
 * Delta / server JSON sometimes uses a trailing ``-`` on ``game_id``; local ``sanitize_game_id`` strips it.
 * Without this, merge sees two ids for one save (upload one key, download the other) forever.
 */
static std::string canonical_game_id(const std::string& id) {
  std::string out = id;
  while (!out.empty() && out.back() == '-') out.pop_back();
  return out.empty() ? "unknown-game" : out;
}

static bool has_sav_extension(const std::string& name) {
  if (name.size() < 4) return false;
  return to_lower(name.substr(name.size() - 4)) == ".sav";
}

static std::string file_stem(const std::string& name) {
  const auto dot = name.find_last_of('.');
  return (dot == std::string::npos) ? name : name.substr(0, dot);
}

static std::vector<unsigned char> read_file(const std::string& path);

static std::string decode_header_field(const unsigned char* start, size_t len) {
  std::string out;
  out.reserve(len);
  for (size_t i = 0; i < len; i++) {
    char c = static_cast<char>(start[i]);
    if (c == '\0') break;
    if (std::isprint(static_cast<unsigned char>(c))) out.push_back(c);
  }
  return trim(out);
}

static std::string game_id_from_rom_header(const std::vector<unsigned char>& rom_data) {
  if (rom_data.size() < 0xB0) return "";
  const std::string title = decode_header_field(rom_data.data() + 0xA0, 12);
  const std::string code = decode_header_field(rom_data.data() + 0xAC, 4);
  if (title.empty() && code.empty()) return "";
  const std::string combined = code.empty() ? title : (title + "-" + code);
  return sanitize_game_id(combined);
}

/* NDS cartridge header @ 0x00 title (12), 0x0C game code (4) — mirror bridge/game_id.py / 3DS client. */
static std::string game_id_from_nds_rom_header(const std::vector<unsigned char>& rom_data) {
  if (rom_data.size() < 0x10) return "";
  const std::string title = decode_header_field(rom_data.data() + 0x00, 12);
  const std::string code = decode_header_field(rom_data.data() + 0x0C, 4);
  if (title.empty() && code.empty()) return "";
  const std::string combined = code.empty() ? title : (title + "-" + code);
  return sanitize_game_id(combined);
}

/* ROM title/gamecode live in the first ~0xB0 bytes; avoid reading multi‑MiB ROMs per save. */
static std::vector<unsigned char> read_file_prefix(const std::string& path, size_t max_bytes) {
  std::ifstream file(path, std::ios::binary);
  if (!file.good()) return {};
  std::vector<unsigned char> out(max_bytes);
  file.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(max_bytes));
  out.resize(static_cast<size_t>(file.gcount()));
  return out;
}

static void strip_trailing_slashes(std::string* s) {
  while (!s->empty() && (s->back() == '/' || s->back() == '\\')) s->pop_back();
}

static std::vector<SaveRoot> build_save_roots(const Config& cfg) {
  std::vector<SaveRoot> out;
  if (strcasecmp(cfg.sync_mode.c_str(), "vc") == 0) {
    SaveRoot r;
    r.kind = SaveRootKind::Gba;
    r.save_dir = cfg.vc_save_dir;
    r.rom_dir = cfg.rom_dir;
    r.rom_extension = cfg.rom_extension.empty() ? ".gba" : cfg.rom_extension;
    strip_trailing_slashes(&r.save_dir);
    strip_trailing_slashes(&r.rom_dir);
    out.push_back(std::move(r));
    return out;
  }
  const bool multi = !cfg.gba_save_dir.empty() || !cfg.nds_save_dir.empty() || !cfg.gb_save_dir.empty();
  if (multi) {
    if (!cfg.gba_save_dir.empty()) {
      SaveRoot r;
      r.kind = SaveRootKind::Gba;
      r.save_dir = cfg.gba_save_dir;
      r.rom_dir = cfg.gba_rom_dir;
      r.rom_extension = cfg.gba_rom_extension.empty() ? ".gba" : cfg.gba_rom_extension;
      strip_trailing_slashes(&r.save_dir);
      strip_trailing_slashes(&r.rom_dir);
      out.push_back(std::move(r));
    }
    if (!cfg.nds_save_dir.empty() && cfg.sync_nds_saves) {
      SaveRoot r;
      r.kind = SaveRootKind::Nds;
      r.save_dir = cfg.nds_save_dir;
      r.rom_dir = cfg.nds_rom_dir;
      r.rom_extension = cfg.nds_rom_extension.empty() ? ".nds" : cfg.nds_rom_extension;
      strip_trailing_slashes(&r.save_dir);
      strip_trailing_slashes(&r.rom_dir);
      out.push_back(std::move(r));
    }
    if (!cfg.gb_save_dir.empty()) {
      SaveRoot r;
      r.kind = SaveRootKind::Gb;
      r.save_dir = cfg.gb_save_dir;
      r.rom_dir = cfg.gb_rom_dir;
      r.rom_extension = cfg.gb_rom_extension.empty() ? ".gb" : cfg.gb_rom_extension;
      strip_trailing_slashes(&r.save_dir);
      strip_trailing_slashes(&r.rom_dir);
      out.push_back(std::move(r));
    }
    if (out.empty()) {
      SaveRoot r;
      r.kind = SaveRootKind::Gba;
      r.save_dir = cfg.save_dir;
      r.rom_dir = cfg.rom_dir;
      r.rom_extension = cfg.rom_extension.empty() ? ".gba" : cfg.rom_extension;
      strip_trailing_slashes(&r.save_dir);
      strip_trailing_slashes(&r.rom_dir);
      out.push_back(std::move(r));
    }
    return out;
  }
  SaveRoot r;
  r.kind = SaveRootKind::Gba;
  r.save_dir = cfg.save_dir;
  r.rom_dir = cfg.rom_dir;
  r.rom_extension = cfg.rom_extension.empty() ? ".gba" : cfg.rom_extension;
  strip_trailing_slashes(&r.save_dir);
  strip_trailing_slashes(&r.rom_dir);
  out.push_back(std::move(r));
  return out;
}

static std::string first_save_dir(const Config& cfg) {
  auto roots = build_save_roots(cfg);
  if (!roots.empty()) return roots[0].save_dir;
  return cfg.save_dir;
}

/** Default extension per root kind when ``rom_extension`` is empty. */
static const char* default_rom_ext_for_kind(SaveRootKind kind) {
  switch (kind) {
    case SaveRootKind::Gba:
      return ".gba";
    case SaveRootKind::Nds:
      return ".nds";
    case SaveRootKind::Gb:
      return ".gb";
  }
  return ".gba";
}

/**
 * One or more ROM suffixes: ``.gb`` or ``.gb,.gbc`` (comma-separated).
 * Tried in order when resolving headers / download target folder.
 */
static std::vector<std::string> rom_extensions_list(const std::string& raw, const char* default_ext) {
  std::string def = default_ext ? default_ext : ".gba";
  if (!def.empty() && def[0] != '.') def = "." + def;
  if (raw.find(',') == std::string::npos) {
    std::string e = raw.empty() ? def : raw;
    if (!e.empty() && e[0] != '.') e = "." + e;
    return {e};
  }
  std::vector<std::string> out;
  std::stringstream ss(raw);
  std::string tok;
  while (std::getline(ss, tok, ',')) {
    tok = trim(tok);
    if (tok.empty()) continue;
    if (tok[0] != '.') tok = "." + tok;
    out.push_back(tok);
  }
  if (out.empty()) out.push_back(def);
  return out;
}

static bool path_is_regular_file(const std::string& path) {
  struct stat st {};
  if (stat(path.c_str(), &st) != 0) return false;
  return S_ISREG(st.st_mode);
}

/** If ``nds_rom_dir`` has ``stem`` + NDS extension, this save is for that NDS title (mixed mGBA folder). */
static bool nds_rom_exists_for_stem(const Config& cfg, const std::string& stem) {
  if (cfg.nds_rom_dir.empty()) return false;
  std::string base = cfg.nds_rom_dir;
  strip_trailing_slashes(&base);
  const auto exts = rom_extensions_list(cfg.nds_rom_extension, ".nds");
  for (const auto& ext : exts) {
    if (path_is_regular_file(base + "/" + stem + ext)) return true;
  }
  return false;
}

/**
 * Retail DS saves are often exactly 512 KiB (e.g. Pokémon). GBA .sav is almost never that large.
 * Used when ``sync_nds_saves=false`` so mixed mGBA folders can skip NDS without manual patterns.
 */
static bool nds_retail_save_size_bytes(std::uint64_t n) {
  return n == 524288ULL;
}

static bool nds_skip_when_sync_off_by_size(const Config& cfg, std::uint64_t size_bytes) {
  return !cfg.sync_nds_saves && nds_retail_save_size_bytes(size_bytes);
}

static bool matches_skip_patterns(const std::string& text, const std::vector<std::string>& patterns) {
  if (patterns.empty()) return false;
  const std::string s = to_lower(text);
  for (const auto& p : patterns) {
    if (p.empty()) continue;
    if (s.find(p) != std::string::npos) return true;
  }
  return false;
}

static bool should_skip_sav_sync(const Config& cfg, const std::string& stem) {
  if (matches_skip_patterns(stem, cfg.skip_save_patterns)) return true;
  if (!cfg.sync_nds_saves && nds_rom_exists_for_stem(cfg, stem)) return true;
  return false;
}

static bool is_skipped_merge_id(const Config& cfg, const std::string& id) {
  return matches_skip_patterns(id, cfg.skip_save_patterns);
}

static bool should_skip_remote_for_nds_policy(const Config& cfg, const SaveMeta& m) {
  if (cfg.sync_nds_saves) return false;
  if (m.size_bytes == 0) return false;
  return nds_retail_save_size_bytes(static_cast<std::uint64_t>(m.size_bytes));
}

static std::vector<std::string> build_merge_ids_filtered(
    const Config& cfg,
    const std::map<std::string, LocalSave>& local_by_id,
    const std::map<std::string, SaveMeta>& remote,
    const std::vector<std::string>* remote_order) {
  std::vector<std::string> merge_ids;
  std::set<std::string> seen;
  merge_ids.reserve(local_by_id.size() + remote.size());

  if (remote_order && !remote_order->empty()) {
    for (const auto& id : *remote_order) {
      if (is_skipped_merge_id(cfg, id)) continue;
      const auto it = remote.find(id);
      if (it == remote.end()) continue;
      if (should_skip_remote_for_nds_policy(cfg, it->second)) continue;
      merge_ids.push_back(id);
      seen.insert(id);
    }
    std::vector<std::string> rest;
    for (const auto& [id, _] : local_by_id) {
      if (is_skipped_merge_id(cfg, id)) continue;
      if (seen.count(id)) continue;
      rest.push_back(id);
    }
    std::sort(rest.begin(), rest.end());
    merge_ids.insert(merge_ids.end(), rest.begin(), rest.end());
    return merge_ids;
  }

  for (const auto& [id, _] : local_by_id) {
    if (!is_skipped_merge_id(cfg, id)) merge_ids.push_back(id);
  }
  for (const auto& [id, meta] : remote) {
    if (local_by_id.count(id)) continue;
    if (is_skipped_merge_id(cfg, id)) continue;
    if (should_skip_remote_for_nds_policy(cfg, meta)) continue;
    merge_ids.push_back(id);
  }
  std::sort(merge_ids.begin(), merge_ids.end());
  return merge_ids;
}

static std::string resolve_game_id_for_save_root(const SaveRoot& root, const std::string& stem) {
  if (root.rom_dir.empty()) return sanitize_game_id(stem);
  const auto exts = rom_extensions_list(root.rom_extension, default_rom_ext_for_kind(root.kind));
  for (const auto& ext : exts) {
    const std::string rom_path = root.rom_dir + "/" + stem + ext;
    const std::vector<unsigned char> rom_hdr = read_file_prefix(rom_path, 512);
    if (root.kind == SaveRootKind::Nds) {
      if (rom_hdr.size() >= 0x10) {
        const std::string from_nds = game_id_from_nds_rom_header(rom_hdr);
        if (!from_nds.empty()) return from_nds;
      }
    } else if (root.kind == SaveRootKind::Gba) {
      if (rom_hdr.size() >= 0xB0) {
        const std::string from_gba = game_id_from_rom_header(rom_hdr);
        if (!from_gba.empty()) return from_gba;
      }
    }
  }
  /* GB / fallback: stem only (same as 3DS). */
  return sanitize_game_id(stem);
}

static std::vector<unsigned char> read_file(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file.good()) return {};
  file.seekg(0, std::ios::end);
  size_t len = static_cast<size_t>(file.tellg());
  file.seekg(0, std::ios::beg);
  std::vector<unsigned char> out(len);
  if (len) file.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(len));
  return out;
}

static bool write_atomic(const std::string& path, const std::vector<unsigned char>& data) {
  std::string tmp = path + ".tmp";
  std::ofstream file(tmp, std::ios::binary);
  if (!file.good()) return false;
  if (!data.empty()) file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
  file.close();
  if (std::rename(tmp.c_str(), path.c_str()) == 0) return true;
  std::remove(path.c_str());
  if (std::rename(tmp.c_str(), path.c_str()) == 0) return true;

  // Fallback path for filesystems/locks where rename replacement fails.
  std::ofstream direct(path, std::ios::binary);
  if (!direct.good()) return false;
  if (!data.empty()) direct.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
  const bool ok = direct.good();
  direct.close();
  std::remove(tmp.c_str());
  return ok;
}

static std::string sanitize_filename(const std::string& input, const std::string& fallback) {
  std::string out;
  out.reserve(input.size());
  for (char c : input) {
    if (c == '/' || c == '\\' || c == ':') continue;
    out.push_back(c);
  }
  if (out.empty()) return fallback;
  return out;
}

namespace sha256_impl {
static constexpr uint32_t K[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};
static inline uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

static void sha256_compress(uint32_t h[8], const uint8_t block[64]) {
  uint32_t w[64] = {0};
  for (int i = 0; i < 16; ++i) {
    const size_t j = static_cast<size_t>(i) * 4;
    w[i] = (uint32_t(block[j]) << 24) | (uint32_t(block[j + 1]) << 16) | (uint32_t(block[j + 2]) << 8) | uint32_t(block[j + 3]);
  }
  for (int i = 16; i < 64; ++i) {
    const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
  for (int i = 0; i < 64; ++i) {
    const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const uint32_t ch = (e & f) ^ ((~e) & g);
    const uint32_t temp1 = hh + s1 + ch + K[i] + w[i];
    const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t temp2 = s0 + maj;
    hh = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }
  h[0] += a;
  h[1] += b;
  h[2] += c;
  h[3] += d;
  h[4] += e;
  h[5] += f;
  h[6] += g;
  h[7] += hh;
}

static std::string digest_to_hex(const uint32_t h[8]) {
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  out << std::setw(8) << h[0] << std::setw(8) << h[1] << std::setw(8) << h[2] << std::setw(8) << h[3]
      << std::setw(8) << h[4] << std::setw(8) << h[5] << std::setw(8) << h[6] << std::setw(8) << h[7];
  return out.str();
}

/** Incremental SHA-256: same digest as buffering the whole message, without holding the file in RAM. */
struct Sha256 {
  uint64_t total_bytes = 0;
  uint8_t buf[64]{};
  size_t buf_len = 0;
  uint32_t h[8] = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                     0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};

  void update(const uint8_t* data, size_t len) {
    total_bytes += len;
    while (len > 0) {
      const size_t n = std::min(len, static_cast<size_t>(64 - buf_len));
      std::memcpy(buf + buf_len, data, n);
      buf_len += n;
      data += n;
      len -= n;
      if (buf_len == 64) {
        sha256_compress(h, buf);
        buf_len = 0;
      }
    }
  }

  std::string finalize() {
    const uint64_t bit_len = total_bytes * 8U;
    std::vector<unsigned char> tail;
    tail.insert(tail.end(), buf, buf + buf_len);
    tail.push_back(0x80);
    while ((tail.size() % 64) != 56) tail.push_back(0x00);
    for (int i = 7; i >= 0; --i) tail.push_back(static_cast<unsigned char>((bit_len >> (i * 8)) & 0xffU));
    for (size_t chunk = 0; chunk < tail.size(); chunk += 64) {
      sha256_compress(h, tail.data() + chunk);
    }
    return digest_to_hex(h);
  }
};

static std::string hash(const std::vector<unsigned char>& data) {
  Sha256 s;
  if (!data.empty()) s.update(data.data(), data.size());
  return s.finalize();
}

/** Hash file in chunks; ``*out_bytes`` matches bytes hashed (same as a full read). Returns false on open failure. */
static bool digest_file(const std::string& path, std::string* out_hex, size_t* out_bytes) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  Sha256 s;
  size_t total = 0;
  unsigned char chunk[65536];
  while (f) {
    f.read(reinterpret_cast<char*>(chunk), sizeof(chunk));
    const std::streamsize n = f.gcount();
    if (n > 0) {
      s.update(chunk, static_cast<size_t>(n));
      total += static_cast<size_t>(n);
    }
  }
  *out_hex = s.finalize();
  *out_bytes = total;
  return true;
}
}  // namespace sha256_impl

struct ParsedUrl {
  std::string host;
  int port = 80;
};

static bool parse_server_url(const std::string& url, ParsedUrl& parsed) {
  const std::string prefix = "http://";
  if (url.rfind(prefix, 0) != 0) return false;
  std::string hostport = url.substr(prefix.size());
  const auto slash = hostport.find('/');
  if (slash != std::string::npos) hostport = hostport.substr(0, slash);
  const auto colon = hostport.find(':');
  if (colon == std::string::npos) {
    parsed.host = hostport;
    parsed.port = 80;
  } else {
    parsed.host = hostport.substr(0, colon);
    parsed.port = std::atoi(hostport.substr(colon + 1).c_str());
  }
  return !parsed.host.empty();
}

static std::string url_encode_simple(const std::string& input) {
  std::ostringstream out;
  static const char* hex = "0123456789ABCDEF";
  for (unsigned char c : input) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out << static_cast<char>(c);
    } else {
      out << '%' << hex[(c >> 4) & 0x0f] << hex[c & 0x0f];
    }
  }
  return out.str();
}

static bool headersHaveChunked(std::string_view hdr) {
  size_t pos = 0;
  while (pos < hdr.size()) {
    const size_t eol = hdr.find("\r\n", pos);
    if (eol == std::string::npos) return false;
    const size_t lineLen = eol - pos;
    if (lineLen >= 18 && strncasecmp(hdr.data() + pos, "transfer-encoding:", 18) == 0) {
      size_t q = pos + 18;
      while (q < eol && (hdr[q] == ' ' || hdr[q] == '\t')) q++;
      for (; q + 7 <= eol; q++) {
        if (strncasecmp(hdr.data() + q, "chunked", 7) == 0) return true;
      }
    }
    pos = eol + 2;
  }
  return false;
}

static long contentLengthFromHeaders(std::string_view hdr) {
  size_t pos = 0;
  while (pos < hdr.size()) {
    const size_t eol = hdr.find("\r\n", pos);
    if (eol == std::string::npos) return -1;
    const size_t lineLen = eol - pos;
    if (lineLen >= 15 && strncasecmp(hdr.data() + pos, "content-length:", 15) == 0) {
      size_t v0 = pos + 15;
      while (v0 < eol && (hdr[v0] == ' ' || hdr[v0] == '\t')) v0++;
      return std::strtol(std::string(hdr.substr(v0, eol - v0)).c_str(), nullptr, 10);
    }
    pos = eol + 2;
  }
  return -1;
}

static bool decodeChunked(const std::vector<unsigned char>& in, std::vector<unsigned char>& out) {
  out.clear();
  size_t pos = 0;
  while (pos < in.size()) {
    const size_t line0 = pos;
    while (pos + 1 < in.size() && !(in[pos] == '\r' && in[pos + 1] == '\n')) pos++;
    if (pos + 1 >= in.size()) return false;
    std::string line(reinterpret_cast<const char*>(in.data() + line0), pos - line0);
    char* endhex = nullptr;
    unsigned long csz = std::strtoul(line.c_str(), &endhex, 16);
    if (endhex == line.c_str()) return false;
    pos += 2;
    if (csz == 0) break;
    if (csz > 100000000UL || pos + csz > in.size()) return false;
    out.insert(out.end(), in.begin() + static_cast<std::ptrdiff_t>(pos),
               in.begin() + static_cast<std::ptrdiff_t>(pos + csz));
    pos += csz;
    if (pos + 1 >= in.size() || in[pos] != '\r' || in[pos + 1] != '\n') return false;
    pos += 2;
  }
  return true;
}

static bool jsonWs(unsigned char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static bool jsonBoolAfterColon(std::string_view body, size_t& j, std::string_view word) {
  while (j < body.size() && jsonWs(static_cast<unsigned char>(body[j]))) j++;
  if (j + word.size() > body.size() || body.substr(j, word.size()) != word) return false;
  j += word.size();
  if (j < body.size()) {
    const unsigned char t = static_cast<unsigned char>(body[j]);
    if (!jsonWs(t) && t != ',' && t != '}' && t != ']') return false;
  }
  return true;
}

static bool jsonBodyHasAppliedMember(std::string_view body) {
  static constexpr std::string_view kApplied = "\"applied\"";
  for (size_t i = 0; i + kApplied.size() <= body.size(); i++) {
    if (body.substr(i, kApplied.size()) != kApplied) continue;
    const size_t after = i + kApplied.size();
    if (after < body.size()) {
      const unsigned char c = static_cast<unsigned char>(body[after]);
      if (std::isalnum(c) != 0 || c == '_') continue;
    }
    size_t j = after;
    while (j < body.size() && jsonWs(static_cast<unsigned char>(body[j]))) j++;
    if (j < body.size() && body[j] == ':') return true;
  }
  return false;
}

static bool jsonBodyAppliedIsTrue(std::string_view body) {
  static constexpr std::string_view kApplied = "\"applied\"";
  for (size_t i = 0; i + kApplied.size() <= body.size(); i++) {
    if (body.substr(i, kApplied.size()) != kApplied) continue;
    const size_t after = i + kApplied.size();
    if (after < body.size()) {
      const unsigned char c = static_cast<unsigned char>(body[after]);
      if (std::isalnum(c) != 0 || c == '_') continue;
    }
    size_t j = after;
    while (j < body.size() && jsonWs(static_cast<unsigned char>(body[j]))) j++;
    if (j >= body.size() || body[j] != ':') continue;
    j++;
    if (jsonBoolAfterColon(body, j, "true")) return true;
  }
  return false;
}

static bool jsonBodyAppliedIsFalse(std::string_view body) {
  static constexpr std::string_view kApplied = "\"applied\"";
  for (size_t i = 0; i + kApplied.size() <= body.size(); i++) {
    if (body.substr(i, kApplied.size()) != kApplied) continue;
    const size_t after = i + kApplied.size();
    if (after < body.size()) {
      const unsigned char c = static_cast<unsigned char>(body[after]);
      if (std::isalnum(c) != 0 || c == '_') continue;
    }
    size_t j = after;
    while (j < body.size() && jsonWs(static_cast<unsigned char>(body[j]))) j++;
    if (j >= body.size() || body[j] != ':') continue;
    j++;
    if (jsonBoolAfterColon(body, j, "false")) return true;
  }
  return false;
}

static bool http_request_once(
    const Config& cfg,
    const std::string& method,
    const std::string& target_path,
    const std::vector<unsigned char>& body,
    int& out_status,
    std::vector<unsigned char>& out_body,
    const char* content_type = nullptr) {
  ParsedUrl parsed;
  if (!parse_server_url(cfg.server_url, parsed)) return false;

  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  const std::string port = std::to_string(parsed.port);
  if (getaddrinfo(parsed.host.c_str(), port.c_str(), &hints, &res) != 0) return false;

  int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (sock < 0) {
    freeaddrinfo(res);
    return false;
  }
  if (connect(sock, res->ai_addr, res->ai_addrlen) != 0) {
    close(sock);
    freeaddrinfo(res);
    return false;
  }
  freeaddrinfo(res);

  const char* ct = content_type ? content_type : "application/octet-stream";
  std::ostringstream request;
  request << method << " " << target_path << " HTTP/1.1\r\n";
  request << "Host: " << parsed.host << "\r\n";
  request << "Accept-Encoding: identity\r\n";
  request << "X-API-Key: " << cfg.api_key << "\r\n";
  request << "Content-Type: " << ct << "\r\n";
  request << "Content-Length: " << body.size() << "\r\n";
  request << "Connection: close\r\n\r\n";
  const std::string header = request.str();

  if (send(sock, header.data(), header.size(), 0) < 0) {
    close(sock);
    return false;
  }
  if (!body.empty() && send(sock, body.data(), body.size(), 0) < 0) {
    close(sock);
    return false;
  }

  std::vector<unsigned char> response;
  unsigned char buffer[1024];
  while (true) {
    const ssize_t n = recv(sock, buffer, sizeof(buffer), 0);
    if (n <= 0) break;
    response.insert(response.end(), buffer, buffer + n);
  }
  close(sock);
  if (response.empty()) return false;

  const std::string response_str(response.begin(), response.end());
  const auto status_end = response_str.find("\r\n");
  if (status_end == std::string::npos) return false;
  std::istringstream status_line(response_str.substr(0, status_end));
  std::string http_ver;
  status_line >> http_ver >> out_status;

  const auto body_pos = response_str.find("\r\n\r\n");
  if (body_pos == std::string::npos) return false;
  const std::string_view header_view(response_str.data(), body_pos);
  const size_t raw_begin = body_pos + 4;
  std::vector<unsigned char> raw_body(response.begin() + static_cast<std::ptrdiff_t>(raw_begin),
                                         response.end());
  if (headersHaveChunked(header_view)) {
    if (!decodeChunked(raw_body, out_body)) return false;
  } else {
    long cl = contentLengthFromHeaders(header_view);
    size_t use = raw_body.size();
    if (cl >= 0) {
      const auto cl_u = static_cast<size_t>(cl);
      if (cl_u < use) use = cl_u;
    }
    out_body.assign(raw_body.begin(), raw_body.begin() + static_cast<std::ptrdiff_t>(use));
  }
  return true;
}

/** Retries transient TCP failures; 3 attempts with 1s backoff. */
static bool http_request(
    const Config& cfg,
    const std::string& method,
    const std::string& target_path,
    const std::vector<unsigned char>& body,
    int& out_status,
    std::vector<unsigned char>& out_body,
    const char* content_type = nullptr) {
  constexpr int kMax = 3;
  for (int attempt = 1; attempt <= kMax; ++attempt) {
    if (http_request_once(cfg, method, target_path, body, out_status, out_body, content_type)) return true;
    if (attempt < kMax) svcSleepThread(1000000000ULL);
  }
  return false;
}

struct SavesParseResult {
  std::map<std::string, SaveMeta> by_id;
  /** GET /saves array order (first occurrence of each game_id). */
  std::vector<std::string> order;
};

static SavesParseResult parse_saves_json(const std::string& json) {
  SavesParseResult result;
  std::unordered_set<std::string> seen_in_order;
  size_t pos = 0;
  int ordinal = 0;
  while (true) {
    const auto gid = json.find("\"game_id\":\"", pos);
    if (gid == std::string::npos) break;
    const auto gstart = gid + 11;
    const auto gend = json.find('"', gstart);
    if (gend == std::string::npos) break;
    const auto next_gid = json.find("\"game_id\":\"", gend + 1);
    const size_t win_end = (next_gid == std::string::npos) ? json.size() : next_gid;
    SaveMeta meta{};
    meta.game_id = canonical_game_id(json.substr(gstart, gend - gstart));
    const std::string chunk = json.substr(gend, win_end - gend);

    const auto ts = chunk.find("\"last_modified_utc\":\"");
    if (ts != std::string::npos) {
      const auto tstart = ts + 21;
      const auto tend = chunk.find('"', tstart);
      if (tend != std::string::npos) meta.last_modified_utc = chunk.substr(tstart, tend - tstart);
    }
    const auto su = chunk.find("\"server_updated_at\":\"");
    if (su != std::string::npos) {
      const auto sstart = su + 21;
      const auto send = chunk.find('"', sstart);
      if (send != std::string::npos) meta.server_updated_at = chunk.substr(sstart, send - sstart);
    }
    const auto sh = chunk.find("\"sha256\":\"");
    if (sh != std::string::npos) {
      const auto sstart = sh + 10;
      const auto send = chunk.find('"', sstart);
      if (send != std::string::npos) meta.sha256 = chunk.substr(sstart, send - sstart);
    }
    const auto fh = chunk.find("\"filename_hint\":\"");
    if (fh != std::string::npos) {
      const auto fstart = fh + 17;
      const auto fend = chunk.find('"', fstart);
      if (fend != std::string::npos) meta.filename_hint = chunk.substr(fstart, fend - fstart);
    }
    const auto dn = chunk.find("\"display_name\":\"");
    if (dn != std::string::npos) {
      const auto dstart = dn + 16;
      const auto dend = chunk.find('"', dstart);
      if (dend != std::string::npos) meta.display_name = chunk.substr(dstart, dend - dstart);
    }
    const auto sz = chunk.find("\"size_bytes\":");
    if (sz != std::string::npos) {
      size_t i = sz + 13;
      while (i < chunk.size() && (chunk[i] == ' ' || chunk[i] == '\t')) i++;
      size_t j = i;
      while (j < chunk.size() && std::isdigit(static_cast<unsigned char>(chunk[j]))) j++;
      if (j > i) meta.size_bytes = static_cast<size_t>(std::strtoull(chunk.substr(i, j - i).c_str(), nullptr, 10));
    }
    const auto lo = chunk.find("\"list_order\":");
    if (lo != std::string::npos) {
      size_t i = lo + 13;
      while (i < chunk.size() && (chunk[i] == ' ' || chunk[i] == '\t')) i++;
      char* endp = nullptr;
      const char* base = chunk.c_str();
      long v = std::strtol(base + static_cast<std::ptrdiff_t>(i), &endp, 10);
      if (endp != base + static_cast<std::ptrdiff_t>(i)) meta.list_order = static_cast<int>(v);
    } else {
      meta.list_order = ordinal;
    }
    ordinal++;
    result.by_id[meta.game_id] = meta;
    if (seen_in_order.insert(meta.game_id).second) {
      result.order.push_back(meta.game_id);
    }
    pos = win_end;
  }
  return result;
}

struct HistoryEntryParsed {
  std::string filename;
  std::string modified_utc;
  std::string display_name;
  /** Index timestamp from backup filename (server_updated_at or last_modified_utc at archive time); matches admin "Index time". */
  std::string indexed_at_utc;
  /** Preformatted UTC 12h from server (optional). */
  std::string time_display;
  bool keep = false;
};

static std::vector<HistoryEntryParsed> parse_history_json(const std::string& json) {
  std::vector<HistoryEntryParsed> out;
  size_t pos = 0;
  while (true) {
    const auto fn_pos = json.find("\"filename\":\"", pos);
    if (fn_pos == std::string::npos) break;
    const auto next_fn = json.find("\"filename\":\"", fn_pos + 14);
    const size_t chunk_end = (next_fn == std::string::npos) ? json.size() : next_fn;
    HistoryEntryParsed row;
    {
      const auto s = fn_pos + 12;
      const auto e = json.find('"', s);
      if (e == std::string::npos) break;
      row.filename = json.substr(s, e - s);
    }
    const std::string chunk = json.substr(fn_pos, chunk_end - fn_pos);
    {
      const auto m = chunk.find("\"modified_utc\":\"");
      if (m != std::string::npos) {
        const size_t ss = m + 16;
        const auto ee = chunk.find('"', ss);
        if (ee != std::string::npos) row.modified_utc = chunk.substr(ss, ee - ss);
      }
    }
    if (chunk.find("\"display_name\":null") == std::string::npos) {
      const auto d = chunk.find("\"display_name\":\"");
      if (d != std::string::npos) {
        const size_t ss = d + 16;
        const auto ee = chunk.find('"', ss);
        if (ee != std::string::npos) row.display_name = chunk.substr(ss, ee - ss);
      }
    }
    if (chunk.find("\"indexed_at_utc\":null") == std::string::npos) {
      const auto ix = chunk.find("\"indexed_at_utc\":\"");
      if (ix != std::string::npos) {
        const size_t ss = ix + 18;
        const auto ee = chunk.find('"', ss);
        if (ee != std::string::npos) row.indexed_at_utc = chunk.substr(ss, ee - ss);
      }
    }
    if (chunk.find("\"time_display\":null") == std::string::npos) {
      const auto td = chunk.find("\"time_display\":\"");
      if (td != std::string::npos) {
        const size_t ss = td + 16;
        const auto ee = chunk.find('"', ss);
        if (ee != std::string::npos) row.time_display = chunk.substr(ss, ee - ss);
      }
    }
    {
      const auto kpos = chunk.find("\"keep\"");
      if (kpos != std::string::npos) {
        const auto colon = chunk.find(':', kpos);
        if (colon != std::string::npos) {
          size_t n = colon + 1;
          while (n < chunk.size() && (chunk[n] == ' ' || chunk[n] == '\t')) {
            n++;
          }
          if (chunk.compare(n, 4, "true") == 0) {
            row.keep = true;
          }
        }
      }
    }
    out.push_back(std::move(row));
    pos = chunk_end;
  }
  return out;
}


static bool history_restore_switch(const Config& cfg, const std::string& game_id, const std::string& filename) {
  std::string body = "{\"filename\":\"";
  body += filename;
  body += "\"}";
  std::vector<unsigned char> body_bytes(body.begin(), body.end());
  int status = 0;
  std::vector<unsigned char> resp;
  const std::string path = "/save/" + game_id + "/restore";
  return http_request(cfg, "POST", path, body_bytes, status, resp, "application/json") && status == 200;
}

static std::string json_quote_string_switch(const std::string& s) {
  std::string o;
  o.reserve(s.size() + 2);
  o += '"';
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') o += '\\';
    if (c < 32 || c > 126) {
      return "";
    }
    o += static_cast<char>(c);
  }
  o += '"';
  return o;
}

static bool history_keep_switch(const Config& cfg, const std::string& game_id, const std::string& filename, bool keep) {
  if (filename.empty()) return false;
  const std::string q = json_quote_string_switch(filename);
  if (q.empty()) return false;
  std::string body = "{\"filename\":";
  body += q;
  body += ",\"keep\":";
  body += keep ? "true" : "false";
  body += "}";
  std::vector<unsigned char> body_bytes(body.begin(), body.end());
  int status = 0;
  std::vector<unsigned char> resp;
  const std::string path = "/save/" + game_id + "/history/revision/keep";
  return http_request(cfg, "PATCH", path, body_bytes, status, resp, "application/json") && status == 200;
}

static bool save_history_picker_switch(PadState* pad, Config& cfg, const std::string& game_id) {
  const std::string path = "/save/" + game_id + "/history";
  int status = 0;
  std::vector<unsigned char> body;
  if (!http_request(cfg, "GET", path, {}, status, body) || status != 200) {
    consoleClear();
    printf("History: GET failed (HTTP %d)\n", status > 0 ? status : 0);
    printf("B: back\n");
    consoleUpdate(NULL);
    while (appletMainLoop()) {
      padUpdate(pad);
      if (padGetButtonsDown(pad) & HidNpadButton_B) return true;
      consoleUpdate(NULL);
    }
    return false;
  }
  const std::string json(body.begin(), body.end());
  std::vector<HistoryEntryParsed> rows = parse_history_json(json);
  if (rows.empty()) {
    consoleClear();
    printf("No history backups for this game.\n");
    printf("B: back\n");
    consoleUpdate(NULL);
    while (appletMainLoop()) {
      padUpdate(pad);
      if (padGetButtonsDown(pad) & HidNpadButton_B) return true;
      consoleUpdate(NULL);
    }
    return false;
  }

  int cursor = 0;
  int scroll = 0;
  constexpr int kVisibleEntries = 8;
  const int total = static_cast<int>(rows.size());
  bool dirty = true;
  while (appletMainLoop()) {
    padUpdate(pad);
    const u64 down = padGetButtonsDown(pad);
    if (down & HidNpadButton_B) return true;
    if (down & HidNpadButton_R) {
      const size_t ci = static_cast<size_t>(cursor);
      const bool nk = !rows[ci].keep;
      if (history_keep_switch(cfg, game_id, rows[ci].filename, nk)) {
        rows[ci].keep = nk;
        dirty = true;
      }
      continue;
    }
    if (down & HidNpadButton_A) {
      const HistoryEntryParsed& ent = rows[static_cast<size_t>(cursor)];
      const std::string& fn = ent.filename;
      consoleClear();
      printf("Restore this version?\n");
      if (!ent.display_name.empty()) printf("Label: %s\n", ent.display_name.c_str());
      printf("Keep: %s\n", ent.keep ? "yes" : "no");
      printf("File: %s\n", fn.c_str());
      printf("A: confirm  B: cancel\n");
      consoleUpdate(NULL);
      while (appletMainLoop()) {
        padUpdate(pad);
        const u64 d2 = padGetButtonsDown(pad);
        if (d2 & HidNpadButton_B) break;
        if (d2 & HidNpadButton_A) {
          if (history_restore_switch(cfg, game_id, fn)) {
            printf("Restored. Download this game to update device.\n");
          } else {
            printf("Restore failed.\n");
          }
          printf("B: back\n");
          consoleUpdate(NULL);
          while (appletMainLoop()) {
            padUpdate(pad);
            if (padGetButtonsDown(pad) & HidNpadButton_B) return true;
            consoleUpdate(NULL);
          }
          return true;
        }
        consoleUpdate(NULL);
      }
      dirty = true;
      continue;
    }
    if (down & HidNpadButton_Up) {
      cursor = (cursor + total - 1) % total;
      dirty = true;
    }
    if (down & HidNpadButton_Down) {
      cursor = (cursor + 1) % total;
      dirty = true;
    }
    if (cursor < scroll) scroll = cursor;
    if (cursor >= scroll + kVisibleEntries) scroll = cursor - kVisibleEntries + 1;
    if (scroll < 0) scroll = 0;
    const int max_scroll = std::max(0, total - kVisibleEntries);
    if (scroll > max_scroll) scroll = max_scroll;

    if (!dirty) {
      consoleUpdate(NULL);
      continue;
    }

    consoleClear();
    printf("--- History ---\n");
    printf("\n");
    {
      constexpr int kMenuLeftCol = 22;
      printf("%-*s%s\n", kMenuLeftCol, "UP/DOWN: move", "");
      printf("%-*s%s\n", kMenuLeftCol, "A: restore", "");
      printf("%-*s%s\n", kMenuLeftCol, "R: keep / unkeep", "");
      printf("%-*s%s\n", kMenuLeftCol, "B: back", "");
    }
    printf("\n");
    for (int row = scroll; row < std::min(scroll + kVisibleEntries, total); row++) {
      const char mark = (row == cursor) ? '>' : ' ';
      const HistoryEntryParsed& r = rows[static_cast<size_t>(row)];
      std::string lbl = r.display_name.empty() ? "-" : r.display_name;
      if (lbl.size() > 44) lbl = lbl.substr(0, 41) + "...";
      printf("%c%s%s\n", mark, r.keep ? "[KEEP] " : "", lbl.c_str());
      printf("  %.48s\n", r.filename.c_str());
      printf("\n");
    }
    dirty = false;
    consoleUpdate(NULL);
  }
  return false;
}

static std::string mtime_to_utc_iso(time_t mtime) {
  std::tm tm_utc{};
  gmtime_r(&mtime, &tm_utc);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S+00:00", &tm_utc);
  return std::string(buf);
}

static std::string id_map_file_path(const std::string& dir) {
  std::string d = dir;
  while (!d.empty() && (d.back() == '/' || d.back() == '\\')) d.pop_back();
  return d + "/.gbasync-idmap";
}

static std::vector<IdMapRow> id_map_load(const std::string& dir) {
  std::vector<IdMapRow> rows;
  std::ifstream in(id_map_file_path(dir));
  std::string line;
  while (rows.size() < 1024 && std::getline(in, line)) {
    const auto tab = line.find('\t');
    if (tab == std::string::npos) continue;
    std::string stem = line.substr(0, tab);
    std::string gid = line.substr(tab + 1);
    while (!gid.empty() && (gid.back() == '\r' || gid.back() == '\n')) gid.pop_back();
    if (stem.empty() || gid.empty()) continue;
    rows.push_back({std::move(stem), std::move(gid)});
  }
  return rows;
}

static bool id_map_save(const std::string& dir, const std::vector<IdMapRow>& rows) {
  const std::string path = id_map_file_path(dir);
  const std::string tmp = path + ".tmp";
  std::ofstream out(tmp);
  if (!out) return false;
  for (const auto& r : rows) out << r.save_stem << '\t' << r.game_id << '\n';
  out.close();
  std::remove(path.c_str());
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    std::remove(tmp.c_str());
    return false;
  }
  return true;
}

static std::string id_map_lookup(const std::vector<IdMapRow>& rows, const std::string& stem) {
  for (const auto& r : rows) {
    if (r.save_stem == stem) return r.game_id;
  }
  return "";
}

/**
 * Old clients could write ``stem -> stem-2`` when multi-root scanned the same folder twice.
 * If only one save uses this stem, that suffix is stale — ignore the map entry so we re-resolve
 * to the canonical id (matches server and avoids paired UPLOAD/DOWNLOAD rows).
 */
static bool is_stale_disambiguation_suffix(
    const std::string& mapped_id, const std::string& stem, int stem_count) {
  if (stem_count != 1 || mapped_id.empty()) return false;
  const std::string stem_id = sanitize_game_id(stem);
  if (mapped_id.size() <= stem_id.size() + 1) return false;
  if (mapped_id.compare(0, stem_id.size(), stem_id) != 0) return false;
  if (mapped_id[stem_id.size()] != '-') return false;
  for (size_t i = stem_id.size() + 1; i < mapped_id.size(); ++i) {
    if (!std::isdigit(static_cast<unsigned char>(mapped_id[i]))) return false;
  }
  return true;
}

/** Same physical ``.sav`` can be scanned once per configured root (e.g. GBA + GB same folder). */
static std::vector<LocalSave> dedupe_local_saves_by_path_keep_last(std::vector<LocalSave>&& in) {
  std::unordered_map<std::string, size_t> path_to_idx;
  path_to_idx.reserve(in.size());
  std::vector<LocalSave> out;
  out.reserve(in.size());
  for (auto& s : in) {
    auto it = path_to_idx.find(s.path);
    if (it == path_to_idx.end()) {
      path_to_idx.emplace(s.path, out.size());
      out.push_back(std::move(s));
    } else {
      out[it->second] = std::move(s);
    }
  }
  return out;
}

static bool id_map_upsert(std::vector<IdMapRow>& rows, const std::string& stem, const std::string& gid) {
  for (auto& r : rows) {
    if (r.save_stem == stem) {
      if (r.game_id == gid) return false;
      r.game_id = gid;
      return true;
    }
  }
  rows.push_back({stem, gid});
  return true;
}

/** Scan one save directory (with a root for ROM header resolution). Appends to ``accum`` for multi-root. */
static void scan_local_saves_in_dir(
    const SaveRoot& root,
    const std::string& dir,
    const Config& cfg,
    std::vector<LocalSave>& accum) {
  /* Suffix disambiguation (``firered-2``) only when two saves in *this* directory map to the same
   * game_id. Seeding from other roots was wrong: the same stem/ID across GBA/GB/NDS dirs must share
   * one server id, not get suffixed and duplicate auto sync rows. */
  std::map<std::string, int> used_ids;
  std::vector<IdMapRow> id_map = id_map_load(dir);
  bool id_map_changed = false;
  std::vector<std::string> names;
  DIR* d = opendir(dir.c_str());
  if (!d) return;
  dirent* ent = nullptr;
  while ((ent = readdir(d)) != nullptr) {
    std::string name(ent->d_name);
    if (name == "." || name == ".." || !has_sav_extension(name)) continue;
    names.push_back(name);
  }
  closedir(d);
  std::sort(names.begin(), names.end());

  std::map<std::string, int> stem_count;
  for (const auto& name : names) {
    stem_count[file_stem(name)]++;
  }

  for (const auto& name : names) {
    LocalSave s{};
    s.name = name;
    s.path = dir + "/" + name;
    const std::string stem = file_stem(name);
    if (should_skip_sav_sync(cfg, stem)) continue;
    struct stat st{};
    if (stat(s.path.c_str(), &st) != 0) continue;
    if (nds_skip_when_sync_off_by_size(cfg, static_cast<std::uint64_t>(st.st_size))) continue;
    std::string mapped_id = id_map_lookup(id_map, stem);
    if (!mapped_id.empty()) mapped_id = canonical_game_id(mapped_id);
    if (is_stale_disambiguation_suffix(mapped_id, stem, stem_count[stem])) mapped_id.clear();
    const std::string resolved_id = resolve_game_id_for_save_root(root, stem);
    const std::string fallback_id = sanitize_game_id(stem);
    std::string chosen_id = mapped_id.empty() ? (resolved_id.empty() ? fallback_id : resolved_id) : mapped_id;
    if (used_ids.find(chosen_id) != used_ids.end()) {
      // Same directory only: two .sav files resolved to the same id (e.g. same ROM header).
      std::string base = fallback_id.empty() ? chosen_id : fallback_id;
      if (base.empty()) base = "unknown-game";
      chosen_id = base;
      int suffix = 2;
      while (used_ids.find(chosen_id) != used_ids.end()) {
        chosen_id = base + "-" + std::to_string(suffix++);
      }
    }
    chosen_id = canonical_game_id(chosen_id);
    id_map_changed = id_map_upsert(id_map, stem, chosen_id) || id_map_changed;
    used_ids[chosen_id] = 1;
    s.game_id = chosen_id;
    s.st_mtime_unix = static_cast<std::int64_t>(st.st_mtime);
    if (st.st_mtime == 0) {
      s.last_modified_utc = mtime_to_utc_iso(std::time(nullptr));
      s.mtime_trusted = true;
    } else {
      s.last_modified_utc = mtime_to_utc_iso(st.st_mtime);
      s.mtime_trusted = (st.st_mtime >= 946684800L);
    }
    std::string dig;
    size_t sz = 0;
    if (!sha256_impl::digest_file(s.path, &dig, &sz)) continue;
    s.size_bytes = sz;
    s.sha256 = std::move(dig);
    accum.push_back(std::move(s));
  }
  if (id_map_changed) (void)id_map_save(dir, id_map);
}

static std::vector<LocalSave> scan_all_local_saves(const Config& cfg) {
  std::vector<LocalSave> out;
  auto roots = build_save_roots(cfg);
  for (const auto& r : roots) {
    if (r.save_dir.empty()) continue;
    scan_local_saves_in_dir(r, r.save_dir, cfg, out);
  }
  return dedupe_local_saves_by_path_keep_last(std::move(out));
}

static std::string baseline_file_path_for_dir(const std::string& save_dir) {
  std::string d = save_dir;
  strip_trailing_slashes(&d);
  return d + "/.gbasync-baseline";
}

static std::string baseline_legacy_file_path_for_dir(const std::string& save_dir) {
  std::string d = save_dir;
  strip_trailing_slashes(&d);
  return d + "/.savesync-baseline";
}

static std::vector<BaselineRow> baseline_load_dir(const std::string& save_dir) {
  std::vector<BaselineRow> rows;
  std::ifstream in(baseline_file_path_for_dir(save_dir));
  if (!in) in.open(baseline_legacy_file_path_for_dir(save_dir));
  std::string line;
  while (rows.size() < 256 && std::getline(in, line)) {
    const auto tab = line.find('\t');
    if (tab == std::string::npos) continue;
    std::string gid = line.substr(0, tab);
    std::string sha = line.substr(tab + 1);
    while (!sha.empty() && (sha.back() == '\r' || sha.back() == '\n')) sha.pop_back();
    if (gid.empty() || sha.size() != 64) continue;
    rows.push_back({canonical_game_id(gid), std::move(sha)});
  }
  return rows;
}

static void baseline_upsert(std::vector<BaselineRow>& rows, const std::string& id, const std::string& sha);

static bool baseline_save_dir(const std::string& save_dir, const std::vector<BaselineRow>& rows) {
  const std::string path = baseline_file_path_for_dir(save_dir);
  const std::string tmp = path + ".tmp";
  std::ofstream out(tmp);
  if (!out) return false;
  for (const auto& r : rows) out << r.game_id << '\t' << r.sha256 << '\n';
  out.close();
  std::remove(path.c_str());
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    std::remove(tmp.c_str());
    return false;
  }
  const std::string legacy_path = baseline_legacy_file_path_for_dir(save_dir);
  const std::string legacy_tmp = legacy_path + ".tmp";
  std::ofstream legacy_out(legacy_tmp);
  if (legacy_out) {
    for (const auto& r : rows) legacy_out << r.game_id << '\t' << r.sha256 << '\n';
    legacy_out.close();
    std::remove(legacy_path.c_str());
    if (std::rename(legacy_tmp.c_str(), legacy_path.c_str()) != 0) std::remove(legacy_tmp.c_str());
  }
  return true;
}

static std::vector<BaselineRow> baseline_load_merged(const Config& cfg) {
  std::vector<BaselineRow> merged;
  auto roots = build_save_roots(cfg);
  for (const auto& r : roots) {
    if (r.save_dir.empty()) continue;
    for (const auto& row : baseline_load_dir(r.save_dir)) baseline_upsert(merged, row.game_id, row.sha256);
  }
  if (merged.empty() && !cfg.save_dir.empty()) {
    for (const auto& row : baseline_load_dir(cfg.save_dir)) baseline_upsert(merged, row.game_id, row.sha256);
  }
  return merged;
}

static int save_dir_cmp(const std::string& a, const std::string& b) {
  std::string x = a;
  std::string y = b;
  strip_trailing_slashes(&x);
  strip_trailing_slashes(&y);
  return x.compare(y);
}

static std::string pick_baseline_root_for_game(
    const Config& cfg,
    const std::string& game_id,
    const std::vector<LocalSave>& locals) {
  auto roots = build_save_roots(cfg);
  if (roots.empty()) return cfg.save_dir;
  for (const auto& loc : locals) {
    if (loc.game_id != game_id) continue;
    const std::string* best = &roots[0].save_dir;
    size_t best_len = 0;
    for (const auto& root : roots) {
      std::string r = root.save_dir;
      strip_trailing_slashes(&r);
      const size_t plen = r.size();
      if (plen > best_len && loc.path.size() >= plen && loc.path.compare(0, plen, r) == 0 &&
          (loc.path.size() == plen || loc.path[plen] == '/')) {
        best = &root.save_dir;
        best_len = plen;
      }
    }
    return *best;
  }
  return roots[0].save_dir;
}

static bool baseline_save_merged(
    const Config& cfg,
    const std::vector<BaselineRow>& rows,
    const std::vector<LocalSave>& locals) {
  auto roots = build_save_roots(cfg);
  if (roots.empty()) return baseline_save_dir(cfg.save_dir, rows);
  bool ok = true;
  for (const auto& root : roots) {
    if (root.save_dir.empty()) continue;
    std::vector<BaselineRow> subset;
    for (const auto& r : rows) {
      const std::string want = pick_baseline_root_for_game(cfg, r.game_id, locals);
      if (save_dir_cmp(want, root.save_dir) == 0) subset.push_back(r);
    }
    if (!subset.empty()) {
      if (!baseline_save_dir(root.save_dir, subset)) ok = false;
    }
  }
  return ok;
}

static bool baseline_get_sha(const std::vector<BaselineRow>& rows, const std::string& id, std::string* out_sha) {
  for (const auto& r : rows) {
    if (r.game_id == id && r.sha256.size() == 64) {
      *out_sha = r.sha256;
      return true;
    }
  }
  return false;
}

static void baseline_upsert(std::vector<BaselineRow>& rows, const std::string& id, const std::string& sha) {
  for (auto& r : rows) {
    if (r.game_id == id) {
      r.sha256 = sha;
      return;
    }
  }
  rows.push_back({id, sha});
}

static std::string gbasync_status_path(const Config& cfg) {
  std::string d = first_save_dir(cfg);
  strip_trailing_slashes(&d);
  return d + "/.gbasync-status";
}

struct SyncStatusSnap {
  std::time_t last_unix = 0;
  bool last_ok = false;
  bool server_ok = false;
  int dropbox = -1;
  std::string err;
};

static bool sync_status_load(const Config& cfg, SyncStatusSnap* out) {
  std::ifstream in(gbasync_status_path(cfg));
  if (!in) return false;
  std::string line;
  while (std::getline(in, line)) {
    const auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string k = trim(line.substr(0, eq));
    std::string v = trim(line.substr(eq + 1));
    if (k == "t") out->last_unix = static_cast<std::time_t>(std::strtoll(v.c_str(), nullptr, 10));
    if (k == "ok") out->last_ok = (v == "1");
    if (k == "srv") out->server_ok = (v == "1");
    if (k == "db") {
      if (v == "u") out->dropbox = -1;
      else if (v == "0") out->dropbox = 0;
      else if (v == "1") out->dropbox = 1;
    }
    if (k == "e") out->err = v;
  }
  return true;
}

static bool sync_status_save(const Config& cfg, const SyncStatusSnap& s) {
  std::ostringstream o;
  o << "v=1\n";
  o << "t=" << static_cast<long long>(s.last_unix) << "\n";
  o << "ok=" << (s.last_ok ? "1" : "0") << "\n";
  o << "srv=" << (s.server_ok ? "1" : "0") << "\n";
  if (s.dropbox < 0) o << "db=u\n";
  else o << "db=" << s.dropbox << "\n";
  o << "e=" << s.err << "\n";
  const std::string str = o.str();
  std::vector<unsigned char> data(str.begin(), str.end());
  return write_atomic(gbasync_status_path(cfg), data);
}

static void sync_status_after_server_work(const Config& cfg, bool last_ok, bool server_ok, const char* err_short) {
  SyncStatusSnap s{};
  (void)sync_status_load(cfg, &s);
  s.last_unix = std::time(nullptr);
  s.last_ok = last_ok;
  s.server_ok = server_ok;
  s.err = err_short ? err_short : "";
  (void)sync_status_save(cfg, s);
}

static void sync_status_after_dropbox_only(const Config& cfg, bool http_ok) {
  SyncStatusSnap s{};
  (void)sync_status_load(cfg, &s);
  s.last_unix = std::time(nullptr);
  s.last_ok = http_ok;
  s.server_ok = http_ok;
  s.dropbox = http_ok ? 1 : 0;
  s.err.clear();
  (void)sync_status_save(cfg, s);
}

static void sync_status_print_menu(const Config& cfg) {
  SyncStatusSnap s{};
  if (!sync_status_load(cfg, &s)) {
    printf("Last sync: (none)\n");
    printf("Server: —\n");
    printf("Dropbox: —\n\n");
    return;
  }
  char tbuf[32] = "unknown";
  if (s.last_unix > 0) {
    std::tm tm_utc{};
    gmtime_r(&s.last_unix, &tm_utc);
    std::strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M UTC", &tm_utc);
  }
  const char* srv = s.server_ok ? "OK" : "fail";
  const char* db = (s.dropbox < 0) ? "—" : (s.dropbox ? "OK" : "fail");
  const char* ok = s.last_ok ? "OK" : "fail";
  printf("Last sync: %s %s\n", ok, tbuf);
  printf("Server: %s\n", srv);
  printf("Dropbox: %s\n", db);
  if (!s.err.empty()) printf("Last error: %.60s\n", s.err.c_str());
  printf("\n");
}

enum class AutoPlanKind {
  Locked,
  Ok,
  Upload,
  Download,
  Conflict,
};

struct AutoPlanRow {
  std::string id;
  AutoPlanKind kind = AutoPlanKind::Ok;
};

static AutoPlanKind classify_auto_row(
    const std::string& id,
    bool has_l,
    bool has_r,
    const LocalSave* l,
    const SaveMeta* r,
    const std::vector<BaselineRow>& baseline,
    bool locked) {
  if (has_l && has_r) {
    if (l->sha256 == r->sha256) return AutoPlanKind::Ok;
    if (locked) return AutoPlanKind::Locked;
    std::string base_sha;
    if (!baseline_get_sha(baseline, id, &base_sha)) return AutoPlanKind::Conflict;
    const bool loc_eq = (strcasecmp(l->sha256.c_str(), base_sha.c_str()) == 0);
    const bool rem_eq = (strcasecmp(r->sha256.c_str(), base_sha.c_str()) == 0);
    if (loc_eq && !rem_eq) return AutoPlanKind::Download;
    if (!loc_eq && rem_eq) return AutoPlanKind::Upload;
    if (!loc_eq && !rem_eq) return AutoPlanKind::Conflict;
  } else if (has_l && !has_r) {
    return AutoPlanKind::Upload;
  } else if (!has_l && has_r) {
    return AutoPlanKind::Download;
  }
  return AutoPlanKind::Ok;
}

static bool parse_ini_key_value(const std::string& line, std::string* key, std::string* val) {
  const auto eq = line.find('=');
  if (eq == std::string::npos) return false;
  *key = trim(line.substr(0, eq));
  *val = trim(line.substr(eq + 1));
  return !key->empty();
}

static bool save_locked_ids_to_ini(const std::string& path, const std::set<std::string>& locked) {
  std::vector<std::string> lines;
  {
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) lines.push_back(line);
  }
  std::ostringstream joined;
  for (auto it = locked.begin(); it != locked.end(); ++it) {
    if (it != locked.begin()) joined << ",";
    joined << *it;
  }
  const std::string new_val = joined.str();

  bool in_sync = false;
  bool replaced = false;
  for (size_t i = 0; i < lines.size(); ++i) {
    const std::string t = trim(lines[i]);
    if (t.size() >= 2 && t.front() == '[' && t.back() == ']') {
      in_sync = (t == "[sync]");
      continue;
    }
    if (in_sync) {
      std::string k;
      std::string v;
      if (parse_ini_key_value(lines[i], &k, &v) && k == "locked_ids") {
        lines[i] = std::string("locked_ids=") + new_val;
        replaced = true;
        break;
      }
    }
  }
  if (!replaced) {
    for (size_t i = 0; i < lines.size(); ++i) {
      if (trim(lines[i]) == "[sync]") {
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(i + 1), std::string("locked_ids=") + new_val);
        replaced = true;
        break;
      }
    }
  }
  if (!replaced) {
    if (!lines.empty()) lines.push_back("");
    lines.push_back("[sync]");
    lines.push_back(std::string("locked_ids=") + new_val);
  }
  std::ostringstream out;
  for (size_t i = 0; i < lines.size(); ++i) out << lines[i] << "\n";
  const std::string data = out.str();
  return write_atomic(path, std::vector<unsigned char>(data.begin(), data.end()));
}

static void build_auto_plan_vector(
    const Config& cfg,
    const std::vector<std::string>& merge_ids,
    const std::map<std::string, LocalSave>& local_by_id,
    const std::map<std::string, SaveMeta>& remote,
    const std::vector<BaselineRow>& baseline,
    std::vector<AutoPlanRow>& plan) {
  plan.clear();
  plan.reserve(merge_ids.size());
  for (const std::string& id : merge_ids) {
    auto lit = local_by_id.find(id);
    auto rit = remote.find(id);
    const bool has_l = lit != local_by_id.end();
    const bool has_r = rit != remote.end();
    const bool locked = cfg.locked_ids.count(to_lower(id)) > 0;
    AutoPlanKind k = AutoPlanKind::Ok;
    if (has_l && has_r) {
      k = classify_auto_row(id, true, true, &lit->second, &rit->second, baseline, locked);
    } else if (has_l && !has_r) {
      k = locked ? AutoPlanKind::Locked : AutoPlanKind::Upload;
    } else if (!has_l && has_r) {
      k = locked ? AutoPlanKind::Locked : AutoPlanKind::Download;
    }
    plan.push_back({id, k});
  }
}

static void recount_auto_plan(const std::vector<AutoPlanRow>& plan, int* nu, int* nd, int* ns, int* nc, int* nl, int* nk) {
  int u = 0, d = 0, c = 0, l = 0, k = 0;
  if (ns) *ns = 0;
  for (const auto& row : plan) {
    switch (row.kind) {
      case AutoPlanKind::Upload:
        u++;
        break;
      case AutoPlanKind::Download:
        d++;
        break;
      case AutoPlanKind::Conflict:
        c++;
        break;
      case AutoPlanKind::Locked:
        l++;
        break;
      case AutoPlanKind::Ok:
        k++;
        break;
    }
  }
  if (nu) *nu = u;
  if (nd) *nd = d;
  if (ns) *ns = 0;
  if (nc) *nc = c;
  if (nl) *nl = l;
  if (nk) *nk = k;
}

static const char* auto_plan_kind_label(AutoPlanKind k) {
  switch (k) {
    case AutoPlanKind::Locked:
      return "SKIP (locked)";
    case AutoPlanKind::Ok:
      return "OK";
    case AutoPlanKind::Upload:
      return "UPLOAD";
    case AutoPlanKind::Download:
      return "DOWNLOAD";
    case AutoPlanKind::Conflict:
      return "CONFLICT (choose side)";
    default:
      return "?";
  }
}

static constexpr const char kSwitchConfigIni[] = "sdmc:/switch/gba-sync/config.ini";

/** 0 = cancel, 1 = confirm, 2 = confirm and exit app after apply. */
static int preview_auto_plan(
    PadState* pad,
    Config& cfg,
    const std::vector<std::string>& merge_ids,
    const std::map<std::string, LocalSave>& local_by_id,
    const std::map<std::string, SaveMeta>& remote,
    const std::vector<BaselineRow>& baseline,
    std::vector<AutoPlanRow>& plan) {
  int nu = 0, nd = 0, ns = 0, nc = 0, nl = 0, nk = 0;
  build_auto_plan_vector(cfg, merge_ids, local_by_id, remote, baseline, plan);
  recount_auto_plan(plan, &nu, &nd, &ns, &nc, &nl, &nk);

  std::vector<size_t> filt;
  filt.reserve(plan.size());
  for (size_t i = 0; i < plan.size(); i++) {
    if (plan[i].kind != AutoPlanKind::Ok) filt.push_back(i);
  }
  int cursor = 0;
  constexpr int kVisible = 14;
  int scroll = 0;
  const int n_filt = static_cast<int>(filt.size());
  int total_rows = std::max(1, n_filt);
  bool dirty = true;

  constexpr u64 kSyncMask =
      HidNpadButton_A | HidNpadButton_B | HidNpadButton_X | HidNpadButton_Y | HidNpadButton_Plus;
  while (appletMainLoop()) {
    padUpdate(pad);
    (void)padGetButtonsDown(pad);
    if ((padGetButtons(pad) & kSyncMask) == 0) break;
    consoleUpdate(NULL);
  }

  while (appletMainLoop()) {
    padUpdate(pad);
    const u64 down = padGetButtonsDown(pad);
    if (down & HidNpadButton_A) return 1;
    if (down & HidNpadButton_Plus) return 2;
    if (down & HidNpadButton_B) return 0;
    if (n_filt > 0) {
      if (down & HidNpadButton_Up) {
        cursor = (cursor + total_rows - 1) % total_rows;
        dirty = true;
      }
      if (down & HidNpadButton_Down) {
        cursor = (cursor + 1) % total_rows;
        dirty = true;
      }
      if (cursor < scroll) scroll = cursor;
      if (cursor >= scroll + kVisible) scroll = cursor - kVisible + 1;
      if (scroll < 0) scroll = 0;
      const int max_scroll = std::max(0, total_rows - kVisible);
      if (scroll > max_scroll) scroll = max_scroll;
    }

    if (!dirty) {
      consoleUpdate(NULL);
      continue;
    }

    consoleClear();
    printf("--- Sync preview ---\n");
    printf("UP:%d DOWN:%d CONF:%d LOCK:%d\n", nu, nd, nc, nl);
    printf("\n");
    {
      constexpr int kPrevCol = 20;
      printf("%-*s%s\n", kPrevCol, "A: confirm", "");
      printf("%-*s%s\n", kPrevCol, "+: sync & exit", "");
      printf("%-*s%s\n", kPrevCol, "B: back", "");
    }
    printf("\n");
    for (int row = scroll; row < std::min(scroll + kVisible, n_filt); row++) {
      const char mark = (row == cursor) ? '>' : ' ';
      const AutoPlanRow& pr = plan[filt[static_cast<size_t>(row)]];
      std::string disp = pr.id;
      const auto rit = remote.find(pr.id);
      if (rit != remote.end() && !rit->second.display_name.empty()) disp = rit->second.display_name;
      printf("%c %.28s  %s\n", mark, disp.c_str(), auto_plan_kind_label(pr.kind));
    }
    dirty = false;
    consoleUpdate(NULL);
  }
  return 0;
}

static void debug_report_sync_start_switch(const Config& cfg, const std::vector<LocalSave>& local) {
  int untrusted = 0;
  for (const auto& s : local) {
    if (!s.mtime_trusted) untrusted++;
  }
  std::ostringstream json;
  json << "{\"utc_iso\":\"" << mtime_to_utc_iso(std::time(nullptr))
       << "\",\"platform\":\"switch-homebrew\",\"phase\":\"sync_auto_start\",\"untrusted_local_saves\":"
       << untrusted << "}";
  const std::string jstr = json.str();
  std::vector<unsigned char> post_body(jstr.begin(), jstr.end());
  int st = 0;
  std::vector<unsigned char> resp;
  (void)http_request(cfg, "POST", "/debug/client-clock", post_body, st, resp, "application/json");
}

static bool bodyContainsSha256Value(const std::string& body, const std::string& expect) {
  if (expect.size() != 64) return false;
  const std::string key = "\"sha256\"";
  size_t pos = 0;
  while ((pos = body.find(key, pos)) != std::string::npos) {
    size_t j = pos + key.size();
    while (j < body.size() && std::isspace(static_cast<unsigned char>(body[j]))) j++;
    if (j >= body.size() || body[j] != ':') {
      pos++;
      continue;
    }
    j++;
    while (j < body.size() && (body[j] == ' ' || body[j] == '\t')) j++;
    if (j >= body.size() || body[j] != '"') {
      pos++;
      continue;
    }
    j++;
    if (j + 64 > body.size()) {
      pos++;
      continue;
    }
    std::string candidate = body.substr(j, 64);
    bool hex = true;
    for (char c : candidate) {
      if (!std::isxdigit(static_cast<unsigned char>(c))) {
        hex = false;
        break;
      }
    }
    if (hex && strcasecmp(candidate.c_str(), expect.c_str()) == 0) return true;
    pos = j;
  }
  return false;
}

static bool put_save_log(const Config& cfg,
                         const LocalSave& l,
                         bool force,
                         const std::string& platform,
                         std::vector<std::string>& logs,
                         std::string* out_uploaded_sha = nullptr) {
  const std::vector<unsigned char> bytes = read_file(l.path);
  const std::string computed_sha = sha256_impl::hash(bytes);
  const size_t size_bytes = bytes.size();
  std::ostringstream path;
  path << "/save/" << l.game_id << "?last_modified_utc=" << url_encode_simple(l.last_modified_utc)
       << "&sha256=" << url_encode_simple(computed_sha) << "&size_bytes=" << size_bytes
       << "&filename_hint=" << url_encode_simple(l.name) << "&platform_source=" << platform
       << "&client_clock_utc=" << url_encode_simple(mtime_to_utc_iso(std::time(nullptr)));
  if (force) path << "&force=1";
  std::vector<unsigned char> put_resp;
  int put_status = 0;
  if (!http_request(cfg, "PUT", path.str(), bytes, put_status, put_resp) || put_status != 200) {
    logs.push_back(l.game_id + ": ERROR(upload)");
    return false;
  }
  const std::string body(put_resp.begin(), put_resp.end());
  const std::string_view bv(body);
  if (jsonBodyAppliedIsFalse(bv)) {
    logs.push_back(l.game_id + ": REJECTED (server kept existing copy)");
    return false;
  }
  if (jsonBodyAppliedIsTrue(bv) || !jsonBodyHasAppliedMember(bv)) {
    logs.push_back(l.game_id + ": UPLOADED");
    if (out_uploaded_sha) *out_uploaded_sha = computed_sha;
    return true;
  }
  bool ok = bodyContainsSha256Value(body, computed_sha);
  if (!ok) {
    std::vector<unsigned char> meta_body;
    int mst = 0;
    if (http_request(cfg, "GET", "/save/" + l.game_id + "/meta", {}, mst, meta_body) && mst == 200) {
      const std::string mb(meta_body.begin(), meta_body.end());
      ok = bodyContainsSha256Value(mb, computed_sha);
    }
  }
  if (!ok) {
    logs.push_back(l.game_id + ": REJECTED (could not confirm save on server)");
    return false;
  }
  logs.push_back(l.game_id + ": UPLOADED");
  if (out_uploaded_sha) *out_uploaded_sha = computed_sha;
  return true;
}

static void pick_download_dir_switch(
    const Config& cfg,
    const std::string& game_id,
    const SaveMeta& r,
    const std::vector<LocalSave>& locals,
    std::string* out_dir) {
  for (const auto& loc : locals) {
    if (loc.game_id != game_id) continue;
    const auto slash = loc.path.find_last_of('/');
    if (slash != std::string::npos) {
      *out_dir = loc.path.substr(0, slash);
      return;
    }
  }
  std::string fallback_name = game_id + ".sav";
  std::string target_name = r.filename_hint.empty() ? fallback_name : sanitize_filename(r.filename_hint, fallback_name);
  if (!has_sav_extension(target_name)) target_name += ".sav";
  const std::string stem = file_stem(target_name);
  auto roots = build_save_roots(cfg);
  for (const auto& root : roots) {
    if (root.rom_dir.empty()) continue;
    for (const auto& ext : rom_extensions_list(root.rom_extension, default_rom_ext_for_kind(root.kind))) {
      const std::string rom_path = root.rom_dir + "/" + stem + ext;
      std::ifstream test(rom_path);
      if (test.good()) {
        *out_dir = root.save_dir;
        return;
      }
    }
  }
  *out_dir = roots.empty() ? cfg.save_dir : roots[0].save_dir;
}

static bool get_save_log(
    const Config& cfg,
    const std::string& id,
    const SaveMeta& r,
    std::vector<std::string>& logs,
    const std::vector<LocalSave>& locals) {
  std::string dest_dir;
  pick_download_dir_switch(cfg, id, r, locals, &dest_dir);
  std::string fallback_name = id + ".sav";
  std::string target_name = r.filename_hint.empty() ? fallback_name : sanitize_filename(r.filename_hint, fallback_name);
  if (!has_sav_extension(target_name)) target_name += ".sav";
  const std::string target_path = dest_dir + "/" + target_name;
  std::vector<unsigned char> save_data;
  int get_status = 0;
  if (!http_request(cfg, "GET", "/save/" + id, {}, get_status, save_data) || get_status != 200) {
    logs.push_back(id + ": ERROR(download)");
    return false;
  }
  if (write_atomic(target_path, save_data)) {
    logs.push_back(id + ": DOWNLOADED");
    return true;
  }
  logs.push_back(id + ": ERROR(write)");
  return false;
}

static void resolve_both_changed_conflict_switch(PadState* pad,
                                                 const Config& cfg,
                                                 const std::string& id,
                                                 const LocalSave& l,
                                                 const SaveMeta& r,
                                                 const std::string& plat,
                                                 const std::vector<LocalSave>& locals,
                                                 std::vector<std::string>& logs,
                                                 std::vector<BaselineRow>& baseline,
                                                 bool no_sync_history) {
  consoleClear();
  printf("\n  -------- Conflict --------\n\n  %s\n\n", id.c_str());
  if (no_sync_history) {
    printf("  No sync history on this device yet.\n");
    printf("  Choose which version to keep:\n\n");
  } else {
    printf("  Local and server both changed since\n");
    printf("  the last successful sync.\n\n");
  }
  printf("  X   Upload local (overwrite server)\n");
  printf("  Y   Download server (overwrite local)\n");
  printf("  B   Skip for now\n\n");
  consoleUpdate(NULL);
  while (appletMainLoop()) {
    padUpdate(pad);
    const u64 down = padGetButtonsDown(pad);
    if (down & HidNpadButton_X) {
      std::string uploaded_sha;
      if (put_save_log(cfg, l, true, plat, logs, &uploaded_sha)) baseline_upsert(baseline, id, uploaded_sha);
      break;
    }
    if (down & HidNpadButton_Y) {
      if (get_save_log(cfg, id, r, logs, locals)) baseline_upsert(baseline, id, r.sha256);
      break;
    }
    if (down & HidNpadButton_B) break;
    consoleUpdate(NULL);
  }
}

static bool pick_upload_selection(PadState* pad, const Config& cfg, SyncManualFilter* out) {
  auto locals_all = scan_all_local_saves(cfg);
  std::vector<LocalSave> locals;
  locals.reserve(locals_all.size());
  for (auto& s : locals_all) {
    if (should_skip_sav_sync(cfg, file_stem(s.name))) continue;
    if (is_skipped_merge_id(cfg, s.game_id)) continue;
    locals.push_back(std::move(s));
  }
  if (locals.empty()) {
    printf("No local .sav files to upload.\n");
    return false;
  }
  /** Server index: display names + list order for this picker. */
  SavesParseResult server_pr;
  {
    int st = 0;
    std::vector<unsigned char> body;
    if (http_request(cfg, "GET", "/saves", {}, st, body) && st == 200) {
      server_pr = parse_saves_json(std::string(body.begin(), body.end()));
    }
  }
  const auto& server_meta = server_pr.by_id;
  if (!server_pr.order.empty()) {
    std::unordered_map<std::string, size_t> pos;
    for (size_t i = 0; i < server_pr.order.size(); ++i) pos[server_pr.order[i]] = i;
    std::sort(locals.begin(), locals.end(), [&](const LocalSave& a, const LocalSave& b) {
      const auto pa = pos.find(a.game_id);
      const auto pb = pos.find(b.game_id);
      const bool ha = pa != pos.end();
      const bool hb = pb != pos.end();
      if (ha && !hb) return true;
      if (!ha && hb) return false;
      if (ha && hb) return pa->second < pb->second;
      return a.game_id < b.game_id;
    });
  }
  const int n = static_cast<int>(locals.size());
  std::vector<bool> picked(static_cast<size_t>(n), true);
  bool master_all = true;
  int cursor = 0;
  int scroll = 0;
  constexpr int kVisible = 16;
  bool dirty = true;

  while (appletMainLoop()) {
    padUpdate(pad);
    const u64 down = padGetButtonsDown(pad);
    if (down & HidNpadButton_Plus) {
      if (master_all) {
        out->all = true;
        out->ids.clear();
        return true;
      }
      out->all = false;
      out->ids.clear();
      for (int i = 0; i < n; i++) {
        if (picked[static_cast<size_t>(i)]) out->ids.insert(locals[static_cast<size_t>(i)].game_id);
      }
      if (out->ids.empty()) continue;
      return true;
    }
    if (down & HidNpadButton_B) return false;

    const int total_rows = n + 1;
    if (down & HidNpadButton_Up) {
      cursor = (cursor + total_rows - 1) % total_rows;
      dirty = true;
    }
    if (down & HidNpadButton_Down) {
      cursor = (cursor + 1) % total_rows;
      dirty = true;
    }
    if (down & HidNpadButton_A) {
      if (cursor == 0) {
        master_all = !master_all;
        const bool v = master_all;
        for (int i = 0; i < n; i++) picked[static_cast<size_t>(i)] = v;
      } else {
        picked[static_cast<size_t>(cursor - 1)] = !picked[static_cast<size_t>(cursor - 1)];
        master_all = true;
        for (int i = 0; i < n; i++) {
          if (!picked[static_cast<size_t>(i)]) master_all = false;
        }
      }
      dirty = true;
    }

    if (cursor < scroll) scroll = cursor;
    if (cursor >= scroll + kVisible) scroll = cursor - kVisible + 1;
    if (scroll < 0) scroll = 0;
    const int max_scroll = std::max(0, total_rows - kVisible);
    if (scroll > max_scroll) scroll = max_scroll;

    if (!dirty) {
      consoleUpdate(NULL);
      continue;
    }

    consoleClear();
    printf("Upload: choose saves\n");
    printf("D-pad: move   A: toggle   +: run upload   B: back to menu\n\n");
    for (int row = scroll; row < std::min(scroll + kVisible, total_rows); row++) {
      const char mark = (row == cursor) ? '>' : ' ';
      if (row == 0) {
        printf("%c [%c] ALL SAVES\n", mark, master_all ? 'x' : ' ');
      } else {
        const LocalSave& L = locals[static_cast<size_t>(row - 1)];
        const char* disp = L.game_id.c_str();
        std::string disp_buf;
        const auto sit = server_meta.find(L.game_id);
        if (sit != server_meta.end() && !sit->second.display_name.empty()) {
          disp_buf = sit->second.display_name;
          disp = disp_buf.c_str();
        }
        printf(
            "%c [%c] %.28s\n",
            mark,
            picked[static_cast<size_t>(row - 1)] ? 'x' : ' ',
            disp);
      }
    }
    dirty = false;
    consoleUpdate(NULL);
  }
  return false;
}

static bool pick_download_selection(PadState* pad, const Config& cfg, SyncManualFilter* out) {
  int status = 0;
  std::vector<unsigned char> body;
  if (!http_request(cfg, "GET", "/saves", {}, status, body) || status != 200) {
    printf("ERROR: GET /saves failed (cannot list downloads)\n");
    return false;
  }
  std::string json(body.begin(), body.end());
  const SavesParseResult pr = parse_saves_json(json);
  if (pr.by_id.empty()) {
    printf("No remote saves to download.\n");
    return false;
  }
  std::vector<std::pair<std::string, SaveMeta>> rows;
  rows.reserve(pr.by_id.size());
  std::set<std::string> added;
  for (const auto& id : pr.order) {
    const auto it = pr.by_id.find(id);
    if (it == pr.by_id.end()) continue;
    if (is_skipped_merge_id(cfg, id)) continue;
    if (should_skip_remote_for_nds_policy(cfg, it->second)) continue;
    rows.push_back({it->first, it->second});
    added.insert(id);
  }
  for (const auto& kv : pr.by_id) {
    if (added.count(kv.first)) continue;
    if (is_skipped_merge_id(cfg, kv.first)) continue;
    if (should_skip_remote_for_nds_policy(cfg, kv.second)) continue;
    rows.push_back(kv);
  }
  if (rows.empty()) {
    printf("No remote saves to download (all filtered).\n");
    return false;
  }

  const int n = static_cast<int>(rows.size());
  std::vector<bool> picked(static_cast<size_t>(n), true);
  bool master_all = true;
  int cursor = 0;
  int scroll = 0;
  constexpr int kVisible = 16;
  bool dirty = true;

  while (appletMainLoop()) {
    padUpdate(pad);
    const u64 down = padGetButtonsDown(pad);
    if (down & HidNpadButton_Plus) {
      if (master_all) {
        out->all = true;
        out->ids.clear();
        return true;
      }
      out->all = false;
      out->ids.clear();
      for (int i = 0; i < n; i++) {
        if (picked[static_cast<size_t>(i)]) out->ids.insert(rows[static_cast<size_t>(i)].first);
      }
      if (out->ids.empty()) continue;
      return true;
    }
    if (down & HidNpadButton_B) return false;

    const int total_rows = n + 1;
    if (down & HidNpadButton_Up) {
      cursor = (cursor + total_rows - 1) % total_rows;
      dirty = true;
    }
    if (down & HidNpadButton_Down) {
      cursor = (cursor + 1) % total_rows;
      dirty = true;
    }
    if (down & HidNpadButton_A) {
      if (cursor == 0) {
        master_all = !master_all;
        const bool v = master_all;
        for (int i = 0; i < n; i++) picked[static_cast<size_t>(i)] = v;
      } else {
        picked[static_cast<size_t>(cursor - 1)] = !picked[static_cast<size_t>(cursor - 1)];
        master_all = true;
        for (int i = 0; i < n; i++) {
          if (!picked[static_cast<size_t>(i)]) master_all = false;
        }
      }
      dirty = true;
    }

    if (cursor < scroll) scroll = cursor;
    if (cursor >= scroll + kVisible) scroll = cursor - kVisible + 1;
    if (scroll < 0) scroll = 0;
    const int max_scroll = std::max(0, total_rows - kVisible);
    if (scroll > max_scroll) scroll = max_scroll;

    if (!dirty) {
      consoleUpdate(NULL);
      continue;
    }

    consoleClear();
    printf("Download: choose saves\n");
    printf("D-pad: move   A: toggle   +: run download   B: back to menu\n\n");
    for (int row = scroll; row < std::min(scroll + kVisible, total_rows); row++) {
      const char mark = (row == cursor) ? '>' : ' ';
      if (row == 0) {
        printf("%c [%c] ALL SAVES\n", mark, master_all ? 'x' : ' ');
      } else {
        const SaveMeta& R = rows[static_cast<size_t>(row - 1)].second;
        const char* disp = R.game_id.c_str();
        std::string disp_buf;
        if (!R.display_name.empty()) {
          disp_buf = R.display_name;
          disp = disp_buf.c_str();
        }
        printf(
            "%c [%c] %.28s\n",
            mark,
            picked[static_cast<size_t>(row - 1)] ? 'x' : ' ',
            disp);
      }
    }
    dirty = false;
    consoleUpdate(NULL);
  }
  return false;
}

static void save_viewer_switch(PadState* pad, Config& cfg) {
  auto locals = scan_all_local_saves(cfg);
  std::map<std::string, LocalSave> local_by_id;
  for (const auto& s : locals) local_by_id[s.game_id] = s;
  int status = 0;
  std::vector<unsigned char> body;
  if (!http_request(cfg, "GET", "/saves", {}, status, body) || status != 200) {
    consoleClear();
    printf("Save viewer: GET /saves failed");
    if (status > 0) printf(" (HTTP %d)", status);
    printf("\nB: back\n");
    consoleUpdate(NULL);
    while (appletMainLoop()) {
      padUpdate(pad);
      if (padGetButtonsDown(pad) & HidNpadButton_B) return;
      consoleUpdate(NULL);
    }
    return;
  }
  std::string json(body.begin(), body.end());
  const SavesParseResult pr = parse_saves_json(json);
  const auto& remote = pr.by_id;
  std::vector<std::string> merge_ids = build_merge_ids_filtered(cfg, local_by_id, remote, &pr.order);
  if (merge_ids.empty()) {
    consoleClear();
    printf("Save viewer: no saves (local or server).\n");
    printf("B: back\n");
    consoleUpdate(NULL);
    while (appletMainLoop()) {
      padUpdate(pad);
      if (padGetButtonsDown(pad) & HidNpadButton_B) return;
      consoleUpdate(NULL);
    }
    return;
  }
  int cursor = 0;
  int scroll = 0;
  constexpr int kVisible = 16;
  const int total_rows = static_cast<int>(merge_ids.size());
  bool dirty = true;
  while (appletMainLoop()) {
    padUpdate(pad);
    const u64 down = padGetButtonsDown(pad);
    if (down & HidNpadButton_B) return;
    if (down & HidNpadButton_A) {
      const std::string& gid = merge_ids[static_cast<size_t>(cursor)];
      (void)save_history_picker_switch(pad, cfg, gid);
      dirty = true;
    }
    if (down & HidNpadButton_R) {
      const std::string& gid = merge_ids[static_cast<size_t>(cursor)];
      const std::string lk = to_lower(gid);
      if (cfg.locked_ids.count(lk)) {
        cfg.locked_ids.erase(lk);
      } else {
        cfg.locked_ids.insert(lk);
      }
      (void)save_locked_ids_to_ini(kSwitchConfigIni, cfg.locked_ids);
      dirty = true;
    }
    if (down & HidNpadButton_Up) {
      cursor = (cursor + total_rows - 1) % total_rows;
      dirty = true;
    }
    if (down & HidNpadButton_Down) {
      cursor = (cursor + 1) % total_rows;
      dirty = true;
    }
    if (cursor < scroll) scroll = cursor;
    if (cursor >= scroll + kVisible) scroll = cursor - kVisible + 1;
    if (scroll < 0) scroll = 0;
    const int max_scroll = std::max(0, total_rows - kVisible);
    if (scroll > max_scroll) scroll = max_scroll;

    if (!dirty) {
      consoleUpdate(NULL);
      continue;
    }

    consoleClear();
    printf("--- Save viewer (lock for Auto) ---\n");
    printf("\n");
    {
      constexpr int kMenuLeftCol = 22;
      printf("%-*s%s\n", kMenuLeftCol, "UP/DOWN: move", "");
      printf("%-*s%s\n", kMenuLeftCol, "A: history / restore", "");
      printf("%-*s%s\n", kMenuLeftCol, "R: toggle lock -> config", "");
      printf("%-*s%s\n", kMenuLeftCol, "B: back", "");
    }
    printf("\n");
    for (int row = scroll; row < std::min(scroll + kVisible, total_rows); row++) {
      const char mark = (row == cursor) ? '>' : ' ';
      const std::string& id = merge_ids[static_cast<size_t>(row)];
      const std::string lk = to_lower(id);
      const char* tag = cfg.locked_ids.count(lk) ? "[L]" : "   ";
      const auto it = remote.find(id);
      const char* disp = id.c_str();
      std::string disp_buf;
      if (it != remote.end() && !it->second.display_name.empty()) {
        disp_buf = it->second.display_name;
        disp = disp_buf.c_str();
      }
      printf("%c%s %.28s\n", mark, tag, disp);
      printf("\n");
    }
    dirty = false;
    consoleUpdate(NULL);
  }
}

static std::vector<std::string> run_sync(
    Config& cfg,
    SyncAction action,
    const SyncManualFilter* xy_filter,
    PadState* pad,
    bool* out_exit_after_sync) {
  std::vector<std::string> logs;
  if (out_exit_after_sync) *out_exit_after_sync = false;
  if (action == SyncAction::Auto) {
    printf("\n");
    printf("Scanning local saves...\n");
    consoleUpdate(NULL);
  }
  auto local = scan_all_local_saves(cfg);
  std::map<std::string, LocalSave> local_by_id;
  for (const auto& s : local) local_by_id[s.game_id] = s;
  if (action != SyncAction::Auto) {
    logs.push_back("Local saves: " + std::to_string(local.size()));
  }

  std::vector<BaselineRow> baseline = baseline_load_merged(cfg);

  int status = 0;
  std::vector<unsigned char> body;
  if (!http_request(cfg, "GET", "/saves", {}, status, body) || status != 200) {
    if (status == 401) {
      logs.push_back("ERROR: GET /saves unauthorized (check api_key)");
    } else if (status > 0) {
      logs.push_back("ERROR: GET /saves failed (HTTP " + std::to_string(status) + ")");
    } else {
      logs.push_back("ERROR: GET /saves failed (network/connect)");
    }
    sync_status_after_server_work(cfg, false, false, "GET /saves");
    return logs;
  }
  std::string json(body.begin(), body.end());
  const SavesParseResult pr = parse_saves_json(json);
  const auto& remote = pr.by_id;
  if (action != SyncAction::Auto) {
    logs.push_back("Remote saves: " + std::to_string(remote.size()));
  }

  const std::string plat = "switch-homebrew";

  if (action == SyncAction::UploadOnly) {
    for (const auto& entry : local_by_id) {
      if (is_skipped_merge_id(cfg, entry.first)) continue;
      if (xy_filter && !xy_filter->all && !xy_filter->ids.count(entry.first)) continue;
      std::string uploaded_sha;
      if (put_save_log(cfg, entry.second, true, plat, logs, &uploaded_sha))
        baseline_upsert(baseline, entry.first, uploaded_sha);
    }
    if (!baseline_save_merged(cfg, baseline, local)) logs.push_back("WARN: could not write .gbasync-baseline");
    sync_status_after_server_work(cfg, true, true, "");
    return logs;
  }

  if (action == SyncAction::DownloadOnly) {
    std::set<std::string> dl_seen;
    for (const auto& id : pr.order) {
      const auto it = remote.find(id);
      if (it == remote.end()) continue;
      if (is_skipped_merge_id(cfg, id)) continue;
      if (should_skip_remote_for_nds_policy(cfg, it->second)) continue;
      if (xy_filter && !xy_filter->all && !xy_filter->ids.count(id)) continue;
      if (get_save_log(cfg, id, it->second, logs, local)) baseline_upsert(baseline, id, it->second.sha256);
      dl_seen.insert(id);
    }
    for (const auto& [id, r] : remote) {
      if (dl_seen.count(id)) continue;
      if (is_skipped_merge_id(cfg, id)) continue;
      if (should_skip_remote_for_nds_policy(cfg, r)) continue;
      if (xy_filter && !xy_filter->all && !xy_filter->ids.count(id)) continue;
      if (get_save_log(cfg, id, r, logs, local)) baseline_upsert(baseline, id, r.sha256);
    }
    if (!baseline_save_merged(cfg, baseline, local)) logs.push_back("WARN: could not write .gbasync-baseline");
    sync_status_after_server_work(cfg, true, true, "");
    return logs;
  }

  /* Auto: hash + .gbasync-baseline (legacy .savesync-baseline still read). */
  debug_report_sync_start_switch(cfg, local);
  std::vector<std::string> merge_ids = build_merge_ids_filtered(cfg, local_by_id, remote, &pr.order);

  std::vector<AutoPlanRow> plan;
  build_auto_plan_vector(cfg, merge_ids, local_by_id, remote, baseline, plan);
  int nu = 0, nd = 0, ns = 0, nc = 0, nl = 0, nk = 0;
  recount_auto_plan(plan, &nu, &nd, &ns, &nc, &nl, &nk);
  /* All rows OK (in sync). Not nu+nd+ns+nc==0 — that matches "all locked" too. */
  if (nk == static_cast<int>(plan.size())) {
    for (const std::string& id : merge_ids) {
      if (cfg.locked_ids.count(to_lower(id)) > 0) continue;
      auto lit = local_by_id.find(id);
      auto rit = remote.find(id);
      if (lit != local_by_id.end() && rit != remote.end() && lit->second.sha256 == rit->second.sha256) {
        baseline_upsert(baseline, id, lit->second.sha256);
      }
    }
    if (!baseline_save_merged(cfg, baseline, local)) logs.push_back("WARN: could not write .gbasync-baseline");
    sync_status_after_server_work(cfg, true, true, "");
    logs.push_back("");
    logs.push_back("Already Up To Date");
    return logs;
  }

  if (!pad) {
    logs.push_back("Preview cancelled.");
    return logs;
  }
  {
    const int pv = preview_auto_plan(pad, cfg, merge_ids, local_by_id, remote, baseline, plan);
    if (pv == 0) {
      logs.push_back("Preview cancelled.");
      return logs;
    }
    if (pv == 2 && out_exit_after_sync) *out_exit_after_sync = true;
  }

  /* Blank line before per-game apply lines (matches 3DS printf("\n") after confirm). */
  logs.push_back("");

  for (const std::string& id : merge_ids) {
    auto lit = local_by_id.find(id);
    auto rit = remote.find(id);
    const bool has_l = lit != local_by_id.end();
    const bool has_r = rit != remote.end();
    if (cfg.locked_ids.count(to_lower(id)) > 0) {
      logs.push_back(id + ": SKIP (locked on this device)");
      continue;
    }

    if (has_l && has_r) {
      const LocalSave& l = lit->second;
      const SaveMeta& r = rit->second;
      if (l.sha256 == r.sha256) {
        baseline_upsert(baseline, id, l.sha256);
        continue;
      }
      std::string base_sha;
      if (!baseline_get_sha(baseline, id, &base_sha)) {
        if (pad) {
          resolve_both_changed_conflict_switch(pad, cfg, id, l, r, plat, local, logs, baseline, true);
        } else {
          logs.push_back(id + ": SKIP (no baseline — need interactive resolution)");
        }
        continue;
      }
      const bool loc_eq = (strcasecmp(l.sha256.c_str(), base_sha.c_str()) == 0);
      const bool rem_eq = (strcasecmp(r.sha256.c_str(), base_sha.c_str()) == 0);
      if (loc_eq && !rem_eq) {
        if (get_save_log(cfg, id, r, logs, local)) baseline_upsert(baseline, id, r.sha256);
      } else if (!loc_eq && rem_eq) {
        std::string uploaded_sha;
        if (put_save_log(cfg, l, false, plat, logs, &uploaded_sha)) baseline_upsert(baseline, id, uploaded_sha);
      } else if (!loc_eq && !rem_eq) {
        if (pad) {
          resolve_both_changed_conflict_switch(pad, cfg, id, l, r, plat, local, logs, baseline, false);
        } else {
          logs.push_back(id + ": SKIP (both changed — need interactive resolution)");
        }
      }
    } else if (has_l && !has_r) {
      std::string uploaded_sha;
      if (put_save_log(cfg, lit->second, false, plat, logs, &uploaded_sha))
        baseline_upsert(baseline, id, uploaded_sha);
    } else if (!has_l && has_r) {
      if (get_save_log(cfg, id, rit->second, logs, local)) baseline_upsert(baseline, id, rit->second.sha256);
    }
  }

  if (!baseline_save_merged(cfg, baseline, local)) logs.push_back("WARN: could not write .gbasync-baseline");
  sync_status_after_server_work(cfg, true, true, "");
  return logs;
}

static std::vector<std::string> run_dropbox_sync_once(const Config& cfg) {
  std::vector<std::string> logs;
  int status = 0;
  std::vector<unsigned char> body;
  std::vector<unsigned char> resp;
  if (!http_request(cfg, "POST", "/dropbox/sync-once", body, status, resp, "application/json")) {
    logs.push_back("Dropbox sync request: ERROR(request)");
    sync_status_after_dropbox_only(cfg, false);
    return logs;
  }
  if (status == 200) {
    logs.push_back("Dropbox sync request: OK");
    sync_status_after_dropbox_only(cfg, true);
  } else {
    logs.push_back("Dropbox sync request: HTTP " + std::to_string(status));
    sync_status_after_dropbox_only(cfg, false);
  }
  return logs;
}

static bool choose_action(PadState* pad, SyncAction* out_action) {
  while (appletMainLoop()) {
    padUpdate(pad);
    const u64 down = padGetButtonsDown(pad);
    if (down & HidNpadButton_A) {
      *out_action = SyncAction::Auto;
      return true;
    }
    if (down & HidNpadButton_X) {
      *out_action = SyncAction::UploadOnly;
      return true;
    }
    if (down & HidNpadButton_Y) {
      *out_action = SyncAction::DownloadOnly;
      return true;
    }
    if (down & HidNpadButton_Minus) {
      *out_action = SyncAction::DropboxSync;
      return true;
    }
    if (down & HidNpadButton_R) {
      *out_action = SyncAction::SaveViewer;
      return true;
    }
    if (down & HidNpadButton_Plus) {
      return false;
    }
    consoleUpdate(NULL);
  }
  return false;
}

/** -1 on error; else games needing Auto (not OK, not locked). */
static int count_pending_auto_sync_switch(const Config& cfg) {
  auto local = scan_all_local_saves(cfg);
  std::map<std::string, LocalSave> local_by_id;
  for (const auto& s : local) local_by_id[s.game_id] = s;
  std::vector<BaselineRow> baseline = baseline_load_merged(cfg);
  int status = 0;
  std::vector<unsigned char> body;
  if (!http_request(cfg, "GET", "/saves", {}, status, body) || status != 200) return -1;
  const SavesParseResult pr = parse_saves_json(std::string(body.begin(), body.end()));
  const auto& remote = pr.by_id;
  std::vector<std::string> merge_ids = build_merge_ids_filtered(cfg, local_by_id, remote, &pr.order);
  std::vector<AutoPlanRow> plan;
  build_auto_plan_vector(cfg, merge_ids, local_by_id, remote, baseline, plan);
  int n = 0;
  for (const auto& row : plan) {
    if (row.kind != AutoPlanKind::Ok && row.kind != AutoPlanKind::Locked) n++;
  }
  return n;
}

static void wait_after_sync_switch(PadState* pad, bool* quit_app) {
  printf("\nA: main menu   +: exit app\n");
  while (appletMainLoop()) {
    padUpdate(pad);
    const u64 down = padGetButtonsDown(pad);
    if (down & HidNpadButton_A) return;
    if (down & HidNpadButton_Plus) {
      *quit_app = true;
      return;
    }
    consoleUpdate(NULL);
  }
  *quit_app = true;
}

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  consoleInit(NULL);
  const Result sock_rc = socketInitializeDefault();
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  PadState pad;
  padInitializeDefault(&pad);

  Config cfg = load_config("sdmc:/switch/gba-sync/config.ini");

  std::vector<std::string> logs;
  bool quit_app = false;
  if (R_FAILED(sock_rc)) {
    logs.push_back("ERROR: socket init failed");
  } else if (cfg.server_url.empty()) {
    logs.push_back("ERROR: missing [server].url in config.ini");
  } else if (cfg.server_url.rfind("http://", 0) != 0) {
    logs.push_back("ERROR: use http:// URL for Switch MVP");
  } else {
    /* -1 = stale (recompute). Avoids full local scan + GET /saves on every menu draw. */
    int cached_pending_sync_badge = -1;
    while (appletMainLoop() && !quit_app) {
      consoleClear();
      printf("GBAsync (Switch)\n");
      printf("-----------------\n");
      printf("Server: %s\n", cfg.server_url.c_str());
      {
        auto roots = build_save_roots(cfg);
        if (roots.empty()) {
          printf("Save dir: %s\n", cfg.save_dir.c_str());
        } else if (roots.size() == 1U) {
          printf("Save dir: %s\n", roots[0].save_dir.c_str());
        } else {
          printf("Save dirs (%zu):\n", roots.size());
          for (const auto& r : roots) {
            const char* tag = r.kind == SaveRootKind::Gba ? "GBA" : (r.kind == SaveRootKind::Nds ? "NDS" : "GB");
            printf("  [%s] %s\n", tag, r.save_dir.c_str());
          }
        }
      }
      printf("\n");
      sync_status_print_menu(cfg);
      {
        if (cached_pending_sync_badge < 0) {
          cached_pending_sync_badge = count_pending_auto_sync_switch(cfg);
        }
        if (cached_pending_sync_badge > 0) {
          printf("  %d game(s) need sync (run Auto)\n\n", cached_pending_sync_badge);
        }
      }
      {
        constexpr int kMenuLeftCol = 20;
        printf("%-*s%s\n", kMenuLeftCol, "A: Auto sync", "R: save viewer");
        printf("%-*s%s\n", kMenuLeftCol, "X: upload only", "-: Dropbox sync");
        printf("%-*s%s\n", kMenuLeftCol, "Y: download only", "+: exit app");
      }
      printf("\n");

      SyncAction action = SyncAction::Auto;
      if (!choose_action(&pad, &action)) {
        quit_app = true;
        break;
      }

      SyncManualFilter xy{};
      xy.all = true;
      std::vector<std::string> sync_logs;
      if (action == SyncAction::DropboxSync) {
        printf("\nDropbox sync now...\n");
        consoleUpdate(NULL);
        sync_logs = run_dropbox_sync_once(cfg);
      } else if (action == SyncAction::SaveViewer) {
        save_viewer_switch(&pad, cfg);
        cached_pending_sync_badge = -1;
        continue;
      } else if (action == SyncAction::UploadOnly) {
        if (!pick_upload_selection(&pad, cfg, &xy)) continue;
        sync_logs = run_sync(cfg, action, &xy, &pad, nullptr);
      } else if (action == SyncAction::DownloadOnly) {
        if (!pick_download_selection(&pad, cfg, &xy)) continue;
        sync_logs = run_sync(cfg, action, &xy, &pad, nullptr);
      } else {
        bool exit_after = false;
        sync_logs = run_sync(cfg, action, nullptr, &pad, &exit_after);
        for (const auto& line : sync_logs) printf("%s\n", line.c_str());
        if (exit_after) {
          quit_app = true;
        } else {
          wait_after_sync_switch(&pad, &quit_app);
        }
        cached_pending_sync_badge = -1;
        continue;
      }
      for (const auto& line : sync_logs) printf("%s\n", line.c_str());
      wait_after_sync_switch(&pad, &quit_app);
      cached_pending_sync_badge = -1;
    }
  }
  for (const auto& line : logs) printf("%s\n", line.c_str());
  if (!quit_app) {
    printf("\nPress + to exit.\n");

    while (appletMainLoop()) {
      padUpdate(&pad);
      if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
      consoleUpdate(NULL);
    }
  }

  socketExit();
  consoleExit(NULL);
  return 0;
}
