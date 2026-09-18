/** \file
 *
 *  \brief CoCoSDC host-folder filesystem (Phase D).
 *
 *  Command strings and 256-byte record layouts follow Studio's
 *  SDC_FileAccess.asm and the CoCo SDC User Guide (Darren Atkinson)
 *  opcode/block dictionary.  Stream ($90/$91) follows SDC_BigLoadm.asm
 *  / SDC_StreamFile_Library.asm / SDC_Play.asm: 512-byte sectors, LSN×512,
 *  abort $D0 (completed in sdc_hw_write so Play BREAK need not poll).
 *
 *  \licenseblock This file is part of XRoar, a Dragon/Tandy CoCo emulator.
 *
 *  XRoar is free software; you can redistribute it and/or modify it under the
 *  terms of the GNU General Public License as published by the Free Software
 *  Foundation, either version 3 of the License, or (at your option) any later
 *  version.
 *
 *  See COPYING.GPL for redistribution conditions.
 *
 *  \endlicenseblock
 */

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "top-config.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cocosdc_fs.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define SDC_PATH_MAX 254

static char *sdc_strdup(const char *s) {
	size_t n = strlen(s) + 1;
	char *p = malloc(n);
	if (p) {
		memcpy(p, s, n);
	}
	return p;
}

static void slot_close(struct sdc_slot *s) {
	if (s->fp) {
		fclose(s->fp);
		s->fp = NULL;
	}
	free(s->host_path);
	s->host_path = NULL;
	s->writable = 0;
	s->attr = 0;
	s->fdc_ok = 0;
	s->type = SDC_DTYPE_RAW;
	s->header = 0;
	s->spt = SDC_FLOPPY_SPT;
	s->sides = 1;
	s->fdc_sectors = 0;
	memset(s->name, ' ', 8);
	memset(s->ext, ' ', 3);
}

static void listing_clear(struct sdc_fs *fs) {
	free(fs->dirents);
	fs->dirents = NULL;
	fs->ndirents = 0;
	fs->dir_cap = 0;
	fs->dir_index = 0;
	fs->listing_active = 0;
}

void sdc_fs_init(struct sdc_fs *fs) {
	memset(fs, 0, sizeof(*fs));
	fs->cwd_rel = sdc_strdup("");
}

void sdc_fs_free(struct sdc_fs *fs) {
	sdc_fs_reset(fs);
	free(fs->root);
	fs->root = NULL;
	free(fs->cwd_rel);
	fs->cwd_rel = NULL;
}

void sdc_fs_reset(struct sdc_fs *fs) {
	for (int i = 0; i < SDC_SLOTS; i++) {
		slot_close(&fs->slot[i]);
	}
	listing_clear(fs);
	free(fs->cwd_rel);
	fs->cwd_rel = sdc_strdup("");
	fs->stream_off = 0;
	fs->stream_end = 0;
}

static void strip_slash(char *s) {
	size_t n = strlen(s);
	while (n > 1 && s[n - 1] == '/') {
		s[--n] = 0;
	}
}

const char *sdc_fs_err_name(int err) {
	if (err == 0) {
		return "ok";
	}
	if (err == 1) {
		return "no STARTUP.CFG";
	}
	if (err & SDC_ERR_NOTFOUND) {
		return "not found";
	}
	if (err & SDC_ERR_INUSE) {
		return "in use";
	}
	if (err & SDC_ERR_INVALID) {
		return "invalid image/path";
	}
	if (err & SDC_ERR_MISC) {
		return "open/io failed";
	}
	return "error";
}

int sdc_fs_set_root(struct sdc_fs *fs, const char *host_root) {
	sdc_fs_reset(fs);
	free(fs->root);
	fs->root = NULL;
	if (!host_root || !host_root[0]) {
		return -1;
	}
	fs->root = sdc_strdup(host_root);
	if (!fs->root) {
		return -1;
	}
	strip_slash(fs->root);

	struct stat st;
	if (stat(fs->root, &st) != 0 || !S_ISDIR(st.st_mode)) {
		return -1;
	}
	(void)sdc_fs_apply_startup(fs);
	return 0;
}

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static int join2(char *dst, size_t dstsz, const char *a, const char *b) {
	int n;
	if (!b || !b[0]) {
		n = snprintf(dst, dstsz, "%s", a ? a : "");
	} else if (!a || !a[0]) {
		n = snprintf(dst, dstsz, "%s", b);
	} else if (a[strlen(a) - 1] == '/') {
		n = snprintf(dst, dstsz, "%s%s", a, b);
	} else {
		n = snprintf(dst, dstsz, "%s/%s", a, b);
	}
	return (n < 0 || (size_t)n >= dstsz) ? -1 : 0;
}

static int under_root(const char *root, const char *path) {
	size_t n;
	if (!root || !root[0] || !path) {
		return 0;
	}
	n = strlen(root);
	if (strncmp(path, root, n) != 0) {
		return 0;
	}
	return path[n] == 0 || path[n] == '/';
}

/* FAT-style 8.3 in 11 bytes (spaces).  pattern: '*' -> rest of field '?'. */
static void to_fat11(const char *name, char fat[11], int pattern) {
	int in_ext = 0;
	int ni = 0;
	int ei = 0;
	const char *base = strrchr(name, '/');
	base = base ? base + 1 : name;
	memset(fat, ' ', 11);
	for (; *base; base++) {
		unsigned char c = (unsigned char)*base;
		if (c == '.' && !in_ext) {
			in_ext = 1;
			continue;
		}
		if (pattern && c == '*') {
			if (in_ext) {
				while (ei < 3) {
					fat[8 + ei++] = '?';
				}
			} else {
				while (ni < 8) {
					fat[ni++] = '?';
				}
			}
			continue;
		}
		if (!(pattern && c == '?')) {
			c = (unsigned char)toupper(c);
		}
		if (in_ext) {
			if (ei < 3) {
				fat[8 + ei++] = (char)c;
			}
		} else if (ni < 8) {
			fat[ni++] = (char)c;
		}
	}
}

static int fat11_match(const char *pat, const char *name) {
	for (int i = 0; i < 11; i++) {
		if (pat[i] == '?') {
			continue;
		}
		if (pat[i] != name[i]) {
			return 0;
		}
	}
	return 1;
}

