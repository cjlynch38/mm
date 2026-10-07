/**
 * Crash-safe saves in a recycled set of files (see save_rotation.h).
 *
 * Files, for a save path P (sd:/mmgcport/mm.fla):
 *   P        the newest save
 *   P.bak    the one before
 *   P.tmp    the one before that, which the next store overwrites
 *   P.spare  exists only during a store's renames, and marks that P.tmp (else P) holds the new save
 *
 * A store:
 *   1. writes the new save over P.tmp in place and syncs it. P.tmp keeps its clusters, so once the three files exist
 *      a store allocates nothing on the card (libfat's search for free clusters can take tens of seconds on a large,
 *      full card, and the first save after boot used to pay it);
 *   2. renames P.bak -> P.spare, P -> P.bak, P.tmp -> P, P.spare -> P.tmp.
 * libfat (FatFs) commits each rename to the card before it returns, and a rename fails if the target exists.
 *
 * Invariant: wherever a power cut stops a store, a complete save remains, and save_pick finds the newest one.
 *  - While P.tmp is written, P (and P.bak) are complete and P.spare does not exist: save_pick takes P.
 *  - From the first rename to the last, P.spare exists. Then P.tmp, if it exists, holds the new save (complete: it
 *    was synced before the first rename), else P does: save_pick takes P.tmp, then P.
 *  - No rename moves the only complete save: each one leaves P, P.bak or P.tmp holding a complete one. A rename that
 *    a power cut loses outright (neither name left: libfat's write-back cache may write the sector that removes the
 *    old name before the one that adds the new) costs at most the save that rename was moving.
 *  - A store first finishes a rotation that a power cut interrupted (which files exist shows how far it got).
 *  The only store that can be lost is one stopped between syncing P.tmp and its first rename; save_pick then finds
 *  the previous save, as after a power cut during the write.
 *
 * Fewer files (the first stores on a card, or saves of builds before this one, which left P and P.bak): when P is
 * missing or empty, a store writes the new save straight into P (created) and touches nothing else. While P.bak is
 * missing the rotation is P -> P.spare, P.tmp -> P, P.spare -> P.bak; while P.tmp is missing or short a store creates
 * it. Only these stores allocate clusters, at most three on a card. Without P.spare, save_pick takes P, then P.bak,
 * then a full-size P.tmp, then P.spare (empty files never), as earlier builds did for the first three.
 *
 * A rename that a power cut interrupts can also leave both names in the directory, on one cluster chain (FatFs
 * writes the new entry before it removes the old one; libfat's cache may keep that order). Removing or truncating
 * either name would free clusters the other still uses. A store therefore moves one of the two names aside to P.dup1
 * (P.dup2, ...), never deleting it, so that the files reach the state the rename was meant to reach, or the one
 * before it, and carries on. save_pick ignores a P.tmp that shares its clusters with P.spare. This needs each
 * file's first cluster, which libfat's stat() reports as st_ino. A disk check on a PC separates such files.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "save_rotation.h"

#define DUP_NAMES 9

static const char* const sSuffix[SAVE_FILES] = { "", ".bak", ".tmp", ".spare" };

typedef struct {
    SaveFileInfo f[SAVE_FILES];
} SaveState;

void save_set_init(SaveSet* set, const char* path) {
    int i;

    for (i = 0; i < SAVE_FILES; i++) {
        snprintf(set->path[i], SAVE_PATH_MAX, "%s%s", path, sSuffix[i]);
    }
}

static void say(const SaveFs* fs, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void say(const SaveFs* fs, const char* fmt, ...) {
    char line[320];
    va_list args;

    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    fs->log(fs->ctx, line);
}

static void scan(const SaveFs* fs, const SaveSet* set, SaveState* st) {
    int i;

    for (i = 0; i < SAVE_FILES; i++) {
        memset(&st->f[i], 0, sizeof(st->f[i]));
        fs->stat(fs->ctx, set->path[i], &st->f[i]);
    }
}

static int has(const SaveState* st, int i) {
    return st->f[i].exists;
}

/* Holds some data (an empty file is a create that a power cut stopped) */
static int has_data(const SaveState* st, int i) {
    return st->f[i].exists && st->f[i].size > 0;
}

static int full(const SaveState* st, int i, unsigned int size) {
    return st->f[i].exists && st->f[i].size == (long)size;
}

