#ifndef HEP_CI_YAML_H
#define HEP_CI_YAML_H

#include <stddef.h>

/* minimal YAML subset parser
 * supports:
 *   key: value
 *   key:
 *     nested: value
 *   - list item
 *   - key: value  (list of maps)
 * no anchors, no multi-line blocks, no flow style
 */

#define YAML_MAX_DEPTH   16
#define YAML_MAX_NODES  512
#define YAML_MAX_STR    1024

typedef enum {
    YAML_NULL,
    YAML_SCALAR,
    YAML_MAP,
    YAML_SEQ,
} yaml_type;

typedef struct yaml_node yaml_node;
struct yaml_node {
    yaml_type   type;
    char        key[YAML_MAX_STR];   /* for map children */
    char        val[YAML_MAX_STR];   /* for scalars */
    yaml_node  *children;
    int         nchildren;
    int         children_cap;
};

typedef struct {
    yaml_node  root;
    char       error[256];
} yaml_doc;

int  yaml_parse(const char *text, yaml_doc *doc);
void yaml_free(yaml_doc *doc);

/* lookup helpers */
yaml_node *yaml_get(yaml_node *node, const char *key);
const char *yaml_str(yaml_node *node, const char *key, const char *def);

#endif
