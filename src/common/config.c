#include "config.h"
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DP_MAX_CHUNK_SIZE (64u * 1024u * 1024u)

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    if (*s == '\0') return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) *end-- = '\0';
    return s;
}

static int parse_u32(const char *value, uint32_t min, uint32_t max, uint32_t *out) {
    if (!value[0] || value[0] == '-') return -1;
    errno = 0;
    char *end = NULL;
    unsigned long parsed = strtoul(value, &end, 10);
    if (errno != 0 || *end != '\0' || parsed < min || parsed > max) return -1;
    *out = (uint32_t)parsed;
    return 0;
}

int dp_config_load(const char *path, dp_config_t *out) {
    FILE *file = fopen(path, "r");
    if (!file) {
        DP_LOGE("config: cannot open '%s'", path);
        return -1;
    }

    memset(out, 0, sizeof(*out));
    out->base_port = 18801;
    out->qp_count = 4;
    out->chunk_size = DP_DEFAULT_CHUNK_SIZE;
    out->window = DP_DEFAULT_SLOTS_PER_PATH;

    char line[512];
    char section[64] = "rdma";
    unsigned line_no = 0;
    const char *bad_key = NULL;
    const char *bad_value = NULL;
    while (fgets(line, sizeof(line), file)) {
        line_no++;
        for (char *c = line; *c; c++) {
            if (*c == '#' || *c == ';') {
                *c = '\0';
                break;
            }
        }
        char *entry = trim(line);
        if (*entry == '\0') continue;

        if (*entry == '[') {
            char *close = strchr(entry, ']');
            if (!close || trim(close + 1)[0] != '\0') {
                DP_LOGE("config:%u: malformed section", line_no);
                fclose(file);
                return -1;
            }
            *close = '\0';
            snprintf(section, sizeof(section), "%s", trim(entry + 1));
            if (strcmp(section, "rdma") != 0 && strcmp(section, "general") != 0) {
                DP_LOGE("config:%u: unsupported section [%s]", line_no, section);
                fclose(file);
                return -1;
            }
            continue;
        }

        char *equals = strchr(entry, '=');
        if (!equals) {
            DP_LOGE("config:%u: expected key=value", line_no);
            fclose(file);
            return -1;
        }
        *equals = '\0';
        char *key = trim(entry);
        char *value = trim(equals + 1);
        uint32_t parsed;

        if (strcmp(key, "local_ip") == 0) {
            snprintf(out->local_ip, sizeof(out->local_ip), "%s", value);
        } else if (strcmp(key, "remote_ip") == 0) {
            snprintf(out->remote_ip, sizeof(out->remote_ip), "%s", value);
        } else if (strcmp(key, "base_port") == 0) {
            if (parse_u32(value, 1, 65535, &parsed) != 0) {
                bad_key = key; bad_value = value; goto invalid_number;
            }
            out->base_port = (int)parsed;
        } else if (strcmp(key, "qp_count") == 0) {
            if (parse_u32(value, 1, DP_MAX_PATHS, &out->qp_count) != 0) {
                bad_key = key; bad_value = value; goto invalid_number;
            }
        } else if (strcmp(key, "chunk_size") == 0) {
            if (parse_u32(value, 4096, DP_MAX_CHUNK_SIZE, &out->chunk_size) != 0) {
                bad_key = key; bad_value = value; goto invalid_number;
            }
        } else if (strcmp(key, "window") == 0) {
            if (parse_u32(value, 1, 256, &out->window) != 0) {
                bad_key = key; bad_value = value; goto invalid_number;
            }
        } else {
            DP_LOGE("config:%u: unknown key '%s'", line_no, key);
            fclose(file);
            return -1;
        }
    }
    fclose(file);

    if (!out->local_ip[0] || !out->remote_ip[0]) {
        DP_LOGE("config: local_ip and remote_ip are required");
        return -1;
    }
    if ((uint32_t)out->base_port + out->qp_count - 1u > 65535u) {
        DP_LOGE("config: base_port + qp_count exceeds 65535");
        return -1;
    }
    return 0;

invalid_number:
    DP_LOGE("config:%u: invalid value '%s' for %s", line_no, bad_value, bad_key);
    fclose(file);
    return -1;
}
