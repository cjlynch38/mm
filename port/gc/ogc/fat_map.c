/**
 * Raw read access to files on a FAT volume (see fat_map.h).
 *
 * The boot sector checks and the volume layout follow FatFs R0.15a (check_fs, find_volume, mount_volume), the
 * version inside libogc 3.1.0's libfat, so this code accepts the volumes libfat mounts and finds the same one. Name
 * matching follows its dir_find: an entry matches by its long name (case-insensitive) or by its 8.3 name, and a
 * long name counts only if its entries are complete and their checksum matches the 8.3 entry. Cluster chains are
 * followed as f_read does: every link must name a cluster of the volume. Case folding covers ASCII and Latin-1
 * only; a name that needs more is not found, and the caller then reads the file through libfat.
 */
#include <string.h>
#include "fat_map.h"

#define SS FAT_MAP_SECTOR_SIZE
#define DIR_ENTRY_SIZE 32u
#define READ_ERROR 0xFFFFFFFFu

/* Boot sector and MBR */
#define BS_JMP_BOOT 0
#define BPB_BYTS_PER_SEC 11
#define BPB_SEC_PER_CLUS 13
#define BPB_RSVD_SEC_CNT 14
#define BPB_NUM_FATS 16
#define BPB_ROOT_ENT_CNT 17
#define BPB_TOT_SEC16 19
#define BPB_FAT_SZ16 22
#define BPB_TOT_SEC32 32
#define BPB_FAT_SZ32 36
#define BPB_FS_VER32 42
#define BPB_ROOT_CLUS32 44
#define BS_FIL_SYS_TYPE32 82
#define BS_55AA 510
#define MBR_TABLE 446
#define PTE_SIZE 16
#define PTE_ST_LBA 8

#define MAX_FAT12 0xFF5u
#define MAX_FAT16 0xFFF5u
#define MAX_FAT32 0x0FFFFFF5u

/* Directory entries */
#define DIR_ATTR 11
#define DIR_FST_CLUS_HI 20
#define DIR_FST_CLUS_LO 26
#define DIR_FILE_SIZE 28
#define LDIR_CHKSUM 13
#define AM_VOL 0x08
#define AM_DIR 0x10
#define AM_LFN 0x0F
#define AM_MASK 0x3F
#define LLEF 0x40
#define DDEM 0xE5
#define MAX_LFN 255
/* FatFs ends a directory after 2 MB (65536 entries) */
#define MAX_DIR_SECTORS (0x200000u / SS)

/* check_boot_sector results, as FatFs's check_fs */
enum { BS_FAT = 0, BS_OTHER = 2, BS_NONE = 3, BS_ERROR = 4 };

static const unsigned char sLfnOffsets[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };

