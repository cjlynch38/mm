/**
 * Host test of port/gc/ogc/save_rotation.c, the save files on the SD card (bridge_save.c).
 *
 * The files live on a model of a FAT card: directory entries (each with its own size field) pointing at cluster
 * chains, renames that fail when the target exists, writes in place or by creating/truncating, and a counter of
 * allocations. Truncating a chain that another name still uses marks it corrupt. The model can lose power at any
 * write or rename, with these effects on the operation it interrupts:
 *   rename: not done; both names left on one chain (cross-linked); neither name left (the chain is lost)
 *   write:  in place, the first half new and the rest old; creating, an entry of size 0
 * after which every operation fails until the "reboot".
 *
 * For start states made by 0-5 clean stores, by earlier builds (P only, P and P.bak, their power-cut states) and
 * by odd card contents, and for every point and effect of a power cut during the next store, it checks that:
 *  - save_pick then finds a complete save, the one from before the store or the new one, and the new one whenever
 *    the cut came after the store's first rename (or a first store's write);
 *  - no chain in use was freed, and nothing is ever deleted;
 *  - the next stores recover: each returns 0, save_pick finds its data, and once P, P.bak and P.tmp exist again
 *    stores allocate nothing and leave P = newest, P.bak = the one before, P.tmp = the one before that.
 * A long random run then mixes stores and power cuts.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "save_rotation.h"

#define SIZE 256
#define NAMES 24
#define CHAINS 64
#define PATH "sd:/mmgcport/mm.fla"

enum { CUT_NONE, CUT_UNDONE, CUT_HALF, CUT_LOST, CUT_KINDS };

typedef struct {
    int refs;
    int corrupt; /* freed while a name still used it */
    unsigned char data[SIZE];
} Chain;

typedef struct {
    int used;
    char name[SAVE_PATH_MAX + 8];
    long size;
    int chain;
} Entry;

typedef struct {
    Entry e[NAMES];
    Chain c[CHAINS];
    int ops;           /* writes and renames so far */
    int cutAt;         /* the operation the power cut interrupts, -1 for none */
    int cutKind;
    int dead;          /* power is off: every operation fails */
    int allocations;
    int writeDone;     /* the store's write of P.tmp completed */
    int mainAtWrite;   /* the write went to P.tmp while P held a save */
    int renamesAfter;  /* renames that reached the card (fully or half) after that write */
    int quiet;
} Card;

static int sFailures;
static int sChecks;
static char sContext[200];

