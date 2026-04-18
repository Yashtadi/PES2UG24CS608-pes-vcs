// index.c — Staging area implementation

#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// ─── PROVIDED ────────────────────────────────────────────────────────────────

IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int remaining = index->count - i - 1;
            if (remaining > 0)
                memmove(&index->entries[i], &index->entries[i + 1],
                        remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;
    for (int i = 0; i < index->count; i++) {
        printf("  staged:     %s\n", index->entries[i].path);
        staged_count++;
    }
    if (staged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Unstaged changes:\n");
    int unstaged_count = 0;
    for (int i = 0; i < index->count; i++) {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0) {
            printf("  deleted:    %s\n", index->entries[i].path);
            unstaged_count++;
        } else {
            if (st.st_mtime != (time_t)index->entries[i].mtime_sec ||
                st.st_size  != (off_t)index->entries[i].size) {
                printf("  modified:   %s\n", index->entries[i].path);
                unstaged_count++;
            }
        }
    }
    if (unstaged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Untracked files:\n");
    int untracked_count = 0;
    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            if (strcmp(ent->d_name, ".pes") == 0) continue;
            if (strcmp(ent->d_name, "pes")  == 0) continue;
            if (strstr(ent->d_name, ".o")   != NULL) continue;

            int is_tracked = 0;
            for (int i = 0; i < index->count; i++) {
                if (strcmp(index->entries[i].path, ent->d_name) == 0) {
                    is_tracked = 1; break;
                }
            }
            if (!is_tracked) {
                struct stat st;
                stat(ent->d_name, &st);
                if (S_ISREG(st.st_mode)) {
                    printf("  untracked:  %s\n", ent->d_name);
                    untracked_count++;
                }
            }
        }
        closedir(dir);
    }
    if (untracked_count == 0) printf("  (nothing to show)\n");
    printf("\n");
    return 0;
}

// ─── IMPLEMENTED ─────────────────────────────────────────────────────────────

int index_load(Index *index) {
    index->count = 0;

    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) return 0;  // No index yet — empty index, not an error

    char hex[HASH_HEX_SIZE + 1];
    unsigned int mode;
    unsigned long long mtime;
    unsigned int size;
    char path[512];

    while (index->count < MAX_INDEX_ENTRIES) {
        int rc = fscanf(f, "%o %64s %llu %u %511s\n",
                        &mode, hex, &mtime, &size, path);
        if (rc == EOF) break;
        if (rc != 5) { fclose(f); return -1; }

        IndexEntry *e = &index->entries[index->count];
        e->mode      = (uint32_t)mode;
        e->mtime_sec = (uint64_t)mtime;
        e->size      = (uint32_t)size;
        strncpy(e->path, path, sizeof(e->path) - 1);
        e->path[sizeof(e->path) - 1] = '\0';

        if (hex_to_hash(hex, &e->hash) != 0) { fclose(f); return -1; }

        index->count++;
    }

    fclose(f);
    return 0;
}

// Pointer-array comparator — used by index_save to sort without copying Index
static int compare_ptrs_by_path(const void *a, const void *b) {
    return strcmp((*(const IndexEntry **)a)->path,
                  (*(const IndexEntry **)b)->path);
}

int index_save(const Index *index) {
    char tmp_path[] = INDEX_FILE ".tmp";

    FILE *f = fopen(tmp_path, "w");
    if (!f) return -1;

    // KEY DESIGN NOTE: We sort a small array of POINTERS (8 bytes each)
    // rather than copying the entire 5.4 MB Index struct onto the stack.
    // The caller (cmd_add in pes.c) already has one Index on its stack;
    // a second copy here would push total stack usage past 10 MB → overflow.
    const IndexEntry **ptrs = malloc((size_t)index->count * sizeof(IndexEntry *));
    if (!ptrs && index->count > 0) { fclose(f); return -1; }

    for (int i = 0; i < index->count; i++)
        ptrs[i] = &index->entries[i];

    qsort(ptrs, (size_t)index->count, sizeof(IndexEntry *), compare_ptrs_by_path);

    for (int i = 0; i < index->count; i++) {
        const IndexEntry *e = ptrs[i];
        char hex[HASH_HEX_SIZE + 1];
        hash_to_hex(&e->hash, hex);
        fprintf(f, "%o %s %llu %u %s\n",
                e->mode, hex,
                (unsigned long long)e->mtime_sec,
                e->size, e->path);
    }
    free(ptrs);

    fflush(f);
    fsync(fileno(f));
    fclose(f);

    if (rename(tmp_path, INDEX_FILE) != 0) {
        unlink(tmp_path);
        return -1;
    }
    return 0;
}

int index_add(Index *index, const char *path) {
    // 1. Read file contents
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "error: cannot open '%s'\n", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (file_size < 0) { fclose(f); return -1; }

    void *contents = malloc((size_t)file_size + 1);
    if (!contents) { fclose(f); return -1; }
    size_t nread = fread(contents, 1, (size_t)file_size, f);
    fclose(f);
    if (nread != (size_t)file_size) { free(contents); return -1; }

    // 2. Write blob to object store
    ObjectID blob_id;
    if (object_write(OBJ_BLOB, contents, nread, &blob_id) != 0) {
        free(contents); return -1;
    }
    free(contents);

    // 3. Get file metadata
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    uint32_t mode = (st.st_mode & S_IXUSR) ? 0100755 : 0100644;

    // 4. Update or insert index entry
    IndexEntry *existing = index_find(index, path);
    if (existing) {
        existing->mode      = mode;
        existing->hash      = blob_id;
        existing->mtime_sec = (uint64_t)st.st_mtime;
        existing->size      = (uint32_t)st.st_size;
    } else {
        if (index->count >= MAX_INDEX_ENTRIES) {
            fprintf(stderr, "error: index is full\n");
            return -1;
        }
        IndexEntry *e   = &index->entries[index->count++];
        e->mode         = mode;
        e->hash         = blob_id;
        e->mtime_sec    = (uint64_t)st.st_mtime;
        e->size         = (uint32_t)st.st_size;
        strncpy(e->path, path, sizeof(e->path) - 1);
        e->path[sizeof(e->path) - 1] = '\0';
    }

    // 5. Persist
    return index_save(index);
}