static int is_wild(const char *s) {
	return strchr(s, '*') || strchr(s, '?');
}

static int match_wild(const char *pat, const char *host) {
	char p[11], n[11];
	if (strcmp(pat, "*") == 0 || strcmp(pat, "*.*") == 0) {
		return 1;
	}
	to_fat11(pat, p, 1);
	to_fat11(host, n, 0);
	return fat11_match(p, n);
}

static int name_eq(const char *req, const char *host) {
	char a[11], b[11];
	if (strcasecmp(req, host) == 0) {
		return 1;
	}
	to_fat11(req, a, 0);
	to_fat11(host, b, 0);
	return memcmp(a, b, 11) == 0;
}

static int name_stem_eq(const char *req, const char *host) {
	char a[11], b[11];
	to_fat11(req, a, 0);
	to_fat11(host, b, 0);
	return memcmp(a, b, 8) == 0;
}

static int lookup_child(const char *parent, const char *name, int allow_wild,
			char *out, size_t outsz) {
	DIR *d;
	struct dirent *de;
	struct stat st;
	int found = 0;

	if (strcmp(name, ".") == 0) {
		return join2(out, outsz, parent, "");
	}
	if (strcmp(name, "..") == 0) {
		char *slash;
		if (join2(out, outsz, parent, "") != 0) {
			return -1;
		}
		slash = strrchr(out, '/');
		if (!slash) {
			return -1;
		}
		if (slash == out) {
			slash[1] = 0;
		} else {
			*slash = 0;
		}
		return 0;
	}

	if (!allow_wild && !is_wild(name)) {
		if (join2(out, outsz, parent, name) != 0) {
			return -1;
		}
		if (stat(out, &st) == 0) {
			return 0;
		}
	}

	d = opendir(parent);
	if (!d) {
		return -1;
	}
	while ((de = readdir(d)) != NULL) {
		int match;
		if (de->d_name[0] == '.' &&
		    (de->d_name[1] == 0 || (de->d_name[1] == '.' && de->d_name[2] == 0))) {
			continue;
		}
		if (allow_wild && is_wild(name)) {
			match = match_wild(name, de->d_name);
		} else {
			match = name_eq(name, de->d_name);
		}
		if (match) {
			if (join2(out, outsz, parent, de->d_name) == 0) {
				found = 1;
			}
			break;
		}
	}
	if (!found && !allow_wild && !strchr(name, '.')) {
		rewinddir(d);
		while ((de = readdir(d)) != NULL) {
			if (de->d_name[0] == '.' &&
			    (de->d_name[1] == 0 || (de->d_name[1] == '.' && de->d_name[2] == 0))) {
				continue;
			}
			if (name_stem_eq(name, de->d_name)) {
				if (join2(out, outsz, parent, de->d_name) == 0) {
					found = 1;
				}
				break;
			}
		}
	}
	closedir(d);
	return found ? 0 : -1;
}

enum {
	R_MUST_EXIST = 1 << 0,
	R_MUST_DIR   = 1 << 1,
	R_CREATE     = 1 << 2,
	R_WILD_LEAF  = 1 << 3,
	R_LIST       = 1 << 4
};

struct resolved {
	char host[PATH_MAX];
	char parent[PATH_MAX];
	char leaf[SDC_PATH_MAX];
	int exists;
	int is_dir;
};

static int start_dir(const struct sdc_fs *fs, const char *sdc_path, char *cur, size_t cursz) {
	if (!fs->root) {
		return -1;
	}
	if (sdc_path && sdc_path[0] == '/') {
		return join2(cur, cursz, fs->root, "");
	}
	return join2(cur, cursz, fs->root, fs->cwd_rel ? fs->cwd_rel : "");
}

static int resolve_path(const struct sdc_fs *fs, const char *sdc_path, unsigned flags,
			struct resolved *r) {
	char copy[SDC_PATH_MAX + 1];
	char cur[PATH_MAX];
	char *p;
	char *parts[64];
	int nparts = 0;
	int i;
	struct stat st;

	memset(r, 0, sizeof(*r));
	if (!fs->root) {
		return SDC_ERR_MISC;
	}
	if (start_dir(fs, sdc_path, cur, sizeof(cur)) != 0) {
		return SDC_ERR_INVALID;
	}
	if (!under_root(fs->root, cur)) {
		return SDC_ERR_INVALID;
	}

	if (!sdc_path) {
		sdc_path = "";
	}
	while (*sdc_path == '/') {
		sdc_path++;
	}
	if (strlen(sdc_path) > SDC_PATH_MAX) {
		return SDC_ERR_INVALID;
	}
	memcpy(copy, sdc_path, strlen(sdc_path) + 1);

	p = copy;
	while (*p) {
		char *start;
		while (*p == '/') {
			p++;
		}
		if (*p == 0) {
			break;
		}
		start = p;
		while (*p && *p != '/') {
			p++;
		}
		if (*p) {
			*p++ = 0;
		}
		if (start[0] == 0) {
			return SDC_ERR_INVALID;
		}
		if (nparts >= 64) {
			return SDC_ERR_INVALID;
		}
		parts[nparts++] = start;
	}

	if (join2(r->parent, sizeof(r->parent), cur, "") != 0) {
		return SDC_ERR_INVALID;
	}

	for (i = 0; i < nparts; i++) {
		int last = (i == nparts - 1);
		int wild = last && (flags & R_WILD_LEAF);
		char next[PATH_MAX];

		if (last) {
			snprintf(r->leaf, sizeof(r->leaf), "%s", parts[i]);
			if (join2(r->parent, sizeof(r->parent), cur, "") != 0) {
				return SDC_ERR_INVALID;
			}
			/* Directory listing: wildcard leaf stays on the parent dir. */
			if ((flags & R_LIST) && is_wild(parts[i])) {
				if (join2(r->host, sizeof(r->host), cur, "") != 0) {
					return SDC_ERR_INVALID;
				}
				r->exists = 1;
				r->is_dir = 1;
				return 0;
			}
		}

		if (lookup_child(cur, parts[i], wild, next, sizeof(next)) != 0) {
			if (last && (flags & R_CREATE) && !is_wild(parts[i])) {
				if (join2(next, sizeof(next), cur, parts[i]) != 0) {
					return SDC_ERR_INVALID;
				}
				if (!under_root(fs->root, next)) {
					return SDC_ERR_INVALID;
				}
				if (join2(r->host, sizeof(r->host), next, "") != 0) {
					return SDC_ERR_INVALID;
				}
				r->exists = 0;
				r->is_dir = 0;
				return 0;
			}
			return SDC_ERR_NOTFOUND;
		}
		if (!under_root(fs->root, next)) {
			return SDC_ERR_INVALID;
		}
		if (join2(cur, sizeof(cur), next, "") != 0) {
			return SDC_ERR_INVALID;
		}
		if (!last) {
			if (stat(cur, &st) != 0 || !S_ISDIR(st.st_mode)) {
				return SDC_ERR_NOTFOUND;
			}
		}
	}

	if (join2(r->host, sizeof(r->host), cur, "") != 0) {
		return SDC_ERR_INVALID;
	}
	if (nparts == 0) {
		r->leaf[0] = 0;
		if (join2(r->parent, sizeof(r->parent), cur, "") != 0) {
			return SDC_ERR_INVALID;
		}
	}

	if (stat(r->host, &st) == 0) {
		r->exists = 1;
		r->is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
	} else {
		r->exists = 0;
		r->is_dir = 0;
		if (flags & R_MUST_EXIST) {
			return SDC_ERR_NOTFOUND;
		}
	}
	if ((flags & R_MUST_DIR) && (!r->exists || !r->is_dir)) {
		return SDC_ERR_NOTFOUND;
	}
	return 0;
}

