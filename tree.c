// tree.c — Tree object serialization and construction

#include "tree.h"
#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

// Forward declaration
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

#define MODE_FILE  0100644
#define MODE_EXEC  0100755
#define MODE_DIR   0040000

// ─── PROVIDED ───────────────────────────────────────────────────────────────

uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    if (S_ISDIR(st.st_mode))   return MODE_DIR;
    if (st.st_mode & S_IXUSR)  return MODE_EXEC;
    return MODE_FILE;
}

int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;

    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];

        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1;

        char mode_str[16] = {0};
        size_t mode_len = space - ptr;
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = strtol(mode_str, NULL, 8);
        ptr = space + 1;

        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1;

        size_t name_len = null_byte - ptr;
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0';
        ptr = null_byte + 1;

        if (ptr + HASH_SIZE > end) return -1;
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;

        tree_out->count++;
    }
    return 0;
}

static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    size_t max_size = tree->count * 296;
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;

    Tree sorted_tree = *tree;
    qsort(sorted_tree.entries, sorted_tree.count, sizeof(TreeEntry), compare_tree_entries);

    size_t offset = 0;
    for (int i = 0; i < sorted_tree.count; i++) {
        const TreeEntry *entry = &sorted_tree.entries[i];
        int written = sprintf((char *)buffer + offset, "%o %s", entry->mode, entry->name);
        offset += written + 1;
        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }

    *data_out = buffer;
    *len_out  = offset;
    return 0;
}

// ─── IMPLEMENTED ─────────────────────────────────────────────────────────────

// Recursive helper: given a slice of index entries (all sharing the same
// directory prefix at `depth` path components), build one Tree object and
// write it to the store.  Returns 0 on success.
//
// All paths in entries[] are relative to the repo root, e.g.
//   entries[0].path = "src/main.c"
//   entries[1].path = "src/util/helper.c"
//   entries[2].path = "README.md"
//
// `prefix` is the directory path that all entries in this slice start with
// (empty string "" for the root level).
static int write_tree_level(IndexEntry *entries, int count,
                             const char *prefix, ObjectID *id_out) {
    Tree tree;
    tree.count = 0;

    int i = 0;
    while (i < count) {
        // Path relative to current directory level
        const char *rel = entries[i].path + strlen(prefix);

        // Find the next '/' — if present, this entry lives in a subdirectory
        const char *slash = strchr(rel, '/');

        if (!slash) {
            // ── Leaf file entry ──────────────────────────────────────────
            TreeEntry *te = &tree.entries[tree.count++];
            te->mode = entries[i].mode;
            te->hash = entries[i].hash;
            // Name is the filename part only (no directory components)
            strncpy(te->name, rel, sizeof(te->name) - 1);
            te->name[sizeof(te->name) - 1] = '\0';
            i++;
        } else {
            // ── Subdirectory: group all entries that share this subdir ──
            // Extract just the subdirectory name
            size_t dir_name_len = (size_t)(slash - rel);
            char dir_name[256];
            if (dir_name_len >= sizeof(dir_name)) return -1;
            memcpy(dir_name, rel, dir_name_len);
            dir_name[dir_name_len] = '\0';

            // Build the full prefix for the subdirectory
            char sub_prefix[512];
            snprintf(sub_prefix, sizeof(sub_prefix), "%s%s/", prefix, dir_name);

            // Count how many consecutive entries belong to this subdirectory
            int j = i;
            while (j < count && strncmp(entries[j].path, sub_prefix, strlen(sub_prefix)) == 0)
                j++;

            // Recursively build the subtree for these entries
            ObjectID sub_id;
            if (write_tree_level(entries + i, j - i, sub_prefix, &sub_id) != 0)
                return -1;

            // Add the subtree as a directory entry
            TreeEntry *te = &tree.entries[tree.count++];
            te->mode = MODE_DIR;
            te->hash = sub_id;
            strncpy(te->name, dir_name, sizeof(te->name) - 1);
            te->name[sizeof(te->name) - 1] = '\0';

            i = j;
        }
    }

    // Serialize and write this tree object
    void *data;
    size_t data_len;
    if (tree_serialize(&tree, &data, &data_len) != 0) return -1;

    int rc = object_write(OBJ_TREE, data, data_len, id_out);
    free(data);
    return rc;
}

// Comparator for sorting index entries by path (for consistent tree hashing)
static int compare_index_by_path(const void *a, const void *b) {
    return strcmp(((const IndexEntry *)a)->path, ((const IndexEntry *)b)->path);
}

int tree_from_index(ObjectID *id_out) {
    // Load the index
    Index index;
    if (index_load(&index) != 0) return -1;

    if (index.count == 0) {
        // Empty index: write an empty tree
        Tree empty;
        empty.count = 0;
        void *data;
        size_t data_len;
        if (tree_serialize(&empty, &data, &data_len) != 0) return -1;
        int rc = object_write(OBJ_TREE, data, data_len, id_out);
        free(data);
        return rc;
    }

    // Sort entries by path so subdirectory grouping works correctly
    qsort(index.entries, index.count, sizeof(IndexEntry), compare_index_by_path);

    // Recursively build the tree from the root level (empty prefix)
    return write_tree_level(index.entries, index.count, "", id_out);
}

// Weak stub: when tree.o is linked WITHOUT index.o (e.g. test_tree),
// this fallback initialises an empty index.  The real index_load in
// index.o overrides this at link time for the main 'pes' binary.
__attribute__((weak)) int index_load(Index *index) {
    index->count = 0;
    return 0;
}
