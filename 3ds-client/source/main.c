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

/** Single source for config path (also referenced in README / distribution). */
#define GBASYNC_3DS_CONFIG_PATH "sdmc:/3ds/gba-sync/config.ini"

typedef struct {
  char server_url[256];
  char api_key[128];
  char save_dir[256];
  char sync_mode[16];
  char vc_save_dir[256];
  char rom_dir[256];
  /** Comma-separated ok, e.g. ``.gb,.gbc`` (tried in order for ROM lookup). */
  char rom_extension[128];
  char gba_save_dir[256];
  char nds_save_dir[256];
  char gb_save_dir[256];
  char gba_rom_dir[256];
  char nds_rom_dir[256];
  char gb_rom_dir[256];
  char gba_rom_extension[128];
  char nds_rom_extension[128];
  char gb_rom_extension[128];
  char locked_ids[512];
} AppConfig;

typedef enum { ROOT_GBA = 0, ROOT_NDS = 1, ROOT_GB = 2 } SaveRootKind;

typedef struct {
  SaveRootKind kind;
  char save_dir[256];
  char rom_dir[256];
  char rom_extension[128];
} SaveRoot;

typedef struct {
  char game_id[128];
  char last_modified_utc[40];
  char server_updated_at[40];
  char sha256[65];
  char filename_hint[128];
  char display_name[128];
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

typedef struct {
  char save_stem[256];
  char game_id[128];
} IdMapRow;

#define MAX_SAVES 384
#define SOC_BUFFERSIZE (0x100000)

typedef enum {
  SYNC_ACTION_AUTO = 0,
  SYNC_ACTION_UPLOAD_ONLY = 1,
  SYNC_ACTION_DOWNLOAD_ONLY = 2,
  SYNC_ACTION_DROPBOX_SYNC = 3,
  SYNC_ACTION_SAVE_VIEWER = 4,
} SyncAction;

typedef struct {
  int uploads;
  int downloads;
  int already_up_to_date;
  /** Set when user confirms preview with START (sync then exit app). */
  int exit_after_sync;
} SyncSummary;

typedef struct {
  int all;
  int n_ids;
  char (*ids)[128];
} SyncManualFilter;

static void ensure_directory_exists(const char* dir);
static void wait_after_sync_3ds(bool* quit_app, bool can_reboot);

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

static RemoteSave* g_upload_sort_remote = NULL;
static int g_upload_sort_rc = 0;

static int cmp_local_for_upload_order(const void* a, const void* b) {
  const LocalSave* la = (const LocalSave*)a;
  const LocalSave* lb = (const LocalSave*)b;
  int ia = -1;
  int ib = -1;
  int i;
  if (g_upload_sort_remote && g_upload_sort_rc > 0) {
    for (i = 0; i < g_upload_sort_rc; i++) {
      if (strcmp(g_upload_sort_remote[i].game_id, la->game_id) == 0) {
        ia = i;
        break;
      }
    }
    for (i = 0; i < g_upload_sort_rc; i++) {
      if (strcmp(g_upload_sort_remote[i].game_id, lb->game_id) == 0) {
        ib = i;
        break;
      }
    }
  }
  if (ia >= 0 && ib < 0) return -1;
  if (ia < 0 && ib >= 0) return 1;
  if (ia >= 0 && ib >= 0) return ia - ib;
  return strcmp(la->game_id, lb->game_id);
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
  copy_cstr(cfg->server_url, sizeof(cfg->server_url), "http://10.0.0.151:8080");
  copy_cstr(cfg->api_key, sizeof(cfg->api_key), "change-me");
  copy_cstr(cfg->save_dir, sizeof(cfg->save_dir), "sdmc:/3ds/open_agb_firm/saves/");
  copy_cstr(cfg->sync_mode, sizeof(cfg->sync_mode), "normal");
  copy_cstr(cfg->vc_save_dir, sizeof(cfg->vc_save_dir), "sdmc:/3ds/Checkpoint/saves");
  copy_cstr(cfg->rom_dir, sizeof(cfg->rom_dir), "sdmc:/roms/gba");
  copy_cstr(cfg->rom_extension, sizeof(cfg->rom_extension), ".gba");
  copy_cstr(cfg->gba_rom_extension, sizeof(cfg->gba_rom_extension), ".gba");
  copy_cstr(cfg->nds_rom_extension, sizeof(cfg->nds_rom_extension), ".nds");
  copy_cstr(cfg->gb_rom_extension, sizeof(cfg->gb_rom_extension), ".gb");
  cfg->locked_ids[0] = '\0';
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
    } else if (strcmp(section, "sync") == 0 && strcmp(key, "gba_save_dir") == 0) {
      copy_cstr(cfg->gba_save_dir, sizeof(cfg->gba_save_dir), value);
    } else if (strcmp(section, "sync") == 0 && strcmp(key, "nds_save_dir") == 0) {
      copy_cstr(cfg->nds_save_dir, sizeof(cfg->nds_save_dir), value);
    } else if (strcmp(section, "sync") == 0 && strcmp(key, "gb_save_dir") == 0) {
      copy_cstr(cfg->gb_save_dir, sizeof(cfg->gb_save_dir), value);
    } else if (strcmp(section, "sync") == 0 && strcmp(key, "vc_save_dir") == 0) {
      copy_cstr(cfg->vc_save_dir, sizeof(cfg->vc_save_dir), value);
    } else if (strcmp(section, "rom") == 0 && strcmp(key, "rom_dir") == 0) {
      copy_cstr(cfg->rom_dir, sizeof(cfg->rom_dir), value);
      /* Avoid sdmc:/foo/ + /stem.gba -> double slash (some paths fail to open). */
      {
        size_t n = strlen(cfg->rom_dir);
        while (n > 0 && (cfg->rom_dir[n - 1] == '/' || cfg->rom_dir[n - 1] == '\\')) {
          cfg->rom_dir[--n] = '\0';
        }
      }
    } else if (strcmp(section, "rom") == 0 && strcmp(key, "rom_extension") == 0) {
      copy_cstr(cfg->rom_extension, sizeof(cfg->rom_extension), value);
    } else if (strcmp(section, "rom") == 0 && strcmp(key, "gba_rom_dir") == 0) {
      copy_cstr(cfg->gba_rom_dir, sizeof(cfg->gba_rom_dir), value);
    } else if (strcmp(section, "rom") == 0 && strcmp(key, "nds_rom_dir") == 0) {
      copy_cstr(cfg->nds_rom_dir, sizeof(cfg->nds_rom_dir), value);
    } else if (strcmp(section, "rom") == 0 && strcmp(key, "gb_rom_dir") == 0) {
      copy_cstr(cfg->gb_rom_dir, sizeof(cfg->gb_rom_dir), value);
    } else if (strcmp(section, "rom") == 0 && strcmp(key, "gba_rom_extension") == 0) {
      copy_cstr(cfg->gba_rom_extension, sizeof(cfg->gba_rom_extension), value);
    } else if (strcmp(section, "rom") == 0 && strcmp(key, "nds_rom_extension") == 0) {
      copy_cstr(cfg->nds_rom_extension, sizeof(cfg->nds_rom_extension), value);
    } else if (strcmp(section, "rom") == 0 && strcmp(key, "gb_rom_extension") == 0) {
      copy_cstr(cfg->gb_rom_extension, sizeof(cfg->gb_rom_extension), value);
    } else if (strcmp(section, "sync") == 0 && strcmp(key, "locked_ids") == 0) {
      copy_cstr(cfg->locked_ids, sizeof(cfg->locked_ids), value);
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

static void strip_trailing_slashes_buf(char* s) {
  if (!s) return;
  size_t n = strlen(s);
  while (n > 0 && (s[n - 1] == '/' || s[n - 1] == '\\')) {
    s[n - 1] = '\0';
    n--;
  }
}

static int build_save_roots(const AppConfig* cfg, SaveRoot* out, int max_out) {
  if (is_vc_mode(cfg)) {
    if (max_out < 1) return 0;
    out[0].kind = ROOT_GBA;
    copy_cstr(out[0].save_dir, sizeof(out[0].save_dir), cfg->vc_save_dir);
    copy_cstr(out[0].rom_dir, sizeof(out[0].rom_dir), cfg->rom_dir);
    copy_cstr(out[0].rom_extension, sizeof(out[0].rom_extension), cfg->rom_extension);
    strip_trailing_slashes_buf(out[0].save_dir);
    strip_trailing_slashes_buf(out[0].rom_dir);
    return 1;
  }
  const int multi = (cfg->gba_save_dir[0] || cfg->nds_save_dir[0] || cfg->gb_save_dir[0]);
  int n = 0;
  if (multi) {
    if (cfg->gba_save_dir[0] && n < max_out) {
      out[n].kind = ROOT_GBA;
      copy_cstr(out[n].save_dir, sizeof(out[n].save_dir), cfg->gba_save_dir);
      copy_cstr(out[n].rom_dir, sizeof(out[n].rom_dir), cfg->gba_rom_dir);
      copy_cstr(out[n].rom_extension, sizeof(out[n].rom_extension),
                cfg->gba_rom_extension[0] ? cfg->gba_rom_extension : ".gba");
      strip_trailing_slashes_buf(out[n].save_dir);
      strip_trailing_slashes_buf(out[n].rom_dir);
      n++;
    }
    if (cfg->nds_save_dir[0] && n < max_out) {
      out[n].kind = ROOT_NDS;
      copy_cstr(out[n].save_dir, sizeof(out[n].save_dir), cfg->nds_save_dir);
      copy_cstr(out[n].rom_dir, sizeof(out[n].rom_dir), cfg->nds_rom_dir);
      copy_cstr(out[n].rom_extension, sizeof(out[n].rom_extension),
                cfg->nds_rom_extension[0] ? cfg->nds_rom_extension : ".nds");
      strip_trailing_slashes_buf(out[n].save_dir);
      strip_trailing_slashes_buf(out[n].rom_dir);
      n++;
    }
    if (cfg->gb_save_dir[0] && n < max_out) {
      out[n].kind = ROOT_GB;
      copy_cstr(out[n].save_dir, sizeof(out[n].save_dir), cfg->gb_save_dir);
      copy_cstr(out[n].rom_dir, sizeof(out[n].rom_dir), cfg->gb_rom_dir);
      copy_cstr(out[n].rom_extension, sizeof(out[n].rom_extension),
                cfg->gb_rom_extension[0] ? cfg->gb_rom_extension : ".gb");
      strip_trailing_slashes_buf(out[n].save_dir);
      strip_trailing_slashes_buf(out[n].rom_dir);
      n++;
    }
    if (n == 0 && max_out >= 1) {
      out[0].kind = ROOT_GBA;
      copy_cstr(out[0].save_dir, sizeof(out[0].save_dir), cfg->save_dir);
      copy_cstr(out[0].rom_dir, sizeof(out[0].rom_dir), cfg->rom_dir);
      copy_cstr(out[0].rom_extension, sizeof(out[0].rom_extension), cfg->rom_extension);
      strip_trailing_slashes_buf(out[0].save_dir);
      strip_trailing_slashes_buf(out[0].rom_dir);
      return 1;
    }
    return n;
  }
  if (n < max_out) {
    out[n].kind = ROOT_GBA;
    copy_cstr(out[n].save_dir, sizeof(out[n].save_dir), cfg->save_dir);
    copy_cstr(out[n].rom_dir, sizeof(out[n].rom_dir), cfg->rom_dir);
    copy_cstr(out[n].rom_extension, sizeof(out[n].rom_extension), cfg->rom_extension);
    strip_trailing_slashes_buf(out[n].save_dir);
    strip_trailing_slashes_buf(out[n].rom_dir);
    n++;
  }
  return n;
}

static void ensure_save_roots_exist(const AppConfig* cfg) {
  SaveRoot roots[8];
  int nr = build_save_roots(cfg, roots, 8);
  for (int i = 0; i < nr; i++) {
    ensure_directory_exists(roots[i].save_dir);
  }
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

static bool game_id_from_gba_rom_header_bytes(const unsigned char* data, size_t len, char* out, size_t out_size) {
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

/* NDS cartridge header @ 0x00 title (12), 0x0C game code (4) — mirror bridge/game_id.py */
static bool game_id_from_nds_rom_header_bytes(const unsigned char* data, size_t len, char* out, size_t out_size) {
  if (!data || len < 0x10) return false;
  char title[32] = {0};
  char code[8] = {0};
  char combined[64] = {0};
  decode_header_field(data + 0x00, 12, title, sizeof(title));
  decode_header_field(data + 0x0C, 4, code, sizeof(code));
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

static size_t read_rom_prefix(const char* path, unsigned char* out, size_t out_sz) {
  FILE* fp = fopen(path, "rb");
  if (!fp) return 0;
  size_t n = fread(out, 1, out_sz, fp);
  fclose(fp);
  return n;
}

static const char* default_rom_ext_for_kind(SaveRootKind k) {
  switch (k) {
    case ROOT_GBA:
      return ".gba";
    case ROOT_NDS:
      return ".nds";
    case ROOT_GB:
      return ".gb";
  }
  return ".gba";
}

/** Normalize one comma-separated extension token into ``out`` (leading dot). */
static void normalize_rom_ext_token(char* tok, char* out, size_t out_sz) {
  while (*tok == ' ' || *tok == '\t') tok++;
  size_t len = strlen(tok);
  while (len > 0 && (tok[len - 1] == ' ' || tok[len - 1] == '\t')) {
    tok[len - 1] = '\0';
    len--;
  }
  if (!tok[0]) {
    out[0] = '\0';
    return;
  }
  if (tok[0] != '.') {
    snprintf(out, out_sz, ".%s", tok);
  } else {
    copy_cstr(out, out_sz, tok);
  }
}

static void resolve_game_id_for_save_root(const SaveRoot* root, const char* save_stem, char* out, size_t out_size) {
  if (!root->rom_dir[0]) {
    sanitize_game_id(save_stem, out, out_size);
    return;
  }
  char list_buf[256];
  if (root->rom_extension[0]) {
    copy_cstr(list_buf, sizeof(list_buf), root->rom_extension);
  } else {
    copy_cstr(list_buf, sizeof(list_buf), default_rom_ext_for_kind(root->kind));
  }
  char work[256];
  copy_cstr(work, sizeof(work), list_buf);
  char* saveptr = NULL;
  char* token = strtok_r(work, ",", &saveptr);
  while (token) {
    char ext[32];
    normalize_rom_ext_token(token, ext, sizeof(ext));
    if (ext[0] != '\0') {
      char rom_path[640];
      snprintf(rom_path, sizeof(rom_path), "%s/%s%s", root->rom_dir, save_stem, ext);
      unsigned char rom_hdr[512];
      const size_t got = read_rom_prefix(rom_path, rom_hdr, sizeof(rom_hdr));
      if (root->kind == ROOT_NDS) {
        if (got >= 0x10 && game_id_from_nds_rom_header_bytes(rom_hdr, got, out, out_size)) return;
      } else if (root->kind == ROOT_GBA) {
        if (got >= 0xB0 && game_id_from_gba_rom_header_bytes(rom_hdr, got, out, out_size)) return;
      }
    }
    token = strtok_r(NULL, ",", &saveptr);
  }
  /* GB / fallback: stem only */
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
    snprintf(tmp_path, sizeof(tmp_path), "%s/.gbasync-%llu.tmp", parent_dir, tick);
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

static void baseline_file_path(char* out, size_t out_sz, const char* save_dir, const char* filename) {
  join_path(out, out_sz, save_dir, filename);
}

static int baseline_load_from_path(const char* path, BaselineRow* rows, int max_rows) {
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

static int baseline_load(const char* save_dir, BaselineRow* rows, int max_rows) {
  char new_path[512];
  baseline_file_path(new_path, sizeof(new_path), save_dir, ".gbasync-baseline");
  int n = baseline_load_from_path(new_path, rows, max_rows);
  if (n > 0) return n;
  char old_path[512];
  baseline_file_path(old_path, sizeof(old_path), save_dir, ".savesync-baseline");
  return baseline_load_from_path(old_path, rows, max_rows);
}

static bool baseline_save_to_path(const char* path, BaselineRow* rows, int n) {
  char tmp[520];
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

static bool baseline_save(const char* save_dir, BaselineRow* rows, int n) {
  char new_path[512];
  baseline_file_path(new_path, sizeof(new_path), save_dir, ".gbasync-baseline");
  bool ok = baseline_save_to_path(new_path, rows, n);
  if (!ok) return false;
  // Write legacy filename too so older client builds stay compatible during migration.
  char old_path[512];
  baseline_file_path(old_path, sizeof(old_path), save_dir, ".savesync-baseline");
  (void)baseline_save_to_path(old_path, rows, n);
  return true;
}

static void id_map_file_path(char* out, size_t out_sz, const char* save_dir) {
  join_path(out, out_sz, save_dir, ".gbasync-idmap");
}

static int id_map_find(IdMapRow* rows, int n, const char* stem) {
  for (int i = 0; i < n; i++) {
    if (strcmp(rows[i].save_stem, stem) == 0) return i;
  }
  return -1;
}

static int id_map_load(const char* save_dir, IdMapRow* rows, int max_rows) {
  char path[512];
  id_map_file_path(path, sizeof(path), save_dir);
  FILE* fp = fopen(path, "r");
  if (!fp) return 0;
  int n = 0;
  char line[520];
  while (n < max_rows && fgets(line, sizeof(line), fp)) {
    char* tab = strchr(line, '\t');
    if (!tab) continue;
    *tab = '\0';
    char* gid = tab + 1;
    trim_line(line);
    trim_line(gid);
    if (line[0] == '\0' || gid[0] == '\0') continue;
    copy_cstr(rows[n].save_stem, sizeof(rows[n].save_stem), line);
    copy_cstr(rows[n].game_id, sizeof(rows[n].game_id), gid);
    n++;
  }
  fclose(fp);
  return n;
}

static bool id_map_save(const char* save_dir, IdMapRow* rows, int n) {
  char path[512], tmp[520];
  id_map_file_path(path, sizeof(path), save_dir);
  if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) return false;
  FILE* fp = fopen(tmp, "w");
  if (!fp) return false;
  for (int i = 0; i < n; i++) {
    fprintf(fp, "%s\t%s\n", rows[i].save_stem, rows[i].game_id);
  }
  fclose(fp);
  remove(path);
  if (rename(tmp, path) != 0) {
    remove(tmp);
    return false;
  }
  return true;
}

static bool id_map_upsert(IdMapRow* rows, int* n, int max_rows, const char* stem, const char* gid) {
  int idx = id_map_find(rows, *n, stem);
  if (idx >= 0) {
    if (strcmp(rows[idx].game_id, gid) == 0) return false;
    copy_cstr(rows[idx].game_id, sizeof(rows[idx].game_id), gid);
    return true;
  }
  if (*n >= max_rows) return false;
  copy_cstr(rows[*n].save_stem, sizeof(rows[*n].save_stem), stem);
  copy_cstr(rows[*n].game_id, sizeof(rows[*n].game_id), gid);
  (*n)++;
  return true;
}

static const char* baseline_find(const BaselineRow* rows, int n, const char* game_id) {
  for (int i = 0; i < n; i++) {
    if (strcmp(rows[i].game_id, game_id) == 0) return rows[i].sha256;
  }
  return NULL;
}

/* Returns true if a row was added or the stored sha changed. */
static bool baseline_upsert(BaselineRow* rows, int* n, int max_n, const char* game_id, const char* sha256) {
  for (int i = 0; i < *n; i++) {
    if (strcmp(rows[i].game_id, game_id) == 0) {
      if (strcmp(rows[i].sha256, sha256) == 0) return false;
      copy_cstr(rows[i].sha256, sizeof(rows[i].sha256), sha256);
      return true;
    }
  }
  if (*n < max_n) {
    copy_cstr(rows[*n].game_id, sizeof(rows[*n].game_id), game_id);
    copy_cstr(rows[*n].sha256, sizeof(rows[*n].sha256), sha256);
    (*n)++;
    return true;
  }
  return false;
}

static int baseline_load_merged(const AppConfig* cfg, BaselineRow* rows, int max_rows) {
  SaveRoot roots[8];
  const int nr = build_save_roots(cfg, roots, 8);
  BaselineRow* tmp = (BaselineRow*)calloc((size_t)MAX_SAVES, sizeof(BaselineRow));
  if (!tmp) return 0;
  int n = 0;
  for (int i = 0; i < nr; i++) {
    const int m = baseline_load(roots[i].save_dir, tmp, MAX_SAVES);
    for (int j = 0; j < m; j++) {
      baseline_upsert(rows, &n, max_rows, tmp[j].game_id, tmp[j].sha256);
    }
  }
  free(tmp);
  return n;
}

/* roots/nr must come from a single build_save_roots — avoids O(n_rows) redundant rebuilds in baseline_save_merged. */
static const char* pick_baseline_root_for_game_roots(
    SaveRoot* roots,
    int nr,
    const char* game_id,
    LocalSave* locals,
    int n_local) {
  static char out_buf[256];
  if (nr <= 0) {
    out_buf[0] = '\0';
    return out_buf;
  }
  for (int i = 0; i < n_local; i++) {
    if (strcmp(locals[i].game_id, game_id) != 0) continue;
    const char* best = roots[0].save_dir;
    size_t best_len = 0;
    for (int j = 0; j < nr; j++) {
      char r[256];
      copy_cstr(r, sizeof(r), roots[j].save_dir);
      strip_trailing_slashes_buf(r);
      const size_t plen = strlen(r);
      if (plen > best_len && strncmp(locals[i].path, r, plen) == 0 &&
          (locals[i].path[plen] == '\0' || locals[i].path[plen] == '/')) {
        best = roots[j].save_dir;
        best_len = plen;
      }
    }
    copy_cstr(out_buf, sizeof(out_buf), best);
    return out_buf;
  }
  copy_cstr(out_buf, sizeof(out_buf), roots[0].save_dir);
  return out_buf;
}

static int save_dir_cmp(const char* a, const char* b) {
  char x[256];
  char y[256];
  copy_cstr(x, sizeof(x), a);
  copy_cstr(y, sizeof(y), b);
  strip_trailing_slashes_buf(x);
  strip_trailing_slashes_buf(y);
  return strcmp(x, y);
}

static bool baseline_save_merged(
    const AppConfig* cfg,
    BaselineRow* rows,
    int n,
    LocalSave* locals,
    int n_local) {
  SaveRoot roots[8];
  const int nr = build_save_roots(cfg, roots, 8);
  if (nr <= 0) return false;
  BaselineRow* subset = (BaselineRow*)calloc((size_t)MAX_SAVES, sizeof(BaselineRow));
  if (!subset) return false;
  bool ok = true;
  for (int i = 0; i < nr; i++) {
    int ns = 0;
    for (int j = 0; j < n; j++) {
      const char* want =
          pick_baseline_root_for_game_roots(roots, nr, rows[j].game_id, locals, n_local);
      if (save_dir_cmp(want, roots[i].save_dir) == 0) {
        baseline_upsert(subset, &ns, MAX_SAVES, rows[j].game_id, rows[j].sha256);
      }
    }
    if (ns > 0) {
      if (!baseline_save(roots[i].save_dir, subset, ns)) ok = false;
    }
    ns = 0;
    memset(subset, 0, (size_t)MAX_SAVES * sizeof(BaselineRow));
  }
  free(subset);
  return ok;
}

static void first_save_dir_for_status(const AppConfig* cfg, char* out, size_t out_sz) {
  SaveRoot roots[8];
  const int nr = build_save_roots(cfg, roots, 8);
  if (nr > 0) {
    copy_cstr(out, out_sz, roots[0].save_dir);
  } else {
    copy_cstr(out, out_sz, cfg->save_dir);
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

static bool http_request_once(
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

/** Retries transient TCP failures (3DS Wi‑Fi); 3 attempts with 1s backoff. */
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
  const int max_attempts = 3;
  for (int attempt = 1; attempt <= max_attempts; attempt++) {
    if (http_request_once(
            cfg, method, target_path, content_type, body, body_len, out_status, out_body, out_body_len))
      return true;
    if (attempt < max_attempts) svcSleepThread(1000000000ULL);
  }
  return false;
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
    const char* next_gid = strstr(gid + 12, "\"game_id\":\"");
    size_t seg_len = next_gid ? (size_t)(next_gid - gid) : strlen(gid);
    char seg[4096];
    if (seg_len >= sizeof(seg)) seg_len = sizeof(seg) - 1;
    memcpy(seg, gid, seg_len);
    seg[seg_len] = '\0';
    RemoteSave item;
    memset(&item, 0, sizeof(item));
    json_extract_string(seg, "game_id", item.game_id, sizeof(item.game_id));
    json_extract_string(seg, "last_modified_utc", item.last_modified_utc, sizeof(item.last_modified_utc));
    json_extract_string(seg, "server_updated_at", item.server_updated_at, sizeof(item.server_updated_at));
    json_extract_string(seg, "sha256", item.sha256, sizeof(item.sha256));
    json_extract_string(seg, "filename_hint", item.filename_hint, sizeof(item.filename_hint));
    json_extract_string(seg, "display_name", item.display_name, sizeof(item.display_name));
    out[count++] = item;
    p = next_gid ? next_gid : gid + strlen(gid);
  }
  return count;
}

#define MAX_HISTORY_FILES 32
#define HISTORY_NAME_LEN 256

typedef struct {
  char filename[HISTORY_NAME_LEN];
  char display_name[128];
  int keep;
} HistoryRow3ds;

static int parse_history_rows_c(const char* json, HistoryRow3ds* rows, int max_rows) {
  static char chunk_buf[2048];
  int n = 0;
  const char* p = json;
  while (n < max_rows) {
    const char* f = strstr(p, "\"filename\":\"");
    if (!f) break;
    const char* next_f = strstr(f + 14, "\"filename\":\"");
    const char* chunk_end = next_f ? next_f : (json + strlen(json));
    size_t chunk_len = (size_t)(chunk_end - f);
    if (chunk_len >= sizeof(chunk_buf)) chunk_len = sizeof(chunk_buf) - 1;
    memcpy(chunk_buf, f, chunk_len);
    chunk_buf[chunk_len] = '\0';

    f += 12;
    const char* e = strchr(f, '"');
    if (!e) break;
    {
      size_t len = (size_t)(e - f);
      if (len >= HISTORY_NAME_LEN) len = HISTORY_NAME_LEN - 1;
      memcpy(rows[n].filename, f, len);
      rows[n].filename[len] = '\0';
    }

    rows[n].display_name[0] = '\0';
    if (strstr(chunk_buf, "\"display_name\":null") == NULL) {
      const char* d = strstr(chunk_buf, "\"display_name\":\"");
      if (d) {
        d += 16;
        const char* de = strchr(d, '"');
        if (de) {
          size_t dl = (size_t)(de - d);
          if (dl >= sizeof(rows[n].display_name)) dl = sizeof(rows[n].display_name) - 1;
          memcpy(rows[n].display_name, d, dl);
          rows[n].display_name[dl] = '\0';
        }
      }
    }

    rows[n].keep = 0;
    {
      const char* kp = strstr(chunk_buf, "\"keep\"");
      if (kp) {
        const char* colon = strchr(kp, ':');
        if (colon) {
          colon++;
          while (*colon == ' ' || *colon == '\t') {
            colon++;
          }
          if (strncmp(colon, "true", 4) == 0) {
            rows[n].keep = 1;
          }
        }
      }
    }

    n++;
    p = chunk_end;
  }
  return n;
}

/** Escape `s` as a JSON string (ASCII printable + quote/backslash escapes). Returns -1 on overflow or invalid char. */
static int json_quote_cstr(const char* s, char* out, size_t out_sz) {
  size_t j = 0;
  if (!s || !out || out_sz < 4) return -1;
  out[j++] = '"';
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    if (c == '"' || c == '\\') {
      if (j + 2 >= out_sz) return -1;
      out[j++] = '\\';
    } else if (c < 32 || c > 126) {
      return -1;
    }
    if (j + 1 >= out_sz) return -1;
    out[j++] = (char)c;
  }
  if (j + 1 >= out_sz) return -1;
  out[j++] = '"';
  out[j] = '\0';
  return (int)j;
}

static bool history_keep_patch_3ds(const AppConfig* cfg, const char* game_id, const char* filename, int keep) {
  char quoted[512];
  if (json_quote_cstr(filename, quoted, sizeof(quoted)) < 0) return false;
  char body_buf[640];
  int bl = snprintf(
      body_buf,
      sizeof(body_buf),
      "{\"filename\":%s,\"keep\":%s}",
      quoted,
      keep ? "true" : "false");
  if (bl <= 0 || (size_t)bl >= sizeof(body_buf)) return false;
  char path[384];
  snprintf(path, sizeof(path), "/save/%s/history/revision/keep", game_id);
  int st = 0;
  unsigned char* resp = NULL;
  size_t resp_len = 0;
  bool ok = http_request(
                cfg,
                "PATCH",
                path,
                "application/json",
                (const unsigned char*)body_buf,
                (size_t)bl,
                &st,
                &resp,
                &resp_len)
            && st == 200;
  free(resp);
  return ok;
}

static bool history_restore_3ds(const AppConfig* cfg, const char* game_id, const char* filename) {
  char body[512];
  int bl = snprintf(body, sizeof(body), "{\"filename\":\"%s\"}", filename);
  if (bl <= 0 || (size_t)bl >= sizeof(body)) return false;
  char path[384];
  snprintf(path, sizeof(path), "/save/%s/restore", game_id);
  int st = 0;
  unsigned char* resp = NULL;
  size_t resp_len = 0;
  bool ok = http_request(
                cfg,
                "POST",
                path,
                "application/json",
                (const unsigned char*)body,
                (size_t)bl,
                &st,
                &resp,
                &resp_len)
            && st == 200;
  free(resp);
  return ok;
}

static void save_history_picker_3ds(AppConfig* cfg, const char* game_id) {
  char path[384];
  snprintf(path, sizeof(path), "/save/%s/history", game_id);
  int status = 0;
  unsigned char* body = NULL;
  size_t body_len = 0;
  if (!http_request(cfg, "GET", path, NULL, NULL, 0, &status, &body, &body_len) || status != 200) {
    consoleClear();
    printf("History: GET failed");
    if (status > 0) printf(" (HTTP %d)", status);
    printf("\nB: back\n");
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
    free(body);
    while (aptMainLoop()) {
      hidScanInput();
      if (hidKeysDown() & KEY_B) break;
      gfxFlushBuffers();
      gfxSwapBuffers();
      gspWaitForVBlank();
    }
    return;
  }
  HistoryRow3ds* rows = (HistoryRow3ds*)calloc((size_t)MAX_HISTORY_FILES, sizeof(HistoryRow3ds));
  if (!rows) {
    free(body);
    consoleClear();
    printf("History: out of memory\nB: back\n");
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
    while (aptMainLoop()) {
      hidScanInput();
      if (hidKeysDown() & KEY_B) break;
      gfxFlushBuffers();
      gfxSwapBuffers();
      gspWaitForVBlank();
    }
    return;
  }
  int n_names = parse_history_rows_c((const char*)body, rows, MAX_HISTORY_FILES);
  free(body);
  if (n_names <= 0) {
    free(rows);
    consoleClear();
    printf("No history backups for this game.\nB: back\n");
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
    while (aptMainLoop()) {
      hidScanInput();
      if (hidKeysDown() & KEY_B) break;
      gfxFlushBuffers();
      gfxSwapBuffers();
      gspWaitForVBlank();
    }
    return;
  }

  int cursor = 0;
  int scroll = 0;
  const int kVisibleEntries = 6;
  const int total = n_names;
  bool dirty = true;
  while (aptMainLoop()) {
    hidScanInput();
    u32 kDown = hidKeysDown();
    if (kDown & KEY_B) break;
    if (kDown & KEY_R) {
      int new_keep = rows[cursor].keep ? 0 : 1;
      if (history_keep_patch_3ds(cfg, game_id, rows[cursor].filename, new_keep)) {
        rows[cursor].keep = new_keep;
        dirty = true;
      }
      continue;
    }
    if (kDown & KEY_A) {
      consoleClear();
      printf("Restore this version?\n");
      if (rows[cursor].display_name[0] != '\0') printf("Label: %s\n", rows[cursor].display_name);
      printf("Keep: %s\n", rows[cursor].keep ? "yes" : "no");
      printf("File: %s\n", rows[cursor].filename);
      printf("A: confirm  B: cancel\n");
      gfxFlushBuffers();
      gfxSwapBuffers();
      gspWaitForVBlank();
      while (aptMainLoop()) {
        hidScanInput();
        u32 k2 = hidKeysDown();
        if (k2 & KEY_B) break;
        if (k2 & KEY_A) {
          if (history_restore_3ds(cfg, game_id, rows[cursor].filename)) {
            consoleClear();
            printf("Restored. Download this game to update device.\nB: back\n");
          } else {
            consoleClear();
            printf("Restore failed.\nB: back\n");
          }
          gfxFlushBuffers();
          gfxSwapBuffers();
          gspWaitForVBlank();
          while (aptMainLoop()) {
            hidScanInput();
            if (hidKeysDown() & KEY_B) {
              free(rows);
              return;
            }
            gfxFlushBuffers();
            gfxSwapBuffers();
            gspWaitForVBlank();
          }
          free(rows);
          return;
        }
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
      }
      dirty = true;
      continue;
    }
    if (kDown & KEY_DUP) {
      cursor = (cursor + total - 1) % total;
      dirty = true;
    }
    if (kDown & KEY_DDOWN) {
      cursor = (cursor + 1) % total;
      dirty = true;
    }
    if (cursor < scroll) scroll = cursor;
    if (cursor >= scroll + kVisibleEntries) scroll = cursor - kVisibleEntries + 1;
    if (scroll < 0) scroll = 0;
    {
      int max_scroll = total > kVisibleEntries ? total - kVisibleEntries : 0;
      if (scroll > max_scroll) scroll = max_scroll;
    }

    if (!dirty) {
      gfxFlushBuffers();
      gfxSwapBuffers();
      gspWaitForVBlank();
      continue;
    }

    consoleClear();
    printf("--- History ---\n\n");
    {
      const int kMenuLeftCol = 22;
      printf("%-*s%s\n", kMenuLeftCol, "UP/DOWN: move", "");
      printf("%-*s%s\n", kMenuLeftCol, "A: restore", "");
      printf("%-*s%s\n", kMenuLeftCol, "R: keep / unkeep", "");
      printf("%-*s%s\n", kMenuLeftCol, "B: back", "");
    }
    printf("\n");
    {
      int row;
      for (row = scroll; row < scroll + kVisibleEntries && row < total; row++) {
        char mark = (row == cursor) ? '>' : ' ';
        const char* lbl = (rows[row].display_name[0] != '\0') ? rows[row].display_name : "-";
        const char* kpre = rows[row].keep ? "[KEEP] " : "";
        printf("%c%s%.40s\n", mark, kpre, lbl);
        printf("  %.48s\n", rows[row].filename);
        printf("\n");
      }
    }
    dirty = false;
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
  }
  free(rows);
}

static bool local_game_id_exists_in_dir(
    LocalSave* local, int dir_start, int dir_end, const char* game_id) {
  for (int i = dir_start; i < dir_end; i++) {
    if (strcmp(local[i].game_id, game_id) == 0) return true;
  }
  return false;
}

static int cmp_name_rows(const void* a, const void* b) {
  return strcmp((const char*)a, (const char*)b);
}

static int scan_local_saves(
    const SaveRoot* root,
    const char* dir,
    LocalSave* out,
    int start_offset,
    int max_items) {
  if (start_offset >= max_items) return 0;
  DIR* d = opendir(dir);
  if (!d) {
    printf("ERROR: cannot open save_dir: %s (errno=%d)\n", dir, errno);
    return 0;
  }
  IdMapRow* id_map = (IdMapRow*)calloc(1024, sizeof(IdMapRow));
  if (!id_map) {
    closedir(d);
    printf("ERROR: out of memory (id map)\n");
    return 0;
  }
  int n_id_map = id_map_load(dir, id_map, 1024);
  bool id_map_changed = false;
  char(*sav_names)[256] = (char(*)[256])calloc((size_t)MAX_SAVES, sizeof(*sav_names));
  if (!sav_names) {
    free(id_map);
    closedir(d);
    printf("ERROR: out of memory (save list)\n");
    return 0;
  }
  int n_sav_names = 0;
  int count = start_offset;
  int sav_candidates = 0;
  int read_failures = 0;
  int stat_failures = 0;
  struct dirent* ent;
  while ((ent = readdir(d)) != NULL && n_sav_names < max_items && n_sav_names < MAX_SAVES) {
    if (!has_sav_extension(ent->d_name)) continue;
    copy_cstr(sav_names[n_sav_names], sizeof(sav_names[n_sav_names]), ent->d_name);
    n_sav_names++;
  }
  closedir(d);
  qsort(sav_names, (size_t)n_sav_names, sizeof(sav_names[0]), cmp_name_rows);

  for (int ni = 0; ni < n_sav_names && count < max_items; ni++) {
    sav_candidates++;
    LocalSave item;
    memset(&item, 0, sizeof(item));
    copy_cstr(item.name, sizeof(item.name), sav_names[ni]);
    size_t dir_len = strlen(dir);
    bool has_trailing_slash = dir_len > 0 && dir[dir_len - 1] == '/';
    snprintf(item.path, sizeof(item.path), "%s%s%s", dir, has_trailing_slash ? "" : "/", item.name);
    char stem[256];
    copy_cstr(stem, sizeof(stem), item.name);
    char* dot = strrchr(stem, '.');
    if (dot) *dot = '\0';
    int map_idx = id_map_find(id_map, n_id_map, stem);
    if (map_idx >= 0 && id_map[map_idx].game_id[0] != '\0') {
      copy_cstr(item.game_id, sizeof(item.game_id), id_map[map_idx].game_id);
    } else {
      resolve_game_id_for_save_root(root, stem, item.game_id, sizeof(item.game_id));
    }
    if (local_game_id_exists_in_dir(out, start_offset, count, item.game_id)) {
      /* Same directory only — do not treat IDs from other save roots as collisions. */
      char base_id[128];
      sanitize_game_id(stem, base_id, sizeof(base_id));
      if (base_id[0] == '\0') copy_cstr(base_id, sizeof(base_id), "unknown-game");
      char base_short[120];
      copy_cstr(base_short, sizeof(base_short), base_id);
      copy_cstr(item.game_id, sizeof(item.game_id), base_short);
      int suffix = 2;
      while (local_game_id_exists_in_dir(out, start_offset, count, item.game_id) && suffix < 1000) {
        snprintf(item.game_id, sizeof(item.game_id), "%s-%d", base_short, suffix++);
      }
    }
    if (id_map_upsert(id_map, &n_id_map, 1024, stem, item.game_id)) {
      id_map_changed = true;
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
  if (id_map_changed) {
    (void)id_map_save(dir, id_map, n_id_map);
  }
  free(sav_names);
  free(id_map);
  if (sav_candidates == 0) {
    printf("No .sav files found in %s\n", dir);
  } else if (count == start_offset) {
    printf(
        "No usable .sav files in %s (%d seen; stat_fail=%d read_fail=%d)\n",
        dir,
        sav_candidates,
        stat_failures,
        read_failures);
  }
  return count - start_offset;
}

static int scan_all_local_saves(const AppConfig* cfg, LocalSave* out, int max_items) {
  SaveRoot roots[8];
  const int nr = build_save_roots(cfg, roots, 8);
  int total = 0;
  for (int i = 0; i < nr && total < max_items; i++) {
    total += scan_local_saves(&roots[i], roots[i].save_dir, out, total, max_items);
  }
  return total;
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
    const char* source_tag,
    char* out_uploaded_sha65) {
  unsigned char* bytes = NULL;
  size_t len = 0;
  if (!read_file_bytes(l->path, &bytes, &len)) {
    printf("%s: ERROR(read)\n", l->game_id);
    return false;
  }
  char computed_sha[65];
  sha256_hash(bytes, len, computed_sha);
  char ts_q[80], hash_q[80], filename_q[400], clk_q[80], path[1200];
  char ts_wall[40];
  mtime_to_utc_iso(time(NULL), ts_wall, sizeof(ts_wall));
  url_encode_simple(ts_wall, ts_q, sizeof(ts_q));
  url_encode_simple(computed_sha, hash_q, sizeof(hash_q));
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
      len,
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
    if (out_uploaded_sha65) memcpy(out_uploaded_sha65, computed_sha, 65);
    return true;
  }
  if (!json_body_has_applied_member(put_resp, put_len)) {
    free(put_resp);
    printf("%s: UPLOADED\n", l->game_id);
    if (out_uploaded_sha65) memcpy(out_uploaded_sha65, computed_sha, 65);
    return true;
  }
  if (!verify_server_has_sha(cfg, l->game_id, put_resp, put_len, computed_sha)) {
    free(put_resp);
    printf(
        "%s: REJECTED (could not confirm save on server — check URL/API key / response truncated)\n",
        l->game_id);
    return false;
  }
  free(put_resp);
  printf("%s: UPLOADED\n", l->game_id);
  if (out_uploaded_sha65) memcpy(out_uploaded_sha65, computed_sha, 65);
  return true;
}

static void pick_download_dir_3ds(
    const AppConfig* cfg,
    const char* game_id,
    const RemoteSave* r,
    LocalSave* locals,
    int n_local,
    char* out_dir,
    size_t out_dir_sz) {
  for (int i = 0; i < n_local; i++) {
    if (strcmp(locals[i].game_id, game_id) == 0) {
      const char* p = locals[i].path;
      const char* slash = strrchr(p, '/');
      if (slash && (size_t)(slash - p) + 1 < out_dir_sz) {
        memcpy(out_dir, p, (size_t)(slash - p));
        out_dir[slash - p] = '\0';
        return;
      }
    }
  }
  char filename[256];
  char fallback_name[160];
  snprintf(fallback_name, sizeof(fallback_name), "%.127s.sav", game_id);
  if (r->filename_hint[0] != '\0') {
    sanitize_filename(filename, sizeof(filename), r->filename_hint, fallback_name);
  } else {
    sanitize_filename(filename, sizeof(filename), fallback_name, fallback_name);
  }
  char stem[256];
  copy_cstr(stem, sizeof(stem), filename);
  char* dot = strrchr(stem, '.');
  if (dot) *dot = '\0';
  SaveRoot roots[8];
  const int nr = build_save_roots(cfg, roots, 8);
  for (int i = 0; i < nr; i++) {
    if (roots[i].rom_dir[0] == '\0') continue;
    char list_buf[256];
    if (roots[i].rom_extension[0]) {
      copy_cstr(list_buf, sizeof(list_buf), roots[i].rom_extension);
    } else {
      copy_cstr(list_buf, sizeof(list_buf), default_rom_ext_for_kind(roots[i].kind));
    }
    char work[256];
    copy_cstr(work, sizeof(work), list_buf);
    char* saveptr = NULL;
    char* token = strtok_r(work, ",", &saveptr);
    while (token) {
      char ext[32];
      normalize_rom_ext_token(token, ext, sizeof(ext));
      if (ext[0] != '\0') {
        char rom_path[700];
        snprintf(rom_path, sizeof(rom_path), "%s/%s%s", roots[i].rom_dir, stem, ext);
        FILE* f = fopen(rom_path, "rb");
        if (f) {
          fclose(f);
          copy_cstr(out_dir, out_dir_sz, roots[i].save_dir);
          return;
        }
      }
      token = strtok_r(NULL, ",", &saveptr);
    }
  }
  copy_cstr(out_dir, out_dir_sz, nr > 0 ? roots[0].save_dir : "");
}

static bool get_one_save(
    const AppConfig* cfg,
    const char* game_id,
    const RemoteSave* r,
    LocalSave* locals,
    int n_local) {
  char dest[256];
  pick_download_dir_3ds(cfg, game_id, r, locals, n_local, dest, sizeof(dest));
  char filename[256];
  char fallback_name[160];
  snprintf(fallback_name, sizeof(fallback_name), "%.127s.sav", game_id);
  if (r->filename_hint[0] != '\0') {
    sanitize_filename(filename, sizeof(filename), r->filename_hint, fallback_name);
  } else {
    sanitize_filename(filename, sizeof(filename), fallback_name, fallback_name);
  }
  char out_path[512];
  join_path(out_path, sizeof(out_path), dest, filename);

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

/* Both sides differ: no baseline yet, or both diverged from baseline — X/Y/B instead of SKIP. */
static void resolve_both_changed_conflict(
    const AppConfig* cfg,
    const char* source_tag,
    LocalSave* l,
    RemoteSave* r,
    const char* id,
    LocalSave* locals,
    int n_local,
    BaselineRow* baseline,
    int* n_baseline,
    SyncSummary* summary,
    int no_sync_history) {
  consoleClear();
  printf("\n");
  printf("  -------- Conflict --------\n");
  printf("\n");
  printf("  %s\n\n", id);
  if (no_sync_history) {
    printf("  No sync history on this device yet.\n");
    printf("  Choose which version to keep:\n\n");
  } else {
    printf("  Local and server both changed since\n");
    printf("  the last successful sync.\n\n");
  }
  printf("  X   Upload local (overwrite server)\n");
  printf("  Y   Download server (overwrite local)\n");
  printf("  B   Skip for now\n");
  printf("\n");
  while (aptMainLoop()) {
    hidScanInput();
    u32 kDown = hidKeysDown();
    if (kDown & KEY_X) {
      char uploaded_sha[65];
      if (put_one_save(cfg, l, true, source_tag, uploaded_sha)) {
        baseline_upsert(baseline, n_baseline, MAX_SAVES, id, uploaded_sha);
        if (summary) summary->uploads++;
      }
      break;
    }
    if (kDown & KEY_Y) {
      if (get_one_save(cfg, id, r, locals, n_local)) {
        baseline_upsert(baseline, n_baseline, MAX_SAVES, id, r->sha256);
        if (summary) summary->downloads++;
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
  LocalSave* local = (LocalSave*)calloc(MAX_SAVES, sizeof(LocalSave));
  if (!local) return false;
  ensure_save_roots_exist(cfg);
  int n = scan_all_local_saves(cfg, local, MAX_SAVES);
  if (n <= 0) {
    printf("No local .sav files to upload.\n");
    free(local);
    return false;
  }

  RemoteSave* remote_meta = (RemoteSave*)calloc(MAX_SAVES, sizeof(RemoteSave));
  int remote_meta_count = 0;
  if (remote_meta) {
    int st = 0;
    unsigned char* body = NULL;
    size_t blen = 0;
    if (http_request(cfg, "GET", "/saves", NULL, NULL, 0, &st, &body, &blen) && st == 200) {
      remote_meta_count = parse_remote_saves((const char*)body, remote_meta, MAX_SAVES);
    }
    free(body);
  }

  if (remote_meta_count > 0) {
    g_upload_sort_remote = remote_meta;
    g_upload_sort_rc = remote_meta_count;
    qsort(local, (size_t)n, sizeof(LocalSave), cmp_local_for_upload_order);
    g_upload_sort_remote = NULL;
    g_upload_sort_rc = 0;
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
    const u32 confirm_upload = KEY_START | KEY_X;

    if (kDown & confirm_upload) {
      if (master_all) {
        manual_filter_free(out);
        out->all = 1;
        g_upload_sort_remote = NULL;
        g_upload_sort_rc = 0;
        free(remote_meta);
        free(local);
        return true;
      }
      manual_filter_free(out);
      out->ids = calloc(MAX_SAVES, 128);
      if (!out->ids) {
        g_upload_sort_remote = NULL;
        g_upload_sort_rc = 0;
        free(remote_meta);
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
      g_upload_sort_remote = NULL;
      g_upload_sort_rc = 0;
      free(remote_meta);
      free(local);
      return true;
    }
    if (kDown & KEY_B) {
      g_upload_sort_remote = NULL;
      g_upload_sort_rc = 0;
      free(remote_meta);
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
      printf("DPad: move  A: toggle  START/X: run  B: back to menu\n\n");
      for (int row = scroll; row < scroll + kVisible && row < total_rows; row++) {
        char mark = (row == cursor) ? '>' : ' ';
        if (row == 0) {
          printf("%c [%c] ALL SAVES\n", mark, master_all ? 'x' : ' ');
        } else {
          const LocalSave* L = &local[row - 1];
          const char* disp = L->game_id;
          if (remote_meta && remote_meta_count > 0) {
            const RemoteSave* r = find_remote_by_id(remote_meta, remote_meta_count, L->game_id);
            if (r && r->display_name[0] != '\0') disp = r->display_name;
          }
          printf(
              "%c [%c] %.28s\n",
              mark,
              picked[row - 1] ? 'x' : ' ',
              disp);
        }
      }
      dirty = false;
    }
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
  }
  g_upload_sort_remote = NULL;
  g_upload_sort_rc = 0;
  free(remote_meta);
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
    const u32 confirm_download = KEY_START | KEY_Y;

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
      printf("DPad: move  A: toggle  START/Y: run  B: back to menu\n\n");
      for (int row = scroll; row < scroll + kVisible && row < total_rows; row++) {
        char mark = (row == cursor) ? '>' : ' ';
        if (row == 0) {
          printf("%c [%c] ALL SAVES\n", mark, master_all ? 'x' : ' ');
        } else {
          const RemoteSave* R = &remote[row - 1];
          const char* disp = R->game_id;
          if (R->display_name[0] != '\0') disp = R->display_name;
          printf(
              "%c [%c] %.28s\n",
              mark,
              picked[row - 1] ? 'x' : ' ',
              disp);
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

static void str_tolower_buf(char* s) {
  if (!s) return;
  for (; *s; s++) *s = (char)tolower((unsigned char)*s);
}

static void trim_end_spaces(char* s) {
  size_t n = strlen(s);
  while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) {
    s[n - 1] = '\0';
    n--;
  }
}

static int is_game_locked(const AppConfig* cfg, const char* game_id) {
  if (!cfg->locked_ids[0]) return 0;
  char want[128];
  copy_cstr(want, sizeof(want), game_id);
  str_tolower_buf(want);
  char buf[512];
  copy_cstr(buf, sizeof(buf), cfg->locked_ids);
  char* saveptr = NULL;
  char* tok = strtok_r(buf, ",", &saveptr);
  while (tok) {
    while (*tok == ' ' || *tok == '\t') tok++;
    trim_end_spaces(tok);
    char entry[128];
    copy_cstr(entry, sizeof(entry), tok);
    str_tolower_buf(entry);
    if (entry[0] && strcmp(entry, want) == 0) return 1;
    tok = strtok_r(NULL, ",", &saveptr);
  }
  return 0;
}

static void toggle_locked_id(AppConfig* cfg, const char* game_id) {
  char want[128];
  copy_cstr(want, sizeof(want), game_id);
  str_tolower_buf(want);
  if (is_game_locked(cfg, game_id)) {
    char newbuf[512];
    newbuf[0] = '\0';
    char buf[512];
    copy_cstr(buf, sizeof(buf), cfg->locked_ids);
    char* saveptr = NULL;
    char* tok = strtok_r(buf, ",", &saveptr);
    int first = 1;
    while (tok) {
      while (*tok == ' ' || *tok == '\t') tok++;
      trim_end_spaces(tok);
      char entry[128];
      copy_cstr(entry, sizeof(entry), tok);
      char el[128];
      copy_cstr(el, sizeof(el), entry);
      str_tolower_buf(el);
      if (strcmp(el, want) != 0) {
        if (!first) strncat(newbuf, ",", sizeof(newbuf) - strlen(newbuf) - 1);
        strncat(newbuf, entry, sizeof(newbuf) - strlen(newbuf) - 1);
        first = 0;
      }
      tok = strtok_r(NULL, ",", &saveptr);
    }
    copy_cstr(cfg->locked_ids, sizeof(cfg->locked_ids), newbuf);
  } else {
    size_t len = strlen(cfg->locked_ids);
    if (len > 0 && len + 1 < sizeof(cfg->locked_ids)) {
      strncat(cfg->locked_ids, ",", sizeof(cfg->locked_ids) - len - 1);
    }
    strncat(cfg->locked_ids, want, sizeof(cfg->locked_ids) - strlen(cfg->locked_ids) - 1);
  }
}

static int save_locked_ids_to_ini_3ds(const char* path, const char* locked_ids_str) {
#define GB_INI_MAX_LINES 160
  /* Heap: ~80KB on stack overflows the 3DS default thread stack when toggling locks. */
  char (*lines)[512] = (char (*)[512])calloc((size_t)GB_INI_MAX_LINES, sizeof(*lines));
  if (!lines) return 0;
  int n = 0;
  FILE* in = fopen(path, "r");
  if (in) {
    while (n < GB_INI_MAX_LINES && fgets(lines[n], (int)sizeof(lines[0]), in)) n++;
    fclose(in);
  }
  int in_sync = 0;
  int replaced = 0;
  for (int i = 0; i < n; i++) {
    char t[512];
    copy_cstr(t, sizeof(t), lines[i]);
    trim_line(t);
    if (t[0] == '[' && strchr(t, ']')) {
      in_sync = (strcmp(t, "[sync]") == 0);
      continue;
    }
    if (in_sync && strncmp(t, "locked_ids=", 11) == 0) {
      snprintf(lines[i], sizeof(lines[i]), "locked_ids=%s\n", locked_ids_str);
      replaced = 1;
      break;
    }
  }
  if (!replaced) {
    for (int i = 0; i < n; i++) {
      char t[512];
      copy_cstr(t, sizeof(t), lines[i]);
      trim_line(t);
      if (strcmp(t, "[sync]") == 0) {
        if (n + 1 < GB_INI_MAX_LINES) {
          memmove(lines + i + 2, lines + i + 1, (size_t)(n - i - 1) * sizeof(lines[0]));
          snprintf(lines[i + 1], sizeof(lines[i + 1]), "locked_ids=%s\n", locked_ids_str);
          n++;
        }
        replaced = 1;
        break;
      }
    }
  }
  if (!replaced && n + 2 < GB_INI_MAX_LINES) {
    if (n > 0 && lines[n - 1][0] != '\0') snprintf(lines[n++], sizeof(lines[0]), "\n");
    snprintf(lines[n++], sizeof(lines[0]), "[sync]\n");
    snprintf(lines[n++], sizeof(lines[0]), "locked_ids=%s\n", locked_ids_str);
  }
  char tmp[600];
  if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) {
    free(lines);
    return 0;
  }
  FILE* out = fopen(tmp, "w");
  if (!out) {
    free(lines);
    return 0;
  }
  for (int i = 0; i < n; i++) fputs(lines[i], out);
  fclose(out);
  remove(path);
  if (rename(tmp, path) != 0) {
    remove(tmp);
    free(lines);
    return 0;
  }
  free(lines);
  return 1;
}

static int cmp_merge_id_str(const void* a, const void* b) {
  return strcmp((const char*)a, (const char*)b);
}

static void save_viewer_3ds(AppConfig* cfg) {
  LocalSave* local = (LocalSave*)calloc(MAX_SAVES, sizeof(LocalSave));
  RemoteSave* remote = (RemoteSave*)calloc(MAX_SAVES, sizeof(RemoteSave));
  if (!local || !remote) {
    free(local);
    free(remote);
    return;
  }
  ensure_save_roots_exist(cfg);
  int lc = scan_all_local_saves(cfg, local, MAX_SAVES);
  int status = 0;
  unsigned char* body = NULL;
  size_t body_len = 0;
  if (!http_request(cfg, "GET", "/saves", NULL, NULL, 0, &status, &body, &body_len) || status != 200) {
    consoleClear();
    printf("Save viewer: GET /saves failed");
    if (status > 0) printf(" (HTTP %d)", status);
    printf("\nB: back\n");
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
    while (aptMainLoop()) {
      hidScanInput();
      if (hidKeysDown() & KEY_B) break;
      gfxFlushBuffers();
      gfxSwapBuffers();
      gspWaitForVBlank();
    }
    free(body);
    free(local);
    free(remote);
    return;
  }
  int rc = parse_remote_saves((const char*)body, remote, MAX_SAVES);
  free(body);
  const int cap = MAX_SAVES * 2;
  char (*merge_ids)[128] = (char (*)[128])calloc((size_t)cap, sizeof(*merge_ids));
  if (!merge_ids) {
    free(local);
    free(remote);
    return;
  }
  int n_merge = 0;
  int i;
  for (i = 0; i < rc; i++) n_merge = add_merge_id(merge_ids, n_merge, cap, remote[i].game_id);
  {
    char (*local_only)[128] = calloc((size_t)MAX_SAVES, sizeof(*local_only));
    if (!local_only) {
      free(merge_ids);
      free(local);
      free(remote);
      return;
    }
    int n_lo = 0;
    for (i = 0; i < lc; i++) {
      if (!find_remote_by_id(remote, rc, local[i].game_id)) {
        copy_cstr(local_only[n_lo++], 128, local[i].game_id);
      }
    }
    qsort(local_only, (size_t)n_lo, sizeof(local_only[0]), cmp_merge_id_str);
    for (i = 0; i < n_lo; i++) n_merge = add_merge_id(merge_ids, n_merge, cap, local_only[i]);
    free(local_only);
  }
  if (n_merge <= 0) {
    consoleClear();
    printf("Save viewer: no saves (local or server).\n");
    printf("B: back\n");
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
    while (aptMainLoop()) {
      hidScanInput();
      if (hidKeysDown() & KEY_B) break;
      gfxFlushBuffers();
      gfxSwapBuffers();
      gspWaitForVBlank();
    }
    free(merge_ids);
    free(local);
    free(remote);
    return;
  }
  int cursor = 0;
  int scroll = 0;
  const int kVisible = 14;
  const int total_rows = n_merge;
  const char* config_path = GBASYNC_3DS_CONFIG_PATH;
  bool dirty = true;
  while (aptMainLoop()) {
    hidScanInput();
    u32 kDown = hidKeysDown();
    if (kDown & KEY_B) break;
    if (kDown & KEY_A) {
      save_history_picker_3ds(cfg, merge_ids[cursor]);
      dirty = true;
    }
    if (kDown & KEY_R) {
      toggle_locked_id(cfg, merge_ids[cursor]);
      (void)save_locked_ids_to_ini_3ds(config_path, cfg->locked_ids);
      dirty = true;
    }
    if (kDown & KEY_DUP) {
      cursor = (cursor + total_rows - 1) % total_rows;
      dirty = true;
    }
    if (kDown & KEY_DDOWN) {
      cursor = (cursor + 1) % total_rows;
      dirty = true;
    }
    if (cursor < scroll) scroll = cursor;
    if (cursor >= scroll + kVisible) scroll = cursor - kVisible + 1;
    if (scroll < 0) scroll = 0;
    {
      int max_scroll = total_rows > kVisible ? total_rows - kVisible : 0;
      if (scroll > max_scroll) scroll = max_scroll;
    }

    if (!dirty) {
      gfxFlushBuffers();
      gfxSwapBuffers();
      gspWaitForVBlank();
      continue;
    }

    consoleClear();
    printf("--- Save viewer (lock for Auto) ---\n");
    printf("\n");
    {
      const int kMenuLeftCol = 22;
      printf("%-*s%s\n", kMenuLeftCol, "UP/DOWN: move", "");
      printf("%-*s%s\n", kMenuLeftCol, "A: history / restore", "");
      printf("%-*s%s\n", kMenuLeftCol, "R: toggle lock -> config", "");
      printf("%-*s%s\n", kMenuLeftCol, "b: back", "");
    }
    printf("\n");
    for (int row = scroll; row < scroll + kVisible && row < total_rows; row++) {
      char mark = (row == cursor) ? '>' : ' ';
      char lk[8];
      copy_cstr(lk, sizeof(lk), is_game_locked(cfg, merge_ids[row]) ? "[L]" : "   ");
      {
        const RemoteSave* r = find_remote_by_id(remote, rc, merge_ids[row]);
        const char* disp =
            (r && r->display_name[0] != '\0') ? r->display_name : merge_ids[row];
        printf("%c%s %.28s\n", mark, lk, disp);
      }
      printf("\n");
    }
    dirty = false;
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
  }
  free(merge_ids);
  free(local);
  free(remote);
}

typedef enum {
  AP_OK = 0,
  AP_UP,
  AP_DN,
  AP_CONF,
  AP_LOCK
} AutoPlanKind;

typedef struct {
  char id[128];
  AutoPlanKind kind;
} AutoPlanRow;

static AutoPlanKind classify_auto_3ds(
    const char* id,
    LocalSave* l,
    RemoteSave* r,
    BaselineRow* baseline,
    int n_baseline,
    int locked) {
  if (l && r) {
    if (strcmp(l->sha256, r->sha256) == 0) return AP_OK;
    if (locked) return AP_LOCK;
    const char* base = baseline_find(baseline, n_baseline, id);
    /* No baseline but both sides present: treat like conflict — user picks X/Y in apply. */
    if (!base || base[0] == '\0') return AP_CONF;
    const int loc_eq = (strcmp(l->sha256, base) == 0);
    const int rem_eq = (strcmp(r->sha256, base) == 0);
    if (loc_eq && !rem_eq) return AP_DN;
    if (!loc_eq && rem_eq) return AP_UP;
    if (!loc_eq && !rem_eq) return AP_CONF;
  } else if (l && !r) {
    return locked ? AP_LOCK : AP_UP;
  } else if (!l && r) {
    return locked ? AP_LOCK : AP_DN;
  }
  return AP_OK;
}

static const char* plan_kind_label(AutoPlanKind k) {
  switch (k) {
    case AP_OK:
      return "OK";
    case AP_UP:
      return "UPLOAD";
    case AP_DN:
      return "DOWNLOAD";
    case AP_CONF:
      return "CONFLICT (choose side)";
    case AP_LOCK:
      return "SKIP (locked)";
    default:
      return "?";
  }
}

static void fill_auto_plan_merge(
    const AppConfig* cfg,
    char (*merge_ids)[128],
    int n_merge,
    LocalSave* local,
    int lc,
    RemoteSave* remote,
    int rc,
    BaselineRow* baseline,
    int n_baseline,
    AutoPlanRow* plan) {
  for (int m = 0; m < n_merge; m++) {
    const char* id = merge_ids[m];
    copy_cstr(plan[m].id, sizeof(plan[m].id), id);
    LocalSave* l = find_local_by_id(local, lc, id);
    RemoteSave* r = find_remote_by_id(remote, rc, id);
    plan[m].kind = classify_auto_3ds(id, l, r, baseline, n_baseline, is_game_locked(cfg, id));
  }
}

static void gbasync_status_path(char* out, size_t out_sz, const char* save_dir) {
  join_path(out, out_sz, save_dir, ".gbasync-status");
}

static void sync_status_save(
    const char* save_dir, time_t t, int last_ok, int server_ok, int dropbox, const char* err) {
  char path[512], tmp[520];
  gbasync_status_path(path, sizeof(path), save_dir);
  if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) return;
  FILE* fp = fopen(tmp, "w");
  if (!fp) return;
  fprintf(fp, "v=1\n");
  fprintf(fp, "t=%lld\n", (long long)t);
  fprintf(fp, "ok=%d\n", last_ok ? 1 : 0);
  fprintf(fp, "srv=%d\n", server_ok ? 1 : 0);
  if (dropbox < 0)
    fprintf(fp, "db=u\n");
  else
    fprintf(fp, "db=%d\n", dropbox ? 1 : 0);
  fprintf(fp, "e=%s\n", err ? err : "");
  fclose(fp);
  remove(path);
  if (rename(tmp, path) != 0) remove(tmp);
}

static void sync_status_after_server(const AppConfig* cfg, int last_ok, int server_ok, const char* err) {
  char sd[256];
  first_save_dir_for_status(cfg, sd, sizeof(sd));
  time_t now = time(NULL);
  int db = -1;
  char path[512];
  gbasync_status_path(path, sizeof(path), sd);
  FILE* fp = fopen(path, "r");
  if (fp) {
    char line[320];
    while (fgets(line, sizeof(line), fp)) {
      if (strncmp(line, "db=u", 4) == 0) db = -1;
      else if (strncmp(line, "db=0", 4) == 0) db = 0;
      else if (strncmp(line, "db=1", 4) == 0) db = 1;
    }
    fclose(fp);
  }
  sync_status_save(sd, now, last_ok, server_ok, db, err ? err : "");
}

static void sync_status_after_dropbox(const AppConfig* cfg, int http_ok) {
  char sd[256];
  first_save_dir_for_status(cfg, sd, sizeof(sd));
  sync_status_save(sd, time(NULL), http_ok, http_ok, http_ok ? 1 : 0, "");
}

static void sync_status_print_menu(const AppConfig* cfg) {
  char sd[256];
  first_save_dir_for_status(cfg, sd, sizeof(sd));
  char path[512];
  gbasync_status_path(path, sizeof(path), sd);
  FILE* fp = fopen(path, "r");
  if (!fp) {
    printf("Last sync: (none)\n");
    printf("Server: —\n");
    printf("Dropbox: —\n\n");
    return;
  }
  time_t t = 0;
  int ok = 0, srv = 0, db = -1;
  char err[80];
  err[0] = '\0';
  char line[320];
  while (fgets(line, sizeof(line), fp)) {
    if (strncmp(line, "t=", 2) == 0) t = (time_t)strtoll(line + 2, NULL, 10);
    if (strncmp(line, "ok=", 3) == 0) ok = atoi(line + 3);
    if (strncmp(line, "srv=", 4) == 0) srv = atoi(line + 4);
    if (strncmp(line, "db=u", 4) == 0) db = -1;
    if (strncmp(line, "db=0", 4) == 0) db = 0;
    if (strncmp(line, "db=1", 4) == 0) db = 1;
    if (strncmp(line, "e=", 2) == 0) {
      copy_cstr(err, sizeof(err), line + 2);
      size_t n = strlen(err);
      while (n > 0 && (err[n - 1] == '\n' || err[n - 1] == '\r')) err[--n] = '\0';
    }
  }
  fclose(fp);
  char tbuf[40] = "unknown";
  if (t > 0) {
    struct tm tm_utc;
    gmtime_r(&t, &tm_utc);
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M UTC", &tm_utc);
  }
  const char* srvs = srv ? "OK" : "fail";
  const char* dbs = (db < 0) ? "—" : (db ? "OK" : "fail");
  const char* oks = ok ? "OK" : "fail";
  printf("Last sync: %s %s\n", oks, tbuf);
  printf("Server: %s\n", srvs);
  printf("Dropbox: %s\n", dbs);
  if (err[0]) printf("Last error: %.60s\n", err);
  printf("\n");
}

static int preview_auto_plan_3ds(
    AppConfig* cfg,
    char (*merge_ids)[128],
    int n_merge,
    LocalSave* local,
    int lc,
    RemoteSave* remote,
    int rc,
    BaselineRow* baseline,
    int n_baseline,
    AutoPlanRow* plan) {
  int nu = 0, nd = 0, nc = 0, nl = 0, nk = 0;
  fill_auto_plan_merge(cfg, merge_ids, n_merge, local, lc, remote, rc, baseline, n_baseline, plan);
  for (int i = 0; i < n_merge; i++) {
    switch (plan[i].kind) {
      case AP_UP:
        nu++;
        break;
      case AP_DN:
        nd++;
        break;
      case AP_CONF:
        nc++;
        break;
      case AP_LOCK:
        nl++;
        break;
      case AP_OK:
        nk++;
        break;
      default:
        break;
    }
  }
  /* Rows to show: everything except OK (upload/download/skip/conflict/lock only). */
  int filt[MAX_SAVES];
  int n_filt = 0;
  for (int i = 0; i < n_merge; i++) {
    if (plan[i].kind != AP_OK) filt[n_filt++] = i;
  }
  int cursor = 0;
  int scroll = 0;
  const int kVisible = 12;
  int total_rows = n_filt > 0 ? n_filt : 1;
  bool dirty = true;

  while (aptMainLoop()) {
    hidScanInput();
    u32 kDown = hidKeysDown();
    if (kDown & KEY_A) return 1;
    if (kDown & KEY_START) return 2;
    if (kDown & KEY_B) return 0;
    if (n_filt > 0) {
      if (kDown & KEY_DUP) {
        cursor = (cursor + total_rows - 1) % total_rows;
        dirty = true;
      }
      if (kDown & KEY_DDOWN) {
        cursor = (cursor + 1) % total_rows;
        dirty = true;
      }
      if (cursor < scroll) scroll = cursor;
      if (cursor >= scroll + kVisible) scroll = cursor - kVisible + 1;
      if (scroll < 0) scroll = 0;
      {
        int max_scroll = total_rows > kVisible ? total_rows - kVisible : 0;
        if (scroll > max_scroll) scroll = max_scroll;
      }
    }

    if (!dirty) {
      gfxFlushBuffers();
      gfxSwapBuffers();
      gspWaitForVBlank();
      continue;
    }

    consoleClear();
    printf("--- Sync preview ---\n");
    printf("UP:%d DN:%d CONF:%d LOCK:%d\n", nu, nd, nc, nl);
    printf("\n");
    {
      const int kPrevCol = 20;
      printf("%-*s%s\n", kPrevCol, "a: confirm", "");
      printf("%-*s%s\n", kPrevCol, "START: sync & exit", "");
      printf("%-*s%s\n", kPrevCol, "b: back", "");
    }
    printf("\n");
    for (int row = scroll; row < scroll + kVisible && row < n_filt; row++) {
      int pi = filt[row];
      char mark = (row == cursor) ? '>' : ' ';
      {
        const RemoteSave* rr = find_remote_by_id(remote, rc, plan[pi].id);
        const char* disp =
            (rr && rr->display_name[0] != '\0') ? rr->display_name : plan[pi].id;
        printf("%c %.28s  %s\n", mark, disp, plan_kind_label(plan[pi].kind));
      }
    }
    dirty = false;
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
  }
  return 0;
}

static SyncSummary run_sync(AppConfig* cfg, SyncAction action, const SyncManualFilter* xy_filter) {
  SyncSummary summary = {0};
  printf("\n");
  printf("Scanning local saves...\n");
  LocalSave* local = (LocalSave*)calloc(MAX_SAVES, sizeof(LocalSave));
  RemoteSave* remote = (RemoteSave*)calloc(MAX_SAVES, sizeof(RemoteSave));
  if (!local || !remote) {
    printf("ERROR: out of memory for sync buffers\n");
    free(local);
    free(remote);
    return summary;
  }
  const char* source_tag = is_vc_mode(cfg) ? "3ds-homebrew-vc" : "3ds-homebrew";
  ensure_save_roots_exist(cfg);
  BaselineRow* baseline = (BaselineRow*)calloc((size_t)MAX_SAVES, sizeof(BaselineRow));
  if (!baseline) {
    printf("ERROR: out of memory (baseline buffer)\n");
    free(local);
    free(remote);
    return summary;
  }
  int n_baseline = baseline_load_merged(cfg, baseline, MAX_SAVES);
  int local_count = scan_all_local_saves(cfg, local, MAX_SAVES);
  printf("Local saves: %d\n", local_count);

  int status = 0;
  unsigned char* body = NULL;
  size_t body_len = 0;
  if (!http_request(cfg, "GET", "/saves", NULL, NULL, 0, &status, &body, &body_len) || status != 200) {
    printf("ERROR: failed GET /saves (check server URL/API key/network)\n");
    sync_status_after_server(cfg, 0, 0, "GET /saves");
    free(body);
    free(baseline);
    free(local);
    free(remote);
    return summary;
  }
  int remote_count = parse_remote_saves((const char*)body, remote, MAX_SAVES);
  free(body);
  printf("Remote saves: %d\n", remote_count);

  if (action == SYNC_ACTION_UPLOAD_ONLY) {
    for (int i = 0; i < local_count; i++) {
      if (!id_in_manual(xy_filter, local[i].game_id)) continue;
      char uploaded_sha[65];
      if (put_one_save(cfg, &local[i], true, source_tag, uploaded_sha)) {
        baseline_upsert(baseline, &n_baseline, MAX_SAVES, local[i].game_id, uploaded_sha);
        summary.uploads++;
      }
    }
    if (!baseline_save_merged(cfg, baseline, n_baseline, local, local_count)) {
      printf("WARN: could not write .gbasync-baseline\n");
    }
    sync_status_after_server(cfg, 1, 1, "");
    free(baseline);
    free(local);
    free(remote);
    return summary;
  }

  if (action == SYNC_ACTION_DOWNLOAD_ONLY) {
    for (int i = 0; i < remote_count; i++) {
      if (!id_in_manual(xy_filter, remote[i].game_id)) continue;
      if (get_one_save(cfg, remote[i].game_id, &remote[i], local, local_count)) {
        baseline_upsert(baseline, &n_baseline, MAX_SAVES, remote[i].game_id, remote[i].sha256);
        summary.downloads++;
      }
    }
    local_count = scan_all_local_saves(cfg, local, MAX_SAVES);
    if (!baseline_save_merged(cfg, baseline, n_baseline, local, local_count)) {
      printf("WARN: could not write .gbasync-baseline\n");
    }
    sync_status_after_server(cfg, 1, 1, "");
    free(baseline);
    free(local);
    free(remote);
    return summary;
  }

  /* AUTO — merge / local-only id lists on heap (~49KB each if stack-allocated overflows 3DS stack) */
  const int merge_cap = MAX_SAVES * 2;
  char (*merge_ids)[128] = calloc((size_t)merge_cap, sizeof(*merge_ids));
  if (!merge_ids) {
    printf("ERROR: out of memory (merge list)\n");
    free(baseline);
    free(local);
    free(remote);
    return summary;
  }
  int n_merge = 0;
  int i;
  for (i = 0; i < remote_count; i++) {
    n_merge = add_merge_id(merge_ids, n_merge, merge_cap, remote[i].game_id);
  }
  {
    char (*local_only)[128] = calloc((size_t)MAX_SAVES, sizeof(*local_only));
    if (!local_only) {
      printf("ERROR: out of memory (local id list)\n");
      free(merge_ids);
      free(baseline);
      free(local);
      free(remote);
      return summary;
    }
    int n_lo = 0;
    for (i = 0; i < local_count; i++) {
      if (!find_remote_by_id(remote, remote_count, local[i].game_id)) {
        copy_cstr(local_only[n_lo++], 128, local[i].game_id);
      }
    }
    qsort(local_only, (size_t)n_lo, sizeof(local_only[0]), cmp_merge_id_str);
    for (i = 0; i < n_lo; i++) {
      n_merge = add_merge_id(merge_ids, n_merge, merge_cap, local_only[i]);
    }
    free(local_only);
  }

  AutoPlanRow* plan = (AutoPlanRow*)calloc((size_t)n_merge, sizeof(AutoPlanRow));
  if (!plan) {
    printf("ERROR: out of memory (plan)\n");
    free(merge_ids);
    free(baseline);
    free(local);
    free(remote);
    return summary;
  }
  fill_auto_plan_merge(cfg, merge_ids, n_merge, local, local_count, remote, remote_count, baseline, n_baseline, plan);

  {
    int nu = 0, nd = 0, nc = 0, nl = 0, nk = 0;
    for (int i = 0; i < n_merge; i++) {
      switch (plan[i].kind) {
        case AP_UP:
          nu++;
          break;
        case AP_DN:
          nd++;
          break;
        case AP_CONF:
          nc++;
          break;
        case AP_LOCK:
          nl++;
          break;
        case AP_OK:
          nk++;
          break;
        default:
          break;
      }
    }
    /* All rows OK (hashes match): truly up to date. Do not use nu+nd+ns+nc==0 — that
     * misfires when every row is locked (no upload/download planned but not "synced"). */
    if (nk == n_merge) {
      int m;
      printf("\n");
      printf("Already Up To Date\n");
      fflush(stdout);
      bool baseline_dirty = false;
      for (m = 0; m < n_merge; m++) {
        const char* id = merge_ids[m];
        if (is_game_locked(cfg, id)) continue;
        {
          LocalSave* l = find_local_by_id(local, local_count, id);
          RemoteSave* r = find_remote_by_id(remote, remote_count, id);
          if (l && r && strcmp(l->sha256, r->sha256) == 0) {
            if (baseline_upsert(baseline, &n_baseline, MAX_SAVES, id, l->sha256)) baseline_dirty = true;
          }
        }
      }
      if (baseline_dirty) {
        if (!baseline_save_merged(cfg, baseline, n_baseline, local, local_count)) {
          printf("WARN: could not write .gbasync-baseline\n");
        }
      }
      sync_status_after_server(cfg, 1, 1, "");
      summary.already_up_to_date = 1;
      free(plan);
      free(merge_ids);
      free(baseline);
      free(local);
      free(remote);
      return summary;
    }
  }

  /* After "Already Up To Date" early return — avoids extra HTTP wait before post-sync menu. */
  debug_report_sync_start_3ds(cfg, local_count, n_baseline);

  {
    int pv = preview_auto_plan_3ds(
        cfg,
        merge_ids,
        n_merge,
        local,
        local_count,
        remote,
        remote_count,
        baseline,
        n_baseline,
        plan);
    if (pv == 0) {
      printf("Preview cancelled.\n");
      free(plan);
      free(merge_ids);
      free(baseline);
      free(local);
      free(remote);
      return summary;
    }
    if (pv == 2) summary.exit_after_sync = 1;
  }
  free(plan);

  printf("\n");

  for (int m = 0; m < n_merge; m++) {
    const char* id = merge_ids[m];
    if (is_game_locked(cfg, id)) {
      printf("%s: SKIP (locked on this device)\n", id);
      continue;
    }
    LocalSave* l = find_local_by_id(local, local_count, id);
    RemoteSave* r = find_remote_by_id(remote, remote_count, id);
    if (l && r) {
      if (strcmp(l->sha256, r->sha256) == 0) {
        baseline_upsert(baseline, &n_baseline, MAX_SAVES, id, l->sha256);
        continue;
      }
      const char* base = baseline_find(baseline, n_baseline, id);
      if (!base || base[0] == '\0') {
        resolve_both_changed_conflict(
            cfg, source_tag, l, r, id, local, local_count, baseline, &n_baseline, &summary, 1);
        continue;
      }
      const int loc_eq = (strcmp(l->sha256, base) == 0);
      const int rem_eq = (strcmp(r->sha256, base) == 0);
      if (loc_eq && !rem_eq) {
        if (get_one_save(cfg, id, r, local, local_count)) {
          baseline_upsert(baseline, &n_baseline, MAX_SAVES, id, r->sha256);
          summary.downloads++;
        }
      } else if (!loc_eq && rem_eq) {
        char uploaded_sha[65];
        if (put_one_save(cfg, l, false, source_tag, uploaded_sha)) {
          baseline_upsert(baseline, &n_baseline, MAX_SAVES, id, uploaded_sha);
          summary.uploads++;
        }
      } else if (!loc_eq && !rem_eq) {
        resolve_both_changed_conflict(
            cfg, source_tag, l, r, id, local, local_count, baseline, &n_baseline, &summary, 0);
      }
    } else if (l && !r) {
      char uploaded_sha[65];
      if (put_one_save(cfg, l, false, source_tag, uploaded_sha)) {
        baseline_upsert(baseline, &n_baseline, MAX_SAVES, id, uploaded_sha);
        summary.uploads++;
      }
    } else if (!l && r) {
      if (get_one_save(cfg, id, r, local, local_count)) {
        baseline_upsert(baseline, &n_baseline, MAX_SAVES, id, r->sha256);
        summary.downloads++;
      }
    }
  }

  local_count = scan_all_local_saves(cfg, local, MAX_SAVES);
  if (!baseline_save_merged(cfg, baseline, n_baseline, local, local_count)) {
    printf("WARN: could not write .gbasync-baseline\n");
  }
  sync_status_after_server(cfg, 1, 1, "");

  free(merge_ids);
  free(baseline);
  free(local);
  free(remote);
  return summary;
}

static bool choose_action(SyncAction* out_action, bool* exit_app) {
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
    if (kDown & KEY_SELECT) {
      *out_action = SYNC_ACTION_DROPBOX_SYNC;
      return true;
    }
    if (kDown & KEY_R) {
      *out_action = SYNC_ACTION_SAVE_VIEWER;
      return true;
    }
    if (kDown & KEY_START) {
      if (exit_app) *exit_app = true;
      return false;
    }
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
  }
  return false;
}

/** -1 = error; else games needing Auto (not OK, not locked). */
static int count_pending_auto_sync_actions(const AppConfig* cfg) {
  LocalSave* local = (LocalSave*)calloc(MAX_SAVES, sizeof(LocalSave));
  RemoteSave* remote = (RemoteSave*)calloc(MAX_SAVES, sizeof(RemoteSave));
  BaselineRow* baseline = (BaselineRow*)calloc((size_t)MAX_SAVES, sizeof(BaselineRow));
  if (!local || !remote || !baseline) {
    free(local);
    free(remote);
    free(baseline);
    return -1;
  }
  int n_baseline = baseline_load_merged(cfg, baseline, MAX_SAVES);
  int local_count = scan_all_local_saves(cfg, local, MAX_SAVES);
  int status = 0;
  unsigned char* body = NULL;
  size_t body_len = 0;
  if (!http_request(cfg, "GET", "/saves", NULL, NULL, 0, &status, &body, &body_len) || status != 200) {
    free(body);
    free(baseline);
    free(local);
    free(remote);
    return -1;
  }
  int remote_count = parse_remote_saves((const char*)body, remote, MAX_SAVES);
  free(body);
  const int merge_cap = MAX_SAVES * 2;
  char (*merge_ids)[128] = calloc((size_t)merge_cap, sizeof(*merge_ids));
  if (!merge_ids) {
    free(baseline);
    free(local);
    free(remote);
    return -1;
  }
  int n_merge = 0;
  int i;
  for (i = 0; i < remote_count; i++) {
    n_merge = add_merge_id(merge_ids, n_merge, merge_cap, remote[i].game_id);
  }
  {
    char (*local_only)[128] = calloc((size_t)MAX_SAVES, sizeof(*local_only));
    if (!local_only) {
      free(merge_ids);
      free(baseline);
      free(local);
      free(remote);
      return -1;
    }
    int n_lo = 0;
    for (i = 0; i < local_count; i++) {
      if (!find_remote_by_id(remote, remote_count, local[i].game_id)) {
        copy_cstr(local_only[n_lo++], 128, local[i].game_id);
      }
    }
    qsort(local_only, (size_t)n_lo, sizeof(local_only[0]), cmp_merge_id_str);
    for (i = 0; i < n_lo; i++) {
      n_merge = add_merge_id(merge_ids, n_merge, merge_cap, local_only[i]);
    }
    free(local_only);
  }
  if (n_merge <= 0) {
    free(merge_ids);
    free(baseline);
    free(local);
    free(remote);
    return 0;
  }
  AutoPlanRow* plan = (AutoPlanRow*)calloc((size_t)n_merge, sizeof(AutoPlanRow));
  if (!plan) {
    free(merge_ids);
    free(baseline);
    free(local);
    free(remote);
    return -1;
  }
  fill_auto_plan_merge(cfg, merge_ids, n_merge, local, local_count, remote, remote_count, baseline, n_baseline, plan);
  int pending = 0;
  for (i = 0; i < n_merge; i++) {
    if (plan[i].kind != AP_OK && plan[i].kind != AP_LOCK) pending++;
  }
  free(plan);
  free(merge_ids);
  free(baseline);
  free(local);
  free(remote);
  return pending;
}

static void run_dropbox_sync_once_3ds(const AppConfig* cfg, bool* quit_app) {
  printf("\nDropbox sync now...\n");
  int st = 0;
  unsigned char* resp = NULL;
  size_t resp_len = 0;
  bool ok = http_request(cfg, "POST", "/dropbox/sync-once", "application/json", NULL, 0, &st, &resp, &resp_len);
  if (!ok) {
    printf("Dropbox sync request: ERROR(request)\n");
    sync_status_after_dropbox(cfg, 0);
  } else if (st == 200) {
    printf("Dropbox sync request: OK\n");
    sync_status_after_dropbox(cfg, 1);
  } else {
    printf("Dropbox sync request: HTTP %d\n", st);
    sync_status_after_dropbox(cfg, 0);
  }
  free(resp);
  wait_after_sync_3ds(quit_app, false);
}

static void wait_after_sync_3ds(bool* quit_app, bool can_reboot) {
  if (can_reboot) {
    printf("\na: main menu   y: reboot now   START: exit\n");
  } else {
    printf("\na: main menu   START: exit\n");
  }
  while (aptMainLoop()) {
    hidScanInput();
    u32 kDown = hidKeysDown();
    if (kDown & KEY_A) return;
    if (can_reboot && (kDown & KEY_Y)) {
      printf("Rebooting...\n");
      (void)gfxFlushBuffers();
      (void)gfxSwapBuffers();
      (void)APT_HardwareResetAsync();
      *quit_app = true;
      return;
    }
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
  load_config(&cfg, GBASYNC_3DS_CONFIG_PATH);

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
    /* -1 = stale: recompute badge (expensive). Cached so we don't rescan + GET /saves every menu draw. */
    int cached_pending_sync_badge = -1;
    while (aptMainLoop() && !quit_app) {
      consoleClear();
      printf("GBA Sync (3DS MVP)\n");
      printf("------------------\n");
      printf("Server: %s\n", cfg.server_url);
      printf("Mode: %s\n", is_vc_mode(&cfg) ? "vc" : "normal");
      if (is_vc_mode(&cfg)) {
        printf("Save dir: %s\n\n", cfg.vc_save_dir);
      } else {
        SaveRoot roots[8];
        const int nr = build_save_roots(&cfg, roots, 8);
        if (nr <= 1) {
          printf("Save dir: %s\n\n", nr > 0 ? roots[0].save_dir : cfg.save_dir);
        } else {
          printf("Save dirs:");
          for (int i = 0; i < nr; i++) printf(" %s", roots[i].save_dir);
          printf("\n\n");
        }
      }
      sync_status_print_menu(&cfg);
      {
        if (cached_pending_sync_badge < 0) {
          cached_pending_sync_badge = count_pending_auto_sync_actions(&cfg);
        }
        if (cached_pending_sync_badge > 0) {
          printf("  %d game(s) need sync (run Auto)\n\n", cached_pending_sync_badge);
        }
      }
      /* Two columns: sync actions (left) | R / SELECT / START hints (right) */
      {
        const int kMenuLeftCol = 20;
        printf("%-*s%s\n", kMenuLeftCol, "a: Auto sync", "R: save viewer");
        printf("%-*s%s\n", kMenuLeftCol, "x: Upload only", "SELECT: dropbox sync");
        printf("%-*s%s\n", kMenuLeftCol, "y: Download only", "START: exit");
      }

      SyncAction action = SYNC_ACTION_AUTO;
      if (!choose_action(&action, &quit_app)) break;

      SyncManualFilter xy;
      memset(&xy, 0, sizeof(xy));
      xy.all = 1;

      if (action == SYNC_ACTION_DROPBOX_SYNC) {
        run_dropbox_sync_once_3ds(&cfg, &quit_app);
        cached_pending_sync_badge = -1;
      } else if (action == SYNC_ACTION_SAVE_VIEWER) {
        save_viewer_3ds(&cfg);
        cached_pending_sync_badge = -1;
      } else if (action == SYNC_ACTION_UPLOAD_ONLY) {
        if (!pick_upload_selection_3ds(&cfg, &xy)) {
          manual_filter_free(&xy);
          continue;
        }
        SyncSummary summary = run_sync(&cfg, action, &xy);
        wait_after_sync_3ds(&quit_app, summary.downloads > 0 || summary.already_up_to_date);
        cached_pending_sync_badge = -1;
      } else if (action == SYNC_ACTION_DOWNLOAD_ONLY) {
        if (!pick_download_selection_3ds(&cfg, &xy)) {
          manual_filter_free(&xy);
          continue;
        }
        SyncSummary summary = run_sync(&cfg, action, &xy);
        wait_after_sync_3ds(&quit_app, summary.downloads > 0 || summary.already_up_to_date);
        cached_pending_sync_badge = -1;
      } else {
        SyncSummary summary = run_sync(&cfg, action, NULL);
        if (summary.exit_after_sync) {
          quit_app = true;
        } else {
          wait_after_sync_3ds(&quit_app, summary.downloads > 0 || summary.already_up_to_date);
        }
        cached_pending_sync_badge = -1;
      }
      manual_filter_free(&xy);
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
