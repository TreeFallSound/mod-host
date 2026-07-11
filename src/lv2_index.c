/*
 * This file is part of mod-host.
 *
 * mod-host is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * mod-host is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with mod-host.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "lv2_index.h"

#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(HAVE_NEW_LILV) && defined(HAVE_SERD)

#include <dirent.h>
#include <serd/serd.h>
#include <sys/stat.h>
#include <unistd.h>

#define NO_INDEX UINT_MAX

#define URI_RDF_TYPE     "http://www.w3.org/1999/02/22-rdf-syntax-ns#type"
#define URI_OWL_ONTOLOGY "http://www.w3.org/2002/07/owl#Ontology"
#define URI_LV2_PLUGIN   "http://lv2plug.in/ns/lv2core#Plugin"
#define URI_LV2_SPEC     "http://lv2plug.in/ns/lv2core#Specification"
#define URI_LV2_APPLIESTO "http://lv2plug.in/ns/lv2core#appliesTo"
#define URI_DYN_MANIFEST "http://lv2plug.in/ns/ext/dynmanifest#DynManifest"

/* lilv's default when LV2_PATH is unset; keep in sync with lilv_config.h */
#ifdef __APPLE__
#define DEFAULT_LV2_PATH "~/.lv2:~/Library/Audio/Plug-Ins/LV2:/usr/local/lib/lv2:" \
                         "/usr/lib/lv2:/Library/Audio/Plug-Ins/LV2"
#else
#define DEFAULT_LV2_PATH "~/.lv2:/usr/local/lib/lv2:/usr/lib/lv2"
#endif

/* ---------------------------------------------------------------- */
/* string -> index map (open addressing; keys are borrowed, not owned) */

typedef struct {
    const char **keys;
    unsigned *vals;
    size_t cap;
    size_t len;
} map_t;

static size_t str_hash(const char *str)
{
    size_t h = 2166136261u;
    for (; *str; str++)
        h = (h ^ (unsigned char)*str) * 16777619u;
    return h;
}

static bool map_grow(map_t *map, size_t cap)
{
    const char **keys = (const char **) calloc(cap, sizeof(char *));
    unsigned *vals = (unsigned *) calloc(cap, sizeof(unsigned));

    if (keys == NULL || vals == NULL)
    {
        free(keys);
        free(vals);
        return false;
    }

    for (size_t i = 0; i < map->cap; i++)
    {
        if (map->keys[i] == NULL)
            continue;

        size_t j = str_hash(map->keys[i]) & (cap - 1);
        while (keys[j] != NULL)
            j = (j + 1) & (cap - 1);

        keys[j] = map->keys[i];
        vals[j] = map->vals[i];
    }

    free(map->keys);
    free(map->vals);
    map->keys = keys;
    map->vals = vals;
    map->cap = cap;
    return true;
}

static bool map_put(map_t *map, const char *key, unsigned val)
{
    if (map->len * 2 >= map->cap && !map_grow(map, map->cap ? map->cap * 2 : 256))
        return false;

    size_t i = str_hash(key) & (map->cap - 1);
    while (map->keys[i] != NULL)
    {
        if (strcmp(map->keys[i], key) == 0)
        {
            map->vals[i] = val;
            return true;
        }
        i = (i + 1) & (map->cap - 1);
    }

    map->keys[i] = key;
    map->vals[i] = val;
    map->len++;
    return true;
}

static unsigned map_get(const map_t *map, const char *key)
{
    if (map->cap == 0)
        return NO_INDEX;

    size_t i = str_hash(key) & (map->cap - 1);
    while (map->keys[i] != NULL)
    {
        if (strcmp(map->keys[i], key) == 0)
            return map->vals[i];
        i = (i + 1) & (map->cap - 1);
    }

    return NO_INDEX;
}

static void map_free(map_t *map)
{
    free(map->keys);
    free(map->vals);
    memset(map, 0, sizeof(*map));
}

/* ---------------------------------------------------------------- */

typedef struct {
    char *path;      /* absolute, with trailing separator */
    bool eager;      /* declares a specification/ontology, or a dyn-manifest */
} bundle_t;

typedef struct {
    char *uri;
    unsigned bundle;      /* declaring bundle, or NO_INDEX if not seen yet */
    unsigned *extra;      /* bundles contributing presets etc. via appliesTo */
    unsigned n_extra;
    unsigned cap_extra;
    char **resolved;      /* NULL-terminated view of bundle+extra, built lazily */
    bool loaded;
} plugin_t;