static int cwd_host(const struct sdc_fs *fs, char *out, size_t outsz) {
	return join2(out, outsz, fs->root, fs->cwd_rel ? fs->cwd_rel : "");
}

static int set_cwd_from_host(struct sdc_fs *fs, const char *host) {
	size_t n;
	if (!under_root(fs->root, host)) {
		return SDC_ERR_INVALID;
	}
	n = strlen(fs->root);
	free(fs->cwd_rel);
	if (host[n] == 0) {
		fs->cwd_rel = sdc_strdup("");
	} else {
		fs->cwd_rel = sdc_strdup(host + n + (host[n] == '/' ? 1 : 0));
	}
	return fs->cwd_rel ? 0 : SDC_ERR_MISC;
}

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static uint32_t file_size_path(const char *path) {
	struct stat st;
	if (stat(path, &st) != 0 || st.st_size < 0) {
		return 0;
	}
	if ((unsigned long long)st.st_size > 0xffffffffull) {
		return 0xffffffffu;
	}
	return (uint32_t)st.st_size;
}

static uint32_t slot_size(struct sdc_slot *s) {
	long pos, end;
	if (!s->fp) {
		return s->host_path ? file_size_path(s->host_path) : 0;
	}
	pos = ftell(s->fp);
	if (pos < 0) {
		return s->host_path ? file_size_path(s->host_path) : 0;
	}
	if (fseek(s->fp, 0, SEEK_END) != 0) {
		return 0;
	}
	end = ftell(s->fp);
	(void)fseek(s->fp, pos, SEEK_SET);
	if (end < 0) {
		return 0;
	}
	if (end > 0x7fffffffL) {
		return 0xffffffffu;
	}
	return (uint32_t)end;
}

static void fat83_into(const char *name, char n[8], char e[3]) {
	char fat[11];
	to_fat11(name, fat, 0);
	memcpy(n, fat, 8);
	memcpy(e, fat + 8, 3);
}

static uint8_t attr_for_path(const char *path, int is_dir) {
	uint8_t a = 0;
	if (is_dir) {
		a |= SDC_ATTR_DIR;
	}
	if (access(path, W_OK) != 0) {
		a |= SDC_ATTR_LOCKED;
	}
	return a;
}

static void slot_fill_meta(struct sdc_slot *s) {
	const char *base;
	if (!s->host_path) {
		return;
	}
	base = strrchr(s->host_path, '/');
	base = base ? base + 1 : s->host_path;
	fat83_into(base, s->name, s->ext);
	s->attr = attr_for_path(s->host_path, 0);
}

static int slot_ensure(struct sdc_slot *s) {
	if (s->fp) {
		return 0;
	}
	if (!s->host_path) {
		return -1;
	}
	s->fp = fopen(s->host_path, "r+b");
	if (s->fp) {
		s->writable = 1;
	} else {
		s->fp = fopen(s->host_path, "rb");
		s->writable = 0;
	}
	if (!s->fp) {
		return -1;
	}
	slot_fill_meta(s);
	return 0;
}

static int file_in_use(struct sdc_fs *fs, const char *host, int except_drive) {
	struct stat st, o;
	if (stat(host, &st) != 0) {
		return 0;
	}
	for (int i = 0; i < SDC_SLOTS; i++) {
		if (i == except_drive || !fs->slot[i].host_path) {
			continue;
		}
		if (stat(fs->slot[i].host_path, &o) == 0 &&
		    o.st_dev == st.st_dev && o.st_ino == st.st_ino) {
			return 1;
		}
	}
	return 0;
}

static void slot_apply_floppy_geom(struct sdc_slot *s, uint32_t payload,
				   int sides_known, uint8_t sides) {
	uint32_t sectors = payload / SDC_BLOCK_SIZE;

	s->spt = SDC_FLOPPY_SPT;
	if (sectors > SDC_FLOPPY_MAX_SEC) {
		s->sides = 1;
		s->fdc_sectors = 1440;
		return;
	}
	if (sides_known && (sides == 1 || sides == 2)) {
		s->sides = sides;
	} else {
		s->sides = (sectors > 720) ? 2 : 1;
	}
	s->fdc_sectors = sectors;
}

