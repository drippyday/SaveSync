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

#define MAX_SAVES 256
#define SOC_BUFFERSIZE (0x100000)

typedef enum {
  SYNC_ACTION_AUTO = 0,
  SYNC_ACTION_UPLOAD_ONLY = 1,
  SYNC_ACTION_DOWNLOAD_ONLY = 2,
} SyncAction;

static void ensure_directory_exists(const char* dir);

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
    printf("DEBUG: fopen write failed path=%s errno=%d\n", tmp_path, errno);
    return false;
  }
  if (len > 0 && fwrite(data, 1, len, fp) != len) {
    fclose(fp);
    printf("DEBUG: fwrite failed path=%s errno=%d\n", tmp_path, errno);
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
      printf("DEBUG: fopen direct-existing failed path=%s errno=%d\n", path, errno);
      remove(tmp_path);
      return false;
    }
    if (len > 0 && fwrite(data, 1, len, direct) != len) {
      printf("DEBUG: fwrite direct-existing failed path=%s errno=%d\n", path, errno);
      fclose(direct);
      remove(tmp_path);
      return false;
    }
    fclose(direct);
    remove(tmp_path);
    return true;
  }

  if (rename(tmp_path, path) != 0) {
    int rename_errno = errno;
    struct stat tmp_st;
    bool tmp_exists = (stat(tmp_path, &tmp_st) == 0);
    printf(
        "DEBUG: rename failed from=%s to=%s errno=%d tmp_exists=%d\n",
        tmp_path,
        path,
        rename_errno,
        tmp_exists ? 1 : 0);
    remove(path);
    if (rename(tmp_path, path) == 0) return true;

    // Fallback path for filesystems/locks where rename replacement fails.
    FILE* direct = fopen(path, "wb");
    if (!direct) {
      printf("DEBUG: fopen fallback failed path=%s errno=%d\n", path, errno);
      remove(tmp_path);
      return false;
    }
    if (len > 0 && fwrite(data, 1, len, direct) != len) {
      printf("DEBUG: fwrite fallback failed path=%s errno=%d\n", path, errno);
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
  if (mkdir(dir, 0777) == 0) {
    printf("INFO: created missing save dir: %s\n", dir);
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

  char req_header[2048];
  int header_len = snprintf(
      req_header,
      sizeof(req_header),
      "%s %s HTTP/1.1\r\nHost: %s\r\nX-API-Key: %s\r\nContent-Type: application/octet-stream\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
      method,
      target_path,
      parsed.host,
      cfg->api_key,
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

  unsigned char* body_start = (unsigned char*)strstr((const char*)resp, "\r\n\r\n");
  if (!body_start) {
    free(resp);
    return false;
  }
  body_start += 4;
  size_t header_bytes = (size_t)(body_start - resp);
  size_t payload_len = resp_len - header_bytes;
  unsigned char* payload = (unsigned char*)malloc(payload_len + 1);
  if (!payload) {
    free(resp);
    return false;
  }
  memcpy(payload, body_start, payload_len);
  payload[payload_len] = '\0';
  free(resp);
  *out_body = payload;
  *out_body_len = payload_len;
  return true;
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
  int stat_fallbacks = 0;
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
      printf("DEBUG: stat failed path=%s errno=%d\n", item.path, errno);
      // Some SD/FAT setups can fail stat() even when file open/read works.
      // Fall back to current time so valid save files are still usable.
      mtime_to_utc_iso(time(NULL), item.last_modified_utc, sizeof(item.last_modified_utc));
      stat_fallbacks++;
    } else {
      mtime_to_utc_iso(st.st_mtime, item.last_modified_utc, sizeof(item.last_modified_utc));
    }

    unsigned char* bytes = NULL;
    size_t len = 0;
    if (!read_file_bytes(item.path, &bytes, &len)) {
      read_failures++;
      printf("DEBUG: read failed path=%s errno=%d\n", item.path, errno);
      continue;
    }
    item.size_bytes = len;
    sha256_hash(bytes, len, item.sha256);
    free(bytes);

    out[count++] = item;
  }
  closedir(d);
  if (sav_candidates == 0) {
    printf("INFO: no .sav files found in %s\n", dir);
  } else if (count == 0) {
    printf(
        "INFO: found %d .sav candidate(s), but none usable (stat_fail=%d read_fail=%d)\n",
        sav_candidates,
        stat_failures,
        read_failures);
  } else if (stat_fallbacks > 0) {
    printf("INFO: stat fallback used for %d save(s)\n", stat_fallbacks);
  }
  return count;
}

static void run_sync(const AppConfig* cfg, SyncAction action) {
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
  int local_count = scan_local_saves(cfg, save_dir, local, MAX_SAVES);
  printf("Local saves: %d\n", local_count);

  int status = 0;
  unsigned char* body = NULL;
  size_t body_len = 0;
  if (!http_request(cfg, "GET", "/saves", NULL, 0, &status, &body, &body_len) || status != 200) {
    printf("ERROR: failed GET /saves (check server URL/API key/network)\n");
    free(body);
    free(local);
    free(remote);
    return;
  }
  int remote_count = parse_remote_saves((const char*)body, remote, MAX_SAVES);
  free(body);
  printf("Remote saves: %d\n", remote_count);

  if (action != SYNC_ACTION_DOWNLOAD_ONLY) {
    for (int i = 0; i < local_count; i++) {
      unsigned char* bytes = NULL;
      size_t len = 0;
      if (!read_file_bytes(local[i].path, &bytes, &len)) {
        printf("%s: ERROR(read)\n", local[i].game_id);
        continue;
      }
      char ts_q[80], hash_q[80], filename_q[400], path[1024];
      url_encode_simple(local[i].last_modified_utc, ts_q, sizeof(ts_q));
      url_encode_simple(local[i].sha256, hash_q, sizeof(hash_q));
      url_encode_simple(local[i].name, filename_q, sizeof(filename_q));
      snprintf(
          path,
          sizeof(path),
          "/save/%s?last_modified_utc=%s&sha256=%s&size_bytes=%zu&filename_hint=%s&platform_source=%s%s",
          local[i].game_id,
          ts_q,
          hash_q,
          local[i].size_bytes,
          filename_q,
          source_tag,
          "&force=1");
      unsigned char* put_resp = NULL;
      size_t put_len = 0;
      int put_status = 0;
      bool ok = http_request(cfg, "PUT", path, bytes, len, &put_status, &put_resp, &put_len);
      free(bytes);
      free(put_resp);
      if (ok && put_status == 200) {
        printf("%s: UPLOADED\n", local[i].game_id);
      } else {
        printf("%s: ERROR(upload)\n", local[i].game_id);
      }
    }
  }

  if (action == SYNC_ACTION_UPLOAD_ONLY) {
    free(local);
    free(remote);
    return;
  }

  if (action == SYNC_ACTION_AUTO) {
    body = NULL;
    body_len = 0;
    if (!http_request(cfg, "GET", "/saves", NULL, 0, &status, &body, &body_len) || status != 200) {
      printf("ERROR: failed refresh GET /saves (check server URL/API key/network)\n");
      free(body);
      free(local);
      free(remote);
      return;
    }
    remote_count = parse_remote_saves((const char*)body, remote, MAX_SAVES);
    free(body);
  }

  for (int i = 0; i < remote_count; i++) {
    char filename[256];
    char fallback_name[160];
    snprintf(fallback_name, sizeof(fallback_name), "%.*s.sav", (int)sizeof(remote[i].game_id) - 1, remote[i].game_id);
    if (remote[i].filename_hint[0] != '\0') {
      sanitize_filename(filename, sizeof(filename), remote[i].filename_hint, fallback_name);
    } else {
      sanitize_filename(filename, sizeof(filename), fallback_name, fallback_name);
    }
    char out_path[512];
    join_path(out_path, sizeof(out_path), save_dir, filename);

    char get_path[256];
    snprintf(get_path, sizeof(get_path), "/save/%.*s", (int)sizeof(remote[i].game_id) - 1, remote[i].game_id);
    unsigned char* save_bytes = NULL;
    size_t save_len = 0;
    int get_status = 0;
    if (!http_request(cfg, "GET", get_path, NULL, 0, &get_status, &save_bytes, &save_len) || get_status != 200) {
      printf("%s: ERROR(download)\n", remote[i].game_id);
      free(save_bytes);
      continue;
    }
    if (write_atomic_file(out_path, save_bytes, save_len)) {
      printf("%s: DOWNLOADED\n", remote[i].game_id);
    } else {
      printf("%s: ERROR(write)\n", remote[i].game_id);
    }
    free(save_bytes);
  }
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

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;

  gfxInitDefault();
  consoleInit(GFX_BOTTOM, NULL);

  AppConfig cfg;
  config_init(&cfg);
  load_config(&cfg, "sdmc:/3ds/gba-sync/config.ini");

  printf("GBA Sync (3DS MVP)\n");
  printf("------------------\n");
  printf("Server: %s\n", cfg.server_url[0] ? cfg.server_url : "(missing config)");
  printf("Mode: %s\n", is_vc_mode(&cfg) ? "vc" : "normal");
  printf("Save dir: %s\n", active_save_dir(&cfg));

  static u32* soc_buffer = NULL;
  soc_buffer = (u32*)memalign(0x1000, SOC_BUFFERSIZE);
  bool soc_ready = false;
  if (soc_buffer && socInit(soc_buffer, SOC_BUFFERSIZE) == 0) {
    soc_ready = true;
  }

  printf("\n");
  if (!cfg.server_url[0]) {
    printf("ERROR: missing [server].url\n");
  } else if (!soc_ready) {
    printf("ERROR: networking init failed\n");
  } else if (strncmp(cfg.server_url, "http://", 7) != 0) {
    printf("ERROR: use http:// URL for 3DS MVP\n");
  } else {
    printf("A: full sync   X: upload only   Y: download only\n");
    printf("Press START to cancel.\n");
    SyncAction action = SYNC_ACTION_AUTO;
    if (choose_action(&action)) {
      run_sync(&cfg, action);
    } else {
      printf("Cancelled.\n");
    }
  }
  printf("\nPress START to exit.\n");

  while (aptMainLoop()) {
    hidScanInput();
    u32 kDown = hidKeysDown();
    if (kDown & KEY_START) break;
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
  }

  if (soc_ready) socExit();
  free(soc_buffer);
  gfxExit();
  return 0;
}