typedef struct {
    char *uri;
    unsigned bundle;
} resource_t;

struct lv2_index_t {
    bundle_t *bundles;
    unsigned n_bundles, cap_bundles;

    plugin_t *plugins;
    unsigned n_plugins, cap_plugins;
    map_t plugin_map;

    resource_t *resources;
    unsigned n_resources, cap_resources;
    map_t resource_map;

    char **eager;         /* NULL-terminated, built on demand */
};

static bool grow(void **items, unsigned *cap, size_t size)
{
    const unsigned new_cap = *cap ? *cap * 2 : 64;
    void *new_items = realloc(*items, new_cap * size);

    if (new_items == NULL)
        return false;

    *items = new_items;
    *cap = new_cap;
    return true;
}

/* Returns the bundle's index, adding it if new. */
static unsigned bundle_intern(lv2_index_t *idx, const char *path)
{
    for (unsigned i = 0; i < idx->n_bundles; i++)
    {
        if (strcmp(idx->bundles[i].path, path) == 0)
            return i;
    }

    if (idx->n_bundles == idx->cap_bundles &&
        !grow((void **)&idx->bundles, &idx->cap_bundles, sizeof(bundle_t)))
        return NO_INDEX;

    char *dup = strdup(path);
    if (dup == NULL)
        return NO_INDEX;

    idx->bundles[idx->n_bundles].path = dup;
    idx->bundles[idx->n_bundles].eager = false;
    return idx->n_bundles++;
}

static unsigned plugin_intern(lv2_index_t *idx, const char *uri)
{
    const unsigned found = map_get(&idx->plugin_map, uri);
    if (found != NO_INDEX)
        return found;

    if (idx->n_plugins == idx->cap_plugins &&
        !grow((void **)&idx->plugins, &idx->cap_plugins, sizeof(plugin_t)))
        return NO_INDEX;

    char *dup = strdup(uri);
    if (dup == NULL)
        return NO_INDEX;

    plugin_t *plugin = &idx->plugins[idx->n_plugins];
    memset(plugin, 0, sizeof(*plugin));
    plugin->uri = dup;
    plugin->bundle = NO_INDEX;

    if (!map_put(&idx->plugin_map, dup, idx->n_plugins))
    {
        free(dup);
        return NO_INDEX;
    }

    return idx->n_plugins++;
}

static void plugin_add_extra(plugin_t *plugin, unsigned bundle)
{
    for (unsigned i = 0; i < plugin->n_extra; i++)
    {
        if (plugin->extra[i] == bundle)
            return;
    }

    if (plugin->n_extra == plugin->cap_extra &&
        !grow((void **)&plugin->extra, &plugin->cap_extra, sizeof(unsigned)))
        return;

    plugin->extra[plugin->n_extra++] = bundle;

    free(plugin->resolved);
    plugin->resolved = NULL;
}

static void resource_put(lv2_index_t *idx, const char *uri, unsigned bundle)
{
    const unsigned found = map_get(&idx->resource_map, uri);
    if (found != NO_INDEX)
    {
        idx->resources[found].bundle = bundle;
        return;
    }

    if (idx->n_resources == idx->cap_resources &&
        !grow((void **)&idx->resources, &idx->cap_resources, sizeof(resource_t)))
        return;

    char *dup = strdup(uri);
    if (dup == NULL)
        return;

    if (!map_put(&idx->resource_map, dup, idx->n_resources))
    {
        free(dup);
        return;
    }

    idx->resources[idx->n_resources].uri = dup;
    idx->resources[idx->n_resources].bundle = bundle;
    idx->n_resources++;
}

/* ---------------------------------------------------------------- */
/* manifest skim */

typedef struct {
    lv2_index_t *idx;
    unsigned bundle;
    SerdEnv *env;
    bool warned;
    const char *path;
} skim_t;

static SerdStatus on_base(void *handle, const SerdNode *uri)
{
    skim_t *skim = (skim_t *) handle;
    return serd_env_set_base_uri(skim->env, uri);
}

static SerdStatus on_prefix(void *handle, const SerdNode *name, const SerdNode *uri)
{
    skim_t *skim = (skim_t *) handle;
    return serd_env_set_prefix(skim->env, name, uri);
}