/* M:/N: disk image: JVC/VDK header and DECB-usable geometry.  m:/n: raw. */
static int slot_probe_image(struct sdc_slot *s, int raw) {
	uint8_t hdr[16];
	uint32_t sz;
	size_t n;
	uint32_t header = 0;
	uint8_t sides = 1;
	int sides_known = 0;

	s->fdc_ok = 0;
	s->type = SDC_DTYPE_RAW;
	s->header = 0;
	s->spt = SDC_FLOPPY_SPT;
	s->sides = 1;
	s->fdc_sectors = 0;
	if (raw) {
		return 0;
	}
	if (!s->fp) {
		return SDC_ERR_MISC;
	}
	sz = slot_size(s);
	if (fseek(s->fp, 0, SEEK_SET) != 0) {
		return SDC_ERR_MISC;
	}
	memset(hdr, 0, sizeof(hdr));
	n = fread(hdr, 1, 12, s->fp);

	if (sz >= 219262u && n >= 4 && memcmp(hdr, "SDF1", 4) == 0) {
		return SDC_ERR_INVALID;
	}

	if (n >= 12 && hdr[0] == 'd' && hdr[1] == 'k') {
		uint32_t hsz = (uint32_t)hdr[2] | ((uint32_t)hdr[3] << 8);
		if (hsz < 12 || hsz > 256 || hsz > sz) {
			return SDC_ERR_INVALID;
		}
		header = hsz;
		sides = hdr[9];
		sides_known = 1;
		if (hdr[10] & 0x01) {
			s->writable = 0;
			s->attr |= SDC_ATTR_LOCKED;
		}
		s->type = SDC_DTYPE_VDK;
	} else {
		header = sz & 255u;
		if (header == 0) {
			if (sz < SDC_DSK_MIN_BYTES) {
				return SDC_ERR_INVALID;
			}
			s->type = SDC_DTYPE_DSK;
		} else if (header <= 4) {
			if (n < header) {
				return SDC_ERR_INVALID;
			}
			/* This controller exposes 18 sectors of 256 bytes per
			 * track. Reject incompatible JVC geometry rather than
			 * mounting it and silently addressing the wrong bytes. */
			if (hdr[0] != SDC_FLOPPY_SPT ||
			    (header >= 3 && hdr[2] != 1) ||
			    (header >= 2 && hdr[1] != 1 && hdr[1] != 2)) {
				return SDC_ERR_INVALID;
			}
			s->type = SDC_DTYPE_JVC;
			/* JVC's omitted side-count byte defaults to one, even
			 * when an 80-track image exceeds the raw-size heuristic. */
			sides_known = 1;
			if (header >= 2) {
				sides = hdr[1];
			}
		} else {
			return SDC_ERR_INVALID;
		}
	}

	if (sz < header + SDC_BLOCK_SIZE) {
		return SDC_ERR_INVALID;
	}
	s->header = header;
	slot_apply_floppy_geom(s, sz - header, sides_known, sides);
	s->fdc_ok = 1;
	return 0;
}

static uint32_t adjust_lsn(const struct sdc_slot *s, uint32_t lsn, uint8_t cmd) {
	uint16_t spt = s->spt ? s->spt : (uint16_t)SDC_FLOPPY_SPT;
	if ((cmd & 0x02) && s->sides == 2 && spt) {
		uint32_t trk = lsn / spt;
		uint32_t sec = lsn % spt;
		return trk * 2u * spt + sec;
	}
	return lsn;
}

static int chs_to_lsn(const struct sdc_slot *s, unsigned track, unsigned sector,
		      unsigned side, uint32_t *out) {
	unsigned sides = s->sides ? s->sides : 1u;
	unsigned spt = s->spt ? s->spt : SDC_FLOPPY_SPT;
	uint32_t lsn;

	if (sector < 1 || sector > spt) {
		return SDC_FDC_RNF;
	}
	if (side >= sides) {
		if (sides == 1) {
			side = 0;
		} else {
			return SDC_FDC_RNF;
		}
	}
	lsn = (uint32_t)(track * sides + side) * spt + (sector - 1);
	if (s->fdc_sectors && lsn >= s->fdc_sectors) {
		return SDC_FDC_RNF;
	}
	*out = lsn;
	return SDC_FDC_OK;
}

static uint32_t lsn_of(const struct sdc_hw *hw) {
	return ((uint32_t)hw->latched_preg[0] << 16) |
	       ((uint32_t)hw->latched_preg[1] << 8) | hw->latched_preg[2];
}

static int parse_cmd_string(const uint8_t *block, char *letter, char *path, size_t path_max) {
	size_t i = 0;
	if (block[0] == 0 || block[1] != ':') {
		return -1;
	}
	*letter = (char)block[0];
	while (i + 2 < SDC_BLOCK_SIZE && block[i + 2] != 0 && i + 1 < path_max) {
		path[i] = (char)block[i + 2];
		i++;
	}
	path[i] = 0;
	return 0;
}

static void fill_info_record(uint8_t *b, const char n[8], const char e[3],
			     uint8_t attr, uint32_t size) {
	memset(b, 0, 32);
	memcpy(b, n, 8);
	memcpy(b + 8, e, 3);
	b[11] = attr;
	b[28] = (uint8_t)size;
	b[29] = (uint8_t)(size >> 8);
	b[30] = (uint8_t)(size >> 16);
	b[31] = (uint8_t)(size >> 24);
}

static void fill_dir_record(uint8_t rec[16], const char *host_name, const char *host_path, int is_dir) {
	char fat[11];
	uint32_t sz = is_dir ? 0 : file_size_path(host_path);
	to_fat11(host_name, fat, 0);
	memset(rec, 0, 16);
	memcpy(rec, fat, 11);
	rec[11] = attr_for_path(host_path, is_dir);
	rec[12] = (uint8_t)(sz >> 24);
	rec[13] = (uint8_t)(sz >> 16);
	rec[14] = (uint8_t)(sz >> 8);
	rec[15] = (uint8_t)sz;
}

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void cmd_eject(struct sdc_fs *fs, struct sdc_hw *hw, unsigned drive) {
	slot_close(&fs->slot[drive]);
	if (hw) {
		sdc_hw_succeed(hw);
	}
}