static void check(int ok, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void check(int ok, const char* fmt, ...) {
    va_list args;

    sChecks++;
    if (ok) {
        return;
    }
    sFailures++;
    if (sFailures <= 40) {
        fprintf(stderr, "FAIL [%s] ", sContext);
        va_start(args, fmt);
        vfprintf(stderr, fmt, args);
        va_end(args);
        fprintf(stderr, "\n");
    }
}

/* ---------------------------------------------------------------------------------------------- */
/* The card                                                                                       */
/* ---------------------------------------------------------------------------------------------- */

static Entry* find(Card* card, const char* name) {
    int i;

    for (i = 0; i < NAMES; i++) {
        if (card->e[i].used && strcmp(card->e[i].name, name) == 0) {
            return &card->e[i];
        }
    }
    return NULL;
}

static Entry* new_entry(Card* card, const char* name) {
    int i;

    for (i = 0; i < NAMES; i++) {
        if (!card->e[i].used) {
            memset(&card->e[i], 0, sizeof(card->e[i]));
            card->e[i].used = 1;
            snprintf(card->e[i].name, sizeof(card->e[i].name), "%s", name);
            return &card->e[i];
        }
    }
    fprintf(stderr, "model: out of directory entries\n");
    exit(2);
}

static int new_chain(Card* card) {
    int i;

    for (i = 0; i < CHAINS; i++) {
        if (card->c[i].refs == 0) {
            memset(&card->c[i], 0, sizeof(card->c[i]));
            card->c[i].refs = 1;
            card->allocations++;
            return i;
        }
    }
    fprintf(stderr, "model: out of chains\n");
    exit(2);
}

/* Save files (not P.dupN, which are never read again) on a chain */
static int live_refs(Card* card, int chain) {
    int n = 0;
    int i;

    for (i = 0; i < NAMES; i++) {
        n += card->e[i].used && card->e[i].chain == chain && strstr(card->e[i].name, ".dup") == NULL;
    }
    return n;
}

static void drop_chain(Card* card, int chain) {
    if (--card->c[chain].refs > 0) {
        card->c[chain].corrupt = 1; // freed while another name uses it
    }
}

/* Count an operation; 1 if the power cut interrupts it */
static int op_start(Card* card) {
    if (card->dead) {
        return -1;
    }
    if (card->ops++ == card->cutAt) {
        card->dead = 1;
        return 1;
    }
    return 0;
}

static void m_stat(void* ctx, const char* path, SaveFileInfo* info) {
    Card* card = ctx;
    Entry* e = find(card, path);

    memset(info, 0, sizeof(*info));
    if (card->dead || e == NULL) {
        return;
    }
    info->exists = 1;
    info->size = e->size;
    info->cluster = (e->size != 0) ? (unsigned long)e->chain + 100 : 0;
}

static int m_rename(void* ctx, const char* from, const char* to) {
    Card* card = ctx;
    Entry* src = find(card, from);
    int cut;

    if (card->dead || src == NULL || find(card, to) != NULL) {
        return -1;
    }
    cut = op_start(card);
    if (cut < 0) {
        return -1;
    }
    if (cut) {
        if (card->cutKind == CUT_HALF) {
            Entry* dst = new_entry(card, to);

            dst->size = src->size;
            dst->chain = src->chain;
            card->c[src->chain].refs++;
            card->renamesAfter += card->writeDone;
        } else if (card->cutKind == CUT_LOST) {
            card->c[src->chain].refs--; // the chain is lost, not freed: no other name uses it
            src->used = 0;
        }
        return -1;
    }
    snprintf(src->name, sizeof(src->name), "%s", to);
    card->renamesAfter += card->writeDone;
    return 0;
}

static int m_write(void* ctx, const char* path, const void* data, unsigned int size, int inPlace) {
    Card* card = ctx;
    Entry* e = find(card, path);
    int cut;

    if (card->dead) {
        return -1;
    }
    check(size == SIZE, "write of %u bytes", size);
    if (inPlace) {
        check(e != NULL && e->size == SIZE, "in-place write of %s, which is %s", path, e ? "short" : "missing");
        if (e == NULL || e->size != SIZE) {
            return -1;
        }
        check(live_refs(card, e->chain) == 1, "in-place write of %s, whose clusters another save file uses", path);
    }
    {
        Entry* main = find(card, PATH);

        card->mainAtWrite = strcmp(path, PATH) != 0 && main != NULL && main->size > 0;
    }
    cut = op_start(card);
    if (cut < 0) {
        return -1;
    }
    if (inPlace) {
        memcpy(card->c[e->chain].data, data, cut ? SIZE / 2 : SIZE);
    } else {
        if (e == NULL) {
            e = new_entry(card, path);
        } else {
            drop_chain(card, e->chain);
        }
        e->chain = new_chain(card);
        e->size = 0;
        if (!cut) {
            memcpy(card->c[e->chain].data, data, SIZE);
            e->size = SIZE;
        }
    }
    if (cut) {
        return -1;
    }
    card->writeDone = 1;
    card->renamesAfter = 0;
    return 0;
}

static void m_log(void* ctx, const char* line) {
    Card* card = ctx;

    if (!card->quiet) {
        printf("    %s\n", line);
    }
}

static SaveFs fs_of(Card* card) {
    SaveFs fs = { card, m_stat, m_rename, m_write, m_log };

    return fs;
}

/* Save data of a version: the version in the first two bytes, then a pattern that depends on it */
static void fill(unsigned char* buf, int version) {
    int i;

    buf[0] = (unsigned char)version;
    buf[1] = (unsigned char)(version >> 8);
    for (i = 2; i < SIZE; i++) {
        buf[i] = (unsigned char)(i * 31 + version * 7 + (version >> 5));
    }
}

/* A file placed directly (start states) */
static void put(Card* card, const char* suffix, int version, long size) {
    char name[SAVE_PATH_MAX + 8];
    Entry* e;
    int i;

    snprintf(name, sizeof(name), "%s%s", PATH, suffix);
    (void)i;
    e = new_entry(card, name);
    e->chain = new_chain(card);
    e->size = size;
    fill(card->c[e->chain].data, version);
}

/* ---------------------------------------------------------------------------------------------- */
/* Checks                                                                                         */
/* ---------------------------------------------------------------------------------------------- */

/* The version a save file holds, or -1 if it is no complete save */
static int version_of(Card* card, const char* path) {
    Entry* e = find(card, path);
    unsigned char want[SIZE];
    int v;

    if (e == NULL || e->size != SIZE || card->c[e->chain].corrupt) {
        return -1;
    }
    v = card->c[e->chain].data[0] | (card->c[e->chain].data[1] << 8);
    fill(want, v);
    return (memcmp(want, card->c[e->chain].data, SIZE) == 0) ? v : -1;
}

/* What save_pick loads: its version, -1 for incomplete data, -2 for no save */
static int loaded(Card* card, const SaveSet* set) {
    SaveFs fs = fs_of(card);
    int which = save_pick(&fs, set, SIZE);

    return (which < 0) ? -2 : version_of(card, set->path[which]);
}

static int store(Card* card, const SaveSet* set, int version, int* created) {
    SaveFs fs = fs_of(card);
    unsigned char buf[SIZE];

    fill(buf, version);
    card->writeDone = 0;
    card->renamesAfter = 0;
    return save_store(&fs, set, buf, SIZE, created);
}

static void check_card(Card* card) {
    int i;

    for (i = 0; i < CHAINS; i++) {
        check(!(card->c[i].refs > 0 && card->c[i].corrupt), "a chain in use was freed");
    }
}

static int count_files(Card* card) {
    int i;
    int n = 0;

    for (i = 0; i < NAMES; i++) {
        n += card->e[i].used;
    }
    return n;
}

/* Clean stores after a cut: they work, and the set settles into P, P.bak, P.tmp without allocations */
static void check_recovery(Card* card, const SaveSet* set, int version) {
    int created = 0;
    int k;

    for (k = 1; k <= 4; k++) {
        int allocBefore = card->allocations;
        int ret = store(card, set, version + k, &created);

        check(ret == 0, "store %d after the cut failed", version + k);
        check(loaded(card, set) == version + k, "after store %d save_pick loads %d", version + k, loaded(card, set));
        check(created == (card->allocations != allocBefore), "store %d: created %d, allocations %d", version + k,
              created, card->allocations - allocBefore);
        if (k >= 4) {
            check(created == 0, "store %d after the cut allocated clusters", version + k);
            check(version_of(card, set->path[SAVE_MAIN]) == version + k &&
                      version_of(card, set->path[SAVE_BAK]) == version + k - 1 &&
                      version_of(card, set->path[SAVE_TMP]) == version + k - 2 && find(card, set->path[SAVE_SPARE]) == NULL,
                  "after store %d the files are not P, P.bak, P.tmp = %d %d %d", version + k,
                  version_of(card, set->path[SAVE_MAIN]), version_of(card, set->path[SAVE_BAK]),
                  version_of(card, set->path[SAVE_TMP]));
        }
    }
    check_card(card);
}

/* ---------------------------------------------------------------------------------------------- */
/* Start states                                                                                   */
/* ---------------------------------------------------------------------------------------------- */

typedef struct {
    const char* name;
    int lenient; /* odd card contents: only "a complete save, or none" is required after a cut */
    void (*make)(Card* card, const SaveSet* set);
} StartState;

static void clean_stores(Card* card, const SaveSet* set, int n) {
    int created;
    int i;

    for (i = 1; i <= n; i++) {
        store(card, set, i, &created);
    }
}

static void s_empty(Card* c, const SaveSet* s) { clean_stores(c, s, 0); }
static void s_one(Card* c, const SaveSet* s) { clean_stores(c, s, 1); }
static void s_two(Card* c, const SaveSet* s) { clean_stores(c, s, 2); }
static void s_three(Card* c, const SaveSet* s) { clean_stores(c, s, 3); }
static void s_five(Card* c, const SaveSet* s) { clean_stores(c, s, 5); }
/* Earlier builds: P, P and P.bak; their cuts: P.bak removed before P -> P.bak; after P -> P.bak; in the write */
static void s_old_one(Card* c, const SaveSet* s) { put(c, "", 1, SIZE); }
static void s_old_two(Card* c, const SaveSet* s) { put(c, "", 2, SIZE); put(c, ".bak", 1, SIZE); }
static void s_old_no_bak(Card* c, const SaveSet* s) { put(c, "", 2, SIZE); put(c, ".tmp", 3, SIZE); }
static void s_old_moved(Card* c, const SaveSet* s) { put(c, ".bak", 2, SIZE); put(c, ".tmp", 3, SIZE); }
static void s_old_partial(Card* c, const SaveSet* s) { put(c, "", 2, SIZE); put(c, ".bak", 1, SIZE); put(c, ".tmp", 9, 100); }
static void s_old_first(Card* c, const SaveSet* s) { put(c, ".tmp", 1, SIZE); }
/* Odd contents: a short P, only P.bak, only P.spare, all four names */
static void s_short(Card* c, const SaveSet* s) { put(c, "", 1, 100); }
static void s_bak_only(Card* c, const SaveSet* s) { put(c, ".bak", 1, SIZE); }
static void s_spare_only(Card* c, const SaveSet* s) { put(c, ".spare", 1, SIZE); }
static void s_all_four(Card* c, const SaveSet* s) {
    put(c, "", 4, SIZE);
    put(c, ".bak", 3, SIZE);
    put(c, ".tmp", 2, SIZE);
    put(c, ".spare", 1, SIZE);
}

static const StartState sStates[] = {
    { "empty card", 0, s_empty },
    { "after 1 store", 0, s_one },
    { "after 2 stores", 0, s_two },
    { "after 3 stores", 0, s_three },
    { "after 5 stores", 0, s_five },
    { "earlier build: P", 0, s_old_one },
    { "earlier build: P, P.bak", 0, s_old_two },
    { "earlier build cut: P, P.tmp", 0, s_old_no_bak },
    { "earlier build cut: P.bak, P.tmp", 0, s_old_moved },
    { "earlier build cut: short P.tmp", 0, s_old_partial },
    { "earlier build cut: only P.tmp", 0, s_old_first },
    { "short P", 1, s_short },
    { "only P.bak", 0, s_bak_only },
    { "only P.spare", 0, s_spare_only },
    { "all four names", 1, s_all_four },
};

/* ---------------------------------------------------------------------------------------------- */
/* Tests                                                                                          */
/* ---------------------------------------------------------------------------------------------- */

static void test_clean(const SaveSet* set) {
    Card card;
    int created = 0;
    int v;

    memset(&card, 0, sizeof(card));
    card.cutAt = -1;
    snprintf(sContext, sizeof(sContext), "clean stores");
    check(loaded(&card, set) == -2, "an empty card has a save");
    for (v = 1; v <= 8; v++) {
        int allocBefore = card.allocations;

        check(store(&card, set, v, &created) == 0, "store %d failed", v);
        check(loaded(&card, set) == v, "store %d: save_pick loads %d", v, loaded(&card, set));
        check(created == (v <= 3) && card.allocations - allocBefore == (v <= 3),
              "store %d: created %d, %d allocations", v, created, card.allocations - allocBefore);
    }
    check(version_of(&card, set->path[SAVE_MAIN]) == 8 && version_of(&card, set->path[SAVE_BAK]) == 7 &&
              version_of(&card, set->path[SAVE_TMP]) == 6 && count_files(&card) == 3,
          "after 8 stores: %d files", count_files(&card));
    check(card.allocations == 3, "%d allocations in 8 stores", card.allocations);
    check_card(&card);
}

static void test_cuts(const SaveSet* set) {
    unsigned int s;
    int cases = 0;

    for (s = 0; s < sizeof(sStates) / sizeof(sStates[0]); s++) {
        const StartState* start = &sStates[s];
        Card probe;
        int created;
        int ops;
        int cut;
        int kind;

        // Count the operations of an uninterrupted store from this state
        memset(&probe, 0, sizeof(probe));
        probe.cutAt = -1;
        probe.quiet = 1;
        start->make(&probe, set);
        probe.ops = 0;
        snprintf(sContext, sizeof(sContext), "%s, no cut", start->name);
        check(store(&probe, set, 100, &created) == 0, "store failed");
        ops = probe.ops;
        check(loaded(&probe, set) == 100, "save_pick loads %d", loaded(&probe, set));
        check_recovery(&probe, set, 100);

        for (cut = 0; cut < ops; cut++) {
            for (kind = CUT_UNDONE; kind < CUT_KINDS; kind++) {
                Card card;
                int before;
                int after;

                memset(&card, 0, sizeof(card));
                card.cutAt = -1;
                card.quiet = 1;
                start->make(&card, set);
                before = loaded(&card, set);
                card.ops = 0;
                card.cutAt = cut;
                card.cutKind = kind;
                snprintf(sContext, sizeof(sContext), "%s, cut at operation %d of %d (%s)", start->name, cut, ops,
                         kind == CUT_UNDONE ? "not done" : kind == CUT_HALF ? "half done" : "lost");
                check(store(&card, set, 100, &created) != 0, "the store did not notice the cut");
                card.dead = 0;
                card.cutAt = -1;
                after = loaded(&card, set);
                check_card(&card);
                if (start->lenient) {
                    check(after >= 0 || after == before, "save_pick loads incomplete data");
                } else {
                    check(after == before || after == 100, "save_pick loads %d (before the store: %d)", after, before);
                    if (card.writeDone && (card.renamesAfter > 0 || !card.mainAtWrite) && kind != CUT_LOST) {
                        check(after == 100, "the new save was complete and committed, but save_pick loads %d", after);
                    }
                }
                check_recovery(&card, set, 100);
                cases++;
            }
        }
    }
    printf("  %d power cuts in %u start states\n", cases, (unsigned int)(sizeof(sStates) / sizeof(sStates[0])));
}

static uint32_t sRandom = 2463534242u;
static uint32_t rnd(void) {
    sRandom ^= sRandom << 13;
    sRandom ^= sRandom >> 17;
    sRandom ^= sRandom << 5;
    return sRandom;
}

/* Stores with power cuts at random points; set-aside .dup files are cleared now and then, as a user would after a
 * disk check (the model keeps their chains, as FAT would until the check) */
static void test_random(const SaveSet* set) {
    Card card;
    int lastGood = -2;
    int version;
    int created;
    int cuts = 0;

    memset(&card, 0, sizeof(card));
    card.cutAt = -1;
    card.quiet = 1;
    for (version = 1; version <= 3000; version++) {
        int after;
        int wasCut;

        snprintf(sContext, sizeof(sContext), "random run, store %d", version);
        card.ops = 0;
        card.cutAt = (rnd() % 3 == 0) ? (int)(rnd() % 8) : -1;
        card.cutKind = CUT_UNDONE + (int)(rnd() % 3);
        if (store(&card, set, version, &created) == 0) {
            lastGood = version;
        }
        wasCut = card.dead;
        cuts += wasCut;
        card.dead = 0;
        card.cutAt = -1;
        after = loaded(&card, set);
        if (wasCut && card.cutKind == CUT_LOST) {
            // A lost rename may cost the save it was moving, here or from an interrupted store before
            check(after >= 0 || lastGood == -2, "save_pick finds no complete save (last good %d)", lastGood);
            lastGood = after;
        } else {
            check(after == lastGood || after == version, "save_pick loads %d (last good %d)", after, lastGood);
        }
        if (after == version) {
            lastGood = version;
        }
        check_card(&card);
        if (version % 50 == 0) {
            int i;

            for (i = 0; i < NAMES; i++) {
                if (card.e[i].used && strstr(card.e[i].name, ".dup") != NULL) {
                    card.c[card.e[i].chain].refs--;
                    card.e[i].used = 0;
                }
            }
        }
    }
    check_recovery(&card, set, version);
    printf("  random run: 3000 stores, %d power cuts\n", cuts);
}

int main(void) {
    SaveSet set;

    save_set_init(&set, PATH);
    test_clean(&set);
    test_cuts(&set);
    test_random(&set);
    printf("save_rotation: %d checks, %d failed\n", sChecks, sFailures);
    return sFailures != 0;
}
