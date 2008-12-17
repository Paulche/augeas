/*
 * builtin.c: builtin primitives
 *
 * Copyright (C) 2007, 2008 Red Hat Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
 *
 * Author: David Lutterkort <dlutter@redhat.com>
 */

#include <config.h>
#include <stdio.h>
#include <stdarg.h>

#include "syntax.h"
#include "memory.h"

#define UNIMPL_BODY(name)                       \
    {                                           \
        FIXME(#name " called");                 \
        abort();                                \
    }

/*
 * Lenses
 */

/* V_REGEXP -> V_STRING -> V_LENS */
static struct value *lns_del(struct info *info,
                             struct value *rxp, struct value *dflt) {
    assert(rxp->tag == V_REGEXP);
    assert(dflt->tag == V_STRING);
    return lns_make_prim(L_DEL, ref(info),
                         ref(rxp->regexp), ref(dflt->string));
}

/* V_REGEXP -> V_LENS */
static struct value *lns_store(struct info *info, struct value *rxp) {
    assert(rxp->tag == V_REGEXP);
    return lns_make_prim(L_STORE, ref(info), ref(rxp->regexp), NULL);
}

/* V_REGEXP -> V_LENS */
static struct value *lns_key(struct info *info, struct value *rxp) {
    assert(rxp->tag == V_REGEXP);
    return lns_make_prim(L_KEY, ref(info), ref(rxp->regexp), NULL);
}

/* V_STRING -> V_LENS */
static struct value *lns_label(struct info *info, struct value *str) {
    assert(str->tag == V_STRING);
    return lns_make_prim(L_LABEL, ref(info), NULL, ref(str->string));
}

/* V_STRING -> V_LENS */
static struct value *lns_seq(struct info *info, struct value *str) {
    assert(str->tag == V_STRING);
    return lns_make_prim(L_SEQ, ref(info), NULL, ref(str->string));
}

/* V_STRING -> V_LENS */
static struct value *lns_counter(struct info *info, struct value *str) {
    assert(str->tag == V_STRING);
    return lns_make_prim(L_COUNTER, ref(info), NULL, ref(str->string));
}

static struct value *make_exn_lns_error(struct info *info,
                                        struct lns_error *err,
                                        const char *text) {
    struct value *v;

    v = make_exn_value(ref(info), "%s", err->message);
    if (err->lens != NULL) {
        char *s = format_info(err->lens->info);
        exn_printf_line(v, "Lens: %s", s);
        free(s);
    }
    if (err->pos >= 0) {
        char *pos = format_pos(text, err->pos);
        exn_printf_line(v,
                        "Error encountered here (%d characters into string)",
                        err->pos);
        if (pos != NULL)
            exn_printf_line(v, "%s", pos);
        free(pos);
    } else {
        exn_printf_line(v, "Error encountered at path %s", err->path);
    }

    return v;
}

static void exn_print_tree(struct value *exn, struct tree *tree) {
    struct memstream ms;

    init_memstream(&ms);
    print_tree(tree, ms.stream, "/*", 1);
    close_memstream(&ms);
    exn_printf_line(exn, "%s", ms.buf);
    FREE(ms.buf);
}

/* V_LENS -> V_STRING -> V_TREE */
static struct value *lens_get(struct info *info, struct value *l,
                              struct value *str) {
    assert(l->tag == V_LENS);
    assert(str->tag == V_STRING);
    struct lns_error *err;
    struct value *v;
    const char *text = str->string->str;

    struct tree *tree = lns_get(info, l->lens, text, &err);
    if (err == NULL) {
        v = make_value(V_TREE, ref(info));
        v->tree = tree;
    } else {
        v = make_exn_lns_error(info, err, text);
        if (tree != NULL) {
            exn_printf_line(v, "Tree generated so far:");
            exn_print_tree(v, tree);
            free_tree(tree);
        }
        free_lns_error(err);
    }
    return v;
}


/* V_LENS -> V_TREE -> V_STRING -> V_STRING */
static struct value *lens_put(struct info *info, struct value *l,
                              struct value *tree, struct value *str) {
    assert(l->tag == V_LENS);
    assert(tree->tag == V_TREE);
    assert(str->tag == V_STRING);

    struct memstream ms;
    struct value *v;
    struct lns_error *err;

    init_memstream(&ms);
    lns_put(ms.stream, l->lens, tree->tree, str->string->str, &err);
    close_memstream(&ms);

    if (err == NULL) {
        v = make_value(V_STRING, ref(info));
        v->string = make_string(ms.buf);
    } else {
        v = make_exn_lns_error(info, err, str->string->str);
        free_lns_error(err);
        FREE(ms.buf);
    }
    return v;
}

/* V_STRING -> V_STRING -> V_TREE -> V_TREE */
static struct value *tree_set_glue(struct info *info, struct value *path,
                                   struct value *val, struct value *tree) {
    // FIXME: This only works if TREE is not referenced more than once;
    // otherwise we'll have some pretty weird semantics, and would really
    // need to copy TREE first
    assert(path->tag == V_STRING);
    assert(val->tag == V_STRING);
    assert(tree->tag == V_TREE);

    struct tree *fake = NULL;

    if (tree->tree == NULL) {
        fake = make_tree(NULL, NULL, NULL, NULL);
        tree->tree = fake;
    }

    if (tree_set(tree->tree, path->string->str, val->string->str) == NULL) {
        return make_exn_value(ref(info),
                              "Tree set of %s to '%s' failed",
                              path->string->str, val->string->str);
    }
    if (fake != NULL) {
        list_remove(fake, tree->tree);
        free_tree(fake);
    }

    return ref(tree);
}

static struct value *tree_insert_glue(struct info *info, struct value *label,
                                      struct value *path, struct value *tree,
                                      int before) {
    // FIXME: This only works if TREE is not referenced more than once;
    // otherwise we'll have some pretty weird semantics, and would really
    // need to copy TREE first
    assert(label->tag == V_STRING);
    assert(path->tag == V_STRING);
    assert(tree->tag == V_TREE);

    int r;
    r = tree_insert(&(tree->tree), path->string->str,
                    label->string->str, before);
    if (r != 0) {
        return make_exn_value(ref(info),
                              "Tree insert of %s at %s failed",
                              label->string->str, path->string->str);
    }

    return ref(tree);
}

/* Insert after */
/* V_STRING -> V_STRING -> V_TREE -> V_TREE */
static struct value *tree_insa_glue(struct info *info, struct value *label,
                                    struct value *path, struct value *tree) {
    return tree_insert_glue(info, label, path, tree, 0);
}

/* Insert before */
/* V_STRING -> V_STRING -> V_TREE -> V_TREE */
static struct value *tree_insb_glue(struct info *info, struct value *label,
                                    struct value *path, struct value *tree) {
    return tree_insert_glue(info, label, path, tree, 1);
}

/* V_STRING -> V_TREE -> V_TREE */
static struct value *tree_rm_glue(struct info *info,
                                  struct value *path,
                                  struct value *tree) {
    // FIXME: This only works if TREE is not referenced more than once;
    // otherwise we'll have some pretty weird semantics, and would really
    // need to copy TREE first
    assert(path->tag == V_STRING);
    assert(tree->tag == V_TREE);
    if (tree_rm(&tree->tree, path->string->str) == -1) {
        return make_exn_value(ref(info), "Tree rm of %s failed",
                              path->string->str);
    }
    return ref(tree);
}

/* V_STRING -> V_STRING */
static struct value *gensym(struct info *info, struct value *prefix) {
    assert(prefix->tag == V_STRING);
    static unsigned int count = 0;
    struct value *v;
    char *s;
    int r;

    r = asprintf(&s, "%s%u", prefix->string->str, count);
    if (r == -1)
        return NULL;
    v = make_value(V_STRING, ref(info));
    v->string = make_string(s);
    return v;
}

/* V_STRING -> V_FILTER */
static struct value *xform_incl(struct info *info, struct value *s) {
    assert(s->tag == V_STRING);
    struct value *v = make_value(V_FILTER, ref(info));
    v->filter = make_filter(ref(s->string), 1);
    return v;
}

/* V_STRING -> V_FILTER */
static struct value *xform_excl(struct info *info, struct value *s) {
    assert(s->tag == V_STRING);
    struct value *v = make_value(V_FILTER, ref(info));
    v->filter = make_filter(ref(s->string), 0);
    return v;
}

/* V_LENS -> V_FILTER -> V_TRANSFORM */
static struct value *xform_transform(struct info *info, struct value *l,
                                     struct value *f) {
    assert(l->tag == V_LENS);
    assert(f->tag == V_FILTER);
    if (l->lens->value || l->lens->key) {
        return make_exn_value(ref(info), "Can not build a transform "
                              "from a lens that leaves a %s behind",
                              l->lens->key ? "key" : "value");
    }
    struct value *v = make_value(V_TRANSFORM, ref(info));
    v->transform = make_transform(ref(l->lens), ref(f->filter));
    return v;
}

struct module *builtin_init(void) {
    struct module *modl = module_create("Builtin");
    define_native(modl, "gensym", 1, gensym, T_STRING, T_STRING);
    /* Primitive lenses */
    define_native(modl, "del",     2, lns_del, T_REGEXP, T_STRING, T_LENS);
    define_native(modl, "store",   1, lns_store, T_REGEXP, T_LENS);
    define_native(modl, "key",     1, lns_key, T_REGEXP, T_LENS);
    define_native(modl, "label",   1, lns_label, T_STRING, T_LENS);
    define_native(modl, "seq",     1, lns_seq, T_STRING, T_LENS);
    define_native(modl, "counter", 1, lns_counter, T_STRING, T_LENS);
    /* Applying lenses (mostly for tests) */
    define_native(modl, "get",     2, lens_get, T_LENS, T_STRING, T_TREE);
    define_native(modl, "put",     3, lens_put, T_LENS, T_TREE, T_STRING,
                                               T_STRING);
    /* Tree manipulation used by the PUT tests */
    define_native(modl, "set", 3, tree_set_glue, T_STRING, T_STRING, T_TREE,
                                                 T_TREE);
    define_native(modl, "rm", 2, tree_rm_glue, T_STRING, T_TREE, T_TREE);
    define_native(modl, "insa", 3, tree_insa_glue, T_STRING, T_STRING, T_TREE,
                                                   T_TREE);
    define_native(modl, "insb", 3, tree_insb_glue, T_STRING, T_STRING, T_TREE,
                                                   T_TREE);
    /* Transforms and filters */
    define_native(modl, "incl", 1, xform_incl, T_STRING, T_FILTER);
    define_native(modl, "excl", 1, xform_excl, T_STRING, T_FILTER);
    define_native(modl, "transform", 2, xform_transform, T_LENS, T_FILTER,
                                                         T_TRANSFORM);
    return modl;
}

/*
 * Local variables:
 *  indent-tabs-mode: nil
 *  c-indent-level: 4
 *  c-basic-offset: 4
 *  tab-width: 4
 * End:
 */