/* Two names of one cluster chain: a rename that was cut short */
static int shared(const SaveState* st, int a, int b) {
    return has(st, a) && has(st, b) && st->f[a].cluster != 0 && st->f[a].cluster == st->f[b].cluster;
}

int save_pick(const SaveFs* fs, const SaveSet* set, unsigned int size) {
    SaveState st;

    scan(fs, set, &st);
    if (has(&st, SAVE_SPARE) && full(&st, SAVE_TMP, size) && !shared(&st, SAVE_TMP, SAVE_SPARE)) {
        return SAVE_TMP;
    }
    if (has_data(&st, SAVE_MAIN)) {
        return SAVE_MAIN;
    }
    if (has_data(&st, SAVE_BAK)) {
        return SAVE_BAK;
    }
    if (full(&st, SAVE_TMP, size)) {
        return SAVE_TMP;
    }
    if (has_data(&st, SAVE_SPARE)) {
        return SAVE_SPARE;
    }
    return -1;
}

static int move(const SaveFs* fs, const SaveSet* set, SaveState* st, int from, int to) {
    if (fs->rename(fs->ctx, set->path[from], set->path[to]) != 0) {
        say(fs, "save: cannot rename %s to %s", set->path[from], set->path[to]);
        return -1;
    }
    st->f[to] = st->f[from];
    memset(&st->f[from], 0, sizeof(st->f[from]));
    return 0;
}

/* Rename file `which` to the first free P.dupN; it keeps its clusters */
static int set_aside(const SaveFs* fs, const SaveSet* set, SaveState* st, int which, const char* why) {
    char dup[SAVE_PATH_MAX + 16];
    unsigned int n;

    for (n = 1; n <= DUP_NAMES; n++) {
        SaveFileInfo info;

        snprintf(dup, sizeof(dup), "%s.dup%u", set->path[SAVE_MAIN], n);
        memset(&info, 0, sizeof(info));
        fs->stat(fs->ctx, dup, &info);
        if (info.exists) {
            continue;
        }
        if (fs->rename(fs->ctx, set->path[which], dup) != 0) {
            break;
        }
        say(fs, "save: %s %s; moved it to %s", set->path[which], why, dup);
        memset(&st->f[which], 0, sizeof(st->f[which]));
        return 0;
    }
    say(fs, "save: %s %s, and it cannot be renamed to %s.dup1 to .dup%d", set->path[which], why,
        set->path[SAVE_MAIN], DUP_NAMES);
    return -1;
}

/* Does file `which` share its clusters with another save file or a P.dupN? Truncating it would free them. */
static int clusters_shared(const SaveFs* fs, const SaveSet* set, const SaveState* st, int which) {
    char dup[SAVE_PATH_MAX + 16];
    unsigned long cluster = st->f[which].cluster;
    unsigned int n;
    int i;

    if (!has(st, which) || cluster == 0) {
        return 0;
    }
    for (i = 0; i < SAVE_FILES; i++) {
        if (i != which && has(st, i) && st->f[i].cluster == cluster) {
            return 1;
        }
    }
    for (n = 1; n <= DUP_NAMES; n++) {
        SaveFileInfo info;

        snprintf(dup, sizeof(dup), "%s.dup%u", set->path[SAVE_MAIN], n);
        memset(&info, 0, sizeof(info));
        fs->stat(fs->ctx, dup, &info);
        if (info.exists && info.cluster == cluster) {
            return 1;
        }
    }
    return 0;
}

/* Pairs of names that a cut-short rename can leave on one cluster chain, and which of the two to move aside (the
 * first): mostly the old name, so the files are where the rename was going */
static const unsigned char sDupPairs[][2] = {
    { SAVE_BAK, SAVE_SPARE },  /* P.bak -> P.spare, or P.spare -> P.bak */
    { SAVE_MAIN, SAVE_BAK },   /* P -> P.bak */
    { SAVE_TMP, SAVE_MAIN },   /* P.tmp -> P */
    { SAVE_SPARE, SAVE_TMP },  /* P.spare -> P.tmp */
    { SAVE_MAIN, SAVE_SPARE }, /* P -> P.spare */
    { SAVE_TMP, SAVE_BAK },    /* no rename joins these, but P.tmp must not be written in place then */
};