static SerdStatus on_error(void *handle, const SerdError *error)
{
    skim_t *skim = (skim_t *) handle;

    /* One line per bundle: a hand-maintained plugin tree can have a lot of
       broken TTL in it, and we must never turn that into a wall of output. */
    if (!skim->warned)
    {
        skim->warned = true;
        fprintf(stderr, "mod-host: malformed manifest, skipping what we cannot read: %s (line %u)\n",
                        skim->path, error->line);
    }

    return SERD_SUCCESS;
}

/* Expands CURIEs and relative URIs. The returned pointer is either borrowed
   from `node` or owned by `expanded`, which the caller must free. */
static const char *expand(const skim_t *skim, const SerdNode *node, SerdNode *expanded)
{
    *expanded = serd_env_expand_node(skim->env, node);

    if (expanded->buf != NULL)
        return (const char *) expanded->buf;

    return (const char *) node->buf;
}

static SerdStatus on_statement(void *handle, SerdStatementFlags flags, const SerdNode *graph,
                               const SerdNode *subject, const SerdNode *predicate,
                               const SerdNode *object, const SerdNode *object_datatype,
                               const SerdNode *object_lang)
{
    skim_t *skim = (skim_t *) handle;
    lv2_index_t *idx = skim->idx;

    (void)flags;
    (void)graph;
    (void)object_datatype;
    (void)object_lang;

    if (subject->type == SERD_BLANK || object->type == SERD_BLANK)
        return SERD_SUCCESS;

    SerdNode pred_node, subj_node, obj_node;
    const char *pred = expand(skim, predicate, &pred_node);
    const char *subj = expand(skim, subject, &subj_node);
    const char *obj = expand(skim, object, &obj_node);

    if (strcmp(pred, URI_RDF_TYPE) == 0)
    {
        if (strcmp(obj, URI_LV2_PLUGIN) == 0)
        {
            const unsigned p = plugin_intern(idx, subj);

            /* First bundle in LV2_PATH order wins, as in lilv. Unlike lilv we
               do not compare lv2:minorVersion/microVersion, which would mean
               parsing the data files we are trying to avoid touching. */
            if (p != NO_INDEX && idx->plugins[p].bundle == NO_INDEX)
            {
                idx->plugins[p].bundle = skim->bundle;
                free(idx->plugins[p].resolved);
                idx->plugins[p].resolved = NULL;
            }
        }
        else if (strcmp(obj, URI_LV2_SPEC) == 0 ||
                 strcmp(obj, URI_OWL_ONTOLOGY) == 0 ||
                 strcmp(obj, URI_DYN_MANIFEST) == 0)
        {
            idx->bundles[skim->bundle].eager = true;
        }
    }
    else if (strcmp(pred, URI_LV2_APPLIESTO) == 0)
    {
        /* Any subject that appliesTo a plugin means this bundle carries
           triples about it -- presets, but also modgui and state. We do not
           insist on pset:Preset: loading the bundle is right either way, and
           this way we do not depend on statement order within the manifest. */
        const unsigned p = plugin_intern(idx, obj);
        if (p != NO_INDEX)
            plugin_add_extra(&idx->plugins[p], skim->bundle);

        resource_put(idx, subj, skim->bundle);
    }

    serd_node_free(&pred_node);
    serd_node_free(&subj_node);
    serd_node_free(&obj_node);
    return SERD_SUCCESS;
}

/* Skims one bundle's manifest.ttl into the index. This is the unit an on-disk
   cache would memoise, keyed on the manifest's path/mtime/size. */
static void scan_bundle(lv2_index_t *idx, const char *bundle_path)
{
    const unsigned bundle = bundle_intern(idx, bundle_path);
    if (bundle == NO_INDEX)
        return;

    char manifest[PATH_MAX];
    if (snprintf(manifest, sizeof(manifest), "%smanifest.ttl", bundle_path) >= (int)sizeof(manifest))
        return;

    SerdNode base = serd_node_new_file_uri((const uint8_t *) manifest, NULL, NULL, true);
    if (base.buf == NULL)
        return;

    skim_t skim = { idx, bundle, NULL, false, manifest };
    skim.env = serd_env_new(&base);

    if (skim.env != NULL)
    {
        SerdReader *reader = serd_reader_new(SERD_TURTLE, &skim, NULL,
                                             on_base, on_prefix, on_statement, NULL);

        if (reader != NULL)
        {
            serd_reader_set_strict(reader, false);
            serd_reader_set_error_sink(reader, on_error, &skim);

            /* A read error still leaves the statements seen before it in the
               index; a half-readable bundle is better than none. */
            serd_reader_read_file(reader, (const uint8_t *) manifest);
            serd_reader_free(reader);
        }

        serd_env_free(skim.env);
    }

    serd_node_free(&base);
}

