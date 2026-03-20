#include <3ds.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <malloc.h>
#include <netdb.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct {
  char server_url[256];
  char api_key[128];
  char save_dir[256];
  char sync_mode[16];
  char vc_save_dir[256];
  char rom_dir[256];
  char rom_extension[16];
} AppConfig;

typedef struct {
  char game_id[128];
  char last_modified_utc[40];
  char server_updated_at[40];
  char sha256[65];
  char filename_hint[128];
} RemoteSave;

typedef struct {
  char path[512];
  char name[256];
  char game_id[128];
  char last_modified_utc[40];
  char sha256[65];
  size_t size_bytes;
} LocalSave;

typedef struct {
  char game_id[128];
  char sha256[65];
} BaselineRow;

#define MAX_SAVES 256
#define SOC_BUFFERSIZE (0x100000)

typedef enum {
  SYNC_ACTION_AUTO = 0,
  SYNC_ACTION_UPLOAD_ONLY = 1,
  SYNC_ACTION_DOWNLOAD_ONLY = 2,
} SyncAction;

typedef struct {
  int all;
  int n_ids;
  char (*ids)[128];
} SyncManualFilter;

static void ensure_directory_exists(const char* dir);

static void manual_filter_free(SyncManualFilter* f) {
  if (!f) return;
  free(f->ids);
  f->ids = NULL;
  f->n_ids = 0;
}

static bool id_in_manual(const SyncManualFilter* f, const char* id) {
  if (!f || f->all) return true;
  if (!f->ids || f->n_ids <= 0) return false;
  for (int i = 0; i < f->n_ids; i++) {
    if (strcmp(f->ids[i], id) == 0) return true;
  }
  return false;
}

static void sort_remotes(RemoteSave* r, int n) {
  for (int i = 0; i < n - 1; i++) {
    for (int j = i + 1; j < n; j++) {
      if (strcmp(r[i].game_id, r[j].game_id) > 0) {
        RemoteSave t = r[i];
        r[i] = r[j];
        r[j] = t;
      }
    }
  }
}

static void copy_cstr(char* dst, size_t dst_size, const char* src) {
  if (!dst || dst_size == 0) return;
  if (!src) {
    dst[0] = '\0';
    return;
  }
  size_t i = 0;
  while (i + 1 < dst_size && src[i] != '\0') {
    dst[i] = src[i];
    i++;
  }
  dst[i] = '\0';
}

static void config_init(AppConfig* cfg) {
  memset(cfg, 0, sizeof(*cfg));
  copy_cstr(cfg->save_dir, sizeof(cfg->save_dir), "sdmc:/saves");
  copy_cstr(cfg->sync_mode, sizeof(cfg->sync_mode), "normal");
  copy_cstr(cfg->vc_save_dir, sizeof(cfg->vc_save_dir), "sdmc:/3ds/Checkpoint/saves");
  copy_cstr(cfg->rom_extension, sizeof(cfg->rom_extension), ".gba");
}

static void trim_line(char* s) {
  while (*s == ' ' || *s == '\t') {
    memmove(s, s + 1, strlen(s));
  }
  size_t len = strlen(s);
  while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r' || s[len - 1] == ' ' || s[len - 1] == '\t')) {
    s[len - 1] = '\0';
    len--;
  }
}

static void load_config(AppConfig* cfg, const char* path) {
  FILE* fp = fopen(path, "r");
  if (!fp) return;

  char section[32] = {0};
  char line[512];
  while (fgets(line, sizeof(line), fp)) {
    trim_line(line);
    if (line[0] == '\0' || line[0] == '#' || line[0] == ';') continue;
    if (line[0] == '[') {
      char* end = strchr(line, ']');
      if (end) {
        *end = '\0';
        copy_cstr(section, sizeof(section), line + 1);
      }
      continue;
    }
    char* eq = strchr(line, '=');
    if (!eq) continue;
    *eq = '\0';
    char* key = line;
    char* value = eq + 1;
    trim_line(key);
    trim_line(value);

    if (strcmp(section, "server") == 0 && strcmp(key, "url") == 0) {
      copy_cstr(cfg->server_url, sizeof(cfg->server_url), value);
    } else if (strcmp(section, "server") == 0 && strcmp(key, "api_key") == 0) {
      copy_cstr(cfg->api_key, sizeof(cfg->api_key), value);
    } else if (strcmp(section, "sync") == 0 && strcmp(key, "mode") == 0) {
      copy_cstr(cfg->sync_mode, sizeof(cfg->sync_mode), value);
    } else if (strcmp(section, "sync") == 0 && strcmp(key, "save_dir") == 0) {
      copy_cstr(cfg->save_dir, sizeof(cfg->save_dir), value);
    } else if (strcmp(section, "sync") == 0 && strcmp(key, "vc_save_dir") == 0) {
      copy_cstr(cfg->vc_save_dir, sizeof(cfg->vc_save_dir), value);
    } else if (strcmp(section, "rom") == 0 && strcmp(key, "rom_dir") == 0) {
      copy_cstr(cfg->rom_dir, sizeof(cfg->rom_dir), value);
    } else if (strcmp(section, "rom") == 0 && strcmp(key, "rom_extension") == 0) {
      copy_cstr(cfg->rom_extension, sizeof(cfg->rom_extension), value);
    }
  }

  fclose(fp);
}

static bool has_sav_extension(const char* name) {
  size_t len = strlen(name);
  if (len < 4) return false;
  return strcasecmp(name + len - 4, ".sav") == 0;
}

static bool is_vc_mode(const AppConfig* cfg) {
  return strcasecmp(cfg->sync_mode, "vc") == 0;
}

static const char* active_save_dir(const AppConfig* cfg) {
  return is_vc_mode(cfg) ? cfg->vc_save_dir : cfg->save_dir;
}

static void sanitize_game_id(const char* in, char* out, size_t out_size) {
  size_t j = 0;
  for (size_t i = 0; in[i] != '\0' && j + 1 < out_size; i++) {
    char c = (char)tolower((unsigned char)in[i]);
    if (isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.') {
      out[j++] = c;
    } else {
      out[j++] = '-';
    }
  }
  out[j] = '\0';
  if (out[0] == '\0') {
    copy_cstr(out, out_size, "unknown-game");
  }
}

static void mtime_to_utc_iso(time_t mtime, char* out, size_t out_size) {
  struct tm tm_utc;
  gmtime_r(&mtime, &tm_utc);
  strftime(out, out_size, "%Y-%m-%dT%H:%M:%S+00:00", &tm_utc);
}

static bool read_file_bytes(const char* path, unsigned char** out, size_t* out_len) {
  FILE* fp = fopen(path, "rb");
  if (!fp) return false;
  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    return false;
  }
  long size = ftell(fp);
  if (size < 0) {
    fclose(fp);
    return false;
  }
  rewind(fp);
  unsigned char* buf = (unsigned char*)malloc((size_t)size);
  if (!buf && size > 0) {
    fclose(fp);
    return false;
  }
  if (size > 0 && fread(buf, 1, (size_t)size, fp) != (size_t)size) {
    free(buf);
    fclose(fp);
    return false;
  }
  fclose(fp);
  *out = buf;
  *out_len = (size_t)size;
  return true;
}

static void decode_header_field(const unsigned char* src, size_t len, char* out, size_t out_size) {
  size_t j = 0;
  for (size_t i = 0; i < len && j + 1 < out_size; i++) {
    char c = (char)src[i];
    if (c == '\0') break;
    if (isprint((unsigned char)c)) out[j++] = c;
  }
  out[j] = '\0';
}

