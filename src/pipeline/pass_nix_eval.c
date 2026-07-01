/*
 * pass_nix_eval.c — resolved Nix flake outputs via `nix flake show --json`.
 *
 * The ground-truth ceiling over pass_flakelock's syntactic floor: shells out to
 * `nix flake show --json` (the packaging guarantees `nix` on PATH) and records the
 * flake's real output surface — `FlakeOutput` nodes for each leaf
 * (packages.<system>.<name>, darwinConfigurations.<name>, overlays.<name>, …) with a
 * `PRODUCES` edge from the repo's `flake.nix` File node.
 *
 * First-class but failure-tolerant: runs only when the repo has a flake.nix, and any
 * eval failure / timeout / missing nix degrades to a clean no-op — the base index is
 * never affected. A hard `timeout` bounds pathological evaluations.
 */
#include "foundation/constants.h"
#include "pipeline/pipeline.h"
#include "pipeline/pipeline_internal.h"
#include "graph_buffer/graph_buffer.h"
#include "foundation/log.h"
#include "foundation/compat_fs.h"
#include "yyjson/yyjson.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Cap on emitted outputs — a backstop against a flake that re-exposes a huge tree
 * (e.g. legacyPackages); our own outputs are a handful, but cbm indexes any repo. */
enum { NIX_EVAL_MAX_OUTPUTS = 4000 };

/* Thread-local rotating buffers for int→string in log statements. */
static const char *itoa_buf(int v) {
    static _Thread_local char bufs[4][CBM_SZ_32];
    static _Thread_local int idx = 0;
    int i = idx;
    idx = (idx + 1) & 3;
    snprintf(bufs[i], sizeof(bufs[i]), "%d", v);
    return bufs[i];
}

/* Run `cmd`, capturing stdout into a malloc'd NUL-terminated buffer (caller frees).
 * Returns NULL on spawn failure or when the command produced no output. */
static char *run_capture(const char *cmd) {
    FILE *fp = cbm_popen(cmd, "r");
    if (!fp) {
        return NULL;
    }
    size_t cap = 65536, len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        cbm_pclose(fp);
        return NULL;
    }
    char tmp[8192];
    size_t r;
    while ((r = fread(tmp, 1, sizeof(tmp), fp)) > 0) {
        if (len + r + 1 > cap) {
            size_t ncap = (len + r + 1) * 2;
            char *nb = realloc(buf, ncap);
            if (!nb) {
                free(buf);
                cbm_pclose(fp);
                return NULL;
            }
            buf = nb;
            cap = ncap;
        }
        memcpy(buf + len, tmp, r);
        len += r;
    }
    buf[len] = '\0';
    cbm_pclose(fp);
    if (len == 0) {
        free(buf);
        return NULL;
    }
    return buf;
}

/* Emit a FlakeOutput leaf node at `path` plus a PRODUCES edge from `producer`. */
static void emit_output(cbm_pipeline_ctx_t *ctx, int64_t producer, const char *path,
                        yyjson_val *leaf) {
    yyjson_mut_doc *pdoc = yyjson_mut_doc_new(NULL);
    if (!pdoc) {
        return;
    }
    yyjson_mut_val *obj = yyjson_mut_obj(pdoc);
    yyjson_mut_doc_set_root(pdoc, obj);
    yyjson_val *type = yyjson_obj_get(leaf, "type");
    if (type && yyjson_is_str(type)) {
        yyjson_mut_obj_add_strcpy(pdoc, obj, "type", yyjson_get_str(type));
    }
    yyjson_val *nm = yyjson_obj_get(leaf, "name");
    if (nm && yyjson_is_str(nm)) {
        yyjson_mut_obj_add_strcpy(pdoc, obj, "drv_name", yyjson_get_str(nm));
    }
    yyjson_val *desc = yyjson_obj_get(leaf, "description");
    if (desc && yyjson_is_str(desc)) {
        yyjson_mut_obj_add_strcpy(pdoc, obj, "description", yyjson_get_str(desc));
    }
    char *props = yyjson_mut_write(pdoc, 0, NULL);
    yyjson_mut_doc_free(pdoc);

    char qn[CBM_PATH_MAX];
    snprintf(qn, sizeof(qn), "%s::flakeout::%s", ctx->project_name, path);
    int64_t id = cbm_gbuf_upsert_node(ctx->gbuf, "FlakeOutput", path, qn, "flake.nix", 0, 0,
                                      props ? props : "{}");
    free(props);
    if (producer && id) {
        cbm_gbuf_insert_edge(ctx->gbuf, producer, id, "PRODUCES", "{}");
    }
}

