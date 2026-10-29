#include "fat.h"
#include "rtc.h"
#include "ata.h"
#include "io.h"

/* ---------- fixed layout constants ---------- */
#define PART_START    1u        /* partition starts right after the MBR */
#define RESERVED      32u
#define NUM_FATS      2u
#define ROOT_CLUS     2u
#define DENT_SIZE     32

/* ---------- tiny freestanding helpers ---------- */
static void memz(void *d, int n) {
    unsigned char *p = (unsigned char *)d;
    while (n--) *p++ = 0;
}
static void memcpy_(void *d, const void *s, int n) {
    unsigned char *dp = (unsigned char *)d;
    const unsigned char *sp = (const unsigned char *)s;
    while (n--) *dp++ = *sp++;
}
static int slen_(const char *s) { int n = 0; while (s[n]) n++; return n; }
static char upper_(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }
static char lower_(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

/* ---------- on-disk directory entry ---------- */
struct __attribute__((packed)) dent {
    uint8_t  name[11];
    uint8_t  attr;
    uint8_t  ntres;
    uint8_t  crt_tenth;
    uint16_t crt_time;
    uint16_t crt_date;
    uint16_t acc_date;
    uint16_t clus_hi;
    uint16_t wrt_time;
    uint16_t wrt_date;
    uint16_t clus_lo;
    uint32_t size;
};

/* ---------- volume state (geometry is dynamic) ---------- */
static uint8_t  fat_cache[FAT_CACHE_MAX];   /* whole FAT #1 cached */
static uint32_t part_start   = PART_START;
static uint32_t fat_sz       = 0;           /* sectors per FAT */
static uint32_t data_start   = 0;           /* LBA of cluster 2 */
static uint32_t root_clus    = ROOT_CLUS;
static uint32_t next_free    = 3;           /* allocation hint */
static uint32_t vol_clusters = 0;           /* usable clusters */
static uint32_t max_clus     = 0;           /* FAT entries (fat_sz*512/4) */
static uint32_t vol_total    = 0;           /* drive sectors (IDENTIFY) */
static uint32_t spc          = 8;           /* sectors per cluster */

static uint32_t clus_lba(uint32_t c) { return data_start + (c - 2) * spc; }

uint32_t fat_cluster_size(void) { return spc * 512; }
uint32_t fat_disk_sectors(void) { return vol_total; }

/* ---------- FAT access ---------- */
uint32_t fat_get(uint32_t c) {
    if (c < 2 || c >= max_clus) return FAT_EOC;
    uint32_t off = c * 4;
    uint32_t v = (uint32_t)fat_cache[off]
               | ((uint32_t)fat_cache[off + 1] << 8)
               | ((uint32_t)fat_cache[off + 2] << 16)
               | ((uint32_t)fat_cache[off + 3] << 24);
    return v & 0x0FFFFFFF;
}

static void fat_set(uint32_t c, uint32_t v) {
    if (c < 2 || c >= max_clus) return;
    uint32_t off = c * 4;
    fat_cache[off]     = (uint8_t)(v & 0xFF);
    fat_cache[off + 1] = (uint8_t)((v >> 8) & 0xFF);
    fat_cache[off + 2] = (uint8_t)((v >> 16) & 0xFF);
    fat_cache[off + 3] = (uint8_t)((v >> 24) & 0x0F);
}

static void write_fat_region(uint32_t start) {
    uint32_t done = 0;
    while (done < fat_sz) {
        uint32_t n = fat_sz - done;
        if (n > 128) n = 128;
        ata_write_sectors(start + done, (uint8_t)n, fat_cache + done * 512);
        done += n;
    }
}

static int read_fat_region(void) {
    uint32_t done = 0;
    while (done < fat_sz) {
        uint32_t n = fat_sz - done;
        if (n > 128) n = 128;
        if (ata_read_sectors(part_start + RESERVED + done, (uint8_t)n,
                             fat_cache + done * 512) != 0) return -1;
        done += n;
    }
    return 0;
}

/* rewrite the FSInfo sector (free count + next-free hint).  It used to
 * be written once at mkfs and went permanently stale; Windows/Linux
 * mounters then suggest chkdsk. */
static void fsinfo_update(void) {
    uint8_t sec[512];
    memz(sec, sizeof(sec));
    sec[0] = 0x52; sec[1] = 0x52; sec[2] = 0x61; sec[3] = 0x41;   /* RRaA */
    sec[0x1E4] = 0x72; sec[0x1E5] = 0x72; sec[0x1E6] = 0x61; sec[0x1E7] = 0x41;
    uint32_t free = 0;
    for (uint32_t c = 2; c < max_clus; c++)
        if (fat_get(c) == 0) free++;
    sec[0x1E8] = (uint8_t)(free & 0xFF);
    sec[0x1E9] = (uint8_t)((free >> 8) & 0xFF);
    sec[0x1EA] = (uint8_t)((free >> 16) & 0xFF);
    sec[0x1EB] = (uint8_t)((free >> 24) & 0xFF);
    uint32_t nf = (next_free >= 3 && next_free < max_clus) ? next_free : 3;
    sec[0x1EC] = (uint8_t)(nf & 0xFF);
    sec[0x1ED] = (uint8_t)((nf >> 8) & 0xFF);
    sec[0x1EE] = (uint8_t)((nf >> 16) & 0xFF);
    sec[0x1EF] = (uint8_t)((nf >> 24) & 0xFF);
    sec[0x1FC] = 0x00; sec[0x1FD] = 0x00; sec[0x1FE] = 0x55; sec[0x1FF] = 0xAA;
    ata_write_sectors(part_start + 1, 1, sec);
}

static void fat_flush(void) {
    write_fat_region(part_start + RESERVED);           /* FAT #1 */
    write_fat_region(part_start + RESERVED + fat_sz);  /* FAT #2 */
    fsinfo_update();
}

uint32_t fat_alloc(void) {
    uint32_t start = (next_free >= 3 && next_free < max_clus) ? next_free : 3;
    for (uint32_t c = start; c < max_clus; c++)
        if (fat_get(c) == 0) { fat_set(c, FAT_EOC); next_free = c + 1; return c; }
    for (uint32_t c = 3; c < start; c++)
        if (fat_get(c) == 0) { fat_set(c, FAT_EOC); next_free = c + 1; return c; }
    return 0;                                  /* disk full */
}

void fat_free_chain(uint32_t clus) {
    uint32_t c = clus;
    while (c >= 2 && c < max_clus && fat_get(c) != FAT_EOC) {
        uint32_t n = fat_get(c);
        fat_set(c, 0);
        c = n;
    }
    if (c >= 2 && c < max_clus) fat_set(c, 0);
    fat_flush();
}

int fat_read_cluster(uint32_t clus, uint8_t *out) {
    if (clus < 2) return -1;
    return ata_read_sectors(clus_lba(clus), (uint8_t)spc, out);
}

int fat_write_cluster(uint32_t clus, const uint8_t *buf) {
    if (clus < 2) return -1;
    return ata_write_sectors(clus_lba(clus), (uint8_t)spc, buf);
}

/* ---------- names ---------- */
void fat_name83(const char *name, uint8_t out[11]) {
    for (int i = 0; i < 11; i++) out[i] = ' ';
    const char *dot = 0;
    for (const char *p = name; *p; p++) if (*p == '.') dot = p;
    int nlen = dot ? (int)(dot - name) : slen_(name);
    if (nlen > 8) nlen = 8;
    for (int i = 0; i < nlen; i++) out[i] = (uint8_t)upper_(name[i]);
    if (dot) {
        const char *e = dot + 1;
        int el = slen_(e);
        if (el > 3) el = 3;
        for (int i = 0; i < el; i++) out[8 + i] = (uint8_t)upper_(e[i]);
    }
}

void fat_name_show(const uint8_t *n83, char *out) {
    int o = 0;
    for (int i = 0; i < 8 && n83[i] != ' '; i++) out[o++] = lower_(n83[i]);
    int ext = 0;
    for (int i = 8; i < 11 && n83[i] != ' '; i++) {
        if (!ext) { out[o++] = '.'; ext = 1; }
        out[o++] = lower_(n83[i]);
    }
    out[o] = 0;
}

static int name83_eq(const uint8_t *a, const uint8_t *b) {
    for (int i = 0; i < 11; i++) if (a[i] != b[i]) return 0;
    return 1;
}

/* is this an 8.3 "." or ".." name? */
static int name83_dot(const uint8_t *n83, int *is_dotdot) {
    if (n83[0] == '.') {
        if (n83[1] == ' ') { *is_dotdot = 0; return 1; }
        if (n83[1] == '.' && n83[2] == ' ') { *is_dotdot = 1; return 1; }
    }
    return 0;
}

/* ---------- directory walking ---------- */
static int dir_walk(uint32_t dirclus,
                    int (*cb)(struct dent *, void *), void *arg) {
    static uint8_t buf[FAT_CLUSTER_SZ_MAX];
    uint32_t c = dirclus;
    int per = (int)(spc * 512 / DENT_SIZE);
    while (c >= 2 && c < max_clus) {
        if (fat_read_cluster(c, buf) != 0) return 1;
        for (int i = 0; i < per; i++) {
            struct dent *e = (struct dent *)(buf + i * DENT_SIZE);
            if (e->name[0] == 0x00) return 1;      /* end of directory */
            if (e->name[0] == 0xE5) continue;      /* deleted */
            if (e->attr == 0x0F) continue;         /* LFN entry */
            if (e->attr & 0x08) continue;          /* volume label */
            if (cb(e, arg) == 0) return 0;
        }
        uint32_t n = fat_get(c);
        if (n == FAT_EOC) break;
        c = n;
    }
    return 1;
}

struct find_arg { const uint8_t *n83; fat_dirent_t *out; int found; };

static int find_cb(struct dent *e, void *p) {
    struct find_arg *a = (struct find_arg *)p;
    if (name83_eq(e->name, a->n83)) {
        a->out->attr = e->attr;
        a->out->cluster = ((uint32_t)e->clus_hi << 16) | e->clus_lo;
        a->out->size = e->size;
        a->found = 1;
        return 0;                                  /* stop */
    }
    return 1;
}

int fat_lookup(uint32_t dirclus, const uint8_t *name83, fat_dirent_t *out) {
    int dotdot;
    if (name83_dot(name83, &dotdot)) {
        struct find_arg a;
        a.n83 = name83; a.out = out; a.found = 0;
        dir_walk(dirclus, find_cb, &a);
        if (!a.found && out) {
            out->attr = FAT_ATTR_DIR;
            out->cluster = dirclus;
            out->size = 0;
        }
        return 0;
    }
    struct find_arg a;
    a.n83 = name83; a.out = out; a.found = 0;
    dir_walk(dirclus, find_cb, &a);
    return a.found ? 0 : -1;
}

/* write a raw entry into dir at the first free slot (0x00 or 0xE5);
 * extends the directory with a new cluster if needed. */
static int dir_write_dent(uint32_t dirclus, const struct dent *nd) {
    static uint8_t buf[FAT_CLUSTER_SZ_MAX];
    uint32_t c = dirclus, last = 0;
    int per = (int)(spc * 512 / DENT_SIZE);
    while (c >= 2 && c < max_clus) {
        last = c;
        if (fat_read_cluster(c, buf) != 0) return -1;
        for (int i = 0; i < per; i++) {
            struct dent *e = (struct dent *)(buf + i * DENT_SIZE);
            if (e->name[0] == 0x00 || e->name[0] == 0xE5) {
                memcpy_(e, nd, DENT_SIZE);
                return fat_write_cluster(c, buf);
            }
        }
        uint32_t n = fat_get(c);
        if (n == FAT_EOC) break;
        c = n;
    }
    /* extend directory */
    uint32_t nc = fat_alloc();
    if (!nc) return -1;
    if (last) fat_set(last, nc);               /* flush deferred to caller */
    memz(buf, sizeof(buf));
    memcpy_(buf, nd, DENT_SIZE);
    if (fat_write_cluster(nc, buf) != 0) return -1;
    return 0;
}

/* FAT date/time from the RTC (files used to be stamped 1980-00-00) */
static void fat_stamp(uint16_t *dt, uint16_t *tm) {
    rtc_time_t t;
    rtc_read(&t);
    uint32_t y = t.year - 1980;
    if (y > 127) y = 0;
    *dt = (uint16_t)((y << 9) | ((t.month & 0xF) << 5) | (t.day & 0x1F));
    *tm = (uint16_t)(((t.hour & 0x1F) << 11) | ((t.minute & 0x3F) << 5)
                     | ((t.second / 2) & 0x1F));
}

static void dent_init(struct dent *d, const uint8_t *name83,
                      uint8_t attr, uint32_t clus, uint32_t size) {
    memz(d, sizeof(*d));
    memcpy_(d->name, name83, 11);
    d->attr = attr;
    d->clus_lo = (uint16_t)(clus & 0xFFFF);
    d->clus_hi = (uint16_t)(clus >> 16);
    d->size = size;
    fat_stamp(&d->crt_date, &d->crt_time);
    d->acc_date = d->crt_date;
    d->wrt_date = d->crt_date;
    d->wrt_time = d->crt_time;
}

int fat_mkdir(uint32_t parent, const uint8_t *name83) {
    fat_dirent_t tmp;
    if (fat_lookup(parent, name83, &tmp) == 0) return -1;   /* exists */

    uint32_t nc = fat_alloc();
    if (!nc) return -1;

    /* new directory: "." and ".." entries */
    static uint8_t buf[FAT_CLUSTER_SZ_MAX];
    memz(buf, sizeof(buf));
    uint8_t dotname[11], dotdotname[11];
    for (int i = 0; i < 11; i++) { dotname[i] = ' '; dotdotname[i] = ' '; }
    dotname[0] = '.'; dotdotname[0] = '.'; dotdotname[1] = '.';
    struct dent *d0 = (struct dent *)buf;
    struct dent *d1 = (struct dent *)(buf + DENT_SIZE);
    dent_init(d0, dotname, FAT_ATTR_DIR, nc, 0);
    dent_init(d1, dotdotname, FAT_ATTR_DIR, parent, 0);
    if (fat_write_cluster(nc, buf) != 0) return -1;

    struct dent de;
    dent_init(&de, name83, FAT_ATTR_DIR, nc, 0);
    if (dir_write_dent(parent, &de) != 0) return -1;
    fat_flush();
    return 0;
}

int fat_mkfile(uint32_t parent, const uint8_t *name83) {
    fat_dirent_t tmp;
    if (fat_lookup(parent, name83, &tmp) == 0) return -1;

    uint32_t nc = fat_alloc();
    if (!nc) return -1;
    static uint8_t buf[FAT_CLUSTER_SZ_MAX];
    memz(buf, sizeof(buf));
    fat_write_cluster(nc, buf);

    struct dent de;
    dent_init(&de, name83, FAT_ATTR_FILE, nc, 0);
    if (dir_write_dent(parent, &de) != 0) { fat_free_chain(nc); return -1; }
    fat_flush();
    return 0;
}

int fat_update_entry(uint32_t dirclus, const uint8_t *name83,
                     uint32_t cluster, uint32_t size) {
    static uint8_t buf[FAT_CLUSTER_SZ_MAX];
    uint32_t c = dirclus;
    int per = (int)(spc * 512 / DENT_SIZE);
    while (c >= 2 && c < max_clus) {
        if (fat_read_cluster(c, buf) != 0) return -1;
        for (int i = 0; i < per; i++) {
            struct dent *e = (struct dent *)(buf + i * DENT_SIZE);
            if (e->name[0] == 0x00) return -1;
            if (e->name[0] == 0xE5) continue;
            if (e->attr == 0x0F || (e->attr & 0x08)) continue;
            if (name83_eq(e->name, name83)) {
                e->clus_lo = (uint16_t)(cluster & 0xFFFF);
                e->clus_hi = (uint16_t)(cluster >> 16);
                e->size = size;
                fat_stamp(&e->wrt_date, &e->wrt_time);
                e->acc_date = e->wrt_date;
                return fat_write_cluster(c, buf);
            }
        }
        uint32_t n = fat_get(c);
        if (n == FAT_EOC) break;
        c = n;
    }
    return -1;
}

/* ---------- file data ---------- */
int fat_read_file(uint32_t clus, uint32_t size, char *out) {
    static uint8_t buf[FAT_CLUSTER_SZ_MAX];
    uint32_t c = clus, got = 0;
    uint32_t csz = spc * 512;
    if (size == 0) { out[0] = 0; return 0; }
    while (c >= 2 && c < max_clus) {
        if (fat_read_cluster(c, buf) != 0) return -1;
        uint32_t take = (size - got) < csz ? (size - got) : csz;
        memcpy_(out + got, buf, take);
        got += take;
        if (got >= size) break;
        uint32_t n = fat_get(c);
        if (n == FAT_EOC) break;
        c = n;
    }
    out[got] = 0;
    return 0;
}

int fat_write_file(uint32_t clus, const char *data, uint32_t size) {
    uint32_t csz = spc * 512;
    uint32_t need = (size == 0) ? 1 : ((size + csz - 1) / csz);
    uint32_t c = (clus >= 2) ? clus : 0;
    uint32_t prev = 0;
    static uint8_t buf[FAT_CLUSTER_SZ_MAX];

    /* undo log: on a mid-write failure the in-memory FAT used to keep the
     * half-finished chain links, which a LATER flush would persist.  Roll
     * every fat_set back so the FAT stays exactly as it was. */
    static struct { uint32_t clus, old; } undo[512];   /* static: a 4 KiB stack frame would pull in __chkstk_ms */
    int undo_n = 0;
    uint32_t c0 = c, prev0 = prev;

    for (uint32_t i = 0; i < need; i++) {
        memz(buf, sizeof(buf));
        uint32_t off = i * csz;
        uint32_t take = (size > off) ? ((size - off) < csz ? (size - off) : csz) : 0;
        if (take) memcpy_(buf, data + off, take);

        if (c < 2 || c >= max_clus) {           /* need a fresh cluster */
            uint32_t nc = fat_alloc();
            if (!nc) goto fail;
            if (prev && undo_n < 512) { undo[undo_n].clus = prev; undo[undo_n].old = fat_get(prev); undo_n++; }
            if (prev) fat_set(prev, nc);
            if (undo_n < 512) { undo[undo_n].clus = nc; undo[undo_n].old = 0; undo_n++; }
            c = nc;
        }
        if (fat_write_cluster(c, buf) != 0) goto fail;
        prev = c;
        c = fat_get(c);
    }

    /* truncate the rest of the old chain */
    while (c >= 2 && c < max_clus && fat_get(c) != FAT_EOC) {
        uint32_t n = fat_get(c);
        if (undo_n < 512) { undo[undo_n].clus = c; undo[undo_n].old = fat_get(c); undo_n++; }
        fat_set(c, 0);
        c = n;
    }
    if (c >= 2 && c < max_clus) {
        if (undo_n < 512) { undo[undo_n].clus = c; undo[undo_n].old = fat_get(c); undo_n++; }
        fat_set(c, 0);
    }
    if (prev) {
        if (undo_n < 512) { undo[undo_n].clus = prev; undo[undo_n].old = fat_get(prev); undo_n++; }
        fat_set(prev, FAT_EOC);
    }
    fat_flush();
    return 0;

fail:
    (void)c0; (void)prev0;
    while (undo_n > 0) { undo_n--; fat_set(undo[undo_n].clus, undo[undo_n].old); }
    return -1;                                  /* FAT memory unchanged */
}

/* ---------- probe ---------- */
uint32_t fat_root(void) { return root_clus; }

int fat_probe(void) {
    uint8_t mbr[512], dbr[512];

    /* discover the IDE slot and drive size first */
    uint32_t sec = 0;
    if (ata_identify(&sec) != 0 || sec == 0) return -1;

    if (ata_read_sectors(0, 1, mbr) != 0) return -1;
    if (!(mbr[510] == 0x55 && mbr[511] == 0xAA)) return -1;

    int found = -1;
    for (int i = 0; i < 4; i++) {
        uint8_t *e = mbr + 446 + i * 16;
        if (e[4] == 0x0B || e[4] == 0x0C) { found = i; break; }
    }
    if (found < 0) return -1;

    uint32_t part = (uint32_t)mbr[446 + found * 16 + 8]
                  | ((uint32_t)mbr[446 + found * 16 + 9] << 8)
                  | ((uint32_t)mbr[446 + found * 16 + 10] << 16)
                  | ((uint32_t)mbr[446 + found * 16 + 11] << 24);
    if (ata_read_sectors(part, 1, dbr) != 0) return -1;
    if (!(dbr[510] == 0x55 && dbr[511] == 0xAA)) return -1;

    uint16_t bps = (uint16_t)(dbr[0x0B] | (dbr[0x0C] << 8));
    if (bps != 512) return -1;
    uint32_t s = dbr[0x0D];
    if (s == 0) return -1;
    if (s * 512 > FAT_CLUSTER_SZ_MAX) return -1;   /* would overflow every
                                                      static cluster buffer */
    uint32_t rsvd = (uint32_t)(dbr[0x0E] | (dbr[0x0F] << 8));
    uint32_t nfats = dbr[0x10];
    uint32_t fatsz = (uint32_t)dbr[0x24]
                   | ((uint32_t)dbr[0x25] << 8)
                   | ((uint32_t)dbr[0x26] << 16)
                   | ((uint32_t)dbr[0x27] << 24);
    if (fatsz == 0 || fatsz * 512 > FAT_CACHE_MAX) return -1;
    uint32_t total32 = (uint32_t)dbr[0x20]
                     | ((uint32_t)dbr[0x21] << 8)
                     | ((uint32_t)dbr[0x22] << 16)
                     | ((uint32_t)dbr[0x23] << 24);
    uint32_t root = (uint32_t)dbr[0x2C]
                  | ((uint32_t)dbr[0x2D] << 8)
                  | ((uint32_t)dbr[0x2E] << 16)
                  | ((uint32_t)dbr[0x2F] << 24);

    if (root < 2) return -1;
    if (total32 <= rsvd + nfats * fatsz) return -1;  /* underflow guard */
    part_start = part;
    spc = s;
    fat_sz = fatsz;
    data_start = part + rsvd + nfats * fatsz;
    root_clus = root;
    max_clus = fatsz * 512 / 4;
    vol_total = total32 + part;
    vol_clusters = (total32 - (rsvd + nfats * fatsz)) / spc;

    if (read_fat_region() != 0) return -1;

    /* find first free cluster as allocation hint */
    next_free = 3;
    for (uint32_t c = 3; c < max_clus; c++)
        if (fat_get(c) == 0) { next_free = c; break; }
    return 0;
}

/* ---------- mkfs (geometry from ATA IDENTIFY) ---------- */
void fat_mkfs(void) {
    uint8_t mbr[512], sec[512];
    uint32_t i;

    /* 0) drive size */
    vol_total = 0;
    if (ata_identify(&vol_total) != 0 || vol_total <= 1)
        vol_total = 131072u;               /* fallback: 64 MiB */
    uint32_t part_sec = vol_total - 1;

    /* pick sectors/cluster by volume size (FAT32 convention) */
    spc = 1;
    if (part_sec >= 524288u)      spc = 8;      /* >= 256 MiB */
    if (part_sec >= 16777216u)    spc = 16;     /* >= 8 GiB   */
    if (part_sec >= 33554432u)    spc = 32;     /* >= 16 GiB  */
    if (part_sec >= 67108864u)    spc = 64;     /* >= 32 GiB  */

    /* FAT size: iterate until it fits; grow SPC if it would exceed cache */
    for (;;) {
        uint32_t fatsz = 1;
        for (;;) {
            uint32_t data_sec = part_sec - RESERVED - NUM_FATS * fatsz;
            uint32_t nclus = data_sec / spc;
            uint32_t need = ((nclus + 2) * 4 + 511) / 512;
            if (need <= fatsz) break;
            fatsz = need;
        }
        if (fatsz * 512 <= FAT_CACHE_MAX) {
            fat_sz = fatsz;
            break;
        }
        spc *= 2;                              /* fewer clusters -> smaller FAT */
    }
    data_start = PART_START + RESERVED + NUM_FATS * fat_sz;
    root_clus = ROOT_CLUS;
    max_clus = fat_sz * 512 / 4;
    vol_clusters = (part_sec - RESERVED - NUM_FATS * fat_sz) / spc;

    /* 1) MBR with one FAT32 partition (type 0x0C) starting at LBA 1 */
    memz(mbr, sizeof(mbr));
    uint8_t *pe = mbr + 446;
    pe[0] = 0x00;                 /* not bootable */
    pe[4] = 0x0C;                 /* FAT32 LBA */
    pe[8] = PART_START & 0xFF;    /* LBA start = 1 */
    pe[9] = 0; pe[10] = 0; pe[11] = 0;
    pe[12] = (uint8_t)(part_sec & 0xFF);
    pe[13] = (uint8_t)((part_sec >> 8) & 0xFF);
    pe[14] = (uint8_t)((part_sec >> 16) & 0xFF);
    pe[15] = (uint8_t)((part_sec >> 24) & 0xFF);
    mbr[510] = 0x55; mbr[511] = 0xAA;
    ata_write_sectors(0, 1, mbr);

    /* 2) DBR / BPB */
    memz(sec, sizeof(sec));
    sec[0] = 0xEB; sec[1] = 0x58; sec[2] = 0x90;
    const char *oem = "CATOS   ";                 /* 8 chars */
    for (i = 0; i < 8; i++) sec[3 + i] = (uint8_t)oem[i];
    sec[0x0B] = 0x00; sec[0x0C] = 0x02;           /* bytes/sector = 512 */
    sec[0x0D] = (uint8_t)spc;                     /* sectors/cluster */
    sec[0x0E] = RESERVED & 0xFF; sec[0x0F] = (RESERVED >> 8) & 0xFF;
    sec[0x10] = NUM_FATS;
    sec[0x11] = 0x00; sec[0x12] = 0x00;           /* root entries = 0 (FAT32) */
    sec[0x13] = 0x00; sec[0x14] = 0x00;           /* total16 = 0 */
    sec[0x15] = 0xF8;                             /* media */
    sec[0x16] = 0x00; sec[0x17] = 0x00;           /* fatsz16 = 0 */
    sec[0x18] = 0x3F; sec[0x19] = 0x00;           /* sectors/track = 63 */
    sec[0x1A] = 0xFF; sec[0x1B] = 0x00;           /* heads = 255 */
    sec[0x1C] = PART_START & 0xFF;                /* hidden sectors = 1 */
    sec[0x1D] = 0; sec[0x1E] = 0; sec[0x1F] = 0;
    sec[0x20] = (uint8_t)(part_sec & 0xFF);
    sec[0x21] = (uint8_t)((part_sec >> 8) & 0xFF);
    sec[0x22] = (uint8_t)((part_sec >> 16) & 0xFF);
    sec[0x23] = (uint8_t)((part_sec >> 24) & 0xFF);  /* total32 */
    sec[0x24] = (uint8_t)(fat_sz & 0xFF);
    sec[0x25] = (uint8_t)((fat_sz >> 8) & 0xFF);
    sec[0x26] = (uint8_t)((fat_sz >> 16) & 0xFF);
    sec[0x27] = (uint8_t)((fat_sz >> 24) & 0xFF);    /* fatsz32 */
    sec[0x2C] = ROOT_CLUS & 0xFF; sec[0x2D] = 0; sec[0x2E] = 0; sec[0x2F] = 0;  /* root clus = 2 */
    sec[0x30] = 0x02; sec[0x31] = 0x00;           /* FSInfo sector = 2 */
    sec[0x32] = 0x06; sec[0x33] = 0x00;           /* backup boot = 6 */
    sec[0x40] = 0x80;                             /* drive number */
    sec[0x42] = 0x29;                             /* boot signature */
    sec[0x43] = 0x12; sec[0x44] = 0x34; sec[0x45] = 0x56; sec[0x46] = 0x78;  /* serial */
    const char *vol = "CATOSVOL   ";              /* 11 chars */
    for (i = 0; i < 11; i++) sec[0x47 + i] = (uint8_t)vol[i];
    const char *ft = "FAT32   ";                  /* 8 chars */
    for (i = 0; i < 8; i++) sec[0x52 + i] = (uint8_t)ft[i];
    sec[510] = 0x55; sec[511] = 0xAA;
    ata_write_sectors(PART_START, 1, sec);        /* DBR at LBA 1 */
    ata_write_sectors(PART_START + 6, 1, sec);    /* backup DBR at LBA 7 */

    /* 3) FSInfo */
    memz(sec, sizeof(sec));
    sec[0] = 0x52; sec[1] = 0x52; sec[2] = 0x61; sec[3] = 0x41;   /* "RRaA" */
    sec[0x1E4] = 0x72; sec[0x1E5] = 0x72; sec[0x1E6] = 0x61; sec[0x1E7] = 0x41; /* "rrAa" */
    sec[0x1FC] = 0x00; sec[0x1FD] = 0x00; sec[0x1FE] = 0x55; sec[0x1FF] = 0xAA;
    ata_write_sectors(PART_START + 1, 1, sec);    /* FSInfo at LBA 2 */

    /* 4) reserved sectors 3..31 = 0 */
    memz(sec, sizeof(sec));
    for (i = 3; i < RESERVED; i++)
        ata_write_sectors(PART_START + i, 1, sec);

    /* 5) FAT tables: cluster 0 = 0x0FFFFFF8, cluster 1 = EOC, cluster 2
     * (root) = EOC, the rest free */
    memz(fat_cache, FAT_CACHE_MAX);
    fat_cache[0] = 0xF8; fat_cache[1] = 0xFF; fat_cache[2] = 0xFF; fat_cache[3] = 0x0F;
    fat_cache[4] = 0xFF; fat_cache[5] = 0xFF; fat_cache[6] = 0xFF; fat_cache[7] = 0x0F;
    fat_cache[8] = 0xFF; fat_cache[9] = 0xFF; fat_cache[10] = 0xFF; fat_cache[11] = 0x0F;  /* root */
    write_fat_region(PART_START + RESERVED);
    write_fat_region(PART_START + RESERVED + fat_sz);
    next_free = 3;

    /* 6) root directory cluster 2: "." and ".." */
    {
        static uint8_t rbuf[FAT_CLUSTER_SZ_MAX];
        memz(rbuf, FAT_CLUSTER_SZ_MAX);
        uint8_t dotname[11], dotdotname[11];
        for (i = 0; i < 11; i++) { dotname[i] = ' '; dotdotname[i] = ' '; }
        dotname[0] = '.'; dotdotname[0] = '.'; dotdotname[1] = '.';
        struct dent *d0 = (struct dent *)rbuf;
        struct dent *d1 = (struct dent *)(rbuf + DENT_SIZE);
        dent_init(d0, dotname, FAT_ATTR_DIR, ROOT_CLUS, 0);
        dent_init(d1, dotdotname, FAT_ATTR_DIR, ROOT_CLUS, 0);
        ata_write_sectors(data_start, (uint8_t)spc, rbuf);   /* cluster 2 */
    }

    /* 7) standard Linux top-level directories (created inline so the FAT
     * is flushed only once at the end) */
    static const char *std_dirs[] = {
        "dev", "etc", "bin", "sbin", "lib", "usr",
        "home", "root", "tmp", "var", "proc", "sys",
        "mnt", "boot", "appdevelop"
    };
    {
        static uint8_t rbuf[FAT_CLUSTER_SZ_MAX];
        uint8_t n83[11];
        for (i = 0; i < sizeof(std_dirs) / sizeof(std_dirs[0]); i++) {
            fat_name83(std_dirs[i], n83);
            uint32_t nc = fat_alloc();
            if (!nc) break;
            memz(rbuf, sizeof(rbuf));
            uint8_t dotname[11], dotdotname[11];
            for (int k = 0; k < 11; k++) { dotname[k] = ' '; dotdotname[k] = ' '; }
            dotname[0] = '.'; dotdotname[0] = '.'; dotdotname[1] = '.';
            struct dent *d0 = (struct dent *)rbuf;
            struct dent *d1 = (struct dent *)(rbuf + DENT_SIZE);
            dent_init(d0, dotname, FAT_ATTR_DIR, nc, 0);
            dent_init(d1, dotdotname, FAT_ATTR_DIR, ROOT_CLUS, 0);
            if (fat_write_cluster(nc, rbuf) != 0) break;
            struct dent de;
            dent_init(&de, n83, FAT_ATTR_DIR, nc, 0);
            if (dir_write_dent(ROOT_CLUS, &de) != 0) break;
        }
    }
    write_fat_region(PART_START + RESERVED);           /* flush FAT #1 */
    write_fat_region(PART_START + RESERVED + fat_sz);  /* flush FAT #2 */

    /* 8) FSInfo free/next update */
    memz(sec, sizeof(sec));
    sec[0] = 0x52; sec[1] = 0x52; sec[2] = 0x61; sec[3] = 0x41;
    sec[0x1E4] = 0x72; sec[0x1E5] = 0x72; sec[0x1E6] = 0x61; sec[0x1E7] = 0x41;
    {
        uint32_t used = 1 + (uint32_t)(sizeof(std_dirs) / sizeof(std_dirs[0]));
        uint32_t freecnt = vol_clusters > used ? vol_clusters - used : 0;
        sec[0x1E8] = (uint8_t)(freecnt & 0xFF);
        sec[0x1E9] = (uint8_t)((freecnt >> 8) & 0xFF);
        sec[0x1EA] = (uint8_t)((freecnt >> 16) & 0xFF);
        sec[0x1EB] = (uint8_t)((freecnt >> 24) & 0xFF);
        sec[0x1EC] = (uint8_t)(next_free & 0xFF);
        sec[0x1ED] = (uint8_t)((next_free >> 8) & 0xFF);
        sec[0x1EE] = (uint8_t)((next_free >> 16) & 0xFF);
        sec[0x1EF] = (uint8_t)((next_free >> 24) & 0xFF);
    }
    sec[0x1FC] = 0x00; sec[0x1FD] = 0x00; sec[0x1FE] = 0x55; sec[0x1FF] = 0xAA;
    ata_write_sectors(PART_START + 1, 1, sec);
}

/* ---- v2.5: append to an existing file (cluster-level, no full-file
 * read-back).  Walks the old chain, continues in the last cluster after
 * old_size, then appends fresh clusters.  Returns 0 or -1. */
int fat_append_file(uint32_t clus, uint32_t old_size, const char *data, uint32_t size) {
    if (size == 0) return 0;
    uint32_t csz = spc * 512;
    /* walk to the last cluster of the old chain */
    uint32_t c = clus, prev = 0;
    while (c >= 2 && c < max_clus) {
        prev = c;
        uint32_t n = fat_get(c);
        if (n == FAT_EOC || n < 2 || n >= max_clus) break;
        c = n;
    }
    if (c < 2 || c >= max_clus) return -1;
    static uint8_t buf[FAT_CLUSTER_SZ_MAX];
    uint32_t tail_off = old_size % csz;
    uint32_t written = 0;
    if (tail_off > 0) {
        /* continue inside the partial last cluster (read-modify-write) */
        if (fat_read_cluster(c, buf) != 0) return -1;
        uint32_t take = csz - tail_off; if (take > size) take = size;
        memcpy_(buf + tail_off, data, (int)take);
        if (fat_write_cluster(c, buf) != 0) return -1;
        written = take;
        prev = c;
        c = fat_get(c);
    } else if (old_size == 0) {
        /* v2.5.1 (FAIL-A1 fix): empty file - reuse the mkfile-preallocated
         * first cluster instead of skipping it (avoids zero prefix). */
        c = clus;
    } else {
        c = fat_get(c);
    }
    while (written < size) {
        if (c < 2 || c >= max_clus) {           /* need a fresh cluster */
            uint32_t nc = fat_alloc();
            if (!nc) return -1;
            if (prev) fat_set(prev, nc);
            c = nc;
        }
        memz(buf, sizeof(buf));
        uint32_t take = size - written; if (take > csz) take = csz;
        memcpy_(buf, data + written, (int)take);
        if (fat_write_cluster(c, buf) != 0) return -1;
        written += take;
        prev = c;
        c = fat_get(c);
    }
    if (prev) fat_set(prev, FAT_EOC);
    fat_flush();
    return 0;
}

/* ---- v2.5: mark a directory entry deleted (0xE5) -------------------------- */
int fat_delete_entry(uint32_t dirclus, const uint8_t *name83) {
    static uint8_t buf[FAT_CLUSTER_SZ_MAX];
    uint32_t c = dirclus;
    while (c >= 2 && c < max_clus) {
        if (fat_read_cluster(c, buf) != 0) return -1;
        for (int i = 0; i < (int)(spc * 512 / 32); i++) {
            uint8_t *en = buf + i * 32;
            if (en[0] == 0x00) return -1;               /* end of dir */
            if (en[0] == 0xE5) continue;                /* deleted */
            if (en[11] == 0x0F) continue;               /* LFN */
            int same = 1;
            for (int k = 0; k < 11; k++) if (en[k] != name83[k]) { same = 0; break; }
            if (same) {
                en[0] = 0xE5;
                return fat_write_cluster(c, buf);
            }
        }
        uint32_t n = fat_get(c);
        if (n == FAT_EOC) break;
        c = n;
    }
    return -1;
}