static bool game_id_from_rom_header_bytes(const unsigned char* data, size_t len, char* out, size_t out_size) {
  if (!data || len < 0xB0) return false;
  char title[32] = {0};
  char code[8] = {0};
  char combined[64] = {0};
  decode_header_field(data + 0xA0, 12, title, sizeof(title));
  decode_header_field(data + 0xAC, 4, code, sizeof(code));
  trim_line(title);
  trim_line(code);
  if (title[0] == '\0' && code[0] == '\0') return false;
  if (code[0] == '\0') {
    copy_cstr(combined, sizeof(combined), title);
  } else {
    snprintf(combined, sizeof(combined), "%s-%s", title, code);
  }
  sanitize_game_id(combined, out, out_size);
  return out[0] != '\0';
}

static void resolve_game_id_for_save(const AppConfig* cfg, const char* save_stem, char* out, size_t out_size) {
  if (cfg->rom_dir[0] != '\0') {
    char ext[16];
    copy_cstr(ext, sizeof(ext), cfg->rom_extension[0] ? cfg->rom_extension : ".gba");
    if (ext[0] != '.') {
      char tmp[16] = {0};
      tmp[0] = '.';
      copy_cstr(tmp + 1, sizeof(tmp) - 1, ext);
      copy_cstr(ext, sizeof(ext), tmp);
    }
    char rom_path[640];
    snprintf(rom_path, sizeof(rom_path), "%s/%s%s", cfg->rom_dir, save_stem, ext);
    unsigned char* rom_bytes = NULL;
    size_t rom_len = 0;
    if (read_file_bytes(rom_path, &rom_bytes, &rom_len)) {
      if (game_id_from_rom_header_bytes(rom_bytes, rom_len, out, out_size)) {
        free(rom_bytes);
        return;
      }
      free(rom_bytes);
    }
  }
  sanitize_game_id(save_stem, out, out_size);
}

static bool write_atomic_file(const char* path, const unsigned char* data, size_t len) {
  char parent_dir[512];
  copy_cstr(parent_dir, sizeof(parent_dir), path);
  char* slash = strrchr(parent_dir, '/');
  if (slash) {
    *slash = '\0';
    ensure_directory_exists(parent_dir);
  }

  char tmp_path[600];
  unsigned long long tick = osGetTime();
  if (slash && parent_dir[0] != '\0') {
    snprintf(tmp_path, sizeof(tmp_path), "%s/.savesync-%llu.tmp", parent_dir, tick);
  } else {
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
  }
  FILE* fp = fopen(tmp_path, "wb");
  if (!fp) {
    return false;
  }
  if (len > 0 && fwrite(data, 1, len, fp) != len) {
    fclose(fp);
    remove(tmp_path);
    return false;
  }
  fclose(fp);

  // Some 3DS SD/FAT setups fail rename() replacement even when both paths exist.
  // If destination already exists, write directly and keep temp as a rollback source.
  struct stat dst_st;
  bool dst_exists = (stat(path, &dst_st) == 0);
  if (dst_exists) {
    FILE* direct = fopen(path, "wb");
    if (!direct) {
      remove(tmp_path);
      return false;
    }
    if (len > 0 && fwrite(data, 1, len, direct) != len) {
      fclose(direct);
      remove(tmp_path);
      return false;
    }
    fclose(direct);
    remove(tmp_path);
    return true;
  }

  if (rename(tmp_path, path) != 0) {
    remove(path);
    if (rename(tmp_path, path) == 0) return true;

    // Fallback path for filesystems/locks where rename replacement fails.
    FILE* direct = fopen(path, "wb");
    if (!direct) {
      remove(tmp_path);
      return false;
    }
    if (len > 0 && fwrite(data, 1, len, direct) != len) {
      fclose(direct);
      remove(tmp_path);
      return false;
    }
    fclose(direct);
    remove(tmp_path);
    return true;
  }
  return true;
}

static void join_path(char* out, size_t out_size, const char* dir, const char* name) {
  size_t len = strlen(dir);
  bool has_trailing_slash = len > 0 && dir[len - 1] == '/';
  snprintf(out, out_size, "%s%s%s", dir, has_trailing_slash ? "" : "/", name);
}

static void sanitize_filename(char* out, size_t out_size, const char* in, const char* fallback) {
  if (!in || in[0] == '\0') {
    copy_cstr(out, out_size, fallback);
    return;
  }
  size_t j = 0;
  for (size_t i = 0; in[i] != '\0' && j + 1 < out_size; i++) {
    char c = in[i];
    if (c == '/' || c == '\\' || c == ':') continue;
    out[j++] = c;
  }
  out[j] = '\0';
  if (out[0] == '\0') copy_cstr(out, out_size, fallback);
}

static void ensure_directory_exists(const char* dir) {
  struct stat st;
  if (stat(dir, &st) == 0 && S_ISDIR(st.st_mode)) {
    return;
  }
  (void)mkdir(dir, 0777);
}

static void baseline_file_path(char* out, size_t out_sz, const char* save_dir) {
  join_path(out, out_sz, save_dir, ".savesync-baseline");
}

static int baseline_load(const char* save_dir, BaselineRow* rows, int max_rows) {
  char path[512];
  baseline_file_path(path, sizeof(path), save_dir);
  FILE* fp = fopen(path, "r");
  if (!fp) return 0;
  int n = 0;
  char line[400];
  while (n < max_rows && fgets(line, sizeof(line), fp)) {
    char* tab = strchr(line, '\t');
    if (!tab) continue;
    *tab = '\0';
    const char* sha = tab + 1;
    size_t shalen = strlen(sha);
    while (shalen > 0 && (sha[shalen - 1] == '\n' || sha[shalen - 1] == '\r')) shalen--;
    if (shalen != 64) continue;
    if (line[0] == '\0') continue;
    copy_cstr(rows[n].game_id, sizeof(rows[n].game_id), line);
    memcpy(rows[n].sha256, sha, 64);
    rows[n].sha256[64] = '\0';
    n++;
  }
  fclose(fp);
  return n;
}

static bool baseline_save(const char* save_dir, BaselineRow* rows, int n) {
  char path[512], tmp[520];
  baseline_file_path(path, sizeof(path), save_dir);
  if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) return false;
  FILE* fp = fopen(tmp, "w");
  if (!fp) return false;
  for (int i = 0; i < n; i++) {
    fprintf(fp, "%s\t%s\n", rows[i].game_id, rows[i].sha256);
  }
  fclose(fp);
  remove(path);
  if (rename(tmp, path) != 0) {
    remove(tmp);
    return false;
  }
  return true;
}

static const char* baseline_find(const BaselineRow* rows, int n, const char* game_id) {
  for (int i = 0; i < n; i++) {
    if (strcmp(rows[i].game_id, game_id) == 0) return rows[i].sha256;
  }
  return NULL;
}

static void baseline_upsert(BaselineRow* rows, int* n, int max_n, const char* game_id, const char* sha256) {
  for (int i = 0; i < *n; i++) {
    if (strcmp(rows[i].game_id, game_id) == 0) {
      copy_cstr(rows[i].sha256, sizeof(rows[i].sha256), sha256);
      return;
    }
  }
  if (*n < max_n) {
    copy_cstr(rows[*n].game_id, sizeof(rows[*n].game_id), game_id);
    copy_cstr(rows[*n].sha256, sizeof(rows[*n].sha256), sha256);
    (*n)++;
  }
}

static void to_hex32(unsigned int value, char* out) {
  static const char* hex = "0123456789abcdef";
  for (int i = 7; i >= 0; i--) {
    out[i] = hex[value & 0x0fU];
    value >>= 4;
  }
}