/* letter M/m/N/n.  hw may be NULL (STARTUP.CFG).  Returns 0 or SDC_ERR_*. */
static int fs_mount_path(struct sdc_fs *fs, unsigned drive, char letter,
			 const char *path, struct sdc_hw *hw) {
	struct resolved r;
	unsigned flags = R_WILD_LEAF;
	int create = (letter == 'n' || letter == 'N');
	int raw = (letter == 'm' || letter == 'n');
	int err;
	struct sdc_slot *s;
	FILE *fp;
	int writable = 0;

	if (drive >= SDC_SLOTS) {
		return SDC_ERR_INVALID;
	}
	if (!path || path[0] == 0) {
		if (letter == 'M' || letter == 'm') {
			slot_close(&fs->slot[drive]);
			return 0;
		}
		return SDC_ERR_INVALID;
	}

	if (create) {
		flags = R_CREATE;
	}

	err = resolve_path(fs, path, flags, &r);
	if (err) {
		return err;
	}
	if (r.is_dir) {
		return SDC_ERR_INVALID;
	}

	slot_close(&fs->slot[drive]);

	if (r.exists && file_in_use(fs, r.host, (int)drive)) {
		return SDC_ERR_INUSE;
	}

	if (!r.exists) {
		if (!create) {
			return SDC_ERR_NOTFOUND;
		}
		fp = fopen(r.host, "w+b");
		if (!fp) {
			return SDC_ERR_MISC;
		}
		if (letter == 'N' && hw && hw->latched_preg[0] == 0 &&
		    hw->latched_preg[1] == 0 && hw->latched_preg[2] == 0) {
			if (fseek(fp, 630L * SDC_BLOCK_SIZE - 1, SEEK_SET) != 0 ||
			    fputc(0, fp) == EOF) {
				fclose(fp);
				return SDC_ERR_MISC;
			}
		}
		writable = 1;
	} else {
		fp = fopen(r.host, "r+b");
		if (fp) {
			writable = 1;
		} else {
			fp = fopen(r.host, "rb");
			writable = 0;
		}
		if (!fp) {
			return SDC_ERR_MISC;
		}
		if (!writable && (letter == 'n' || letter == 'N')) {
			fclose(fp);
			return SDC_ERR_MISC;
		}
	}

	s = &fs->slot[drive];
	s->fp = fp;
	s->writable = writable;
	s->host_path = sdc_strdup(r.host);
	if (!s->host_path) {
		slot_close(s);
		return SDC_ERR_MISC;
	}
	slot_fill_meta(s);
	err = slot_probe_image(s, raw);
	if (err) {
		slot_close(s);
		return err;
	}
	return 0;
}

static void cmd_mount(struct sdc_fs *fs, struct sdc_hw *hw, unsigned drive,
		      char letter, const char *path) {
	int err;

	if (path[0] == 0 && (letter == 'M' || letter == 'm')) {
		cmd_eject(fs, hw, drive);
		return;
	}
	err = fs_mount_path(fs, drive, letter, path, hw);
	if (err) {
		sdc_hw_fail(hw, (uint8_t)err);
		return;
	}
	sdc_hw_succeed(hw);
}

static int fs_set_cwd(struct sdc_fs *fs, const char *path) {
	struct resolved r;
	int err;
	const char *p = (path && path[0]) ? path : "/";
	err = resolve_path(fs, p, R_MUST_EXIST | R_MUST_DIR, &r);
	if (err) {
		return err;
	}
	return set_cwd_from_host(fs, r.host);
}

static void cmd_set_cwd(struct sdc_fs *fs, struct sdc_hw *hw, const char *path) {
	int err = fs_set_cwd(fs, path);
	if (err) {
		sdc_hw_fail(hw, (uint8_t)err);
		return;
	}
	sdc_hw_succeed(hw);
}

static int dirents_push(struct sdc_fs *fs, const uint8_t rec[16]) {
	struct sdc_dir_rec *n;
	if (fs->ndirents >= fs->dir_cap) {
		unsigned cap = fs->dir_cap ? fs->dir_cap * 2 : 16;
		n = realloc(fs->dirents, cap * sizeof(*n));
		if (!n) {
			return -1;
		}
		fs->dirents = n;
		fs->dir_cap = cap;
	}
	memcpy(fs->dirents[fs->ndirents].rec, rec, 16);
	fs->ndirents++;
	return 0;
}