static uint32_t le16(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t le32(const unsigned char* p) {
    return le16(p) | (le16(p + 2) << 16);
}

uint32_t fat_map_cluster_size(const FatMap* m) {
    return m->clusterSectors * SS;
}

/* The bytes of `sector` through the scratch buffer, which is refilled with up to bufSectors sectors from `sector`
 * on but not past `limit`. NULL on a read error. */
static const unsigned char* sector_get(FatMap* m, uint32_t sector, uint32_t limit) {
    uint32_t n = m->bufSectors;

    if (m->bufCount != 0 && sector - m->bufStart < m->bufCount) {
        return m->buf + (sector - m->bufStart) * SS;
    }
    if (limit > sector && limit - sector < n) {
        n = limit - sector;
    } else if (limit <= sector) {
        n = 1;
    }
    m->bufCount = 0;
    if (m->read(m->ctx, sector, n, m->buf) != 0) {
        return NULL;
    }
    m->bufStart = sector;
    m->bufCount = n;
    return m->buf;
}

/* ---------------------------------------------------------------------------------------------- */
/* Volume                                                                                         */
/* ---------------------------------------------------------------------------------------------- */

static int is_exfat(const unsigned char* b) {
    return memcmp(b + BS_JMP_BOOT, "\xEB\x76\x90" "EXFAT   ", 11) == 0;
}

/* What `sector` holds (FatFs's check_fs without exFAT, which libfat leaves out): a FAT boot sector (one with
 * "FAT32" in its FAT32 type field, or a FAT12/16 BPB that looks sane), another boot sector (signature 0xAA55, so
 * possibly an MBR), no boot sector, or a read error. *exfat is set for an exFAT boot sector. */
static int check_boot_sector(FatMap* m, uint32_t sector, int* exfat) {
    const unsigned char* b = sector_get(m, sector, sector + 1);
    uint32_t sign;
    uint32_t w;
    unsigned int c;

    if (b == NULL) {
        return BS_ERROR;
    }
    sign = le16(b + BS_55AA);
    if (sign == 0xAA55 && is_exfat(b)) {
        *exfat = 1;
    }
    c = b[BS_JMP_BOOT];
    if (c == 0xEB || c == 0xE9 || c == 0xE8) {
        if (sign == 0xAA55 && memcmp(b + BS_FIL_SYS_TYPE32, "FAT32   ", 8) == 0) {
            return BS_FAT;
        }
        // FAT volumes formatted by early MS-DOS have no signature or type field
        w = le16(b + BPB_BYTS_PER_SEC);
        c = b[BPB_SEC_PER_CLUS];
        if ((w & (w - 1)) == 0 && w >= SS && w <= SS && c != 0 && (c & (c - 1)) == 0 &&
            le16(b + BPB_RSVD_SEC_CNT) != 0 && (unsigned int)b[BPB_NUM_FATS] - 1 <= 1 &&
            le16(b + BPB_ROOT_ENT_CNT) != 0 && (le16(b + BPB_TOT_SEC16) >= 128 || le32(b + BPB_TOT_SEC32) >= 0x10000) &&
            le16(b + BPB_FAT_SZ16) != 0) {
            return BS_FAT;
        }
    }
    return (sign == 0xAA55) ? BS_OTHER : BS_NONE;
}

/* The layout of the FAT volume whose boot sector is `bsect` (FatFs's mount_volume) */
static const char* mount_layout(FatMap* m, uint32_t bsect) {
    const unsigned char* b = sector_get(m, bsect, bsect + 1);
    uint64_t sysect;
    uint64_t needed;
    uint32_t fasize;
    uint32_t nfats;
    uint32_t csize;
    uint32_t nrootdir;
    uint32_t tsect;
    uint32_t nrsv;
    uint32_t nclst;

    if (b == NULL) {
        return "cannot read the boot sector";
    }
    if (le16(b + BPB_BYTS_PER_SEC) != SS) {
        return "the sector size is not 512 bytes";
    }
    fasize = le16(b + BPB_FAT_SZ16);
    if (fasize == 0) {
        fasize = le32(b + BPB_FAT_SZ32);
    }
    nfats = b[BPB_NUM_FATS];
    if (nfats != 1 && nfats != 2) {
        return "the boot sector gives a wrong number of FATs";
    }
    csize = b[BPB_SEC_PER_CLUS];
    if (csize == 0 || (csize & (csize - 1)) != 0) {
        return "the boot sector gives a wrong cluster size";
    }
    nrootdir = le16(b + BPB_ROOT_ENT_CNT);
    if (nrootdir % (SS / DIR_ENTRY_SIZE) != 0) {
        return "the boot sector gives a wrong root directory size";
    }
    tsect = le16(b + BPB_TOT_SEC16);
    if (tsect == 0) {
        tsect = le32(b + BPB_TOT_SEC32);
    }
    nrsv = le16(b + BPB_RSVD_SEC_CNT);
    if (nrsv == 0) {
        return "the boot sector gives no reserved sectors";
    }
    sysect = (uint64_t)nrsv + (uint64_t)fasize * nfats + nrootdir / (SS / DIR_ENTRY_SIZE);
    if (tsect < sysect) {
        return "the boot sector gives a wrong volume size";
    }
    nclst = (uint32_t)((tsect - sysect) / csize);
    if (nclst == 0) {
        return "the volume has no clusters";
    }
    m->type = 0;
    if (nclst <= MAX_FAT32) {
        m->type = 32;
    }
    if (nclst <= MAX_FAT16) {
        m->type = 16;
    }
    if (nclst <= MAX_FAT12) {
        m->type = 12;
    }
    if (m->type == 0) {
        return "the volume has too many clusters";
    }
    m->entries = nclst + 2;
    m->volume = bsect;
    m->fat = bsect + nrsv;
    m->fatSectors = fasize;
    m->data = bsect + (uint32_t)sysect;
    m->clusterSectors = csize;
    if (m->type == 32) {
        if (le16(b + BPB_FS_VER32) != 0) {
            return "unknown FAT32 version";
        }
        if (nrootdir != 0) {
            return "the FAT32 boot sector gives a root directory size";
        }
        m->root = le32(b + BPB_ROOT_CLUS32);
        needed = (uint64_t)m->entries * 4;
    } else {
        if (nrootdir == 0) {
            return "the FAT12/16 boot sector gives no root directory";
        }
        m->root = m->fat + fasize * nfats;
        m->rootSectors = nrootdir / (SS / DIR_ENTRY_SIZE);
        needed = (m->type == 16) ? (uint64_t)m->entries * 2 : (uint64_t)m->entries * 3 / 2 + (m->entries & 1);
    }
    if (fasize < (needed + SS - 1) / SS) {
        return "the FAT is smaller than the volume needs";
    }
    return NULL;
}

const char* fat_map_mount(FatMap* m, FatMapRead read, void* ctx, void* buf, uint32_t bufSectors) {
    uint32_t lba[4];
    uint32_t bsect = 0;
    int exfat = 0;
    int fmt;
    int i;

    memset(m, 0, sizeof(*m));
    m->read = read;
    m->ctx = ctx;
    m->buf = buf;
    m->bufSectors = (bufSectors != 0) ? bufSectors : 1;

    fmt = check_boot_sector(m, 0, &exfat);
    if (fmt == BS_ERROR) {
        return "cannot read sector 0";
    }
    if (fmt == BS_NONE) {
        return "sector 0 holds neither a FAT boot sector nor an MBR";
    }
    if (fmt == BS_OTHER) {
        // An MBR: the first of its four entries (by start sector, whatever the type) with a FAT boot sector
        const unsigned char* b = sector_get(m, 0, 1);

        if (b == NULL) {
            return "cannot read sector 0";
        }
        for (i = 0; i < 4; i++) {
            lba[i] = le32(b + MBR_TABLE + i * PTE_SIZE + PTE_ST_LBA);
        }
        for (i = 0; i < 4 && fmt != BS_FAT; i++) {
            if (lba[i] != 0) {
                bsect = lba[i];
                fmt = check_boot_sector(m, bsect, &exfat);
            }
        }
        if (fmt != BS_FAT) {
            return exfat ? "exFAT volume (libfat reads only FAT12/16/32)" : "no FAT volume in the MBR's partitions";
        }
    }
    return mount_layout(m, bsect);
}

/* ---------------------------------------------------------------------------------------------- */
/* FAT and directories                                                                            */
/* ---------------------------------------------------------------------------------------------- */

static uint32_t cluster_sector(const FatMap* m, uint32_t cluster) {
    return m->data + (cluster - 2) * m->clusterSectors;
}

/* The FAT entry of `cluster` (2 <= cluster < entries), READ_ERROR if the FAT cannot be read */
static uint32_t fat_get(FatMap* m, uint32_t cluster) {
    uint32_t limit = m->fat + m->fatSectors;
    const unsigned char* p;
    uint32_t offset;
    uint32_t value;

    switch (m->type) {
        case 12:
            // 12-bit entries: the two bytes can lie in different sectors
            offset = cluster + cluster / 2;
            p = sector_get(m, m->fat + offset / SS, limit);
            if (p == NULL) {
                return READ_ERROR;
            }
            value = p[offset % SS];
            offset++;
            p = sector_get(m, m->fat + offset / SS, limit);
            if (p == NULL) {
                return READ_ERROR;
            }
            value |= (uint32_t)p[offset % SS] << 8;
            return (cluster & 1) ? value >> 4 : value & 0xFFF;
        case 16:
            offset = cluster * 2;
            p = sector_get(m, m->fat + offset / SS, limit);
            return (p != NULL) ? le16(p + offset % SS) : READ_ERROR;
        default:
            offset = cluster * 4;
            p = sector_get(m, m->fat + offset / SS, limit);
            return (p != NULL) ? le32(p + offset % SS) & 0x0FFFFFFF : READ_ERROR;
    }
}

/* A walk over the sectors of one directory */
typedef struct {
    uint32_t cluster; /* current cluster; 0 in the FAT12/16 root directory */
    uint32_t sector;  /* next sector */
    uint32_t left;    /* sectors left in the cluster (or the root directory) */
    uint32_t read;    /* sectors read so far */
} DirWalk;

/* Start a walk of the directory at `cluster` (0: the root directory) */
static const char* dir_open(FatMap* m, uint32_t cluster, DirWalk* w) {
    memset(w, 0, sizeof(*w));
    if (cluster == 0 && m->type != 32) {
        w->sector = m->root;
        w->left = m->rootSectors;
        return NULL;
    }
    if (cluster == 0) {
        cluster = m->root;
    }
    if (cluster < 2 || cluster >= m->entries) {
        return "a directory starts at a cluster outside the volume";
    }
    w->cluster = cluster;
    w->sector = cluster_sector(m, cluster);
    w->left = m->clusterSectors;
    return NULL;
}

/* The next sector of the directory into *data: 1, or 0 at the directory's end, or -1 with *why */
static int dir_next_sector(FatMap* m, DirWalk* w, const unsigned char** data, const char** why) {
    if (w->read == MAX_DIR_SECTORS) {
        return 0;
    }
    if (w->left == 0) {
        uint32_t next;

        if (w->cluster == 0) {
            return 0;
        }
        next = fat_get(m, w->cluster);
        if (next == READ_ERROR) {
            *why = "cannot read the FAT";
            return -1;
        }
        if (next < 2) {
            *why = "a directory's cluster chain is broken";
            return -1;
        }
        if (next >= m->entries) {
            return 0;
        }
        w->cluster = next;
        w->sector = cluster_sector(m, next);
        w->left = m->clusterSectors;
    }
    *data = sector_get(m, w->sector, w->sector + w->left);
    if (*data == NULL) {
        *why = "cannot read a directory";
        return -1;
    }
    w->sector++;
    w->left--;
    w->read++;
    return 1;
}

/* Upper case for name comparisons: ASCII and Latin-1 */
static uint32_t upcase(uint32_t c) {
    if ((c >= 'a' && c <= 'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7)) {
        return c - 0x20;
    }
    if (c == 0xFF) {
        return 0x178;
    }
    return c;
}

/* `name` (len bytes of UTF-8) as UTF-16 into out (MAX_LFN units at most). Its length, or -1. */
static int utf8_to_utf16(const char* name, uint32_t len, uint16_t* out) {
    const unsigned char* s = (const unsigned char*)name;
    uint32_t i = 0;
    int n = 0;

    while (i < len) {
        uint32_t c = s[i];
        uint32_t more;

        if (c < 0x80) {
            more = 0;
        } else if ((c & 0xE0) == 0xC0) {
            c &= 0x1F;
            more = 1;
        } else if ((c & 0xF0) == 0xE0) {
            c &= 0x0F;
            more = 2;
        } else if ((c & 0xF8) == 0xF0) {
            c &= 0x07;
            more = 3;
        } else {
            return -1;
        }
        i++;
        while (more-- != 0) {
            if (i >= len || (s[i] & 0xC0) != 0x80) {
                return -1;
            }
            c = (c << 6) | (s[i++] & 0x3F);
        }
        if (c >= 0x10000) {
            if (c > 0x10FFFF || n + 2 > MAX_LFN) {
                return -1;
            }
            c -= 0x10000;
            out[n++] = (uint16_t)(0xD800 | (c >> 10));
            out[n++] = (uint16_t)(0xDC00 | (c & 0x3FF));
        } else {
            if (n + 1 > MAX_LFN) {
                return -1;
            }
            out[n++] = (uint16_t)c;
        }
    }
    return n;
}

/* Store the characters of one long name entry in lfn (FatFs's pick_lfn). 0 if the entry is not valid. */
static int pick_lfn(uint16_t* lfn, const unsigned char* e) {
    uint32_t i = ((e[0] & ~LLEF) - 1u) * 13;
    uint32_t wc = 1;
    int s;

    if (le16(e + DIR_FST_CLUS_LO) != 0) {
        return 0;
    }
    for (s = 0; s < 13; s++) {
        uint32_t uc = le16(e + sLfnOffsets[s]);

        if (wc != 0) {
            if (i >= MAX_LFN + 1) {
                return 0;
            }
            lfn[i++] = (uint16_t)uc;
            wc = uc;
        } else if (uc != 0xFFFF) {
            return 0;
        }
    }
    if ((e[0] & LLEF) && wc != 0) {
        if (i >= MAX_LFN + 1) {
            return 0;
        }
        lfn[i] = 0;
    }
    return 1;
}

/* Checksum of an 8.3 name, kept in its long name entries */
static unsigned int sfn_sum(const unsigned char* e) {
    unsigned int sum = 0;
    int i;

    for (i = 0; i < 11; i++) {
        sum = (((sum & 1) << 7) + (sum >> 1) + e[i]) & 0xFF;
    }
    return sum;
}

static int lfn_equal(const uint16_t* lfn, const uint16_t* want, int wantLen) {
    int i;

    for (i = 0; i < wantLen; i++) {
        if (lfn[i] == 0 || upcase(lfn[i]) != upcase(want[i])) {
            return 0;
        }
    }
    return lfn[wantLen] == 0;
}

/* Does the 8.3 name of entry e read `name` (len bytes), ignoring the case of ASCII letters? */
static int sfn_equal(const unsigned char* e, const char* name, uint32_t len) {
    char text[13];
    uint32_t n = 0;
    int base = 8;
    int ext = 3;
    int i;

    while (base > 0 && e[base - 1] == ' ') {
        base--;
    }
    while (ext > 0 && e[8 + ext - 1] == ' ') {
        ext--;
    }
    for (i = 0; i < base; i++) {
        text[n++] = (char)((i == 0 && e[0] == 0x05) ? DDEM : e[i]);
    }
    if (ext > 0) {
        text[n++] = '.';
        for (i = 0; i < ext; i++) {
            text[n++] = (char)e[8 + i];
        }
    }
    if (n != len) {
        return 0;
    }
    for (i = 0; i < (int)n; i++) {
        unsigned char a = (unsigned char)text[i];
        unsigned char b = (unsigned char)name[i];

        if (a != b && (a >= 0x80 || b >= 0x80 || upcase(a) != upcase(b))) {
            return 0;
        }
    }
    return 1;
}

/* Find `name` (len bytes of UTF-8) in the directory at `dir` (0: the root). NULL on success. */
static const char* dir_find(FatMap* m, uint32_t dir, const char* name, uint32_t len, uint32_t* cluster,
                            uint32_t* size, int* isDir) {
    uint16_t want[MAX_LFN + 1];
    uint16_t lfn[MAX_LFN + 1];
    const unsigned char* data;
    const char* why;
    unsigned int ord = 0xFF;
    unsigned int sum = 0;
    DirWalk w;
    int wantLen = utf8_to_utf16(name, len, want);
    int r;

    if (wantLen < 0) {
        return "a path component is not a valid name";
    }
    why = dir_open(m, dir, &w);
    if (why != NULL) {
        return why;
    }
    while ((r = dir_next_sector(m, &w, &data, &why)) == 1) {
        uint32_t i;

        for (i = 0; i < SS; i += DIR_ENTRY_SIZE) {
            const unsigned char* e = data + i;
            unsigned int c = e[0];
            unsigned int a = e[DIR_ATTR] & AM_MASK;

            if (c == 0) {
                return "no such file or directory";
            }
            if (c == DDEM || ((a & AM_VOL) && a != AM_LFN)) {
                ord = 0xFF;
                continue;
            }
            if (a == AM_LFN) {
                if (c & LLEF) {
                    sum = e[LDIR_CHKSUM];
                    c &= ~LLEF;
                    ord = c;
                }
                ord = (c == ord && sum == e[LDIR_CHKSUM] && pick_lfn(lfn, e)) ? ord - 1 : 0xFF;
                continue;
            }
            if ((ord == 0 && sum == sfn_sum(e) && lfn_equal(lfn, want, wantLen)) || sfn_equal(e, name, len)) {
                *cluster = le16(e + DIR_FST_CLUS_LO);
                if (m->type == 32) {
                    *cluster |= le16(e + DIR_FST_CLUS_HI) << 16;
                }
                *size = le32(e + DIR_FILE_SIZE);
                *isDir = (a & AM_DIR) != 0;
                return NULL;
            }
            ord = 0xFF;
        }
    }
    return (r == 0) ? "no such file or directory" : why;
}

const char* fat_map_find(FatMap* m, const char* path, uint32_t* cluster, uint32_t* size) {
    const char* p = path;
    uint32_t dir = 0;
    uint32_t c = 0;
    uint32_t s = 0;
    int isDir = 1;

    for (;;) {
        const char* start;
        uint32_t len;
        const char* why;

        while (*p == '/' || *p == '\\') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        start = p;
        while (*p != '\0' && *p != '/' && *p != '\\') {
            p++;
        }
        len = (uint32_t)(p - start);
        if (!isDir) {
            return "a path component is a file";
        }
        if (len == 1 && start[0] == '.') {
            continue;
        }
        if (!(len == 2 && start[0] == '.' && start[1] == '.')) {
            // FatFs ignores trailing dots and spaces
            while (len > 0 && (start[len - 1] == '.' || start[len - 1] == ' ')) {
                len--;
            }
            if (len == 0) {
                return "a path component is not a valid name";
            }
        }
        why = dir_find(m, dir, start, len, &c, &s, &isDir);
        if (why != NULL) {
            if (len == 2 && start[0] == '.' && start[1] == '.' && dir == 0) {
                // The root directory has no dot entries: ".." stays there, as in FatFs
                isDir = 1;
                c = 0;
                continue;
            }
            return why;
        }
        dir = c;
    }
    if (isDir) {
        return "the path names a directory";
    }
    *cluster = c;
    *size = s;
    return NULL;
}

/* ---------------------------------------------------------------------------------------------- */
/* Files                                                                                          */
/* ---------------------------------------------------------------------------------------------- */

const char* fat_map_extents(FatMap* m, uint32_t cluster, uint32_t size, FatExtent* out, uint32_t max,
                            uint32_t* count) {
    uint32_t bytes = fat_map_cluster_size(m);
    uint32_t clusters = (uint32_t)(((uint64_t)size + bytes - 1) / bytes);
    uint32_t runs = 0;
    uint32_t runStart = 0;
    uint32_t runLen = 0;
    uint32_t i;

    *count = 0;
    if (size == 0) {
        return NULL;
    }
    if (cluster < 2 || cluster >= m->entries) {
        return "the file starts at a cluster outside the volume";
    }
    for (i = 0; i <= clusters; i++) {
        if (i == clusters || runLen == 0 || cluster != runStart + runLen) {
            if (runLen != 0) {
                if (out != NULL && runs < max) {
                    out[runs].offset = (i - runLen) * bytes;
                    out[runs].sector = cluster_sector(m, runStart);
                    out[runs].count = runLen * m->clusterSectors;
                }
                runs++;
            }
            if (i == clusters) {
                break;
            }
            runStart = cluster;
            runLen = 0;
        }
        runLen++;
        if (i + 1 < clusters) {
            uint32_t next = fat_get(m, cluster);

            if (next == READ_ERROR) {
                return "cannot read the FAT";
            }
            if (next < 2 || next >= m->entries) {
                return "the file's cluster chain is broken";
            }
            cluster = next;
        }
    }
    *count = runs;
    if (out != NULL && runs > max) {
        return "the file has too many fragments";
    }
    return NULL;
}

/* Index of the run holding `offset` (the runs are sorted by offset and start at 0) */
static uint32_t extent_find(const FatExtent* ext, uint32_t count, uint32_t offset) {
    uint32_t lo = 0;
    uint32_t hi = count;

    while (hi - lo > 1) {
        uint32_t mid = lo + (hi - lo) / 2;

        if (ext[mid].offset <= offset) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return lo;
}

int fat_map_read(const FatExtent* ext, uint32_t count, FatMapRead read, void* ctx, uint32_t offset, void* dst,
                 uint32_t size, void* bounce) {
    unsigned char* out = dst;
    uint32_t i;

    if (count == 0) {
        return (size == 0) ? 0 : -1;
    }
    i = extent_find(ext, count, offset);
    while (size != 0) {
        const FatExtent* e;
        uint64_t inRun;
        uint32_t sector;
        uint32_t head;
        uint32_t n;

        if (i >= count || offset < ext[i].offset) {
            return -1;
        }
        e = &ext[i];
        inRun = offset - e->offset;
        if (inRun >= (uint64_t)e->count * SS) {
            i++;
            continue;
        }
        sector = e->sector + (uint32_t)(inRun / SS);
        head = (uint32_t)(inRun % SS);
        if (head != 0 || size < SS) {
            n = SS - head;
            if (n > size) {
                n = size;
            }
            if (read(ctx, sector, 1, bounce) != 0) {
                return -1;
            }
            memcpy(out, (const unsigned char*)bounce + head, n);
        } else {
            uint32_t sectors = size / SS;
            uint32_t runLeft = e->count - (uint32_t)(inRun / SS);

            if (sectors > runLeft) {
                sectors = runLeft;
            }
            if (read(ctx, sector, sectors, out) != 0) {
                return -1;
            }
            n = sectors * SS;
        }
        offset += n;
        out += n;
        size -= n;
    }
    return 0;
}
