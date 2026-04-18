// object.c — Content-addressable object store

#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/evp.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

void hash_to_hex(const ObjectID *id, char *hex_out) {
    for (int i = 0; i < HASH_SIZE; i++)
        sprintf(hex_out + i * 2, "%02x", id->hash[i]);
    hex_out[HASH_HEX_SIZE] = '\0';
}

int hex_to_hash(const char *hex, ObjectID *id_out) {
    if (strlen(hex) < HASH_HEX_SIZE) return -1;
    for (int i = 0; i < HASH_SIZE; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
        id_out->hash[i] = (uint8_t)byte;
    }
    return 0;
}

void compute_hash(const void *data, size_t len, ObjectID *id_out) {
    unsigned int hash_len;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, id_out->hash, &hash_len);
    EVP_MD_CTX_free(ctx);
}

void object_path(const ObjectID *id, char *path_out, size_t path_size) {
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id, hex);
    snprintf(path_out, path_size, "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);
}

int object_exists(const ObjectID *id) {
    char path[512];
    object_path(id, path, sizeof(path));
    return access(path, F_OK) == 0;
}

// ─── IMPLEMENTED ─────────────────────────────────────────────────────────────

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    // 1. Determine type string
    const char *type_str;
    switch (type) {
        case OBJ_BLOB:   type_str = "blob";   break;
        case OBJ_TREE:   type_str = "tree";   break;
        case OBJ_COMMIT: type_str = "commit"; break;
        default: return -1;
    }

    // 2. Build full object: "<type> <size>\0<data>"
    char header[64];
    int header_len = snprintf(header, sizeof(header), "%s %zu", type_str, len);
    size_t full_len = (size_t)header_len + 1 + len;

    uint8_t *full_obj = malloc(full_len);
    if (!full_obj) return -1;
    memcpy(full_obj, header, (size_t)header_len);
    full_obj[header_len] = '\0';
    memcpy(full_obj + header_len + 1, data, len);

    // 3. Compute SHA-256 of the full object
    ObjectID id;
    compute_hash(full_obj, full_len, &id);

    // 4. Deduplication
    if (object_exists(&id)) {
        *id_out = id;
        free(full_obj);
        return 0;
    }

    // 5. Create shard directory
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(&id, hex);
    char shard_dir[256];
    snprintf(shard_dir, sizeof(shard_dir), "%s/%.2s", OBJECTS_DIR, hex);
    mkdir(shard_dir, 0755);

    // 6. Write to temp file then rename (atomic write)
    char final_path[512];
    object_path(&id, final_path, sizeof(final_path));
    char tmp_path[520];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", final_path);

    int fd = open(tmp_path, O_CREAT | O_WRONLY | O_TRUNC, 0444);
    if (fd < 0) { free(full_obj); return -1; }

    ssize_t written = write(fd, full_obj, full_len);
    free(full_obj);

    if (written < 0 || (size_t)written != full_len) {
        close(fd); unlink(tmp_path); return -1;
    }

    // 7. fsync temp file
    if (fsync(fd) != 0) { close(fd); unlink(tmp_path); return -1; }
    close(fd);

    // 8. Atomic rename
    if (rename(tmp_path, final_path) != 0) { unlink(tmp_path); return -1; }

    // 9. fsync shard directory
    int dir_fd = open(shard_dir, O_RDONLY);
    if (dir_fd >= 0) { fsync(dir_fd); close(dir_fd); }

    *id_out = id;
    return 0;
}

int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    // 1. Build file path and open
    char path[512];
    object_path(id, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size <= 0) { fclose(f); return -1; }

    uint8_t *buf = malloc((size_t)file_size);
    if (!buf) { fclose(f); return -1; }

    if (fread(buf, 1, (size_t)file_size, f) != (size_t)file_size) {
        fclose(f); free(buf); return -1;
    }
    fclose(f);

    // 2. Integrity check — recompute hash and compare to requested hash
    ObjectID computed;
    compute_hash(buf, (size_t)file_size, &computed);
    if (memcmp(computed.hash, id->hash, HASH_SIZE) != 0) {
        free(buf); return -1;
    }

    // 3. Find null separator between header and data
    uint8_t *null_ptr = memchr(buf, '\0', (size_t)file_size);
    if (!null_ptr) { free(buf); return -1; }

    // 4. Parse type and size from header
    char type_str[16] = {0};
    size_t data_size = 0;
    if (sscanf((char *)buf, "%15s %zu", type_str, &data_size) != 2) {
        free(buf); return -1;
    }

    // 5. Map type string → ObjectType
    if      (strcmp(type_str, "blob")   == 0) *type_out = OBJ_BLOB;
    else if (strcmp(type_str, "tree")   == 0) *type_out = OBJ_TREE;
    else if (strcmp(type_str, "commit") == 0) *type_out = OBJ_COMMIT;
    else { free(buf); return -1; }

    // 6. Extract data portion (after '\0')
    uint8_t *data_start = null_ptr + 1;
    size_t data_actual = (size_t)file_size - (size_t)(data_start - buf);

    if (data_actual != data_size) { free(buf); return -1; }

    // 7. Copy and return data
    void *out = malloc(data_actual + 1);
    if (!out) { free(buf); return -1; }
    memcpy(out, data_start, data_actual);
    ((char *)out)[data_actual] = '\0';
    free(buf);

    *data_out = out;
    *len_out  = data_actual;
    return 0;
}
