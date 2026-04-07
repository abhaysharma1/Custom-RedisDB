#include "hashtable.h"

#include <assert.h>
#include <stdlib.h>

// hashtable inititlization and safety checks
static void h_init(HTab* htab, size_t n) {
    assert(n > 0 && (((n - 1) & n) == 0));
    htab->tab = (HNode**)calloc(n, sizeof(HNode*));
    htab->mask = n - 1;
    htab->size = 0;
}

// insert at beginning
static void h_insert(HTab* htab, HNode* node) {
    size_t pos = node->hcode & htab->mask;
    HNode* next = htab->tab[pos];
    node->next = next;
    htab->tab[pos] = node;
    htab->size++;
}

// hashtable lookup
static HNode** h_lookup(HTab* htab, HNode* key, bool (*eq)(HNode*, HNode*)) {
    if (!htab->tab) {
        return NULL;
    }

    size_t pos = key->hcode & htab->mask;
    HNode** from = &htab->tab[pos];  // check notes for logic
    for (HNode* cur; (cur = *from) != NULL; from = &cur->next) {
        if (cur->hcode == key->hcode && eq(cur, key)) {
            return from;
        }
    }
    return NULL;
}

// deletion
static HNode* h_detach(HTab* htab, HNode** from) {
    HNode* node = *from;
    *from = node->next;
    htab->size--;
    return node;
}

const size_t k_rehashing_work = 128;

// migrate_pos points to a slot in the hashtable
// if the slot is not empty
// we detach the first node in the linked list in the slot
// and move it to the new table
// we only move 128 items per function call
static void hm_help_rehashing(HMap* hmap) {
    size_t nwork = 0;
    while (nwork < k_rehashing_work && hmap->older.size > 0) {
        HNode** from = &hmap->older.tab[hmap->migrate_pos];  // select slot from where left off
        if (!*from) {                                        // if slot is empty skip
            hmap->migrate_pos++;
            continue;
        }
        // move the first item in the slot to the new table
        h_insert(&hmap->newer, h_detach(&hmap->older, from));
        nwork++;
    }
    // discard the old table if done
    if (hmap->older.size == 0 && hmap->older.tab) {
        free(hmap->older.tab);
        hmap->older = HTab{};  // intialize it to empty/default table
    }
}

// initate rehashing
static void hm_trigger_rehashing(HMap* hmap) {
    assert(hmap->older.tab == NULL);
    hmap->older = hmap->newer;
    h_init(&hmap->newer, (hmap->newer.mask + 1) * 2);
    hmap->migrate_pos = 0;
}

// checking and initiating hashing before looking up
HNode* hm_lookup(HMap* hmap, HNode* key, bool (*eq)(HNode*, HNode*)) {
    hm_help_rehashing(hmap);
    HNode** from = h_lookup(&hmap->newer, key, eq);
    if (!from) {
        from = h_lookup(&hmap->older, key, eq);
    }
    return from ? *from : NULL;
}

const size_t k_max_load_factor = 8;

// insert while maintaining progressive rehashing
void hm_insert(HMap* hmap, HNode* node) {
    if (!hmap->newer.tab) {
        h_init(&hmap->newer, 4);
    }
    h_insert(&hmap->newer, node);  // insert to new table

    if (!hmap->older.tab) {  // check whether rehashing is happening
        size_t threshold = (hmap->newer.mask + 1) * k_max_load_factor;
        if (hmap->newer.size >= threshold) {
            hm_trigger_rehashing(hmap);
        }
    }
    hm_help_rehashing(hmap);
}

// bool(*eq)(HNode *, HNode *) passes the two HNode* to the eq function
HNode* hm_delete(HMap* hmap, HNode* key, bool (*eq)(HNode*, HNode*)) {
    // check progressive rehashing
    hm_help_rehashing(hmap);
    // delete form newer table
    if (HNode** from = h_lookup(&hmap->newer, key, eq)) {
        return h_detach(&hmap->newer, from);
    }
    // if doesn't exists in newer table, delete from older table
    if (HNode** from = h_lookup(&hmap->older, key, eq)) {
        return h_detach(&hmap->older, from);
    }
    return NULL;
}

// delete the table
void hm_clear(HMap* hmap) {
    free(hmap->newer.tab);
    free(hmap->older.tab);
    *hmap = HMap{};
}

// return total size of the current table
size_t hm_size(HMap* hmap) { return hmap->newer.size + hmap->older.size; }

static bool h_foreach(HTab* htab, bool (*f)(HNode*, void*), void* arg) {
    for (size_t i = 0; htab->mask != 0 && i <= htab->mask; i++) {  // looping through the table
        for (HNode* node = htab->tab[i]; node != NULL;  node = node->next) {  // looping throught linked List in the table
            if (!f(node, arg)) {
                return false;
            }
        }
    }
    return true;
}

void hm_foreach(HMap* hmap, bool (*f)(HNode*, void*), void* arg) {
    // && is so that if the left side is false the right side is never executed
    // this in actual does it so that
    // if we fail to write a key to the output buffer from the newer table we never start reading
    // the old table
    h_foreach(&hmap->newer, f, arg) && h_foreach(&hmap->older, f, arg);
}