static int resolve_shared(const SaveFs* fs, const SaveSet* set, SaveState* st) {
    unsigned int i;

    for (i = 0; i < sizeof(sDupPairs) / sizeof(sDupPairs[0]); i++) {
        int aside = sDupPairs[i][0];
        int keep = sDupPairs[i][1];

        if (shared(st, aside, keep)) {
            char why[SAVE_PATH_MAX + 64];

            snprintf(why, sizeof(why), "shares its clusters with %s (a rename was cut short)", set->path[keep]);
            if (set_aside(fs, set, st, aside, why) != 0) {
                return -1;
            }
            i = (unsigned int)-1; // look at every pair again
        }
    }
    return 0;
}

/* Finish a rotation that a power cut interrupted (P.spare exists) */
static int finish_rotation(const SaveFs* fs, const SaveSet* set, SaveState* st, unsigned int size) {
    if (!has(st, SAVE_SPARE)) {
        return 0;
    }
    if (has(st, SAVE_MAIN) && has(st, SAVE_BAK) && has(st, SAVE_TMP)) {
        // Not a state a store leaves: keep the file, out of the way
        return set_aside(fs, set, st, SAVE_SPARE, "exists beside the other three save files");
    }
    if (has(st, SAVE_TMP) && !full(st, SAVE_TMP, size) &&
        set_aside(fs, set, st, SAVE_TMP, "is not a complete save beside the rotation's spare") != 0) {
        return -1;
    }
    if (has(st, SAVE_TMP)) {
        // P.tmp holds the newest save
        say(fs, "save: finishing the renames of the save that a power cut interrupted");
        if (has(st, SAVE_MAIN) && move(fs, set, st, SAVE_MAIN, SAVE_BAK) != 0) {
            return -1;
        }
        if (move(fs, set, st, SAVE_TMP, SAVE_MAIN) != 0) {
            return -1;
        }
    } else if (!has_data(st, SAVE_MAIN) && !has_data(st, SAVE_BAK)) {
        return 0; // P.spare is the only save: save_store writes P beside it, the next store renames it
    }
    return move(fs, set, st, SAVE_SPARE, has(st, SAVE_BAK) ? SAVE_TMP : SAVE_BAK);
}

int save_store(const SaveFs* fs, const SaveSet* set, const void* src, unsigned int size, int* created) {
    SaveState st;
    int inPlace;

    *created = 0;
    scan(fs, set, &st);
    if (resolve_shared(fs, set, &st) != 0 || finish_rotation(fs, set, &st, size) != 0) {
        return -1;
    }

    // No save in P: write P itself and leave the others alone (one of them may be the only save)
    if (!has_data(&st, SAVE_MAIN)) {
        if (fs->write(fs->ctx, set->path[SAVE_MAIN], src, size, 0) != 0) {
            say(fs, "save: writing %s failed", set->path[SAVE_MAIN]);
            return -1;
        }
        *created = 1;
        return 0;
    }

    // 1. The new save into P.tmp, in place if it is a complete save already. A short P.tmp is truncated, unless
    // another name still uses its clusters.
    inPlace = full(&st, SAVE_TMP, size);
    if (!inPlace && clusters_shared(fs, set, &st, SAVE_TMP) &&
        set_aside(fs, set, &st, SAVE_TMP, "is short and shares its clusters with another file") != 0) {
        return -1;
    }
    if (fs->write(fs->ctx, set->path[SAVE_TMP], src, size, inPlace) != 0) {
        say(fs, "save: writing %s failed", set->path[SAVE_TMP]);
        return -1;
    }
    *created = !inPlace;
    st.f[SAVE_TMP].exists = 1;
    st.f[SAVE_TMP].size = (long)size;

    // 2. The renames
    if (has(&st, SAVE_BAK)) {
        if (move(fs, set, &st, SAVE_BAK, SAVE_SPARE) != 0 || move(fs, set, &st, SAVE_MAIN, SAVE_BAK) != 0) {
            return -1;
        }
    } else if (move(fs, set, &st, SAVE_MAIN, SAVE_SPARE) != 0) {
        return -1;
    }
    if (move(fs, set, &st, SAVE_TMP, SAVE_MAIN) != 0) {
        return -1;
    }
    return move(fs, set, &st, SAVE_SPARE, has(&st, SAVE_BAK) ? SAVE_TMP : SAVE_BAK);
}
