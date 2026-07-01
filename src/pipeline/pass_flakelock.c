/*
 * pass_flakelock.c — Nix flake topology from flake.lock (no Nix evaluation).
 *
 * flake.lock is plain JSON: the locked input DAG, with `follows` already resolved
 * into node references and `path:` sub-flake mounts recorded. We parse it with the
 * vendored yyjson (the same reader pass_compile_commands / pass_pkgmap use) and emit:
 *   - one `Flake` node per lock node (props copied from its `locked` entry),
 *   - `DEPENDS_ON`  for a direct input  (string value = another lock node key),
 *   - `FOLLOWS`     for a deduped input (array "follows" path, resolved from root),
 *   - `MOUNTS`      from a path-type (or the root) flake node to its in-repo
 *                   `flake.nix` File node, when that file is in the graph.
 * No-op when the repo has no readable flake.lock.
 */
#include "foundation/constants.h"
#include "pipeline/pipeline.h"
#include "pipeline/pipeline_internal.h"
#include "graph_buffer/graph_buffer.h"
#include "foundation/log.h"
#include "yyjson/yyjson.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Thread-local rotating buffers for int→string in log statements (mirrors the
 * helper in artifact.c; rotating lets several itoa_buf() share one log call). */
static const char *itoa_buf(int v) {
    static _Thread_local char bufs[4][CBM_SZ_32];
    static _Thread_local int idx = 0;
    int i = idx;
    idx = (idx + 1) & 3;
    snprintf(bufs[i], sizeof(bufs[i]), "%d", v);
    return bufs[i];
}

/* Synthetic stable QN for a flake (lock) node: "<project>::flake::<name>". */
static char *flake_qn(const char *project, const char *name) {
    size_t n = strlen(project) + strlen(name) + sizeof("::flake::");
    char *qn = malloc(n);
    if (qn) {
        snprintf(qn, n, "%s::flake::%s", project, name);
    }
    return qn;
}

/* Temp node ID of the Flake node for `name`, or 0 if absent. */
static int64_t flake_id(const cbm_pipeline_ctx_t *ctx, const char *name) {
    char *qn = flake_qn(ctx->project_name, name);
    if (!qn) {
        return 0;
    }
    const cbm_gbuf_node_t *n = cbm_gbuf_find_by_qn(ctx->gbuf, qn);
    free(qn);
    return n ? n->id : 0;
}

/* Resolve a follows-path array (e.g. ["nixpkgs"]) to a target node key by walking
 * from the lock root: cur=root_key; for each element, cur = nodes[cur].inputs[elem]
 * (a string node key, or recurse when that input is itself a follows array).
 * Returns a key borrowed from the doc (alive until yyjson_doc_free), or NULL. */
static const char *resolve_follows(yyjson_val *nodes, const char *root_key, yyjson_val *path) {
    const char *cur = root_key;
    yyjson_val *elem;
    yyjson_arr_iter it;
    yyjson_arr_iter_init(path, &it);
    while ((elem = yyjson_arr_iter_next(&it))) {
        const char *name = yyjson_get_str(elem);
        if (!name) {
            return NULL;
        }
        yyjson_val *cnode = yyjson_obj_get(nodes, cur);
        yyjson_val *inputs = cnode ? yyjson_obj_get(cnode, "inputs") : NULL;
        yyjson_val *v = inputs ? yyjson_obj_get(inputs, name) : NULL;
        if (!v) {
            return NULL;
        }
        if (yyjson_is_str(v)) {
            cur = yyjson_get_str(v);
        } else if (yyjson_is_arr(v)) {
            cur = resolve_follows(nodes, root_key, v);
            if (!cur) {
                return NULL;
            }
        } else {
            return NULL;
        }
    }
    return cur;
}

/* Build the Flake node's properties JSON from its `locked` entry + metadata.
 * Caller free()s the returned string. Returns NULL on allocation failure. */
static char *build_props(const char *name, bool is_root, yyjson_val *locked) {
    yyjson_mut_doc *pdoc = yyjson_mut_doc_new(NULL);
    if (!pdoc) {
        return NULL;
    }
    yyjson_mut_val *obj = yyjson_mut_obj(pdoc);
    yyjson_mut_doc_set_root(pdoc, obj);
    yyjson_mut_obj_add_bool(pdoc, obj, "is_root", is_root);
    yyjson_mut_obj_add_strcpy(pdoc, obj, "input_name", name);
    if (locked && yyjson_is_obj(locked)) {
        static const char *str_fields[] = {"type",    "owner", "repo", "rev", "ref",
                                           "narHash", "path",  "url",  NULL};
        for (int i = 0; str_fields[i]; i++) {
            yyjson_val *fv = yyjson_obj_get(locked, str_fields[i]);
            if (fv && yyjson_is_str(fv)) {
                yyjson_mut_obj_add_strcpy(pdoc, obj, str_fields[i], yyjson_get_str(fv));
            }
        }
        yyjson_val *lm = yyjson_obj_get(locked, "lastModified");
        if (lm && yyjson_is_int(lm)) {
            yyjson_mut_obj_add_sint(pdoc, obj, "lastModified", yyjson_get_sint(lm));
        }
    }
    char *json = yyjson_mut_write(pdoc, 0, NULL);
    yyjson_mut_doc_free(pdoc);
    return json;
}

/* Strip a leading "./" from a path:-type lock path. */
static const char *strip_dot_slash(const char *p) {
    if (p && p[0] == '.' && p[1] == '/') {
        return p + 2;
    }
    return p;
}

/* MOUNTS edge from the flake node `name` to its in-repo flake.nix File node, given
 * the sub-flake's relative dir (e.g. "flakes/ccglass"; NULL/"" for the repo root).
 * No-op if the File node is absent (e.g. flake.nix not indexed). Returns 1 if an
 * edge was emitted. */
