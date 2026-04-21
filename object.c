// object.c — Content-addressable object store

#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/evp.h>

// ─── PROVIDED ─────────────────────────────────────────

void hash_to_hex(const ObjectID *id, char *hex_out) {
    for (int i = 0; i < HASH_SIZE; i++) {
        sprintf(hex_out + i * 2, "%02x", id->hash[i]);
    }
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

// ─── IMPLEMENTATION ───────────────────────────────────

// Write object
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    const char *type_str = (type == OBJ_BLOB) ? "blob" :
                           (type == OBJ_TREE ? "tree" : "commit");

    // 1. Create header
    char header[64];
    int header_len = sprintf(header, "%s %zu", type_str, len) + 1;

    size_t full_len = header_len + len;

    uint8_t *full_obj = malloc(full_len);
    if (!full_obj) return -1;

    memcpy(full_obj, header, header_len);
    memcpy(full_obj + header_len, data, len);

    // 2. Compute hash
    compute_hash(full_obj, full_len, id_out);

    // 3. Deduplication
    if (object_exists(id_out)) {
        free(full_obj);
        return 0;
    }

    // 4. Create shard directory
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id_out, hex);

    char dir_path[512];
    snprintf(dir_path, sizeof(dir_path), "%s/%.2s", OBJECTS_DIR, hex);
    mkdir(dir_path, 0755);

    // 5. Final path
    char path[512];
    object_path(id_out, path, sizeof(path));

    // 6. Temp file
    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    int fd = open(tmp_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        free(full_obj);
        return -1;
    }

    // 7. Write
    if (write(fd, full_obj, full_len) != (ssize_t)full_len) {
        close(fd);
        free(full_obj);
        return -1;
    }

    fsync(fd);
    close(fd);

    // 8. Rename (atomic)
    if (rename(tmp_path, path) != 0) {
        free(full_obj);
        return -1;
    }

    free(full_obj);
    return 0;
}

// Read object
int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    char path[512];
    object_path(id, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    // File size
    fseek(f, 0, SEEK_END);
    size_t file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *full_data = malloc(file_size);
    if (!full_data) {
        fclose(f);
        return -1;
    }

    if (fread(full_data, 1, file_size, f) != file_size) {
        fclose(f);
        free(full_data);
        return -1;
    }
    fclose(f);

    // 4. Integrity check
    ObjectID actual_id;
    compute_hash(full_data, file_size, &actual_id);

    if (memcmp(id->hash, actual_id.hash, HASH_SIZE) != 0) {
        free(full_data);
        return -1;
    }

    // 5. Find header end
    void *null_pos = memchr(full_data, '\0', file_size);
    if (!null_pos) {
        free(full_data);
        return -1;
    }

    size_t header_len = (uint8_t *)null_pos - full_data + 1;
    char *header = (char *)full_data;

    // 6. Parse type
    if (strncmp(header, "blob", 4) == 0)
        *type_out = OBJ_BLOB;
    else if (strncmp(header, "tree", 4) == 0)
        *type_out = OBJ_TREE;
    else if (strncmp(header, "commit", 6) == 0)
        *type_out = OBJ_COMMIT;
    else {
        free(full_data);
        return -1;
    }

    // 7. Parse size
    size_t size;
    if (sscanf(header, "%*s %zu", &size) != 1) {
        free(full_data);
        return -1;
    }

    // 8. Extract data
    *data_out = malloc(size);
    if (!*data_out) {
        free(full_data);
        return -1;
    }

    memcpy(*data_out, full_data + header_len, size);
    *len_out = size;

    free(full_data);
    return 0;
}