/* Recursively walk the `nix flake show --json` tree. A leaf is an object carrying a
 * string "type"; every other object is a branch whose keys extend the dotted path
 * (packages → aarch64-darwin → ccglass). Bounded by NIX_EVAL_MAX_OUTPUTS. */
static void walk_outputs(cbm_pipeline_ctx_t *ctx, int64_t producer, yyjson_val *val,
                         const char *path, int *count) {
    if (!val || !yyjson_is_obj(val) || *count >= NIX_EVAL_MAX_OUTPUTS) {
        return;
    }
    yyjson_val *type = yyjson_obj_get(val, "type");
    if (type && yyjson_is_str(type)) {
        emit_output(ctx, producer, path, val);
        (*count)++;
        return;
    }
    yyjson_obj_iter it;
    yyjson_obj_iter_init(val, &it);
    yyjson_val *k;
    while ((k = yyjson_obj_iter_next(&it)) && *count < NIX_EVAL_MAX_OUTPUTS) {
        const char *key = yyjson_get_str(k);
        yyjson_val *child = yyjson_obj_iter_get_val(k);
        if (!key || !child) {
            continue;
        }
        char newpath[CBM_PATH_MAX];
        if (path[0]) {
            snprintf(newpath, sizeof(newpath), "%s.%s", path, key);
        } else {
            snprintf(newpath, sizeof(newpath), "%s", key);
        }
        walk_outputs(ctx, producer, child, newpath, count);
    }
}

int cbm_pipeline_pass_nix_eval(cbm_pipeline_ctx_t *ctx) {
    if (!ctx || !ctx->repo_path || !ctx->gbuf || !ctx->project_name) {
        return 0;
    }
    char flake_path[CBM_PATH_MAX];
    snprintf(flake_path, sizeof(flake_path), "%s/flake.nix", ctx->repo_path);
    if (access(flake_path, F_OK) != 0) {
        return 0; /* not a flake — nothing to evaluate */
    }

    /* `timeout` (coreutils, on the wrapped PATH) bounds a pathological evaluation;
     * stderr is discarded so a failing eval just yields empty stdout → no-op. */
    char cmd[CBM_PATH_MAX + 160];
    snprintf(cmd, sizeof(cmd),
             "timeout 180 nix --extra-experimental-features 'nix-command flakes' "
             "flake show '%s' --json 2>/dev/null",
             ctx->repo_path);
    char *out = run_capture(cmd);
    if (!out) {
        return 0; /* nix missing / eval error / timeout: failure-tolerant no-op */
    }

    yyjson_doc *doc = yyjson_read(out, strlen(out), 0);
    free(out);
    if (!doc) {
        return 0;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!root || !yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        return 0;
    }

    /* Producer = the repo's flake.nix File node (if indexed). */
    int64_t producer = 0;
    char *fqn = cbm_pipeline_fqn_compute(ctx->project_name, "flake.nix", "__file__");
    if (fqn) {
        const cbm_gbuf_node_t *fn = cbm_gbuf_find_by_qn(ctx->gbuf, fqn);
        free(fqn);
        if (fn) {
            producer = fn->id;
        }
    }

    int count = 0;
    walk_outputs(ctx, producer, root, "", &count);
    yyjson_doc_free(doc);
    cbm_log_info("pass.done", "pass", "nix_eval", "outputs", itoa_buf(count));
    return 0;
}