static int emit_mount(cbm_pipeline_ctx_t *ctx, const char *name, const char *reldir) {
    char rel[CBM_PATH_MAX];
    if (reldir && reldir[0]) {
        snprintf(rel, sizeof(rel), "%s/flake.nix", reldir);
    } else {
        snprintf(rel, sizeof(rel), "flake.nix");
    }
    char *fqn = cbm_pipeline_fqn_compute(ctx->project_name, rel, "__file__");
    if (!fqn) {
        return 0;
    }
    const cbm_gbuf_node_t *file = cbm_gbuf_find_by_qn(ctx->gbuf, fqn);
    free(fqn);
    int64_t src = flake_id(ctx, name);
    if (!file || !src) {
        return 0;
    }
    cbm_gbuf_insert_edge(ctx->gbuf, src, file->id, "MOUNTS", "{}");
    return 1;
}

int cbm_pipeline_pass_flakelock(cbm_pipeline_ctx_t *ctx) {
    if (!ctx || !ctx->repo_path || !ctx->gbuf || !ctx->project_name) {
        return 0;
    }

    char lock_path[CBM_PATH_MAX];
    snprintf(lock_path, sizeof(lock_path), "%s/flake.lock", ctx->repo_path);

    yyjson_doc *doc = yyjson_read_file(lock_path, 0, NULL, NULL);
    if (!doc) {
        return 0; /* no flake.lock — not a flake repo, or unreadable: no-op */
    }

    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *nodes = root ? yyjson_obj_get(root, "nodes") : NULL;
    yyjson_val *rkv = root ? yyjson_obj_get(root, "root") : NULL;
    const char *root_key = (rkv && yyjson_is_str(rkv)) ? yyjson_get_str(rkv) : "root";
    if (!nodes || !yyjson_is_obj(nodes)) {
        yyjson_doc_free(doc);
        return 0;
    }

    int n_nodes = 0, n_dep = 0, n_follow = 0, n_mount = 0;
    yyjson_obj_iter it;
    yyjson_val *key;

    /* Phase A — one Flake node per lock node (so edge endpoints exist). */
    yyjson_obj_iter_init(nodes, &it);
    while ((key = yyjson_obj_iter_next(&it))) {
        const char *name = yyjson_get_str(key);
        yyjson_val *node = yyjson_obj_iter_get_val(key);
        if (!name || !node) {
            continue;
        }
        bool is_root = strcmp(name, root_key) == 0;
        yyjson_val *locked = yyjson_obj_get(node, "locked");
        char *props = build_props(name, is_root, locked);
        char *qn = flake_qn(ctx->project_name, name);
        if (qn) {
            cbm_gbuf_upsert_node(ctx->gbuf, "Flake", name, qn, "flake.lock", 0, 0,
                                 props ? props : "{}");
            free(qn);
            n_nodes++;
        }
        free(props);
    }

    /* Phase B — edges: DEPENDS_ON / FOLLOWS per input, MOUNTS per path/root node. */
    yyjson_obj_iter_init(nodes, &it);
    while ((key = yyjson_obj_iter_next(&it))) {
        const char *name = yyjson_get_str(key);
        yyjson_val *node = yyjson_obj_iter_get_val(key);
        if (!name || !node) {
            continue;
        }
        int64_t src_id = flake_id(ctx, name);

        yyjson_val *inputs = yyjson_obj_get(node, "inputs");
        if (src_id && inputs && yyjson_is_obj(inputs)) {
            yyjson_obj_iter iit;
            yyjson_obj_iter_init(inputs, &iit);
            yyjson_val *ik;
            while ((ik = yyjson_obj_iter_next(&iit))) {
                yyjson_val *iv = yyjson_obj_iter_get_val(ik);
                const char *target = NULL;
                const char *etype = NULL;
                if (yyjson_is_str(iv)) {
                    target = yyjson_get_str(iv);
                    etype = "DEPENDS_ON";
                } else if (yyjson_is_arr(iv)) {
                    target = resolve_follows(nodes, root_key, iv);
                    etype = "FOLLOWS";
                }
                if (target && etype) {
                    int64_t tgt_id = flake_id(ctx, target);
                    if (tgt_id) {
                        cbm_gbuf_insert_edge(ctx->gbuf, src_id, tgt_id, etype, "{}");
                        if (etype[0] == 'D') {
                            n_dep++;
                        } else {
                            n_follow++;
                        }
                    }
                }
            }
        }

        /* MOUNTS: path-type sub-flake → its flake.nix; the root → the repo flake.nix.
         * Anchored on the flake node itself, so the chain reads
         * root -DEPENDS_ON-> ccglass(Flake) -MOUNTS-> flakes/ccglass/flake.nix(File). */
        yyjson_val *locked = yyjson_obj_get(node, "locked");
        yyjson_val *type = locked ? yyjson_obj_get(locked, "type") : NULL;
        const char *tstr = (type && yyjson_is_str(type)) ? yyjson_get_str(type) : NULL;
        if (strcmp(name, root_key) == 0) {
            n_mount += emit_mount(ctx, name, NULL);
        } else if (tstr && strcmp(tstr, "path") == 0) {
            yyjson_val *pv = yyjson_obj_get(locked, "path");
            const char *p = (pv && yyjson_is_str(pv)) ? strip_dot_slash(yyjson_get_str(pv)) : NULL;
            if (p) {
                n_mount += emit_mount(ctx, name, p);
            }
        }
    }

    yyjson_doc_free(doc);
    cbm_log_info("pass.done", "pass", "flakelock", "flakes", itoa_buf(n_nodes), "edges",
                 itoa_buf(n_dep + n_follow + n_mount));
    return 0;
}
