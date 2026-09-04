#include "config.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    if (*s == 0) return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) *end-- = 0;
    return s;
}

static int current_path_index(const char *section) {
    if (strncmp(section, "path", 4) != 0) return -1;
    char *end = NULL;
    long idx = strtol(section + 4, &end, 10);
    if (end == section + 4 || idx < 0 || idx >= DP_MAX_PATHS) return -1;
    return (int)idx;
}

int dp_config_load(const char *path, dp_config_t *out) {
    FILE *f = fopen(path, "r");
    if (!f) {
        DP_LOGE("config: cannot open '%s'", path);
        return -1;
    }

    memset(out, 0, sizeof(*out));
    out->transport = DP_TRANSPORT_TCP;
    out->chunk_size = DP_DEFAULT_CHUNK_SIZE;
    out->window = DP_DEFAULT_SLOTS_PER_PATH;
    out->num_paths = 0;
    for (int i = 0; i < DP_MAX_PATHS; i++) out->paths[i].weight = 1.0;

    char line[512];
    char section[64] = "general";
    int max_path_seen = -1;

    while (fgets(line, sizeof(line), f)) {
        char *l = line;
        /* 去掉行内注释 (# 或 ; 开头的部分，但保留在字符串外的情况已经足够简单场景使用) */
        for (char *c = l; *c; c++) {
            if (*c == '#' || *c == ';') { *c = 0; break; }
        }
        l = trim(l);
        if (*l == 0) continue;

        if (l[0] == '[') {
            char *close = strchr(l, ']');
            if (!close) { DP_LOGE("config: malformed section line '%s'", line); fclose(f); return -1; }
            *close = 0;
            snprintf(section, sizeof(section), "%s", l + 1);
            continue;
        }

        char *eq = strchr(l, '=');
        if (!eq) { DP_LOGE("config: malformed line '%s'", line); continue; }
        *eq = 0;
        char *key = trim(l);
        char *val = trim(eq + 1);

        if (strcmp(section, "general") == 0) {
            if (strcmp(key, "transport") == 0) {
                if (strcmp(val, "rdma") == 0) out->transport = DP_TRANSPORT_RDMA;
                else out->transport = DP_TRANSPORT_TCP;
            } else if (strcmp(key, "chunk_size") == 0) {
                out->chunk_size = (uint32_t)strtoul(val, NULL, 10);
            } else if (strcmp(key, "window") == 0) {
                out->window = (uint32_t)strtoul(val, NULL, 10);
            }
            continue;
        }

        int idx = current_path_index(section);
        if (idx < 0) {
            DP_LOGE("config: unknown section [%s]", section);
            continue;
        }
        if (idx > max_path_seen) max_path_seen = idx;

        if (strcmp(key, "local_ip") == 0) {
            snprintf(out->paths[idx].local_ip, sizeof(out->paths[idx].local_ip), "%s", val);
        } else if (strcmp(key, "remote_ip") == 0) {
            snprintf(out->paths[idx].remote_ip, sizeof(out->paths[idx].remote_ip), "%s", val);
        } else if (strcmp(key, "port") == 0) {
            out->paths[idx].port = atoi(val);
        } else if (strcmp(key, "weight") == 0) {
            out->paths[idx].weight = atof(val);
        } else {
            DP_LOGW("config: unknown key '%s' in [%s]", key, section);
        }
    }
    fclose(f);

    out->num_paths = max_path_seen + 1;
    if (out->num_paths <= 0) {
        DP_LOGE("config: no [pathN] sections found in '%s'", path);
        return -1;
    }
    for (int i = 0; i < out->num_paths; i++) {
        if (out->paths[i].local_ip[0] == 0 || out->paths[i].remote_ip[0] == 0 || out->paths[i].port == 0) {
            DP_LOGE("config: path%d missing local_ip/remote_ip/port", i);
            return -1;
        }
    }
    return 0;
}