/* ---------------------------------------------------------------- */
/* LV2_PATH traversal */

static char *expand_home(const char *path)
{
    if (path[0] != '~' || (path[1] != '/' && path[1] != '\0'))
        return strdup(path);

    const char *home = getenv("HOME");
    if (home == NULL)
        return strdup(path);

    const size_t len = strlen(home) + strlen(path + 1) + 1;
    char *expanded = (char *) malloc(len);

    if (expanded != NULL)
        snprintf(expanded, len, "%s%s", home, path + 1);

    return expanded;
}

/* Each LV2_PATH entry is a parent directory; its immediate subdirectories are
   bundles. A bundle is anything holding a manifest.ttl -- lilv does not
   require the .lv2 suffix, so neither do we. */
static void scan_dir(lv2_index_t *idx, const char *dirname)
{
    DIR *dir = opendir(dirname);
    if (dir == NULL)
        return;

    const struct dirent *entry;
    while ((entry = readdir(dir)) != NULL)
    {
        if (entry->d_name[0] == '.')
            continue;

        char path[PATH_MAX], real[PATH_MAX];
        if (snprintf(path, sizeof(path), "%s/%s/manifest.ttl", dirname, entry->d_name) >= (int)sizeof(path))
            continue;

        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
            continue;

        /* Canonicalise so the same bundle reached by two paths (or a symlink,
           as mod-ui makes) is scanned once, and so the path we later hand to
           lilv_new_file_uri is absolute. */
        if (snprintf(path, sizeof(path), "%s/%s", dirname, entry->d_name) >= (int)sizeof(path))
            continue;

        if (realpath(path, real) == NULL)
            continue;

        const size_t len = strlen(real);
        if (len + 2 > sizeof(real))
            continue;

        real[len] = '/';
        real[len + 1] = '\0';

        scan_bundle(idx, real);
    }

    closedir(dir);
}

/* ---------------------------------------------------------------- */

lv2_index_t *lv2_index_build(void)
{
    if (getenv("MOD_HOST_NO_LV2_INDEX") != NULL)
        return NULL;

    lv2_index_t *idx = (lv2_index_t *) calloc(1, sizeof(lv2_index_t));
    if (idx == NULL)
        return NULL;

    const char *lv2_path = getenv("LV2_PATH");
    if (lv2_path == NULL || lv2_path[0] == '\0')
        lv2_path = DEFAULT_LV2_PATH;

    char *paths = strdup(lv2_path);
    if (paths == NULL)
    {
        free(idx);
        return NULL;
    }

    /* Order matters: it is what makes "first bundle wins" match lilv. */
    for (char *saveptr = NULL, *entry = strtok_r(paths, ":", &saveptr);
         entry != NULL;
         entry = strtok_r(NULL, ":", &saveptr))
    {
        char *dirname = expand_home(entry);

        if (dirname != NULL)
        {
            scan_dir(idx, dirname);
            free(dirname);
        }
    }

    free(paths);

    /* If we read a pile of manifests and got nothing whatsoever out of them --
       no plugins, no presets, not even a specification -- the skim is broken
       (a serd API change, say) rather than the tree being empty. Give up and
       let the caller load the whole world: slow beats dead.

       Note the test cannot be "no plugins": a tree holding only specification
       bundles, which is exactly what /usr/lib/lv2 is on some systems, has no
       plugins in it and is perfectly valid. */
    unsigned bundles, plugins, resources, eager;
    lv2_index_stats(idx, &bundles, &plugins, &resources, &eager);

    if (bundles > 0 && plugins == 0 && resources == 0 && eager == 0)
    {
        fprintf(stderr, "mod-host: LV2 index read %u bundles but understood none of them, "
                        "falling back to a full world load\n", bundles);
        lv2_index_free(idx);
        return NULL;
    }

    return idx;
}

