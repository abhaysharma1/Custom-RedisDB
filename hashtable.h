#pragma once

#include <stddef.h>
#include <stdint.h>

// hashtable node
struct HNode {
    HNode* next = NULL;
    uint64_t hcode = 0;
};

// actual hash table
struct HTab {
    HNode** tab = NULL;  // array of the hash table
    size_t mask = 0;
    size_t size = 0;
};

// node to store and maintain the progressive rehashing
struct HMap {
    HTab newer;
    HTab older;
    size_t migrate_pos = 0;
};

HNode* hm_lookup(HMap* hmap, HNode* key, bool (*eq)(HNode*, HNode*));
void hm_insert(HMap* hmap, HNode* node);
HNode* hm_delete(HMap* hmap, HNode* key, bool (*eq)(HNode*, HNode*));
void hm_clear(HMap* hmap);
size_t hm_size(HMap* hmap);