static void cmd_init_dir(struct sdc_fs *fs, struct sdc_hw *hw, const char *path) {
	struct resolved r;
	char dirpath[PATH_MAX];
	char pattern[SDC_PATH_MAX];
	DIR *d;
	struct dirent *de;
	unsigned flags;
	int err;

	listing_clear(fs);

	snprintf(pattern, sizeof(pattern), "*.*");
	if (!path[0] || strcmp(path, "*") == 0 || strcmp(path, "*.*") == 0) {
		if (cwd_host(fs, dirpath, sizeof(dirpath)) != 0) {
			sdc_hw_fail(hw, SDC_ERR_MISC);
			return;
		}
	} else {
		int wild = is_wild(path) || is_wild(strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
		flags = wild ? (R_LIST | R_WILD_LEAF) : (R_MUST_EXIST);
		err = resolve_path(fs, path, flags, &r);
		if (err) {
			sdc_hw_fail(hw, (uint8_t)err);
			return;
		}
		if (r.is_dir) {
			if (join2(dirpath, sizeof(dirpath), r.host, "") != 0) {
				sdc_hw_fail(hw, SDC_ERR_INVALID);
				return;
			}
		} else {
			if (join2(dirpath, sizeof(dirpath), r.parent, "") != 0) {
				sdc_hw_fail(hw, SDC_ERR_INVALID);
				return;
			}
			snprintf(pattern, sizeof(pattern), "%s", r.leaf[0] ? r.leaf : "*.*");
		}
		if (wild && r.leaf[0]) {
			snprintf(pattern, sizeof(pattern), "%s", r.leaf);
		}
	}

	d = opendir(dirpath);
	if (!d) {
		sdc_hw_fail(hw, SDC_ERR_NOTFOUND);
		return;
	}
	while ((de = readdir(d)) != NULL) {
		char full[PATH_MAX];
		struct stat st;
		uint8_t rec[16];
		int is_dir;
		if (de->d_name[0] == '.' &&
		    (de->d_name[1] == 0 || (de->d_name[1] == '.' && de->d_name[2] == 0))) {
			continue;
		}
		if (!match_wild(pattern, de->d_name)) {
			continue;
		}
		if (join2(full, sizeof(full), dirpath, de->d_name) != 0) {
			continue;
		}
		if (stat(full, &st) != 0) {
			continue;
		}
		is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
		if (!is_dir && !S_ISREG(st.st_mode)) {
			continue;
		}
		fill_dir_record(rec, de->d_name, full, is_dir);
		if (dirents_push(fs, rec) != 0) {
			closedir(d);
			listing_clear(fs);
			sdc_hw_fail(hw, SDC_ERR_MISC);
			return;
		}
	}
	closedir(d);
	fs->listing_active = 1;
	fs->dir_index = 0;
	sdc_hw_succeed(hw);
}

static void cmd_mkdir(struct sdc_fs *fs, struct sdc_hw *hw, const char *path) {
	struct resolved r;
	int err;
	if (!path[0]) {
		sdc_hw_fail(hw, SDC_ERR_INVALID);
		return;
	}
	err = resolve_path(fs, path, R_CREATE, &r);
	if (err) {
		sdc_hw_fail(hw, (uint8_t)err);
		return;
	}
	if (r.exists) {
		sdc_hw_fail(hw, SDC_ERR_INUSE);
		return;
	}
	if (mkdir(r.host, 0755) != 0) {
		sdc_hw_fail(hw, errno == ENOENT ? SDC_ERR_NOTFOUND : SDC_ERR_MISC);
		return;
	}
	sdc_hw_succeed(hw);
}

static void cmd_delete(struct sdc_fs *fs, struct sdc_hw *hw, const char *path) {
	struct resolved r;
	int err;
	if (!path[0]) {
		sdc_hw_fail(hw, SDC_ERR_INVALID);
		return;
	}
	err = resolve_path(fs, path, R_MUST_EXIST, &r);
	if (err) {
		sdc_hw_fail(hw, (uint8_t)err);
		return;
	}
	if (file_in_use(fs, r.host, -1)) {
		sdc_hw_fail(hw, SDC_ERR_INUSE);
		return;
	}
	if (r.is_dir) {
		if (rmdir(r.host) != 0) {
			sdc_hw_fail(hw, SDC_ERR_MISC);
			return;
		}
	} else if (unlink(r.host) != 0) {
		sdc_hw_fail(hw, SDC_ERR_MISC);
		return;
	}
	sdc_hw_succeed(hw);
}

static void cmd_extd(struct sdc_fs *fs, struct sdc_hw *hw, unsigned drive) {
	char letter;
	char path[SDC_PATH_MAX];

	if (parse_cmd_string(hw->block, &letter, path, sizeof(path)) != 0) {
		sdc_hw_fail(hw, SDC_ERR_INVALID);
		return;
	}
	switch (letter) {
	case 'M':
	case 'm':
	case 'N':
	case 'n':
		cmd_mount(fs, hw, drive, letter, path);
		break;
	case 'D':
		cmd_set_cwd(fs, hw, path);
		break;
	case 'L':
		cmd_init_dir(fs, hw, path);
		break;
	case 'K':
		cmd_mkdir(fs, hw, path);
		break;
	case 'X':
		cmd_delete(fs, hw, path);
		break;
	default:
		sdc_hw_fail(hw, SDC_ERR_INVALID);
		break;
	}
}

static void cmd_info(struct sdc_fs *fs, struct sdc_hw *hw, unsigned drive) {
	struct sdc_slot *s = &fs->slot[drive];
	memset(hw->block, 0, SDC_BLOCK_SIZE);
	if (slot_ensure(s) != 0) {
		sdc_hw_fail(hw, SDC_ERR_NOTFOUND);
		return;
	}
	fill_info_record(hw->block, s->name, s->ext, s->attr, slot_size(s));
	sdc_hw_start_rx(hw);
}

static void cmd_query(struct sdc_fs *fs, struct sdc_hw *hw, unsigned drive) {
	struct sdc_slot *s = &fs->slot[drive];
	uint32_t sz, sectors;
	if (slot_ensure(s) != 0) {
		sdc_hw_fail(hw, SDC_ERR_NOTFOUND);
		return;
	}
	sz = slot_size(s);
	if (s->header && sz > s->header) {
		sz -= s->header;
	}
	sectors = (sz + (SDC_BLOCK_SIZE - 1)) / SDC_BLOCK_SIZE;
	hw->preg[0] = (uint8_t)(sectors >> 16);
	hw->preg[1] = (uint8_t)(sectors >> 8);
	hw->preg[2] = (uint8_t)sectors;
	sdc_hw_succeed(hw);
}

static void cmd_cwd(struct sdc_fs *fs, struct sdc_hw *hw) {
	const char *leaf;
	char n[8], e[3];
	memset(hw->block, 0, SDC_BLOCK_SIZE);
	if (!fs->root) {
		sdc_hw_fail(hw, SDC_ERR_NOTFOUND);
		return;
	}
	if (!fs->cwd_rel || !fs->cwd_rel[0]) {
		/* Root: User Guide sets bits 4 and 7.  CommSDC treats bit 7 as
		 * FAILED, so the 256-byte zero record is not transferred. */
		sdc_hw_fail(hw, SDC_ERR_NOTFOUND);
		return;
	}
	leaf = strrchr(fs->cwd_rel, '/');
	leaf = leaf ? leaf + 1 : fs->cwd_rel;
	fat83_into(leaf, n, e);
	fill_info_record(hw->block, n, e, SDC_ATTR_DIR, 0);
	sdc_hw_start_rx(hw);
}

static void cmd_dir_page(struct sdc_fs *fs, struct sdc_hw *hw) {
	unsigned i;
	memset(hw->block, 0, SDC_BLOCK_SIZE);
	if (!fs->listing_active) {
		sdc_hw_fail(hw, 0);
		return;
	}
	for (i = 0; i < 16; i++) {
		if (fs->dir_index >= fs->ndirents) {
			break;
		}
		memcpy(hw->block + i * 16, fs->dirents[fs->dir_index].rec, 16);
		fs->dir_index++;
	}
	sdc_hw_start_rx(hw);
}

static void cmd_ext(struct sdc_fs *fs, struct sdc_hw *hw, unsigned drive) {
	switch (hw->preg[0]) {
	case 'I':
		cmd_info(fs, hw, drive);
		break;
	case '>':
		cmd_dir_page(fs, hw);
		break;
	case 'C':
		cmd_cwd(fs, hw);
		break;
	case 'Q':
		cmd_query(fs, hw, drive);
		break;
	default:
		sdc_hw_succeed(hw);
		break;
	}
}

static void cmd_read(struct sdc_fs *fs, struct sdc_hw *hw, unsigned drive) {
	struct sdc_slot *s = &fs->slot[drive];
	uint32_t lsn = adjust_lsn(s, lsn_of(hw), hw->cmd);
	uint64_t off = (uint64_t)s->header + (uint64_t)lsn * SDC_BLOCK_SIZE;
	uint32_t sz;
	size_t n;

	if (slot_ensure(s) != 0) {
		sdc_hw_fail(hw, SDC_ERR_NOTFOUND);
		return;
	}
	sz = slot_size(s);
	if (off >= (uint64_t)sz) {
		sdc_hw_fail(hw, 0);
		return;
	}
	memset(hw->block, 0, SDC_BLOCK_SIZE);
	if (fseek(s->fp, (long)off, SEEK_SET) != 0) {
		sdc_hw_fail(hw, SDC_ERR_MISC);
		return;
	}
	n = (size_t)(sz - (uint32_t)off);
	if (n > SDC_BLOCK_SIZE) {
		n = SDC_BLOCK_SIZE;
	}
	if (fread(hw->block, 1, n, s->fp) != n && ferror(s->fp)) {
		sdc_hw_fail(hw, SDC_ERR_MISC);
		return;
	}
	/* $8x bit 2 selects byte reads through $FF4B. Bit 1 is the
	 * independent single-sided LSN flag already handled above. */
	hw->stream_8bit = (hw->cmd & 0x04) != 0;
	sdc_hw_start_rx(hw);
}

static void cmd_write(struct sdc_fs *fs, struct sdc_hw *hw, unsigned drive) {
	struct sdc_slot *s = &fs->slot[drive];
	uint32_t lsn = adjust_lsn(s, lsn_of(hw), hw->cmd);
	uint64_t off = (uint64_t)s->header + (uint64_t)lsn * SDC_BLOCK_SIZE;

	if (slot_ensure(s) != 0) {
		sdc_hw_fail(hw, SDC_ERR_NOTFOUND);
		return;
	}
	if (!s->writable) {
		sdc_hw_fail(hw, 0);
		return;
	}
	/* Mounted images have fixed geometry. Raw FileAccess mounts may grow
	 * by appending logical blocks, but an invalid disk LSN must not resize
	 * the image (and change how its geometry is inferred next time). */
	if (s->type != SDC_DTYPE_RAW && off + SDC_BLOCK_SIZE > slot_size(s)) {
		sdc_hw_fail(hw, 0);
		return;
	}
	if (fseek(s->fp, (long)off, SEEK_SET) != 0) {
		sdc_hw_fail(hw, SDC_ERR_MISC);
		return;
	}
	if (fwrite(hw->block, 1, SDC_BLOCK_SIZE, s->fp) != SDC_BLOCK_SIZE) {
		sdc_hw_fail(hw, SDC_ERR_MISC);
		return;
	}
	if (fflush(s->fp) != 0 || fsync(fileno(s->fp)) != 0) {
		sdc_hw_fail(hw, SDC_ERR_MISC);
		return;
	}
	sdc_hw_succeed(hw);
}

static void stream_fill(struct sdc_fs *fs, struct sdc_hw *hw) {
	struct sdc_slot *s = &fs->slot[hw->cmd & 0x01];
	uint32_t remain;
	size_t n;

	if (slot_ensure(s) != 0) {
		sdc_hw_fail(hw, SDC_ERR_NOTFOUND);
		return;
	}
	if (fs->stream_off >= fs->stream_end) {
		sdc_hw_succeed(hw);
		return;
	}
	remain = fs->stream_end - fs->stream_off;
	n = remain > SDC_STREAM_SIZE ? SDC_STREAM_SIZE : remain;
	memset(hw->block, 0, SDC_STREAM_SIZE);
	if (fseek(s->fp, (long)fs->stream_off, SEEK_SET) != 0) {
		sdc_hw_fail(hw, SDC_ERR_MISC);
		return;
	}
	if (fread(hw->block, 1, n, s->fp) != n && ferror(s->fp)) {
		sdc_hw_fail(hw, SDC_ERR_MISC);
		return;
	}
	fs->stream_off += (uint32_t)n;
	sdc_hw_start_stream_rx(hw);
}

/* $90/$91 (16-bit $FF4A/$FF4B) or $92/$93 (8-bit $FF4B).  LSN is a 512-byte
 * stream sector, not a 256-byte FileAccess LBN.  Refill uses hw->streaming
 * so a new $90 (start_command clears it) restarts from the latched LSN. */
static void cmd_stream(struct sdc_fs *fs, struct sdc_hw *hw) {
	struct sdc_slot *s;
	uint32_t lsn;
	uint64_t off;
	uint32_t sz;

	if (hw->streaming) {
		stream_fill(fs, hw);
		return;
	}

	s = &fs->slot[hw->cmd & 0x01];
	hw->stream_8bit = (hw->cmd & 0x02) != 0;
	if (slot_ensure(s) != 0) {
		sdc_hw_fail(hw, SDC_ERR_NOTFOUND);
		return;
	}
	lsn = lsn_of(hw);
	off = (uint64_t)lsn * (uint64_t)SDC_STREAM_SIZE;
	sz = slot_size(s);
	if (off >= (uint64_t)sz) {
		sdc_hw_fail(hw, 0);
		return;
	}
	fs->stream_off = (uint32_t)off;
	fs->stream_end = sz;
	stream_fill(fs, hw);
}

void sdc_fs_execute(struct sdc_fs *fs, struct sdc_hw *hw) {
	unsigned family;
	unsigned drive;

	if (sdc_hw_take_builtin(hw)) {
		return;
	}
	if (!hw->cmd_ready) {
		return;
	}

	family = hw->cmd & 0xfe;
	drive = hw->cmd & 0x01;

	switch (family) {
	case 0xc0:
		cmd_ext(fs, hw, drive);
		break;
	case 0xe0:
		cmd_extd(fs, hw, drive);
		break;
	case 0xa0:
	case 0xa2: /* single-sided logical sector numbering */
		cmd_write(fs, hw, drive);
		break;
	case 0x80:
	case 0x82: /* single-sided logical sector numbering */
	case 0x84: /* byte reads through $FF4B */
	case 0x86: /* both flags */
		cmd_read(fs, hw, drive);
		break;
	case 0x90:
	case 0x92:
		cmd_stream(fs, hw);
		break;
	case 0xd0:
		/* Abort is handled in sdc_hw_write ($FF48=$D0).  Kept so a
		 * cmd_ready $D0 (tests / older path) still cannot hang. */
		sdc_hw_succeed(hw);
		break;
	default:
		/* Never report successful I/O for an unsupported opcode. */
		sdc_hw_fail(hw, SDC_ERR_INVALID);
		break;
	}
}

static char *trim_inplace(char *s) {
	char *e;
	while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
		s++;
	}
	e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) {
		*--e = 0;
	}
	return s;
}