void lv2_index_free(lv2_index_t *idx)
{
    if (idx == NULL)
        return;

    for (unsigned i = 0; i < idx->n_bundles; i++)
        free(idx->bundles[i].path);

    for (unsigned i = 0; i < idx->n_plugins; i++)
    {
        free(idx->plugins[i].uri);
        free(idx->plugins[i].extra);
        free(idx->plugins[i].resolved);
    }

    for (unsigned i = 0; i < idx->n_resources; i++)
        free(idx->resources[i].uri);

    map_free(&idx->plugin_map);
    map_free(&idx->resource_map);

    free(idx->bundles);
    free(idx->plugins);
    free(idx->resources);
    free(idx->eager);
    free(idx);
}

const char *const *lv2_index_eager_bundles(const lv2_index_t *idx)
{
    if (idx == NULL)
        return NULL;

    lv2_index_t *self = (lv2_index_t *) idx;

    if (self->eager == NULL)
    {
        unsigned count = 0;
        for (unsigned i = 0; i < self->n_bundles; i++)
        {
            if (self->bundles[i].eager)
                count++;
        }

        self->eager = (char **) calloc(count + 1, sizeof(char *));
        if (self->eager == NULL)
            return NULL;

        unsigned j = 0;
        for (unsigned i = 0; i < self->n_bundles; i++)
        {
            if (self->bundles[i].eager)
                self->eager[j++] = self->bundles[i].path;
        }
    }

    return (const char *const *) self->eager;
}

const char *const *lv2_index_bundles_for_plugin(const lv2_index_t *idx, const char *plugin_uri)
{
    if (idx == NULL || plugin_uri == NULL)
        return NULL;

    lv2_index_t *self = (lv2_index_t *) idx;

    const unsigned p = map_get(&self->plugin_map, plugin_uri);
    if (p == NO_INDEX)
        return NULL;

    plugin_t *plugin = &self->plugins[p];

    /* We know of presets for it but never saw it declared: as far as we are
       concerned the plugin does not exist. */
    if (plugin->bundle == NO_INDEX || plugin->loaded)
        return NULL;

    if (plugin->resolved == NULL)
    {
        plugin->resolved = (char **) calloc(plugin->n_extra + 2, sizeof(char *));
        if (plugin->resolved == NULL)
            return NULL;

        unsigned j = 0;
        plugin->resolved[j++] = self->bundles[plugin->bundle].path;

        for (unsigned i = 0; i < plugin->n_extra; i++)
        {
            if (plugin->extra[i] != plugin->bundle)
                plugin->resolved[j++] = self->bundles[plugin->extra[i]].path;
        }
    }

    return (const char *const *) plugin->resolved;
}

void lv2_index_mark_plugin_loaded(lv2_index_t *idx, const char *plugin_uri)
{
    if (idx == NULL || plugin_uri == NULL)
        return;

    const unsigned p = map_get(&idx->plugin_map, plugin_uri);
    if (p != NO_INDEX)
        idx->plugins[p].loaded = true;
}

const char *lv2_index_bundle_for_resource(const lv2_index_t *idx, const char *resource_uri)
{
    if (idx == NULL || resource_uri == NULL)
        return NULL;

    const unsigned r = map_get(&idx->resource_map, resource_uri);
    if (r == NO_INDEX)
        return NULL;

    return idx->bundles[idx->resources[r].bundle].path;
}

void lv2_index_add_bundle(lv2_index_t *idx, const char *bundle_path)
{
    if (idx == NULL || bundle_path == NULL)
        return;

    char real[PATH_MAX];
    if (realpath(bundle_path, real) == NULL)
        return;

    const size_t len = strlen(real);
    if (len + 2 > sizeof(real))
        return;

    real[len] = '/';
    real[len + 1] = '\0';

    scan_bundle(idx, real);

    /* The world already has it -- this call follows a lilv_world_load_bundle --
       so do not make effects_add load it a second time. */
    const unsigned bundle = bundle_intern(idx, real);
    for (unsigned i = 0; i < idx->n_plugins; i++)
    {
        if (idx->plugins[i].bundle == bundle)
            idx->plugins[i].loaded = true;
    }

    free(idx->eager);
    idx->eager = NULL;
}

