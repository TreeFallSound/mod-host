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

/*
 * A lightweight index of the LV2 bundles reachable from LV2_PATH.
 *
 * Building a full lilv world at startup (lilv_world_load_all) parses every
 * bundle's manifest.ttl into a sord model. With ~600 bundles on a low-end
 * board that dominates startup time and resident memory, and mod-host does
 * not even need it: it never enumerates plugins, it only ever looks one up
 * by URI when instantiating it.
 *
 * This index skims the same manifests with a streaming serd reader, keeping
 * only what is needed to answer "which bundles must be in the world before
 * plugin <uri> can be used?". Bundles are then loaded on demand through
 * lilv_world_load_bundle. Nothing else about the manifest is retained.
 */

#ifndef LV2_INDEX_H
#define LV2_INDEX_H

typedef struct lv2_index_t lv2_index_t;

/*
 * Scan LV2_PATH and build the index. Bundles with unreadable or malformed
 * manifests are warned about and skipped; this never fails on bad input.
 * Returns NULL if the index is unavailable or unusable, in which case the
 * caller must fall back to lilv_world_load_all.
 */
lv2_index_t *lv2_index_build(void);
void lv2_index_free(lv2_index_t *idx);

/*
 * Bundles that must be loaded eagerly: those declaring an lv2:Specification
 * or owl:Ontology (their triples are needed to resolve things like a patch
 * property's rdfs:range, which lives in the spec rather than in the plugin),
 * and those with a dyn-manifest (whose plugin URIs cannot be known without
 * running it). NULL-terminated, owned by the index.
 */
const char *const *lv2_index_eager_bundles(const lv2_index_t *idx);

/*
 * Every bundle that must be in the world before plugin_uri is usable: the
 * bundle declaring the plugin, plus any bundle holding a subject that
 * lv2:appliesTo it (presets saved by mod-ui live in their own bundles, and
 * lilv_plugin_get_related only finds them if their manifest is loaded).
 *
 * NULL-terminated and owned by the index. NULL means "nothing to do" -- the
 * URI is unknown, or its bundles were already loaded. Callers must treat that
 * as a fall-through, not an error: dyn-manifest plugins are legitimately
 * absent from the index, and are already in the world via the eager load.
 */
const char *const *lv2_index_bundles_for_plugin(const lv2_index_t *idx, const char *plugin_uri);
void lv2_index_mark_plugin_loaded(lv2_index_t *idx, const char *plugin_uri);

/* The bundle declaring a preset (or any other manifest subject), or NULL. */
const char *lv2_index_bundle_for_resource(const lv2_index_t *idx, const char *resource_uri);

/* bundle_path must be absolute. Both are idempotent and tolerate junk. */
void lv2_index_add_bundle(lv2_index_t *idx, const char *bundle_path);
void lv2_index_remove_bundle(lv2_index_t *idx, const char *bundle_path);

void lv2_index_stats(const lv2_index_t *idx, unsigned *bundles, unsigned *plugins,
                     unsigned *resources, unsigned *eager);

#endif // LV2_INDEX_H
