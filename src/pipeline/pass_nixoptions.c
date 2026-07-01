/*
 * pass_nixoptions.c — NixOS/nix-darwin option dataflow: define ↔ set linking.
 *
 * A feature module DEFINES an option with `options.<path> = lib.mkEnableOption …;`
 * and a host/module SETS it with `<path> = <value>;`. In a Dendritic repo the set
 * is nested, e.g. `configurations.darwin.<host>.module = { kriswill.dnsmasq.enable
 * = true; }`. After the qualified-attrpath walk (extract_defs.c walk_nix_bindings),
 * defines surface as Variable nodes named `options.<path>` (the module-lambda
 * boundary resets the prefix, so the name keeps its `options.` head) and sets
 * surface as Variables whose qualified name equals `<path>` or ends with `.<path>`
 * (tolerating a wrapper prefix such as `configurations.darwin.k.module.` or a
 * `config.` block).
 *
 * This pre-dump pass emits a CONFIGURES edge from each set node to its define node
 * — in a Dendritic repo the highest-value cross-file edge, linking each host to
 * the feature modules it toggles. Modeled on strategy_key_symbols in
 * pass_configlink.c.
 *
 * Known limitation: `mkEnableOption` / `mkOption`-generated sub-options
 * (e.g. `programs.nh.enable`) are produced by a function call at eval time and are
 * invisible to static extraction. Only options written as explicit
 * `options.<path>` attrpaths are matched — this does not pretend full coverage.
 */
#include "foundation/constants.h"
#include "pipeline/pipeline.h"
#include "pipeline/pipeline_internal.h"
#include "graph_buffer/graph_buffer.h"
#include "foundation/log.h"
#include "foundation/str_util.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* CONFIGURES confidence: a cross-file set→define link (host sets, module defines)
 * is the high-value signal; a same-file match (a module setting its own option) is
 * weaker. */
#define CONF_NIXOPT_CROSS 0.90
#define CONF_NIXOPT_SAME 0.60

#define NIXOPT_OPTIONS_PREFIX "options."

/* Bounded scan cap for collected option defines. */
enum { NIXOPT_MAX_DEFINES = CBM_SZ_2K };

typedef struct {
    int64_t node_id;
    const char *file_path; /* borrowed from gbuf node */
    const char *key;       /* borrowed: node name past the "options." segment */
    size_t key_len;        /* strlen(key), precomputed for the O(sets×defines) scan */
} nixopt_define_t;

static bool nixopt_ends_with(const char *s, const char *suffix) {
    size_t ls = strlen(s);
    size_t lz = strlen(suffix);
    return ls >= lz && strcmp(s + ls - lz, suffix) == 0;
}

static bool nixopt_is_nix_file(const char *path) {
    return path && nixopt_ends_with(path, ".nix");
}

/* If `name` is an option define, return the option path after the `options.`
 * segment, else NULL. Handles both the lambda-reset form (`options.<path>`) and
 * a module written as a bare attrset with no `{…}:` head, whose walk keeps the
 * mount prefix (`flake.modules.nixos.foo.options.<path>`). The last `.options.`
 * segment wins so the returned key is the option path, not the mount path. */
static const char *nixopt_option_key(const char *name) {
    const size_t plen = sizeof(NIXOPT_OPTIONS_PREFIX) - 1; /* "options." */
    if (strncmp(name, NIXOPT_OPTIONS_PREFIX, plen) == 0) {
        return name + plen;
    }
    const char *key = NULL;
    const char *scan = name;
    const char *hit;
    while ((hit = strstr(scan, ".options.")) != NULL) {
        key = hit + (sizeof(".options.") - 1);
        scan = hit + 1;
    }
    return key;
}

/* A set Variable `name` (length `ln`) sets define `key` (length `lk`) when it
 * equals the key or ends with ".<key>" at a segment boundary — so
 * `configurations.darwin.k.module.kriswill.dnsmasq.enable` and
 * `config.kriswill.dnsmasq.enable` both set `kriswill.dnsmasq.enable`, and a
 * plain top-level set matches by equality. Lengths are passed in so the caller's
 * O(sets×defines) scan does not recompute strlen(name) per define. */