void lv2_index_remove_bundle(lv2_index_t *idx, const char *bundle_path)
{
    if (idx == NULL || bundle_path == NULL)
        return;

    char real[PATH_MAX];
    if (realpath(bundle_path, real) == NULL)
    {
        /* Already gone from disk: fall back to matching the string as given. */
        if (snprintf(real, sizeof(real), "%s", bundle_path) >= (int)sizeof(real))
            return;
    }

    size_t len = strlen(real);
    if (len > 0 && real[len - 1] == '/')
        real[--len] = '\0';

    if (len + 2 > sizeof(real))
        return;

    real[len] = '/';
    real[len + 1] = '\0';

    unsigned bundle = NO_INDEX;
    for (unsigned i = 0; i < idx->n_bundles; i++)
    {
        if (strcmp(idx->bundles[i].path, real) == 0)
        {
            bundle = i;
            break;
        }
    }

    if (bundle == NO_INDEX)
        return;

    /* Bundle indices stay stable: we blank the entry rather than compact the
       array, so every plugin/resource reference remains valid. */
    for (unsigned i = 0; i < idx->n_plugins; i++)
    {
        plugin_t *plugin = &idx->plugins[i];

        for (unsigned j = 0; j < plugin->n_extra; j++)
        {
            if (plugin->extra[j] == bundle)
            {
                plugin->extra[j] = plugin->extra[--plugin->n_extra];
                break;
            }
        }

        free(plugin->resolved);
        plugin->resolved = NULL;

        if (plugin->bundle == bundle)
        {
            free(plugin->uri);
            plugin->uri = NULL;
            plugin->bundle = NO_INDEX;
            plugin->loaded = false;
        }
    }

    for (unsigned i = 0; i < idx->n_resources; i++)
    {
        if (idx->resources[i].bundle == bundle)
        {
            free(idx->resources[i].uri);
            idx->resources[i].uri = NULL;
        }
    }

    idx->bundles[bundle].eager = false;

    /* The maps borrow the uri pointers we just freed, so rebuild both from the
       surviving entries. Cheap, and only on an explicit bundle removal. */
    map_free(&idx->plugin_map);
    for (unsigned i = 0; i < idx->n_plugins; i++)
    {
        if (idx->plugins[i].uri != NULL)
            map_put(&idx->plugin_map, idx->plugins[i].uri, i);
    }

    map_free(&idx->resource_map);
    for (unsigned i = 0; i < idx->n_resources; i++)
    {
        if (idx->resources[i].uri != NULL)
            map_put(&idx->resource_map, idx->resources[i].uri, i);
    }

    free(idx->eager);
    idx->eager = NULL;
}

void lv2_index_stats(const lv2_index_t *idx, unsigned *bundles, unsigned *plugins,
                     unsigned *resources, unsigned *eager)
{
    *bundles = *plugins = *resources = *eager = 0;

    if (idx == NULL)
        return;

    *bundles = idx->n_bundles;
    *plugins = idx->n_plugins;
    *resources = idx->n_resources;

    for (unsigned i = 0; i < idx->n_bundles; i++)
    {
        if (idx->bundles[i].eager)
            (*eager)++;
    }
}

#else // HAVE_NEW_LILV && HAVE_SERD

/* Without bundle loading (lilv < 0.22) or without serd there is nothing to
   index against; effects.c falls back to lilv_world_load_all. */

lv2_index_t *lv2_index_build(void) { return NULL; }
void lv2_index_free(lv2_index_t *idx) { (void)idx; }
const char *const *lv2_index_eager_bundles(const lv2_index_t *idx) { (void)idx; return NULL; }
void lv2_index_mark_plugin_loaded(lv2_index_t *idx, const char *uri) { (void)idx; (void)uri; }
void lv2_index_add_bundle(lv2_index_t *idx, const char *path) { (void)idx; (void)path; }
void lv2_index_remove_bundle(lv2_index_t *idx, const char *path) { (void)idx; (void)path; }

const char *const *lv2_index_bundles_for_plugin(const lv2_index_t *idx, const char *uri)
{
    (void)idx; (void)uri;
    return NULL;
}

const char *lv2_index_bundle_for_resource(const lv2_index_t *idx, const char *uri)
{
    (void)idx; (void)uri;
    return NULL;
}

void lv2_index_stats(const lv2_index_t *idx, unsigned *bundles, unsigned *plugins,
                     unsigned *resources, unsigned *eager)
{
    (void)idx;
    *bundles = *plugins = *resources = *eager = 0;
}

#endif
