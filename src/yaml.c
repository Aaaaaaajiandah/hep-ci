#include "yaml.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ── node helpers ────────────────────────────────────────────────────────── */

static void node_init(yaml_node *n, yaml_type t, const char *key) {
    memset(n, 0, sizeof(*n));
    n->type = t;
    if (key) strncpy(n->key, key, YAML_MAX_STR - 1);
}

static yaml_node *node_add_child(yaml_node *parent) {
    if (parent->nchildren >= parent->children_cap) {
        int new_cap = parent->children_cap ? parent->children_cap * 2 : 4;
        parent->children = realloc(parent->children,
                                   new_cap * sizeof(yaml_node));
        /* zero new slots */
        memset(parent->children + parent->children_cap, 0,
               (new_cap - parent->children_cap) * sizeof(yaml_node));
        parent->children_cap = new_cap;
    }
    yaml_node *child = &parent->children[parent->nchildren++];
    memset(child, 0, sizeof(*child));
    return child;
}

void yaml_free(yaml_doc *doc) {
    /* recursive free */
    yaml_node *stack[YAML_MAX_DEPTH * YAML_MAX_NODES];
    int top = 0;
    stack[top++] = &doc->root;
    while (top > 0) {
        yaml_node *n = stack[--top];
        for (int i = 0; i < n->nchildren; i++)
            stack[top++] = &n->children[i];
        if (n != &doc->root && n->children)
            free(n->children);
    }
    if (doc->root.children) {
        free(doc->root.children);
        doc->root.children = NULL;
    }
}

/* ── line parser ─────────────────────────────────────────────────────────── */

/* count leading spaces */
static int indent_of(const char *line) {
    int i = 0;
    while (line[i] == ' ') i++;
    return i;
}

/* strip trailing whitespace */
static void rstrip(char *s) {
    int n = (int)strlen(s);
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\n' ||
                     s[n-1] == '\r' || s[n-1] == '\t'))
        s[--n] = '\0';
}

/* strip surrounding quotes */
static void unquote(char *s) {
    int n = (int)strlen(s);
    if (n >= 2 && ((s[0] == '"' && s[n-1] == '"') ||
                   (s[0] == '\'' && s[n-1] == '\''))) {
        memmove(s, s+1, n-2);
        s[n-2] = '\0';
    }
}

/* split "key: value" into key and value parts */
static int split_kv(const char *line, char *key, char *val) {
    const char *colon = strchr(line, ':');
    if (!colon) return 0;
    int klen = (int)(colon - line);
    if (klen >= YAML_MAX_STR) klen = YAML_MAX_STR - 1;
    memcpy(key, line, klen); key[klen] = '\0';
    rstrip(key);

    const char *v = colon + 1;
    while (*v == ' ') v++;
    strncpy(val, v, YAML_MAX_STR - 1);
    rstrip(val);
    unquote(val);
    return 1;
}

/* ── parse ───────────────────────────────────────────────────────────────── */

int yaml_parse(const char *text, yaml_doc *doc) {
    memset(doc, 0, sizeof(*doc));
    node_init(&doc->root, YAML_MAP, NULL);

    /* split into lines */
    char *buf = strdup(text);
    char *lines[4096]; int nlines = 0;
    char *p = buf;
    while (*p && nlines < 4096) {
        lines[nlines++] = p;
        char *nl = strchr(p, '\n');
        if (nl) { *nl = '\0'; p = nl + 1; }
        else break;
    }

    /* parent stack: (node*, indent) */
    yaml_node *pstack[YAML_MAX_DEPTH];
    int        istack[YAML_MAX_DEPTH];
    int        depth = 0;
    pstack[0] = &doc->root;
    istack[0] = -1;

    for (int li = 0; li < nlines; li++) {
        char *line = lines[li];
        /* strip comments */
        char *hash = strchr(line, '#');
        /* only strip if not inside quotes (simple heuristic) */
        if (hash && (hash == line || *(hash-1) == ' '))
            *hash = '\0';
        rstrip(line);
        if (!line[0]) continue;

        int ind = indent_of(line);
        const char *content = line + ind;

        /* pop stack to find correct parent for this indent */
        while (depth > 0 && istack[depth] >= ind)
            depth--;

        yaml_node *parent = pstack[depth];

        if (content[0] == '-') {
            /* sequence item */
            if (parent->type != YAML_SEQ) parent->type = YAML_SEQ;

            const char *item = content + 1;
            while (*item == ' ') item++;

            yaml_node *child = node_add_child(parent);

            /* check if item has key: value */
            char key[YAML_MAX_STR], val[YAML_MAX_STR];
            if (strchr(item, ':') && split_kv(item, key, val)) {
                if (val[0]) {
                    /* inline scalar map entry */
                    node_init(child, YAML_MAP, NULL);
                    yaml_node *kv = node_add_child(child);
                    node_init(kv, YAML_SCALAR, key);
                    strncpy(kv->val, val, YAML_MAX_STR - 1);
                } else {
                    /* map entry with children to follow */
                    node_init(child, YAML_MAP, key);
                    pstack[++depth] = child;
                    istack[depth]   = ind + 1;
                }
            } else if (item[0]) {
                node_init(child, YAML_SCALAR, NULL);
                strncpy(child->val, item, YAML_MAX_STR - 1);
                unquote(child->val);
            } else {
                /* bare '-', children follow */
                node_init(child, YAML_MAP, NULL);
                pstack[++depth] = child;
                istack[depth]   = ind + 1;
            }
        } else {
            /* key: value or key: (map) */
            char key[YAML_MAX_STR], val[YAML_MAX_STR];
            if (!split_kv(content, key, val)) continue;

            yaml_node *child = node_add_child(parent);

            if (val[0]) {
                node_init(child, YAML_SCALAR, key);
                strncpy(child->val, val, YAML_MAX_STR - 1);
            } else {
                /* value is on next lines — could be map or seq */
                node_init(child, YAML_MAP, key);
                pstack[++depth] = child;
                istack[depth]   = ind + 1;
            }
        }
    }

    free(buf);
    return 0;
}

/* ── lookup ──────────────────────────────────────────────────────────────── */

yaml_node *yaml_get(yaml_node *node, const char *key) {
    if (!node) return NULL;
    for (int i = 0; i < node->nchildren; i++) {
        if (strcmp(node->children[i].key, key) == 0)
            return &node->children[i];
    }
    return NULL;
}

const char *yaml_str(yaml_node *node, const char *key, const char *def) {
    yaml_node *n = yaml_get(node, key);
    if (!n) return def;
    if (n->type == YAML_SCALAR) return n->val;
    return def;
}