static void sha256_hash(const unsigned char* data, size_t len, char out_hex[65]) {
  static const unsigned int k[64] = {
      0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
      0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
      0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
      0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
      0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
      0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
      0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
      0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};
  unsigned int h[8] = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                       0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
  size_t new_len = len + 1;
  while ((new_len % 64) != 56) new_len++;
  unsigned char* msg = (unsigned char*)calloc(new_len + 8, 1);
  if (!msg) {
    memset(out_hex, 0, 65);
    return;
  }
  memcpy(msg, data, len);
  msg[len] = 0x80;
  unsigned long long bit_len = (unsigned long long)len * 8ULL;
  for (int i = 0; i < 8; i++) msg[new_len + i] = (unsigned char)((bit_len >> ((7 - i) * 8)) & 0xffU);

  for (size_t offset = 0; offset < new_len + 8; offset += 64) {
    unsigned int w[64] = {0};
    for (int i = 0; i < 16; i++) {
      size_t j = offset + (size_t)i * 4;
      w[i] = ((unsigned int)msg[j] << 24) | ((unsigned int)msg[j + 1] << 16) |
             ((unsigned int)msg[j + 2] << 8) | (unsigned int)msg[j + 3];
    }
    for (int i = 16; i < 64; i++) {
      unsigned int s0 = ((w[i - 15] >> 7) | (w[i - 15] << 25)) ^ ((w[i - 15] >> 18) | (w[i - 15] << 14)) ^ (w[i - 15] >> 3);
      unsigned int s1 = ((w[i - 2] >> 17) | (w[i - 2] << 15)) ^ ((w[i - 2] >> 19) | (w[i - 2] << 13)) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    unsigned int a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
      unsigned int S1 = ((e >> 6) | (e << 26)) ^ ((e >> 11) | (e << 21)) ^ ((e >> 25) | (e << 7));
      unsigned int ch = (e & f) ^ ((~e) & g);
      unsigned int temp1 = hh + S1 + ch + k[i] + w[i];
      unsigned int S0 = ((a >> 2) | (a << 30)) ^ ((a >> 13) | (a << 19)) ^ ((a >> 22) | (a << 10));
      unsigned int maj = (a & b) ^ (a & c) ^ (b & c);
      unsigned int temp2 = S0 + maj;
      hh = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
  }
  free(msg);
  for (int i = 0; i < 8; i++) to_hex32(h[i], out_hex + i * 8);
  out_hex[64] = '\0';
}

typedef struct {
  char host[128];
  int port;
} ParsedUrl;

static bool parse_server_url(const char* url, ParsedUrl* out) {
  if (strncmp(url, "http://", 7) != 0) return false;
  const char* p = url + 7;
  const char* slash = strchr(p, '/');
  char hostport[180];
  if (slash) {
    size_t n = (size_t)(slash - p);
    if (n >= sizeof(hostport)) return false;
    memcpy(hostport, p, n);
    hostport[n] = '\0';
  } else {
    copy_cstr(hostport, sizeof(hostport), p);
  }
  char* colon = strchr(hostport, ':');
  if (colon) {
    *colon = '\0';
    out->port = atoi(colon + 1);
  } else {
    out->port = 80;
  }
  if (strnlen(hostport, sizeof(out->host)) >= sizeof(out->host)) return false;
  copy_cstr(out->host, sizeof(out->host), hostport);
  return out->host[0] != '\0';
}

static void url_encode_simple(const char* in, char* out, size_t out_sz) {
  static const char* hex = "0123456789ABCDEF";
  size_t j = 0;
  for (size_t i = 0; in[i] != '\0' && j + 4 < out_sz; i++) {
    unsigned char c = (unsigned char)in[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out[j++] = (char)c;
    } else {
      out[j++] = '%';
      out[j++] = hex[(c >> 4) & 0x0f];
      out[j++] = hex[c & 0x0f];
    }
  }
  out[j] = '\0';
}

static bool json_ws(unsigned char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* Exact JSON key "applied" (not applied_*); allows arbitrary whitespace before ':'. */
static bool json_body_has_applied_member(const unsigned char* body, size_t n) {
  static const char key[] = "\"applied\"";
  const size_t kl = sizeof(key) - 1;
  for (size_t i = 0; i + kl <= n; i++) {
    if (memcmp(body + i, key, kl) != 0) continue;
    if (i + kl < n && (isalnum((unsigned char)body[i + kl]) || body[i + kl] == '_')) continue;
    size_t j = i + kl;
    while (j < n && json_ws(body[j])) j++;
    if (j < n && body[j] == ':') return true;
  }
  return false;
}

static bool json_bool_after_colon(
    const unsigned char* body, size_t n, size_t* scan_from, const char* word) {
  size_t j = *scan_from;
  while (j < n && json_ws(body[j])) j++;
  size_t wlen = strlen(word);
  if (j + wlen > n || memcmp(body + j, word, wlen) != 0) return false;
  j += wlen;
  if (j < n && !json_ws(body[j]) && body[j] != ',' && body[j] != '}' && body[j] != ']') return false;
  *scan_from = j;
  return true;
}

static bool json_body_applied_is_true(const unsigned char* body, size_t n) {
  static const char key[] = "\"applied\"";
  const size_t kl = sizeof(key) - 1;
  for (size_t i = 0; i + kl <= n; i++) {
    if (memcmp(body + i, key, kl) != 0) continue;
    if (i + kl < n && (isalnum((unsigned char)body[i + kl]) || body[i + kl] == '_')) continue;
    size_t j = i + kl;
    while (j < n && json_ws(body[j])) j++;
    if (j >= n || body[j] != ':') continue;
    j++;
    if (json_bool_after_colon(body, n, &j, "true")) return true;
  }
  return false;
}

static bool json_body_applied_is_false(const unsigned char* body, size_t n) {
  static const char key[] = "\"applied\"";
  const size_t kl = sizeof(key) - 1;
  for (size_t i = 0; i + kl <= n; i++) {
    if (memcmp(body + i, key, kl) != 0) continue;
    if (i + kl < n && (isalnum((unsigned char)body[i + kl]) || body[i + kl] == '_')) continue;
    size_t j = i + kl;
    while (j < n && json_ws(body[j])) j++;
    if (j >= n || body[j] != ':') continue;
    j++;
    if (json_bool_after_colon(body, n, &j, "false")) return true;
  }
  return false;
}

static bool http_headers_have_chunked(const char* hdr) {
  const char* p = hdr;
  for (;;) {
    const char* eol = strstr(p, "\r\n");
    if (!eol) return false;
    if ((size_t)(eol - p) >= 18 && strncasecmp(p, "transfer-encoding:", 18) == 0) {
      const char* v = p + 18;
      while (v < eol && (*v == ' ' || *v == '\t')) v++;
      for (; v + 7 <= eol; v++) {
        if (strncasecmp(v, "chunked", 7) == 0) return true;
      }
    }
    p = eol + 2;
    if (*p == '\0') break;
  }
  return false;
}

static long http_content_length_header(const char* hdr) {
  const char* p = hdr;
  for (;;) {
    const char* eol = strstr(p, "\r\n");
    if (!eol) return -1;
    if ((size_t)(eol - p) >= 15 && strncasecmp(p, "content-length:", 15) == 0) {
      const char* v = p + 15;
      while (v < eol && (*v == ' ' || *v == '\t')) v++;
      char tmp[40];
      size_t n = (size_t)(eol - v);
      if (n >= sizeof(tmp)) n = sizeof(tmp) - 1;
      memcpy(tmp, v, n);
      tmp[n] = '\0';
      return strtol(tmp, NULL, 10);
    }
    p = eol + 2;
    if (*p == '\0') break;
  }
  return -1;
}

/* RFC 9112 chunked body after the header terminator; output is NUL-terminated for JSON scans. */
static unsigned char* http_decode_chunked(const unsigned char* in, size_t in_len, size_t* out_len) {
  size_t out_cap = (in_len > 256 ? in_len : 256) + 1;
  unsigned char* out = (unsigned char*)malloc(out_cap);
  if (!out) return NULL;
  size_t o = 0;
  size_t pos = 0;
  while (pos < in_len) {
    size_t line0 = pos;
    while (pos + 1 < in_len && !(in[pos] == '\r' && in[pos + 1] == '\n')) pos++;
    if (pos + 1 >= in_len) goto fail;
    char linebuf[64];
    size_t linelen = pos - line0;
    if (linelen >= sizeof(linebuf)) goto fail;
    memcpy(linebuf, in + line0, linelen);
    linebuf[linelen] = '\0';
    char* endhex = NULL;
    unsigned long csz = strtoul(linebuf, &endhex, 16);
    if (endhex == linebuf) goto fail;
    pos += 2;
    if (csz == 0) break;
    if (csz > 100000000UL || pos + csz > in_len) goto fail;
    if (o + csz > out_cap) {
      while (o + csz > out_cap) out_cap *= 2;
      unsigned char* no = (unsigned char*)realloc(out, out_cap);
      if (!no) goto fail;
      out = no;
    }
    memcpy(out + o, in + pos, csz);
    o += csz;
    pos += csz;
    if (pos + 1 >= in_len || in[pos] != '\r' || in[pos + 1] != '\n') goto fail;
    pos += 2;
  }
  out[o] = '\0';
  *out_len = o;
  return out;
fail:
  free(out);
  return NULL;
}

static bool send_all_socket(int sock_fd, const unsigned char* data, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    ssize_t n = send(sock_fd, data + sent, len - sent, 0);
    if (n <= 0) return false;
    sent += (size_t)n;
  }
  return true;
}

static bool http_request(
    const AppConfig* cfg,
    const char* method,
    const char* target_path,
    const char* content_type,
    const unsigned char* body,
    size_t body_len,
    int* out_status,
    unsigned char** out_body,
    size_t* out_body_len) {
  ParsedUrl parsed = {0};
  if (!parse_server_url(cfg->server_url, &parsed)) return false;

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  char portbuf[16];
  snprintf(portbuf, sizeof(portbuf), "%d", parsed.port);
  struct addrinfo* res = NULL;
  if (getaddrinfo(parsed.host, portbuf, &hints, &res) != 0) return false;

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

  const char* ct = (content_type && content_type[0]) ? content_type : "application/octet-stream";
  char req_header[2048];
  int header_len = snprintf(
      req_header,
      sizeof(req_header),
      "%s %s HTTP/1.1\r\nHost: %s\r\nAccept-Encoding: identity\r\nX-API-Key: %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
      method,
      target_path,
      parsed.host,
      cfg->api_key,
      ct,
      body_len);
  if (header_len <= 0 || header_len >= (int)sizeof(req_header)) {
    close(sock);
    return false;
  }
  if (!send_all_socket(sock, (const unsigned char*)req_header, (size_t)header_len)) {
    close(sock);
    return false;
  }
  if (body_len > 0 && !send_all_socket(sock, body, body_len)) {
    close(sock);
    return false;
  }

  unsigned char* resp = NULL;
  size_t resp_cap = 0;
  size_t resp_len = 0;
  unsigned char chunk[1024];
  while (true) {
    ssize_t n = recv(sock, chunk, sizeof(chunk), 0);
    if (n <= 0) break;
    if (resp_len + (size_t)n > resp_cap) {
      size_t new_cap = resp_cap == 0 ? 4096 : resp_cap * 2;
      while (new_cap < resp_len + (size_t)n) new_cap *= 2;
      unsigned char* new_buf = (unsigned char*)realloc(resp, new_cap);
      if (!new_buf) {
        free(resp);
        close(sock);
        return false;
      }
      resp = new_buf;
      resp_cap = new_cap;
    }
    memcpy(resp + resp_len, chunk, (size_t)n);
    resp_len += (size_t)n;
  }
  close(sock);
  if (!resp || resp_len == 0) {
    free(resp);
    return false;
  }

  int status = 0;
  sscanf((const char*)resp, "HTTP/%*s %d", &status);
  *out_status = status;

  unsigned char* hdr_sep = (unsigned char*)strstr((const char*)resp, "\r\n\r\n");
  if (!hdr_sep) {
    free(resp);
    return false;
  }
  unsigned char* body_start = hdr_sep + 4;
  size_t raw_body_len = resp_len - (size_t)(body_start - resp);

  char hdr_buf[4096];
  size_t hdr_len = (size_t)(hdr_sep - resp);
  if (hdr_len >= sizeof(hdr_buf)) hdr_len = sizeof(hdr_buf) - 1;
  memcpy(hdr_buf, resp, hdr_len);
  hdr_buf[hdr_len] = '\0';

  unsigned char* final_payload = NULL;
  size_t final_len = 0;
  if (http_headers_have_chunked(hdr_buf)) {
    final_payload = http_decode_chunked(body_start, raw_body_len, &final_len);
    if (!final_payload) {
      free(resp);
      return false;
    }
  } else {
    long cl = http_content_length_header(hdr_buf);
    size_t use_len = raw_body_len;
    if (cl >= 0) {
      size_t cl_u = (size_t)cl;
      if (cl_u < use_len) use_len = cl_u;
    }
    final_payload = (unsigned char*)malloc(use_len + 1);
    if (!final_payload) {
      free(resp);
      return false;
    }
    memcpy(final_payload, body_start, use_len);
    final_payload[use_len] = '\0';
    final_len = use_len;
  }
  free(resp);
  *out_body = final_payload;
  *out_body_len = final_len;
  return true;
}

/* Scan JSON for any "sha256" : "..." value equal to expect (64 hex). Tolerates spaces around : . */
static bool json_body_has_matching_sha256(const unsigned char* body, size_t n, const char* expect_sha) {
  static const char key[] = "\"sha256\"";
  const size_t kl = sizeof(key) - 1;
  if (!body || !expect_sha || strlen(expect_sha) != 64) return false;
  for (size_t i = 0; i + kl < n; i++) {
    if (memcmp(body + i, key, kl) != 0) continue;
    size_t j = i + kl;
    while (j < n && (body[j] == ' ' || body[j] == '\t' || body[j] == '\r' || body[j] == '\n')) j++;
    if (j >= n || body[j] != ':') continue;
    j++;
    while (j < n && (body[j] == ' ' || body[j] == '\t')) j++;
    if (j >= n || body[j] != '"') continue;
    j++;
    if (j + 64 > n) continue;
    char got[65];
    int valid = 1;
    for (size_t k = 0; k < 64; k++) {
      unsigned char c = (unsigned char)body[j + k];
      if (!isxdigit(c)) {
        valid = 0;
        break;
      }
      got[k] = (char)c;
    }
    if (!valid) continue;
    got[64] = '\0';
    if (strcasecmp(got, expect_sha) == 0) return true;
  }
  return false;
}

/* After PUT: confirm index really has our payload (handles silent reject / JSON variants). */
static bool verify_server_has_sha(
    const AppConfig* cfg,
    const char* game_id,
    const unsigned char* put_body,
    size_t put_len,
    const char* expect_sha) {
  if (json_body_has_matching_sha256(put_body, put_len, expect_sha)) return true;
  char meta_path[384];
  snprintf(meta_path, sizeof(meta_path), "/save/%.200s/meta", game_id);
  unsigned char* mbody = NULL;
  size_t mlen = 0;
  int mst = 0;
  if (!http_request(cfg, "GET", meta_path, NULL, NULL, 0, &mst, &mbody, &mlen) || mst != 200) {
    free(mbody);
    return false;
  }
  bool ok = json_body_has_matching_sha256(mbody, mlen, expect_sha);
  free(mbody);
  return ok;
}

static bool json_extract_string(const char* src, const char* key, char* out, size_t out_sz) {
  char pattern[64];
  snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
  const char* start = strstr(src, pattern);
  if (!start) return false;
  start += strlen(pattern);
  const char* end = strchr(start, '"');
  if (!end) return false;
  size_t n = (size_t)(end - start);
  if (n >= out_sz) n = out_sz - 1;
  memcpy(out, start, n);
  out[n] = '\0';
  return true;
}

static int parse_remote_saves(const char* json, RemoteSave* out, int max_items) {
  int count = 0;
  const char* p = json;
  while (count < max_items) {
    const char* gid = strstr(p, "\"game_id\":\"");
    if (!gid) break;
    RemoteSave item;
    memset(&item, 0, sizeof(item));
    if (!json_extract_string(gid, "game_id", item.game_id, sizeof(item.game_id))) break;
    json_extract_string(gid, "last_modified_utc", item.last_modified_utc, sizeof(item.last_modified_utc));
    json_extract_string(gid, "server_updated_at", item.server_updated_at, sizeof(item.server_updated_at));
    json_extract_string(gid, "sha256", item.sha256, sizeof(item.sha256));
    json_extract_string(gid, "filename_hint", item.filename_hint, sizeof(item.filename_hint));
    out[count++] = item;
    p = gid + 10;
  }
  return count;
}

static bool local_game_id_exists(LocalSave* local, int local_count, const char* game_id) {
  for (int i = 0; i < local_count; i++) {
    if (strcmp(local[i].game_id, game_id) == 0) return true;
  }
  return false;
}

static int scan_local_saves(const AppConfig* cfg, const char* dir, LocalSave* out, int max_items) {
  DIR* d = opendir(dir);
  if (!d) {
    printf("ERROR: cannot open save_dir: %s (errno=%d)\n", dir, errno);
    return 0;
  }
  int count = 0;
  int sav_candidates = 0;
  int read_failures = 0;
  int stat_failures = 0;
  struct dirent* ent;
  while ((ent = readdir(d)) != NULL && count < max_items) {
    if (!has_sav_extension(ent->d_name)) continue;
    sav_candidates++;
    LocalSave item;
    memset(&item, 0, sizeof(item));
    snprintf(item.name, sizeof(item.name), "%s", ent->d_name);
    size_t dir_len = strlen(dir);
    bool has_trailing_slash = dir_len > 0 && dir[dir_len - 1] == '/';
    snprintf(item.path, sizeof(item.path), "%s%s%s", dir, has_trailing_slash ? "" : "/", ent->d_name);
    char stem[256];
    snprintf(stem, sizeof(stem), "%s", ent->d_name);
    char* dot = strrchr(stem, '.');
    if (dot) *dot = '\0';
    resolve_game_id_for_save(cfg, stem, item.game_id, sizeof(item.game_id));
    if (local_game_id_exists(out, count, item.game_id)) {
      // Avoid local collisions when multiple ROMs share the same header ID.
      char base_id[128];
      sanitize_game_id(stem, base_id, sizeof(base_id));
      if (base_id[0] == '\0') copy_cstr(base_id, sizeof(base_id), "unknown-game");
      char base_short[120];
      copy_cstr(base_short, sizeof(base_short), base_id);
      copy_cstr(item.game_id, sizeof(item.game_id), base_short);
      int suffix = 2;
      while (local_game_id_exists(out, count, item.game_id) && suffix < 1000) {
        snprintf(item.game_id, sizeof(item.game_id), "%s-%d", base_short, suffix++);
      }
    }

    struct stat st;
    if (stat(item.path, &st) != 0) {
      stat_failures++;
      mtime_to_utc_iso(time(NULL), item.last_modified_utc, sizeof(item.last_modified_utc));
    } else {
      mtime_to_utc_iso(st.st_mtime, item.last_modified_utc, sizeof(item.last_modified_utc));
    }

    unsigned char* bytes = NULL;
    size_t len = 0;
    if (!read_file_bytes(item.path, &bytes, &len)) {
      read_failures++;
      continue;
    }
    item.size_bytes = len;
    sha256_hash(bytes, len, item.sha256);
    free(bytes);

    out[count++] = item;
  }
  closedir(d);
  if (sav_candidates == 0) {
    printf("No .sav files found in %s\n", dir);
  } else if (count == 0) {
    printf(
        "No usable .sav files in %s (%d seen; stat_fail=%d read_fail=%d)\n",
        dir,
        sav_candidates,
        stat_failures,
        read_failures);
  }
  return count;
}

static LocalSave* find_local_by_id(LocalSave* locals, int n, const char* id) {
  for (int i = 0; i < n; i++) {
    if (strcmp(locals[i].game_id, id) == 0) return &locals[i];
  }
  return NULL;
}

static RemoteSave* find_remote_by_id(RemoteSave* remotes, int n, const char* id) {
  for (int i = 0; i < n; i++) {
    if (strcmp(remotes[i].game_id, id) == 0) return &remotes[i];
  }
  return NULL;
}

/* Best-effort: server logs when SAVE_SYNC_LOG_CLIENT_DEBUG=1; ignored on older servers. */
static void debug_report_sync_start_3ds(const AppConfig* cfg, int local_count, int baseline_rows) {
  char iso[48];
  mtime_to_utc_iso(time(NULL), iso, sizeof(iso));
  char json[400];
  snprintf(
      json,
      sizeof(json),
      "{\"utc_iso\":\"%s\",\"platform\":\"3ds\",\"phase\":\"sync_auto_start\",\"local_saves\":%d,"
      "\"baseline_rows\":%d}",
      iso,
      local_count,
      baseline_rows);
  unsigned char* resp = NULL;
  size_t resp_len = 0;
  int st = 0;
  (void)http_request(
      cfg,
      "POST",
      "/debug/client-clock",
      "application/json",
      (const unsigned char*)json,
      strlen(json),
      &st,
      &resp,
      &resp_len);
  free(resp);
}

static int add_merge_id(char (*ids)[128], int n_ids, int max_ids, const char* id) {
  for (int i = 0; i < n_ids; i++) {
    if (strcmp(ids[i], id) == 0) return n_ids;
  }
  if (n_ids >= max_ids) return n_ids;
  copy_cstr(ids[n_ids], 128, id);
  return n_ids + 1;
}

static bool put_one_save(
    const AppConfig* cfg,
    const LocalSave* l,
    bool force,
    const char* source_tag) {
  unsigned char* bytes = NULL;
  size_t len = 0;
  if (!read_file_bytes(l->path, &bytes, &len)) {
    printf("%s: ERROR(read)\n", l->game_id);
    return false;
  }
  char ts_q[80], hash_q[80], filename_q[400], clk_q[80], path[1200];
  char ts_wall[40];
  mtime_to_utc_iso(time(NULL), ts_wall, sizeof(ts_wall));
  url_encode_simple(ts_wall, ts_q, sizeof(ts_q));
  url_encode_simple(l->sha256, hash_q, sizeof(hash_q));
  url_encode_simple(l->name, filename_q, sizeof(filename_q));
  url_encode_simple(ts_wall, clk_q, sizeof(clk_q));
  const char* force_q = force ? "&force=1" : "";
  snprintf(
      path,
      sizeof(path),
      "/save/%s?last_modified_utc=%s&sha256=%s&size_bytes=%zu&filename_hint=%s&platform_source=%s&client_clock_utc=%s%s",
      l->game_id,
      ts_q,
      hash_q,
      l->size_bytes,
      filename_q,
      source_tag,
      clk_q,
      force_q);
  unsigned char* put_resp = NULL;
  size_t put_len = 0;
  int put_status = 0;
  bool ok = http_request(cfg, "PUT", path, NULL, bytes, len, &put_status, &put_resp, &put_len);
  free(bytes);
  bool applied_not_false = true;
  if (ok && put_resp && put_len > 0) {
    if (json_body_applied_is_false(put_resp, put_len)) applied_not_false = false;
  }
  if (!ok || put_status != 200) {
    free(put_resp);
    printf("%s: ERROR(upload)\n", l->game_id);
    return false;
  }
  if (!applied_not_false) {
    free(put_resp);
    printf("%s: REJECTED (server kept existing; try force X upload)\n", l->game_id);
    return false;
  }
  if (json_body_applied_is_true(put_resp, put_len)) {
    free(put_resp);
    printf("%s: UPLOADED\n", l->game_id);
    return true;
  }
  if (!json_body_has_applied_member(put_resp, put_len)) {
    free(put_resp);
    printf("%s: UPLOADED\n", l->game_id);
    return true;
  }
  if (!verify_server_has_sha(cfg, l->game_id, put_resp, put_len, l->sha256)) {
    free(put_resp);
    printf(
        "%s: REJECTED (could not confirm save on server — check URL/API key / response truncated)\n",
        l->game_id);
    return false;
  }
  free(put_resp);
  printf("%s: UPLOADED\n", l->game_id);
  return true;
}

static bool get_one_save(
    const AppConfig* cfg, const char* save_dir, const char* game_id, const RemoteSave* r) {
  char filename[256];
  char fallback_name[160];
  snprintf(fallback_name, sizeof(fallback_name), "%.127s.sav", game_id);
  if (r->filename_hint[0] != '\0') {
    sanitize_filename(filename, sizeof(filename), r->filename_hint, fallback_name);
  } else {
    sanitize_filename(filename, sizeof(filename), fallback_name, fallback_name);
  }
  char out_path[512];
  join_path(out_path, sizeof(out_path), save_dir, filename);

  char get_path[256];
  snprintf(get_path, sizeof(get_path), "/save/%.127s", game_id);
  unsigned char* save_bytes = NULL;
  size_t save_len = 0;
  int get_status = 0;
  if (!http_request(cfg, "GET", get_path, NULL, NULL, 0, &get_status, &save_bytes, &save_len) || get_status != 200) {
    printf("%s: ERROR(download)\n", game_id);
    free(save_bytes);
    return false;
  }
  if (write_atomic_file(out_path, save_bytes, save_len)) {
    printf("%s: DOWNLOADED\n", game_id);
    free(save_bytes);
    return true;
  }
  printf("%s: ERROR(write)\n", game_id);
  free(save_bytes);
  return false;
}

/* Both local and server differ from baseline — wait for X/Y instead of finishing sync with SKIP. */
static void resolve_both_changed_conflict(
    const AppConfig* cfg,
    const char* save_dir,
    const char* source_tag,
    LocalSave* l,
    RemoteSave* r,
    const char* id,
    BaselineRow* baseline,
    int* n_baseline) {
  consoleClear();
  printf("\n");
  printf("  -------- Conflict --------\n");
  printf("\n");
  printf("  %s\n\n", id);
  printf("  Local and server both changed since\n");
  printf("  the last successful sync.\n\n");
  printf("  X   Upload local (overwrite server)\n");
  printf("  Y   Download server (overwrite local)\n");
  printf("  B   Skip for now\n");
  printf("\n");
  while (aptMainLoop()) {
    hidScanInput();
    u32 kDown = hidKeysDown();
    if (kDown & KEY_X) {
      if (put_one_save(cfg, l, true, source_tag)) {
        baseline_upsert(baseline, n_baseline, MAX_SAVES, id, l->sha256);
      }
      break;
    }
    if (kDown & KEY_Y) {
      if (get_one_save(cfg, save_dir, id, r)) {
        baseline_upsert(baseline, n_baseline, MAX_SAVES, id, r->sha256);
      }
      break;
    }
    if (kDown & KEY_B) {
      break;
    }
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
  }
}

static bool pick_upload_selection_3ds(const AppConfig* cfg, SyncManualFilter* out) {
  const char* save_dir = active_save_dir(cfg);
  LocalSave* local = (LocalSave*)calloc(MAX_SAVES, sizeof(LocalSave));
  if (!local) return false;
  ensure_directory_exists(save_dir);
  int n = scan_local_saves(cfg, save_dir, local, MAX_SAVES);
  if (n <= 0) {
    printf("No local .sav files to upload.\n");
    free(local);
    return false;
  }

  bool picked[MAX_SAVES];
  for (int i = 0; i < n; i++) picked[i] = true;
  bool master_all = true;
  int cursor = 0;
  int scroll = 0;
  /* One line per game + footer (redraw only on changes avoids console flicker) */
  const int kVisible = 14;
  const int total_rows = n + 1;
  bool dirty = true;

  while (aptMainLoop()) {
    hidScanInput();
    u32 kDown = hidKeysDown();
    /* Avoid X while still in main "upload" flow if undesired; START is primary. */
    const u32 confirm_upload = KEY_START | KEY_R | KEY_X;

    if (kDown & confirm_upload) {
      if (master_all) {
        manual_filter_free(out);
        out->all = 1;
        free(local);
        return true;
      }
      manual_filter_free(out);
      out->ids = calloc(MAX_SAVES, 128);
      if (!out->ids) {
        free(local);
        return false;
      }
      out->all = 0;
      out->n_ids = 0;
      for (int i = 0; i < n; i++) {
        if (picked[i] && out->n_ids < MAX_SAVES) {
          copy_cstr(out->ids[out->n_ids], 128, local[i].game_id);
          out->n_ids++;
        }
      }
      if (out->n_ids == 0) {
        manual_filter_free(out);
        continue;
      }
      free(local);
      return true;
    }
    if (kDown & KEY_B) {
      free(local);
      return false;
    }
    if (kDown & KEY_DUP) {
      cursor = (cursor + total_rows - 1) % total_rows;
      dirty = true;
    }
    if (kDown & KEY_DDOWN) {
      cursor = (cursor + 1) % total_rows;
      dirty = true;
    }
    if (kDown & KEY_A) {
      if (cursor == 0) {
        master_all = !master_all;
        for (int i = 0; i < n; i++) picked[i] = master_all;
      } else {
        picked[cursor - 1] = !picked[cursor - 1];
        master_all = true;
        for (int i = 0; i < n; i++) {
          if (!picked[i]) master_all = false;
        }
      }
      dirty = true;
    }

    if (dirty) {
      if (cursor < scroll) scroll = cursor;
      if (cursor >= scroll + kVisible) scroll = cursor - kVisible + 1;
      if (scroll < 0) scroll = 0;
      int max_scroll = total_rows > kVisible ? total_rows - kVisible : 0;
      if (scroll > max_scroll) scroll = max_scroll;

      consoleClear();
      printf("Upload: choose saves\n");
      printf("DPad: move  A: toggle  START/R/X: run  B: back to menu\n\n");
      for (int row = scroll; row < scroll + kVisible && row < total_rows; row++) {
        char mark = (row == cursor) ? '>' : ' ';
        if (row == 0) {
          printf("%c [%c] ALL SAVES\n", mark, master_all ? 'x' : ' ');
        } else {
          const LocalSave* L = &local[row - 1];
          printf(
              "%c [%c] %.28s (%.36s)\n",
              mark,
              picked[row - 1] ? 'x' : ' ',
              L->game_id,
              L->name);
        }
      }
      dirty = false;
    }
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
  }
  free(local);
  return false;
}

static bool pick_download_selection_3ds(const AppConfig* cfg, SyncManualFilter* out) {
  RemoteSave* remote = (RemoteSave*)calloc(MAX_SAVES, sizeof(RemoteSave));
  if (!remote) return false;
  int status = 0;
  unsigned char* body = NULL;
  size_t body_len = 0;
  if (!http_request(cfg, "GET", "/saves", NULL, NULL, 0, &status, &body, &body_len) || status != 200) {
    printf("ERROR: GET /saves failed (cannot list downloads)\n");
    free(body);
    free(remote);
    return false;
  }
  int remote_count = parse_remote_saves((const char*)body, remote, MAX_SAVES);
  free(body);
  if (remote_count <= 0) {
    printf("No remote saves to download.\n");
    free(remote);
    return false;
  }
  sort_remotes(remote, remote_count);

  bool picked[MAX_SAVES];
  for (int i = 0; i < remote_count; i++) picked[i] = true;
  bool master_all = true;
  int cursor = 0;
  int scroll = 0;
  /* One line per game row (game_id only) — can show more than upload picker */
  const int kVisible = 14;
  const int total_rows = remote_count + 1;
  bool dirty = true;

  while (aptMainLoop()) {
    hidScanInput();
    u32 kDown = hidKeysDown();
    const u32 confirm_download = KEY_START | KEY_R | KEY_Y;

    if (kDown & confirm_download) {
      if (master_all) {
        manual_filter_free(out);
        out->all = 1;
        free(remote);
        return true;
      }
      manual_filter_free(out);
      out->ids = calloc(MAX_SAVES, 128);
      if (!out->ids) {
        free(remote);
        return false;
      }
      out->all = 0;
      out->n_ids = 0;
      for (int i = 0; i < remote_count; i++) {
        if (picked[i] && out->n_ids < MAX_SAVES) {
          copy_cstr(out->ids[out->n_ids], 128, remote[i].game_id);
          out->n_ids++;
        }
      }
      if (out->n_ids == 0) {
        manual_filter_free(out);
        continue;
      }
      free(remote);
      return true;
    }
    if (kDown & KEY_B) {
      free(remote);
      return false;
    }
    if (kDown & KEY_DUP) {
      cursor = (cursor + total_rows - 1) % total_rows;
      dirty = true;
    }
    if (kDown & KEY_DDOWN) {
      cursor = (cursor + 1) % total_rows;
      dirty = true;
    }
    if (kDown & KEY_A) {
      if (cursor == 0) {
        master_all = !master_all;
        for (int i = 0; i < remote_count; i++) picked[i] = master_all;
      } else {
        picked[cursor - 1] = !picked[cursor - 1];
        master_all = true;
        for (int i = 0; i < remote_count; i++) {
          if (!picked[i]) master_all = false;
        }
      }
      dirty = true;
    }

    if (dirty) {
      if (cursor < scroll) scroll = cursor;
      if (cursor >= scroll + kVisible) scroll = cursor - kVisible + 1;
      if (scroll < 0) scroll = 0;
      int max_scroll = total_rows > kVisible ? total_rows - kVisible : 0;
      if (scroll > max_scroll) scroll = max_scroll;

      consoleClear();
      printf("Download: choose saves\n");
      printf("DPad: move  A: toggle  START/R/Y: run  B: back to menu\n\n");
      for (int row = scroll; row < scroll + kVisible && row < total_rows; row++) {
        char mark = (row == cursor) ? '>' : ' ';
        if (row == 0) {
          printf("%c [%c] ALL SAVES\n", mark, master_all ? 'x' : ' ');
        } else {
          const RemoteSave* R = &remote[row - 1];
          const char* hint = R->filename_hint[0] != '\0' ? R->filename_hint : "-";
          printf(
              "%c [%c] %.28s (%.36s)\n",
              mark,
              picked[row - 1] ? 'x' : ' ',
              R->game_id,
              hint);
        }
      }
      dirty = false;
    }
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
  }
  free(remote);
  return false;
}

static void run_sync(const AppConfig* cfg, SyncAction action, const SyncManualFilter* xy_filter) {
  printf("Scanning local saves...\n");
  LocalSave* local = (LocalSave*)calloc(MAX_SAVES, sizeof(LocalSave));
  RemoteSave* remote = (RemoteSave*)calloc(MAX_SAVES, sizeof(RemoteSave));
  if (!local || !remote) {
    printf("ERROR: out of memory for sync buffers\n");
    free(local);
    free(remote);
    return;
  }
  const char* save_dir = active_save_dir(cfg);
  const char* source_tag = is_vc_mode(cfg) ? "3ds-homebrew-vc" : "3ds-homebrew";
  ensure_directory_exists(save_dir);
  BaselineRow* baseline = (BaselineRow*)calloc((size_t)MAX_SAVES, sizeof(BaselineRow));
  if (!baseline) {
    printf("ERROR: out of memory (baseline buffer)\n");
    free(local);
    free(remote);
    return;
  }
  int n_baseline = baseline_load(save_dir, baseline, MAX_SAVES);
  int local_count = scan_local_saves(cfg, save_dir, local, MAX_SAVES);
  printf("Local saves: %d\n", local_count);

  int status = 0;
  unsigned char* body = NULL;
  size_t body_len = 0;
  if (!http_request(cfg, "GET", "/saves", NULL, NULL, 0, &status, &body, &body_len) || status != 200) {
    printf("ERROR: failed GET /saves (check server URL/API key/network)\n");
    free(body);
    free(baseline);
    free(local);
    free(remote);
    return;
  }
  int remote_count = parse_remote_saves((const char*)body, remote, MAX_SAVES);
  free(body);
  printf("Remote saves: %d\n", remote_count);

  if (action == SYNC_ACTION_UPLOAD_ONLY) {
    for (int i = 0; i < local_count; i++) {
      if (!id_in_manual(xy_filter, local[i].game_id)) continue;
      if (put_one_save(cfg, &local[i], true, source_tag)) {
        baseline_upsert(baseline, &n_baseline, MAX_SAVES, local[i].game_id, local[i].sha256);
      }
    }
    if (!baseline_save(save_dir, baseline, n_baseline)) {
      printf("WARN: could not write .savesync-baseline\n");
    }
    free(baseline);
    free(local);
    free(remote);
    return;
  }

  if (action == SYNC_ACTION_DOWNLOAD_ONLY) {
    for (int i = 0; i < remote_count; i++) {
      if (!id_in_manual(xy_filter, remote[i].game_id)) continue;
      if (get_one_save(cfg, save_dir, remote[i].game_id, &remote[i])) {
        baseline_upsert(baseline, &n_baseline, MAX_SAVES, remote[i].game_id, remote[i].sha256);
      }
    }
    if (!baseline_save(save_dir, baseline, n_baseline)) {
      printf("WARN: could not write .savesync-baseline\n");
    }
    free(baseline);
    free(local);
    free(remote);
    return;
  }

  /* AUTO — merge id list on heap (large stack arrays overflow 3DS default stack) */
  const int merge_cap = MAX_SAVES * 2;
  char (*merge_ids)[128] = calloc((size_t)merge_cap, sizeof(*merge_ids));
  if (!merge_ids) {
    printf("ERROR: out of memory (merge list)\n");
    free(baseline);
    free(local);
    free(remote);
    return;
  }
  int n_merge = 0;
  for (int i = 0; i < local_count; i++) {
    n_merge = add_merge_id(merge_ids, n_merge, merge_cap, local[i].game_id);
  }
  for (int i = 0; i < remote_count; i++) {
    n_merge = add_merge_id(merge_ids, n_merge, merge_cap, remote[i].game_id);
  }

  debug_report_sync_start_3ds(cfg, local_count, n_baseline);

  for (int m = 0; m < n_merge; m++) {
    const char* id = merge_ids[m];
    LocalSave* l = find_local_by_id(local, local_count, id);
    RemoteSave* r = find_remote_by_id(remote, remote_count, id);
    if (l && r) {
      if (strcmp(l->sha256, r->sha256) == 0) {
        printf("%s: OK\n", id);
        baseline_upsert(baseline, &n_baseline, MAX_SAVES, id, l->sha256);
        continue;
      }
      const char* base = baseline_find(baseline, n_baseline, id);
      if (!base || base[0] == '\0') {
        printf(
            "%s: SKIP (no baseline yet — 3DS ignores unreliable file dates). "
            "Use X upload or Y download once per game, then Auto works.\n",
            id);
        continue;
      }
      const int loc_eq = (strcmp(l->sha256, base) == 0);
      const int rem_eq = (strcmp(r->sha256, base) == 0);
      if (loc_eq && !rem_eq) {
        if (get_one_save(cfg, save_dir, id, r)) {
          baseline_upsert(baseline, &n_baseline, MAX_SAVES, id, r->sha256);
        }
      } else if (!loc_eq && rem_eq) {
        if (put_one_save(cfg, l, false, source_tag)) {
          baseline_upsert(baseline, &n_baseline, MAX_SAVES, id, l->sha256);
        }
      } else if (!loc_eq && !rem_eq) {
        resolve_both_changed_conflict(cfg, save_dir, source_tag, l, r, id, baseline, &n_baseline);
      }
    } else if (l && !r) {
      if (put_one_save(cfg, l, false, source_tag)) {
        baseline_upsert(baseline, &n_baseline, MAX_SAVES, id, l->sha256);
      }
    } else if (!l && r) {
      if (get_one_save(cfg, save_dir, id, r)) {
        baseline_upsert(baseline, &n_baseline, MAX_SAVES, id, r->sha256);
      }
    }
  }

  if (!baseline_save(save_dir, baseline, n_baseline)) {
    printf("WARN: could not write .savesync-baseline\n");
  }

  free(merge_ids);
  free(baseline);
  free(local);
  free(remote);
}

static bool choose_action(SyncAction* out_action) {
  while (aptMainLoop()) {
    hidScanInput();
    u32 kDown = hidKeysDown();
    if (kDown & KEY_A) {
      *out_action = SYNC_ACTION_AUTO;
      return true;
    }
    if (kDown & KEY_X) {
      *out_action = SYNC_ACTION_UPLOAD_ONLY;
      return true;
    }
    if (kDown & KEY_Y) {
      *out_action = SYNC_ACTION_DOWNLOAD_ONLY;
      return true;
    }
    if (kDown & KEY_START) {
      return false;
    }
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
  }
  return false;
}

static bool confirm_auto_sync(void) {
  printf("\n");
  printf("      -------- Confirm --------\n");
  printf("\n");
  printf("  Full sync uses hash baselines on 3DS\n");
  printf("  (not unreliable file dates).\n");
  printf("\n");
  printf("  If a game conflicts: X or Y picks a side\n");
  printf("  (press once; use again if both changed).\n");
  printf("\n");
  printf("  A           Continue\n");
  printf("  B / START   Back to main menu\n");
  printf("\n");

  while (aptMainLoop()) {
    hidScanInput();
    u32 kDown = hidKeysDown();
    if (kDown & KEY_A) return true;
    if (kDown & KEY_B || kDown & KEY_START) return false;
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
  }
  return false;
}

static void wait_after_sync_3ds(bool* quit_app) {
  printf("\na: main menu   START: exit app\n");
  while (aptMainLoop()) {
    hidScanInput();
    u32 kDown = hidKeysDown();
    if (kDown & KEY_A) return;
    if (kDown & KEY_START) {
      *quit_app = true;
      return;
    }
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
  }
  *quit_app = true;
}

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;

  gfxInitDefault();
  consoleInit(GFX_BOTTOM, NULL);

  AppConfig cfg;
  config_init(&cfg);
  load_config(&cfg, "sdmc:/3ds/gba-sync/config.ini");

  static u32* soc_buffer = NULL;
  soc_buffer = (u32*)memalign(0x1000, SOC_BUFFERSIZE);
  bool soc_ready = false;
  if (soc_buffer && socInit(soc_buffer, SOC_BUFFERSIZE) == 0) {
    soc_ready = true;
  }

  bool quit_app = false;

  printf("\n");
  if (!cfg.server_url[0]) {
    printf("GBA Sync (3DS MVP)\n\nERROR: missing [server].url\n");
  } else if (!soc_ready) {
    printf("GBA Sync (3DS MVP)\n\nERROR: networking init failed\n");
  } else if (strncmp(cfg.server_url, "http://", 7) != 0) {
    printf("GBA Sync (3DS MVP)\n\nERROR: use http:// URL for 3DS MVP\n");
  } else {
    while (aptMainLoop() && !quit_app) {
      consoleClear();
      printf("GBA Sync (3DS MVP)\n");
      printf("------------------\n");
      printf("Server: %s\n", cfg.server_url);
      printf("Mode: %s\n", is_vc_mode(&cfg) ? "vc" : "normal");
      printf("Save dir: %s\n\n", active_save_dir(&cfg));
      printf("a: Full sync\n");
      printf("x: Upload only\n");
      printf("y: Download only\n\n");
      printf("START on this menu exits the app.\n");

      SyncAction action = SYNC_ACTION_AUTO;
      if (!choose_action(&action)) break;

      SyncManualFilter xy;
      memset(&xy, 0, sizeof(xy));
      xy.all = 1;
      if (action == SYNC_ACTION_AUTO && !confirm_auto_sync()) {
        manual_filter_free(&xy);
        continue;
      }

      if (action == SYNC_ACTION_UPLOAD_ONLY) {
        if (!pick_upload_selection_3ds(&cfg, &xy)) {
          manual_filter_free(&xy);
          continue;
        }
        run_sync(&cfg, action, &xy);
      } else if (action == SYNC_ACTION_DOWNLOAD_ONLY) {
        if (!pick_download_selection_3ds(&cfg, &xy)) {
          manual_filter_free(&xy);
          continue;
        }
        run_sync(&cfg, action, &xy);
      } else {
        run_sync(&cfg, action, NULL);
      }
      manual_filter_free(&xy);
      wait_after_sync_3ds(&quit_app);
    }
  }
  if (!quit_app) {
    printf("\nPress START to exit.\n");

    while (aptMainLoop()) {
      hidScanInput();
      u32 kDown = hidKeysDown();
      if (kDown & KEY_START) break;
      gfxFlushBuffers();
      gfxSwapBuffers();
      gspWaitForVBlank();
    }
  }

  if (soc_ready) socExit();
  free(soc_buffer);
  gfxExit();
  return 0;
}