static char *strip_quotes(char *s) {
	size_t n;
	if (!s || s[0] == 0) {
		return s;
	}
	n = strlen(s);
	if (n >= 2 && ((s[0] == '"' && s[n - 1] == '"') || (s[0] == '\'' && s[n - 1] == '\''))) {
		s[n - 1] = 0;
		return s + 1;
	}
	return s;
}

int sdc_fs_apply_startup(struct sdc_fs *fs) {
	char cfg[PATH_MAX];
	char buf[512];
	FILE *fp;
	int first_err = 0;
	int saw_cfg = 0;

	if (!fs || !fs->root) {
		return 1;
	}
	if (lookup_child(fs->root, "STARTUP.CFG", 0, cfg, sizeof(cfg)) != 0) {
		return 1;
	}
	fp = fopen(cfg, "r");
	if (!fp) {
		return 1;
	}
	saw_cfg = 1;
	while (fgets(buf, (int)sizeof(buf), fp) != NULL) {
		char *p = trim_inplace(buf);
		char key;
		int err;
		if (p[0] == (char)0xef && (unsigned char)p[1] == 0xbb &&
		    (unsigned char)p[2] == 0xbf) {
			p += 3;
			p = trim_inplace(p);
		}
		if (p[0] == 0 || p[0] == '#' || p[0] == ';') {
			continue;
		}
		key = (char)toupper((unsigned char)p[0]);
		p++;
		p = trim_inplace(p);
		if (*p != '=') {
			continue;
		}
		p++;
		p = trim_inplace(p);
		p = strip_quotes(p);
		if (key == '0') {
			err = fs_mount_path(fs, 0, 'M', p, NULL);
			if (err && !first_err) {
				first_err = err;
			}
		} else if (key == '1') {
			err = fs_mount_path(fs, 1, 'M', p, NULL);
			if (err && !first_err) {
				first_err = err;
			}
		} else if (key == 'D') {
			(void)fs_set_cwd(fs, p);
		}
	}
	fclose(fp);
	(void)saw_cfg;
	return first_err;
}