static bool nixopt_name_sets_key(const char *name, size_t ln, const char *key, size_t lk) {
    if (ln == lk) {
        return memcmp(name, key, lk) == 0;
    }
    return ln > lk + 1 && name[ln - lk - 1] == '.' && memcmp(name + ln - lk, key, lk) == 0;
}

int cbm_pipeline_pass_nixoptions(cbm_pipeline_ctx_t *ctx) {
    if (!ctx || !ctx->gbuf) {
        return 0;
    }
    cbm_gbuf_t *gb = ctx->gbuf;

    const cbm_gbuf_node_t **vars = NULL;
    int var_count = 0;
    if (cbm_gbuf_find_by_label(gb, "Variable", &vars, &var_count) != 0) {
        return 0;
    }

    /* Phase A — collect option defines: a .nix Variable whose (qualified) name
     * carries an `options.<path>` segment with a real dotted option path. */
    static nixopt_define_t defines[NIXOPT_MAX_DEFINES];
    int define_count = 0;
    for (int i = 0; i < var_count && define_count < NIXOPT_MAX_DEFINES; i++) {
        const char *name = vars[i]->name;
        if (!name || !nixopt_is_nix_file(vars[i]->file_path)) {
            continue;
        }
        const char *key = nixopt_option_key(name);
        if (!key || !key[0] || !strchr(key, '.')) {
            continue; /* not an option define, or not a real dotted path (≥2 segments) */
        }
        defines[define_count].node_id = vars[i]->id;
        defines[define_count].file_path = vars[i]->file_path;
        defines[define_count].key = key;
        defines[define_count].key_len = strlen(key);
        define_count++;
    }

    if (define_count == 0) {
        cbm_log_info("pass.done", "pass", "nixoptions", "defines", "0", "edges", "0");
        return 0;
    }

    /* Phase B — link each set candidate to the MOST SPECIFIC define(s) it sets.
     * Only the longest matching key wins, so a set (e.g. …services.nginx.enable)
     * is not also linked to every define whose key is a shorter trailing suffix
     * of its path (e.g. a separate options.nginx.enable). */
    int edge_count = 0;
    for (int vi = 0; vi < var_count; vi++) {
        const char *name = vars[vi]->name;
        if (!name || !nixopt_is_nix_file(vars[vi]->file_path)) {
            continue;
        }
        if (nixopt_option_key(name)) {
            continue; /* a define is never a set */
        }
        size_t ln = strlen(name);
        /* Pass 1: the length of the longest define key this set matches. */
        size_t best_len = 0;
        for (int di = 0; di < define_count; di++) {
            if (vars[vi]->id != defines[di].node_id &&
                defines[di].key_len > best_len &&
                nixopt_name_sets_key(name, ln, defines[di].key, defines[di].key_len)) {
                best_len = defines[di].key_len;
            }
        }
        if (best_len == 0) {
            continue;
        }
        /* Pass 2: emit to every define matching at that longest length (an option
         * declared in more than one file links from the set to each). */
        for (int di = 0; di < define_count; di++) {
            if (vars[vi]->id == defines[di].node_id || defines[di].key_len != best_len ||
                !nixopt_name_sets_key(name, ln, defines[di].key, defines[di].key_len)) {
                continue;
            }
            bool same_file = defines[di].file_path && vars[vi]->file_path &&
                             strcmp(defines[di].file_path, vars[vi]->file_path) == 0;
            double confidence = same_file ? CONF_NIXOPT_SAME : CONF_NIXOPT_CROSS;
            char keybuf[CBM_SZ_256];
            cbm_json_escape(keybuf, (int)sizeof(keybuf), defines[di].key);
            char props[CBM_SZ_512];
            snprintf(props, sizeof(props),
                     "{\"strategy\":\"nix_option_set\",\"confidence\":%.2f,\"config_key\":\"%s\"}",
                     confidence, keybuf);
            cbm_gbuf_insert_edge(gb, vars[vi]->id, defines[di].node_id, "CONFIGURES", props);
            edge_count++;
        }
    }

    char bufd[CBM_SZ_16];
    char bufe[CBM_SZ_16];
    snprintf(bufd, sizeof(bufd), "%d", define_count);
    snprintf(bufe, sizeof(bufe), "%d", edge_count);
    cbm_log_info("pass.done", "pass", "nixoptions", "defines", bufd, "edges", bufe);
    return edge_count;
}