int sdc_fs_fdc_ready(const struct sdc_fs *fs, unsigned drive) {
	if (!fs || drive >= SDC_SLOTS) {
		return 0;
	}
	return fs->slot[drive].fdc_ok && fs->slot[drive].host_path != NULL;
}

int sdc_fs_fdc_wp(const struct sdc_fs *fs, unsigned drive) {
	if (!fs || drive >= SDC_SLOTS) {
		return 1;
	}
	return !fs->slot[drive].writable;
}

static int fdc_xfer(struct sdc_fs *fs, unsigned drive, unsigned track,
		    unsigned sector, unsigned side, uint8_t *buf, int wr) {
	struct sdc_slot *s;
	uint32_t lsn;
	uint64_t off;
	int rc;

	if (!fs || drive >= SDC_SLOTS || !buf) {
		return SDC_FDC_NOTREADY;
	}
	s = &fs->slot[drive];
	if (!s->fdc_ok || slot_ensure(s) != 0) {
		return SDC_FDC_NOTREADY;
	}
	if (wr && !s->writable) {
		return SDC_FDC_WP;
	}
	rc = chs_to_lsn(s, track, sector, side, &lsn);
	if (rc != SDC_FDC_OK) {
		return rc;
	}
	off = (uint64_t)s->header + (uint64_t)lsn * SDC_BLOCK_SIZE;
	if (fseek(s->fp, (long)off, SEEK_SET) != 0) {
		return SDC_FDC_IO;
	}
	if (wr) {
		if (fwrite(buf, 1, SDC_BLOCK_SIZE, s->fp) != SDC_BLOCK_SIZE) {
			return SDC_FDC_IO;
		}
		if (fflush(s->fp) != 0 || fsync(fileno(s->fp)) != 0) {
			return SDC_FDC_IO;
		}
	} else {
		size_t n = fread(buf, 1, SDC_BLOCK_SIZE, s->fp);
		if (n < SDC_BLOCK_SIZE) {
			if (ferror(s->fp)) {
				return SDC_FDC_IO;
			}
			memset(buf + n, 0, SDC_BLOCK_SIZE - n);
			if (n == 0) {
				return SDC_FDC_RNF;
			}
		}
	}
	return SDC_FDC_OK;
}

int sdc_fs_fdc_read(struct sdc_fs *fs, unsigned drive, unsigned track,
		    unsigned sector, unsigned side, uint8_t *buf) {
	return fdc_xfer(fs, drive, track, sector, side, buf, 0);
}

int sdc_fs_fdc_write(struct sdc_fs *fs, unsigned drive, unsigned track,
		     unsigned sector, unsigned side, const uint8_t *buf) {
	return fdc_xfer(fs, drive, track, sector, side, (uint8_t *)buf, 1);
}

void sdc_fs_flush(struct sdc_fs *fs) {
	int i;

	if (!fs) {
		return;
	}
	for (i = 0; i < SDC_SLOTS; i++) {
		if (fs->slot[i].fp) {
			fflush(fs->slot[i].fp);
			(void)fsync(fileno(fs->slot[i].fp));
		}
	}
}
