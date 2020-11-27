/*
 * Copyright (C) 2006, Jamie McCracken <jamiemcc@gnome.org>
 * Copyright (C) 2008, Nokia <ivan.frade@nokia.com>
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
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#include "config.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>

#include <libtracker-common/tracker-common.h>

#include "tracker-class.h"
#include "tracker-data-manager.h"
#include "tracker-data-update.h"
#include "tracker-data-query.h"
#include "tracker-db-interface-sqlite.h"
#include "tracker-db-manager.h"
#include "tracker-ontologies.h"
#include "tracker-property.h"
#include "tracker-sparql.h"
#include "tracker-turtle-reader.h"

typedef struct _TrackerDataUpdateBuffer TrackerDataUpdateBuffer;
typedef struct _TrackerDataUpdateBufferGraph TrackerDataUpdateBufferGraph;
typedef struct _TrackerDataUpdateBufferResource TrackerDataUpdateBufferResource;
typedef struct _TrackerDataUpdateBufferProperty TrackerDataUpdateBufferProperty;
typedef struct _TrackerDataUpdateBufferTable TrackerDataUpdateBufferTable;
typedef struct _TrackerDataBlankBuffer TrackerDataBlankBuffer;
typedef struct _TrackerStatementDelegate TrackerStatementDelegate;
typedef struct _TrackerCommitDelegate TrackerCommitDelegate;

struct _TrackerDataUpdateBuffer {
	/* string -> integer */
	GHashTable *resource_cache;
	/* string -> TrackerDataUpdateBufferGraph */
	GPtrArray *graphs;

	gboolean fts_ever_updated;
};

struct _TrackerDataUpdateBufferGraph {
	gchar *graph;
	gint id;

	/* string -> TrackerDataUpdateBufferResource */
	GHashTable *resources;
};

struct _TrackerDataUpdateBufferResource {
	const TrackerDataUpdateBufferGraph *graph;
	const gchar *subject;
	gint id;
	gboolean create;
	gboolean modified;
	/* TrackerProperty -> GArray */
	GHashTable *predicates;
	/* string -> TrackerDataUpdateBufferTable */
	GHashTable *tables;
	/* TrackerClass */
	GPtrArray *types;

	gboolean fts_updated;
};

struct _TrackerDataUpdateBufferProperty {
	const gchar *name;
	GValue value;
	guint date_time : 1;
	guint fts : 1;
	guint delete_all_values : 1;
};

struct _TrackerDataUpdateBufferTable {
	gboolean insert;
	gboolean delete_row;
	gboolean delete_value;
	gboolean multiple_values;
	TrackerClass *class;
	/* TrackerDataUpdateBufferProperty */
	GArray *properties;
};

struct _TrackerStatementDelegate {
	TrackerStatementCallback callback;
	gpointer user_data;
};

struct _TrackerCommitDelegate {
	TrackerCommitCallback callback;
	gpointer user_data;
};

struct _TrackerData {
	GObject parent_instance;

	TrackerDataManager *manager;

	gboolean in_transaction;
	gboolean in_ontology_transaction;
	TrackerDataUpdateBuffer update_buffer;

	/* current resource */
	TrackerDataUpdateBufferResource *resource_buffer;
	time_t resource_time;
	gint transaction_modseq;
	gboolean has_persistent;

	GPtrArray *insert_callbacks;
	GPtrArray *delete_callbacks;
	GPtrArray *commit_callbacks;
	GPtrArray *rollback_callbacks;
	gint max_service_id;
	gint max_ontology_id;
};

struct _TrackerDataClass {
	GObjectClass parent_class;
};

enum {
	PROP_0,
	PROP_MANAGER
};

G_DEFINE_TYPE (TrackerData, tracker_data, G_TYPE_OBJECT);

static gint         ensure_resource_id         (TrackerData      *data,
                                                const gchar      *uri,
                                                gboolean         *create);
static void         cache_insert_value         (TrackerData      *data,
                                                const gchar      *table_name,
                                                const gchar      *field_name,
                                                GValue           *value,
                                                gboolean          multiple_values,
                                                gboolean          fts,
                                                gboolean          date_time);
static GArray      *get_old_property_values    (TrackerData      *data,
                                                TrackerProperty  *property,
                                                GError          **error);
static gboolean     delete_metadata_decomposed (TrackerData      *data,
                                                TrackerProperty  *property,
                                                GBytes           *object,
                                                GError          **error);
static gboolean     resource_buffer_switch     (TrackerData  *data,
                                                const gchar  *graph,
                                                const gchar  *subject,
                                                gint          subject_id,
                                                GError      **error);
static gboolean update_resource_single (TrackerData      *data,
                                        const gchar      *graph,
                                        TrackerResource  *resource,
                                        GHashTable       *visited,
                                        GHashTable       *bnodes,
                                        GError          **error);

void tracker_data_insert_statement_with_uri    (TrackerData  *data,
                                                const gchar  *graph,
                                                const gchar  *subject,
                                                const gchar  *predicate,
                                                GBytes       *object,
                                                GError      **error);
void tracker_data_insert_statement_with_string (TrackerData  *data,
                                                const gchar  *graph,
                                                const gchar  *subject,
                                                const gchar  *predicate,
                                                GBytes       *object,
                                                GError      **error);


void
tracker_data_add_commit_statement_callback (TrackerData             *data,
                                            TrackerCommitCallback    callback,
                                            gpointer                 user_data)
{
	TrackerCommitDelegate *delegate = g_new0 (TrackerCommitDelegate, 1);

	if (!data->commit_callbacks) {
		data->commit_callbacks = g_ptr_array_new_with_free_func (g_free);
	}

	delegate->callback = callback;
	delegate->user_data = user_data;

	g_ptr_array_add (data->commit_callbacks, delegate);
}

void
tracker_data_remove_commit_statement_callback (TrackerData           *data,
                                               TrackerCommitCallback  callback,
                                               gpointer               user_data)
{
	TrackerCommitDelegate *delegate;
	guint i;

	if (!data->commit_callbacks) {
		return;
	}

	for (i = 0; i < data->commit_callbacks->len; i++) {
		delegate = g_ptr_array_index (data->commit_callbacks, i);
		if (delegate->callback == callback && delegate->user_data == user_data) {
			g_ptr_array_remove_index (data->commit_callbacks, i);
			return;
		}
	}
}

void
tracker_data_dispatch_commit_statement_callbacks (TrackerData *data)
{
	if (data->commit_callbacks) {
		guint n;
		for (n = 0; n < data->commit_callbacks->len; n++) {
			TrackerCommitDelegate *delegate;
			delegate = g_ptr_array_index (data->commit_callbacks, n);
			delegate->callback (delegate->user_data);
		}
	}
}

void
tracker_data_add_rollback_statement_callback (TrackerData             *data,
                                              TrackerCommitCallback    callback,
                                              gpointer                 user_data)
{
	TrackerCommitDelegate *delegate = g_new0 (TrackerCommitDelegate, 1);

	if (!data->rollback_callbacks) {
		data->rollback_callbacks = g_ptr_array_new_with_free_func (g_free);
	}

	delegate->callback = callback;
	delegate->user_data = user_data;

	g_ptr_array_add (data->rollback_callbacks, delegate);
}


void
tracker_data_remove_rollback_statement_callback (TrackerData          *data,
                                                 TrackerCommitCallback callback,
                                                 gpointer              user_data)
{
	TrackerCommitDelegate *delegate;
	guint i;

	if (!data->rollback_callbacks) {
		return;
	}

	for (i = 0; i < data->rollback_callbacks->len; i++) {
		delegate = g_ptr_array_index (data->rollback_callbacks, i);
		if (delegate->callback == callback && delegate->user_data == user_data) {
			g_ptr_array_remove_index (data->rollback_callbacks, i);
			return;
		}
	}
}

void
tracker_data_dispatch_rollback_statement_callbacks (TrackerData *data)
{
	if (data->rollback_callbacks) {
		guint n;
		for (n = 0; n < data->rollback_callbacks->len; n++) {
			TrackerCommitDelegate *delegate;
			delegate = g_ptr_array_index (data->rollback_callbacks, n);
			delegate->callback (delegate->user_data);
		}
	}
}

void
tracker_data_add_insert_statement_callback (TrackerData             *data,
                                            TrackerStatementCallback callback,
                                            gpointer                 user_data)
{
	TrackerStatementDelegate *delegate = g_new0 (TrackerStatementDelegate, 1);

	if (!data->insert_callbacks) {
		data->insert_callbacks = g_ptr_array_new_with_free_func (g_free);
	}

	delegate->callback = callback;
	delegate->user_data = user_data;

	g_ptr_array_add (data->insert_callbacks, delegate);
}

void
tracker_data_remove_insert_statement_callback (TrackerData             *data,
                                               TrackerStatementCallback callback,
                                               gpointer                 user_data)
{
	TrackerStatementDelegate *delegate;
	guint i;

	if (!data->insert_callbacks) {
		return;
	}

	for (i = 0; i < data->insert_callbacks->len; i++) {
		delegate = g_ptr_array_index (data->insert_callbacks, i);
		if (delegate->callback == callback && delegate->user_data == user_data) {
			g_ptr_array_remove_index (data->insert_callbacks, i);
			return;
		}
	}
}

void
tracker_data_dispatch_insert_statement_callbacks (TrackerData *data,
                                                  gint         predicate_id,
                                                  gint         object_id,
                                                  const gchar *object)
{
	if (data->insert_callbacks) {
		guint n;

		for (n = 0; n < data->insert_callbacks->len; n++) {
			TrackerStatementDelegate *delegate;

			delegate = g_ptr_array_index (data->insert_callbacks, n);
			delegate->callback (data->resource_buffer->graph->id,
			                    data->resource_buffer->graph->graph,
			                    data->resource_buffer->id,
			                    data->resource_buffer->subject,
			                    predicate_id,
			                    object_id,
			                    object,
			                    data->resource_buffer->types,
			                    delegate->user_data);
		}
	}
}

void
tracker_data_add_delete_statement_callback (TrackerData             *data,
                                            TrackerStatementCallback callback,
                                            gpointer                 user_data)
{
	TrackerStatementDelegate *delegate = g_new0 (TrackerStatementDelegate, 1);

	if (!data->delete_callbacks) {
		data->delete_callbacks = g_ptr_array_new_with_free_func (g_free);
	}

	delegate->callback = callback;
	delegate->user_data = user_data;

	g_ptr_array_add (data->delete_callbacks, delegate);
}

void
tracker_data_remove_delete_statement_callback (TrackerData             *data,
                                               TrackerStatementCallback callback,
                                               gpointer                 user_data)
{
	TrackerStatementDelegate *delegate;
	guint i;

	if (!data->delete_callbacks) {
		return;
	}

	for (i = 0; i < data->delete_callbacks->len; i++) {
		delegate = g_ptr_array_index (data->delete_callbacks, i);
		if (delegate->callback == callback && delegate->user_data == user_data) {
			g_ptr_array_remove_index (data->delete_callbacks, i);
			return;
		}
	}
}

void
tracker_data_dispatch_delete_statement_callbacks (TrackerData *data,
                                                  gint         predicate_id,
                                                  gint         object_id,
                                                  const gchar *object)
{
	if (data->delete_callbacks) {
		guint n;

		for (n = 0; n < data->delete_callbacks->len; n++) {
			TrackerStatementDelegate *delegate;

			delegate = g_ptr_array_index (data->delete_callbacks, n);
			delegate->callback (data->resource_buffer->graph->id,
			                    data->resource_buffer->graph->graph,
			                    data->resource_buffer->id,
			                    data->resource_buffer->subject,
			                    predicate_id,
			                    object_id,
			                    object,
			                    data->resource_buffer->types,
			                    delegate->user_data);
		}
	}
}

static gint
tracker_data_update_get_new_service_id (TrackerData  *data,
                                        GError      **error)
{
	TrackerDBCursor    *cursor = NULL;
	TrackerDBInterface *iface;
	TrackerDBStatement *stmt;
	GError *inner_error = NULL;

	if (data->in_ontology_transaction) {
		if (G_LIKELY (data->max_ontology_id != 0)) {
			return ++data->max_ontology_id;
		}

		iface = tracker_data_manager_get_writable_db_interface (data->manager);

		stmt = tracker_db_interface_create_statement (iface, TRACKER_DB_STATEMENT_CACHE_TYPE_SELECT, &inner_error,
		                                              "SELECT MAX(ID) AS A FROM Resource WHERE ID <= %d", TRACKER_ONTOLOGIES_MAX_ID);

		if (stmt) {
			cursor = tracker_db_statement_start_cursor (stmt, &inner_error);
			g_object_unref (stmt);
		}

		if (cursor) {
			if (tracker_db_cursor_iter_next (cursor, NULL, &inner_error)) {
				data->max_ontology_id = MAX (tracker_db_cursor_get_int (cursor, 0), data->max_ontology_id);
			}

			g_object_unref (cursor);
		}

		if (G_UNLIKELY (inner_error)) {
			g_propagate_error (error, inner_error);
			return 0;
		}

		return ++data->max_ontology_id;
	} else {
		if (G_LIKELY (data->max_service_id != 0)) {
			return ++data->max_service_id;
		}

		data->max_service_id = TRACKER_ONTOLOGIES_MAX_ID;

		iface = tracker_data_manager_get_writable_db_interface (data->manager);

		stmt = tracker_db_interface_create_statement (iface, TRACKER_DB_STATEMENT_CACHE_TYPE_SELECT, &inner_error,
		                                              "SELECT MAX(ID) AS A FROM Resource");

		if (stmt) {
			cursor = tracker_db_statement_start_cursor (stmt, &inner_error);
			g_object_unref (stmt);
		}

		if (cursor) {
			if (tracker_db_cursor_iter_next (cursor, NULL, &inner_error)) {
				data->max_service_id = MAX (tracker_db_cursor_get_int (cursor, 0), data->max_service_id);
			}

			g_object_unref (cursor);
		}

		if (G_UNLIKELY (inner_error)) {
			g_propagate_error (error, inner_error);
			return 0;
		}

		return ++data->max_service_id;
	}
}

static gint
tracker_data_update_get_next_modseq (TrackerData *data)
{
	TrackerDBCursor    *cursor = NULL;
	TrackerDBInterface *temp_iface;
	TrackerDBStatement *stmt;
	GError             *error = NULL;
	gint                max_modseq = 0;

	temp_iface = tracker_data_manager_get_writable_db_interface (data->manager);

	stmt = tracker_db_interface_create_statement (temp_iface, TRACKER_DB_STATEMENT_CACHE_TYPE_SELECT, &error,
	                                              "SELECT MAX(\"nrl:modified\") AS A FROM \"rdfs:Resource\"");

	if (stmt) {
		cursor = tracker_db_statement_start_cursor (stmt, &error);
		g_object_unref (stmt);
	}

	if (cursor) {
		if (tracker_db_cursor_iter_next (cursor, NULL, &error)) {
			max_modseq = MAX (tracker_db_cursor_get_int (cursor, 0), max_modseq);
		}

		g_object_unref (cursor);
	}

	if (G_UNLIKELY (error)) {
		g_warning ("Could not get new resource ID: %s\n", error->message);
		g_error_free (error);
	}

	return ++max_modseq;
}

static void
tracker_data_init (TrackerData *data)
{
}

static void
tracker_data_set_property (GObject      *object,
                           guint         prop_id,
                           const GValue *value,
                           GParamSpec   *pspec)
{
	TrackerData *data = TRACKER_DATA (object);

	switch (prop_id) {
	case PROP_MANAGER:
		data->manager = g_value_get_object (value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
tracker_data_get_property (GObject    *object,
                           guint       prop_id,
                           GValue     *value,
                           GParamSpec *pspec)
{
	TrackerData *data = TRACKER_DATA (object);

	switch (prop_id) {
	case PROP_MANAGER:
		g_value_set_object (value, data->manager);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
tracker_data_class_init (TrackerDataClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->set_property = tracker_data_set_property;
	object_class->get_property = tracker_data_get_property;

	g_object_class_install_property (object_class,
	                                 PROP_MANAGER,
	                                 g_param_spec_object ("manager",
	                                                      "manager",
	                                                      "manager",
	                                                      TRACKER_TYPE_DATA_MANAGER,
	                                                      G_PARAM_READWRITE |
	                                                      G_PARAM_CONSTRUCT_ONLY));
}

TrackerData *
tracker_data_new (TrackerDataManager *manager)
{
	return g_object_new (TRACKER_TYPE_DATA,
	                     "manager", manager,
	                     NULL);
}

static gint
get_transaction_modseq (TrackerData *data)
{
	if (G_UNLIKELY (data->transaction_modseq == 0)) {
		data->transaction_modseq = tracker_data_update_get_next_modseq (data);
	}

	/* Always use 1 for ontology transactions */
	if (data->in_ontology_transaction) {
		return 1;
	}

	return data->transaction_modseq;
}

static TrackerDataUpdateBufferTable *
cache_table_new (gboolean multiple_values)
{
	TrackerDataUpdateBufferTable *table;

	table = g_slice_new0 (TrackerDataUpdateBufferTable);
	table->multiple_values = multiple_values;
	table->properties = g_array_sized_new (FALSE, FALSE, sizeof (TrackerDataUpdateBufferProperty), 4);

	return table;
}

static void
cache_table_free (TrackerDataUpdateBufferTable *table)
{
	TrackerDataUpdateBufferProperty *property;
	gint                            i;

	for (i = 0; i < table->properties->len; i++) {
		property = &g_array_index (table->properties, TrackerDataUpdateBufferProperty, i);
		g_value_unset (&property->value);
	}

	g_array_free (table->properties, TRUE);
	g_slice_free (TrackerDataUpdateBufferTable, table);
}

static TrackerDataUpdateBufferTable *
cache_ensure_table (TrackerData *data,
                    const gchar *table_name,
                    gboolean     multiple_values)
{
	TrackerDataUpdateBufferTable *table;

	if (!data->resource_buffer->modified) {
		/* first modification of this particular resource, update nrl:modified */

		GValue gvalue = { 0 };

		data->resource_buffer->modified = TRUE;

		g_value_init (&gvalue, G_TYPE_INT64);
		g_value_set_int64 (&gvalue, get_transaction_modseq (data));
		cache_insert_value (data, "rdfs:Resource", "nrl:modified",
		                    &gvalue, FALSE, FALSE, FALSE);
	}

	table = g_hash_table_lookup (data->resource_buffer->tables, table_name);
	if (table == NULL) {
		table = cache_table_new (multiple_values);
		g_hash_table_insert (data->resource_buffer->tables, g_strdup (table_name), table);
		table->insert = multiple_values;
	}

	return table;
}

static void
cache_insert_row (TrackerData  *data,
                  TrackerClass *class)
{
	TrackerDataUpdateBufferTable *table;

	table = cache_ensure_table (data, tracker_class_get_name (class), FALSE);
	table->class = class;
	table->insert = TRUE;
}

static void
cache_insert_value (TrackerData *data,
                    const gchar *table_name,
                    const gchar *field_name,
                    GValue      *value,
                    gboolean     multiple_values,
                    gboolean     fts,
                    gboolean     date_time)
{
	TrackerDataUpdateBufferTable    *table;
	TrackerDataUpdateBufferProperty  property = { 0 };

	/* No need to strdup here, the incoming string is either always static, or
	 * long-standing as tracker_property_get_name return value. */
	property.name = field_name;

	g_value_init (&property.value, G_VALUE_TYPE (value));
	g_value_copy (value, &property.value);
	property.fts = fts;
	property.date_time = date_time;

	table = cache_ensure_table (data, table_name, multiple_values);
	g_array_append_val (table->properties, property);
}

static void
cache_delete_row (TrackerData  *data,
                  TrackerClass *class)
{
	TrackerDataUpdateBufferTable    *table;

	table = cache_ensure_table (data, tracker_class_get_name (class), FALSE);
	table->class = class;
	table->delete_row = TRUE;
}

/* Use only for multi-valued properties */
static void
cache_delete_all_values (TrackerData *data,
                         const gchar *table_name,
                         const gchar *field_name,
                         gboolean     fts,
                         gboolean     date_time)
{
	TrackerDataUpdateBufferTable    *table;
	TrackerDataUpdateBufferProperty  property = { 0 };

	property.name = field_name;
	property.fts = fts;
	property.date_time = date_time;
	property.delete_all_values = TRUE;

	table = cache_ensure_table (data, table_name, TRUE);
	table->delete_value = TRUE;
	g_array_append_val (table->properties, property);
}

static void
cache_delete_value (TrackerData *data,
                    const gchar *table_name,
                    const gchar *field_name,
                    GValue      *value,
                    gboolean     multiple_values,
                    gboolean     fts,
                    gboolean     date_time)
{
	TrackerDataUpdateBufferTable    *table;
	TrackerDataUpdateBufferProperty  property = { 0 };

	property.name = field_name;

	g_value_init (&property.value, G_VALUE_TYPE (value));
	g_value_copy (value, &property.value);
	property.fts = fts;
	property.date_time = date_time;

	table = cache_ensure_table (data, table_name, multiple_values);
	table->delete_value = TRUE;
	g_array_append_val (table->properties, property);
}

static gint
query_resource_id (TrackerData *data,
                   const gchar *uri)
{
	TrackerDBInterface *iface;
	gint id;

	id = GPOINTER_TO_INT (g_hash_table_lookup (data->update_buffer.resource_cache, uri));
	iface = tracker_data_manager_get_writable_db_interface (data->manager);

	if (id == 0) {
		id = tracker_data_query_resource_id (data->manager, iface, uri);

		if (id) {
			g_hash_table_insert (data->update_buffer.resource_cache, g_strdup (uri), GINT_TO_POINTER (id));
		}
	}

	return id;
}

static gint
ensure_resource_id (TrackerData *data,
                    const gchar *uri,
                    gboolean    *create)
{
	TrackerDBInterface *iface;
	TrackerDBStatement *stmt = NULL;
	GError *error = NULL;
	gint id;

	id = query_resource_id (data, uri);

	if (create) {
		*create = (id == 0);
	}

	if (id == 0) {
		iface = tracker_data_manager_get_writable_db_interface (data->manager);

		id = tracker_data_update_get_new_service_id (data, &error);

		if (id > 0) {
			stmt = tracker_db_interface_create_statement (iface, TRACKER_DB_STATEMENT_CACHE_TYPE_UPDATE, &error,
			                                              "INSERT INTO Resource (ID, Uri, BlankNode) VALUES (?, ?, ?)");
		}

		if (stmt) {
			tracker_db_statement_bind_int (stmt, 0, id);
			tracker_db_statement_bind_text (stmt, 1, uri);
			tracker_db_statement_bind_int (stmt, 2, g_str_has_prefix (uri, "urn:bnode:"));
			tracker_db_statement_execute (stmt, &error);
			g_object_unref (stmt);
		}

		if (error) {
			g_error ("Could not ensure resource existence: %s", error->message);
			g_error_free (error);
		}

		g_hash_table_insert (data->update_buffer.resource_cache, g_strdup (uri), GINT_TO_POINTER (id));
	}

	return id;
}

static void
statement_bind_gvalue (TrackerDBStatement *stmt,
                       gint               *idx,
                       const GValue       *value)
{
	GType type;

	type = G_VALUE_TYPE (value);
	switch (type) {
	case G_TYPE_STRING:
		tracker_db_statement_bind_text (stmt, (*idx)++, g_value_get_string (value));
		break;
	case G_TYPE_INT64:
		tracker_db_statement_bind_int (stmt, (*idx)++, g_value_get_int64 (value));
		break;
	case G_TYPE_DOUBLE:
		tracker_db_statement_bind_double (stmt, (*idx)++, g_value_get_double (value));
		break;
	default:
		if (type == TRACKER_TYPE_DATE_TIME) {
			gdouble time = tracker_date_time_get_time (value);
			int offset = tracker_date_time_get_offset (value);

			/* If we have anything that prevents a unix timestamp to be
			 * lossless, we use the ISO8601 string.
			 */
			if (offset != 0 || floor (time) != time) {
				gchar *str;

				str = tracker_date_to_string (time, offset);
				tracker_db_statement_bind_text (stmt, (*idx)++, str);
				g_free (str);
			} else {
				tracker_db_statement_bind_int (stmt, (*idx)++, round (time));
			}
		} else if (type == G_TYPE_BYTES) {
			GBytes *bytes;
			gconstpointer data;
			gsize len;

			bytes = g_value_get_boxed (value);
			data = g_bytes_get_data (bytes, &len);

			if (len == strlen (data) + 1) {
				/* No ancillary data */
				tracker_db_statement_bind_text (stmt, (*idx)++, data);
			} else {
				/* String with langtag */
				tracker_db_statement_bind_bytes (stmt, (*idx)++, bytes);
			}
		} else {
			g_warning ("Unknown type for binding: %s\n", G_VALUE_TYPE_NAME (value));
		}
		break;
	}
}

static void
tracker_data_resource_buffer_flush (TrackerData                      *data,
                                    TrackerDataUpdateBufferResource  *resource,
                                    GError                          **error)
{
	TrackerDBInterface             *iface;
	TrackerDBStatement             *stmt;
	TrackerDataUpdateBufferTable    *table;
	TrackerDataUpdateBufferProperty *property;
	GHashTableIter                  iter;
	const gchar                    *table_name, *database;
	gint                            i, param;
	GError                         *actual_error = NULL;

	iface = tracker_data_manager_get_writable_db_interface (data->manager);
	database = resource->graph->graph ? resource->graph->graph : "main";

	g_hash_table_iter_init (&iter, resource->tables);
	while (g_hash_table_iter_next (&iter, (gpointer*) &table_name, (gpointer*) &table)) {
		if (table->multiple_values) {
			for (i = 0; i < table->properties->len; i++) {
				property = &g_array_index (table->properties, TrackerDataUpdateBufferProperty, i);

				if (table->delete_value && property->delete_all_values) {
					stmt = tracker_db_interface_create_statement (iface, TRACKER_DB_STATEMENT_CACHE_TYPE_UPDATE, &actual_error,
					                                              "DELETE FROM \"%s\".\"%s\" WHERE ID = ?",
					                                              database,
					                                              table_name);
				} else if (table->delete_value) {
					/* delete rows for multiple value properties */
					stmt = tracker_db_interface_create_statement (iface, TRACKER_DB_STATEMENT_CACHE_TYPE_UPDATE, &actual_error,
					                                              "DELETE FROM \"%s\".\"%s\" WHERE ID = ? AND \"%s\" = ?",
					                                              database,
					                                              table_name,
					                                              property->name);
				} else {
					stmt = tracker_db_interface_create_statement (iface, TRACKER_DB_STATEMENT_CACHE_TYPE_UPDATE, &actual_error,
					                                              "INSERT OR IGNORE INTO \"%s\".\"%s\" (ID, \"%s\") VALUES (?, ?)",
					                                              database,
					                                              table_name,
					                                              property->name);
				}

				if (actual_error) {
					g_propagate_error (error, actual_error);
					return;
				}

				param = 0;

				tracker_db_statement_bind_int (stmt, param++, resource->id);

				if (!property->delete_all_values)
					statement_bind_gvalue (stmt, &param, &property->value);

				tracker_db_statement_execute (stmt, &actual_error);
				g_object_unref (stmt);

				if (actual_error) {
					g_propagate_error (error, actual_error);
					return;
				}
			}
		} else {
			GString *sql, *values_sql;

			if (table->delete_row) {
				/* remove entry from rdf:type table */
				stmt = tracker_db_interface_create_statement (iface, TRACKER_DB_STATEMENT_CACHE_TYPE_UPDATE, &actual_error,
				                                              "DELETE FROM \"%s\".\"rdfs:Resource_rdf:type\" WHERE ID = ? AND \"rdf:type\" = ?",
				                                              database);

				if (stmt) {
					tracker_db_statement_bind_int (stmt, 0, resource->id);
					tracker_db_statement_bind_int (stmt, 1, ensure_resource_id (data, tracker_class_get_uri (table->class), NULL));
					tracker_db_statement_execute (stmt, &actual_error);
					g_object_unref (stmt);
				}

				if (actual_error) {
					g_propagate_error (error, actual_error);
					return;
				}

				/* remove row from class table */
				stmt = tracker_db_interface_create_statement (iface, TRACKER_DB_STATEMENT_CACHE_TYPE_UPDATE, &actual_error,
				                                              "DELETE FROM \"%s\".\"%s\" WHERE ID = ?",
				                                              database, table_name);

				if (stmt) {
					tracker_db_statement_bind_int (stmt, 0, resource->id);
					tracker_db_statement_execute (stmt, &actual_error);
					g_object_unref (stmt);
				}

				if (actual_error) {
					g_propagate_error (error, actual_error);
					return;
				}

				continue;
			}

			if (table->insert) {
				sql = g_string_new ("INSERT INTO ");
				values_sql = g_string_new ("VALUES (?");
			} else {
				sql = g_string_new ("UPDATE ");
				values_sql = NULL;
			}

			g_string_append_printf (sql, "\"%s\".\"%s\"",
			                        database, table_name);

			if (table->insert) {
				g_string_append (sql, " (ID");

				if (strcmp (table_name, "rdfs:Resource") == 0) {
					g_string_append (sql, ", \"nrl:added\", \"nrl:modified\"");
					g_string_append (values_sql, ", ?, ?");
				}
			} else {
				g_string_append (sql, " SET ");
			}

			for (i = 0; i < table->properties->len; i++) {
				property = &g_array_index (table->properties, TrackerDataUpdateBufferProperty, i);
				if (table->insert) {
					g_string_append_printf (sql, ", \"%s\"", property->name);
					g_string_append (values_sql, ", ?");
				} else {
					if (i > 0) {
						g_string_append (sql, ", ");
					}
					g_string_append_printf (sql, "\"%s\" = ?", property->name);
				}
			}

			if (table->insert) {
				g_string_append (sql, ")");
				g_string_append (values_sql, ")");

				stmt = tracker_db_interface_create_statement (iface, TRACKER_DB_STATEMENT_CACHE_TYPE_UPDATE, &actual_error,
				                                              "%s %s", sql->str, values_sql->str);
				g_string_free (sql, TRUE);
				g_string_free (values_sql, TRUE);
			} else {
				g_string_append (sql, " WHERE ID = ?");

				stmt = tracker_db_interface_create_statement (iface, TRACKER_DB_STATEMENT_CACHE_TYPE_UPDATE, &actual_error,
				                                              "%s", sql->str);
				g_string_free (sql, TRUE);
			}

			if (actual_error) {
				g_propagate_error (error, actual_error);
				return;
			}

			if (table->insert) {
				tracker_db_statement_bind_int (stmt, 0, resource->id);

				if (strcmp (table_name, "rdfs:Resource") == 0) {
					g_warn_if_fail	(data->resource_time != 0);
					tracker_db_statement_bind_int (stmt, 1, (gint64) data->resource_time);
					tracker_db_statement_bind_int (stmt, 2, get_transaction_modseq (data));
					param = 3;
				} else {
					param = 1;
				}
			} else {
				param = 0;
			}

			for (i = 0; i < table->properties->len; i++) {
				property = &g_array_index (table->properties, TrackerDataUpdateBufferProperty, i);
				if (table->delete_value) {
					/* just set value to NULL for single value properties */
					tracker_db_statement_bind_null (stmt, param++);
				} else {
					statement_bind_gvalue (stmt, &param, &property->value);
				}
			}

			if (!table->insert) {
				tracker_db_statement_bind_int (stmt, param++, resource->id);
			}

			tracker_db_statement_execute (stmt, &actual_error);
			g_object_unref (stmt);

			if (actual_error) {
				g_propagate_error (error, actual_error);
				return;
			}
		}
	}

	if (resource->fts_updated) {
		TrackerProperty *prop;
		GArray *values;
		GPtrArray *properties, *text;

		properties = text = NULL;
		g_hash_table_iter_init (&iter, resource->predicates);
		while (g_hash_table_iter_next (&iter, (gpointer*) &prop, (gpointer*) &values)) {
			if (tracker_property_get_fulltext_indexed (prop)) {
				GString *fts;

				fts = g_string_new ("");
				for (i = 0; i < values->len; i++) {
					GValue *v = &g_array_index (values, GValue, i);
					g_string_append (fts, g_value_get_string (v));
					g_string_append_c (fts, ' ');
				}

				if (!properties && !text) {
					properties = g_ptr_array_new ();
					text = g_ptr_array_new_with_free_func ((GDestroyNotify) g_free);
				}

				g_ptr_array_add (properties, (gpointer) tracker_property_get_name (prop));
				g_ptr_array_add (text, g_string_free (fts, FALSE));
			}
		}

		if (properties && text) {
			g_ptr_array_add (properties, NULL);
			g_ptr_array_add (text, NULL);

			tracker_db_interface_sqlite_fts_update_text (iface,
								     database,
			                                             resource->id,
			                                             (const gchar **) properties->pdata,
			                                             (const gchar **) text->pdata);
			data->update_buffer.fts_ever_updated = TRUE;
			g_ptr_array_free (properties, TRUE);
			g_ptr_array_free (text, TRUE);
		}
	}
}

static void
graph_buffer_free (TrackerDataUpdateBufferGraph *graph)
{
	g_hash_table_unref (graph->resources);
	g_free (graph->graph);
	g_slice_free (TrackerDataUpdateBufferGraph, graph);
}

static void resource_buffer_free (TrackerDataUpdateBufferResource *resource)
{
	g_hash_table_unref (resource->predicates);
	g_hash_table_unref (resource->tables);
	resource->subject = NULL;

	g_ptr_array_free (resource->types, TRUE);
	resource->types = NULL;

	g_slice_free (TrackerDataUpdateBufferResource, resource);
}

void
tracker_data_update_buffer_flush (TrackerData  *data,
                                  GError      **error)
{
	TrackerDataUpdateBufferGraph *graph;
	TrackerDataUpdateBufferResource *resource;
	GHashTableIter iter;
	GError *actual_error = NULL;
	gint i;

	for (i = 0; i < data->update_buffer.graphs->len; i++) {
		graph = g_ptr_array_index (data->update_buffer.graphs, i);
		g_hash_table_iter_init (&iter, graph->resources);

		while (g_hash_table_iter_next (&iter, NULL, (gpointer*) &resource)) {
			tracker_data_resource_buffer_flush (data, resource, &actual_error);
			if (actual_error) {
				g_propagate_error (error, actual_error);
				goto out;
			}
		}
	}

out:
	g_ptr_array_set_size (data->update_buffer.graphs, 0);
	data->resource_buffer = NULL;
}

void
tracker_data_update_buffer_might_flush (TrackerData  *data,
                                        GError      **error)
{
	TrackerDataUpdateBufferGraph *graph;
	gint i, count = 0;

	for (i = 0; i < data->update_buffer.graphs->len; i++) {
		graph = g_ptr_array_index (data->update_buffer.graphs, i);
		count += g_hash_table_size (graph->resources);

		if (count >= 1000) {
			tracker_data_update_buffer_flush (data, error);
			break;
		}
	}
}

static void
tracker_data_update_buffer_clear (TrackerData *data)
{
	g_ptr_array_set_size (data->update_buffer.graphs, 0);
	g_hash_table_remove_all (data->update_buffer.resource_cache);
	data->resource_buffer = NULL;

	data->update_buffer.fts_ever_updated = FALSE;
}

static void
cache_create_service_decomposed (TrackerData  *data,
                                 TrackerClass *cl)
{
	TrackerClass       **super_classes;
	TrackerProperty    **domain_indexes;
	GValue              gvalue = { 0 };
	gint                i, class_id;
	TrackerOntologies  *ontologies;

	/* also create instance of all super classes */
	super_classes = tracker_class_get_super_classes (cl);
	while (*super_classes) {
		cache_create_service_decomposed (data, *super_classes);
		super_classes++;
	}

	for (i = 0; i < data->resource_buffer->types->len; i++) {
		if (g_ptr_array_index (data->resource_buffer->types, i) == cl) {
			/* ignore duplicate statement */
			return;
		}
	}

	g_ptr_array_add (data->resource_buffer->types, cl);

	g_value_init (&gvalue, G_TYPE_INT64);

	cache_insert_row (data, cl);

	/* This is the original, no idea why tracker_class_get_id wasn't used here:
	 * class_id = ensure_resource_id (tracker_class_get_uri (cl), NULL); */

	class_id = tracker_class_get_id (cl);
	ontologies = tracker_data_manager_get_ontologies (data->manager);

	g_value_set_int64 (&gvalue, class_id);
	cache_insert_value (data, "rdfs:Resource_rdf:type", "rdf:type",
	                    &gvalue, TRUE, FALSE, FALSE);

	tracker_data_dispatch_insert_statement_callbacks (data,
	                                                  tracker_property_get_id (tracker_ontologies_get_rdf_type (ontologies)),
	                                                  class_id,
	                                                  tracker_class_get_uri (cl));

	/* When a new class created, make sure we propagate to the domain indexes
	 * the property values already set, if any. */
	domain_indexes = tracker_class_get_domain_indexes (cl);
	if (!domain_indexes) {
		/* Nothing else to do, return */
		return;
	}

	while (*domain_indexes) {
		GError *error = NULL;
		GArray *old_values;

		/* read existing property values */
		old_values = get_old_property_values (data, *domain_indexes, &error);
		if (error) {
			g_critical ("Couldn't get old values for property '%s': '%s'",
			            tracker_property_get_name (*domain_indexes),
			            error->message);
			g_clear_error (&error);
			domain_indexes++;
			continue;
		}

		if (old_values &&
		    old_values->len > 0) {
			GValue *v;

			/* Don't expect several values for property which is a domain index */
			g_assert_cmpint (old_values->len, ==, 1);

			g_debug ("Propagating '%s' property value from '%s' to domain index in '%s'",
			         tracker_property_get_name (*domain_indexes),
			         tracker_property_get_table_name (*domain_indexes),
			         tracker_class_get_name (cl));

			v = &g_array_index (old_values, GValue, 0);

			cache_insert_value (data,
			                    tracker_class_get_name (cl),
			                    tracker_property_get_name (*domain_indexes),
			                    v,
			                    tracker_property_get_multiple_values (*domain_indexes),
			                    tracker_property_get_fulltext_indexed (*domain_indexes),
			                    tracker_property_get_data_type (*domain_indexes) == TRACKER_PROPERTY_TYPE_DATETIME);
		}

		domain_indexes++;
	}
}

static gboolean
value_equal (GValue *value1,
             GValue *value2)
{
	GType type = G_VALUE_TYPE (value1);

	if (type != G_VALUE_TYPE (value2)) {
		return FALSE;
	}

	switch (type) {
	case G_TYPE_STRING:
		return (strcmp (g_value_get_string (value1), g_value_get_string (value2)) == 0);
	case G_TYPE_INT64:
		return g_value_get_int64 (value1) == g_value_get_int64 (value2);
	case G_TYPE_DOUBLE:
		/* does RDF define equality for floating point values? */
		return g_value_get_double (value1) == g_value_get_double (value2);
	default:
		if (type == TRACKER_TYPE_DATE_TIME) {
			/* ignore UTC offset for comparison, irrelevant for comparison according to xsd:dateTime spec
			 * http://www.w3.org/TR/xmlschema-2/#dateTime
			 * also ignore sub-millisecond as this is a floating point comparison
			 */
			return fabs (tracker_date_time_get_time (value1) - tracker_date_time_get_time (value2)) < 0.001;
		}
		g_assert_not_reached ();
	}
}

static gboolean
value_set_add_value (GArray *value_set,
                     GValue *value)
{
	GValue gvalue_copy = { 0 };
	gint i;

	g_return_val_if_fail (G_VALUE_TYPE (value), FALSE);

	for (i = 0; i < value_set->len; i++) {
		GValue *v;

		v = &g_array_index (value_set, GValue, i);
		if (value_equal (v, value)) {
			/* no change, value already in set */
			return FALSE;
		}
	}

	g_value_init (&gvalue_copy, G_VALUE_TYPE (value));
	g_value_copy (value, &gvalue_copy);
	g_array_append_val (value_set, gvalue_copy);

	return TRUE;
}

static gboolean
value_set_remove_value (GArray *value_set,
                        GValue *value)
{
	gint i;

	g_return_val_if_fail (G_VALUE_TYPE (value), FALSE);

	for (i = 0; i < value_set->len; i++) {
		GValue *v = &g_array_index (value_set, GValue, i);

		if (value_equal (v, value)) {
			/* value found, remove from set */
			g_array_remove_index (value_set, i);

			return TRUE;
		}
	}

	/* no change, value not found */
	return FALSE;
}

static gboolean
check_property_domain (TrackerData     *data,
                       TrackerProperty *property)
{
	gint type_index;

	for (type_index = 0; type_index < data->resource_buffer->types->len; type_index++) {
		if (tracker_property_get_domain (property) == g_ptr_array_index (data->resource_buffer->types, type_index)) {
			return TRUE;
		}
	}
	return FALSE;
}

static GArray *
get_property_values (TrackerData     *data,
                     TrackerProperty *property)
{
	gboolean multiple_values;
	const gchar *database;
	GArray *old_values;

	multiple_values = tracker_property_get_multiple_values (property);

	old_values = g_array_sized_new (FALSE, TRUE, sizeof (GValue), multiple_values ? 4 : 1);
	g_array_set_clear_func (old_values, (GDestroyNotify) g_value_unset);
	g_hash_table_insert (data->resource_buffer->predicates, g_object_ref (property), old_values);

	database = data->resource_buffer->graph->graph ?
		data->resource_buffer->graph->graph : "main";

	if (!data->resource_buffer->create) {
		TrackerDBInterface *iface;
		TrackerDBStatement *stmt;
		TrackerDBCursor    *cursor = NULL;
		const gchar        *table_name;
		const gchar        *field_name;
		GError             *error = NULL;

		table_name = tracker_property_get_table_name (property);
		field_name = tracker_property_get_name (property);

		iface = tracker_data_manager_get_writable_db_interface (data->manager);

		stmt = tracker_db_interface_create_statement (iface, TRACKER_DB_STATEMENT_CACHE_TYPE_SELECT, &error,
		                                              "SELECT \"%s\" FROM \"%s\".\"%s\" WHERE ID = ?",
		                                              field_name, database, table_name);

		if (stmt) {
			tracker_db_statement_bind_int (stmt, 0, data->resource_buffer->id);
			cursor = tracker_db_statement_start_cursor (stmt, &error);
			g_object_unref (stmt);
		}

		if (error) {
			g_warning ("Could not get property values: %s\n", error->message);
			g_error_free (error);
		}

		if (cursor) {
			while (tracker_db_cursor_iter_next (cursor, NULL, &error)) {
				GValue gvalue = { 0 };

				tracker_db_cursor_get_value (cursor, 0, &gvalue);

				if (G_VALUE_TYPE (&gvalue)) {
					if (tracker_property_get_data_type (property) == TRACKER_PROPERTY_TYPE_DATETIME) {
						if (G_VALUE_TYPE (&gvalue) == G_TYPE_INT64) {
							gdouble time;

							time = g_value_get_int64 (&gvalue);
							g_value_unset (&gvalue);
							g_value_init (&gvalue, TRACKER_TYPE_DATE_TIME);
							/* UTC offset is irrelevant for comparison */
							tracker_date_time_set (&gvalue, time, 0);
						} else {
							gchar *time;

							time = g_value_dup_string (&gvalue);
							g_value_unset (&gvalue);
							g_value_init (&gvalue, TRACKER_TYPE_DATE_TIME);
							tracker_date_time_set_from_string (&gvalue, time, &error);
							g_free (time);

							if (error) {
								g_warning ("Error in date conversion: %s", error->message);
								g_error_free (error);
							}
						}
					}

					g_array_append_val (old_values, gvalue);
				}
			}
			g_object_unref (cursor);
		}
	}

	return old_values;
}

static GArray *
get_old_property_values (TrackerData      *data,
                         TrackerProperty  *property,
                         GError          **error)
{
	GArray *old_values;
	const gchar *database;

	database = data->resource_buffer->graph->graph ?
		data->resource_buffer->graph->graph : "main";

	/* read existing property values */
	old_values = g_hash_table_lookup (data->resource_buffer->predicates, property);
	if (old_values == NULL) {
		if (!check_property_domain (data, property)) {
			g_set_error (error, TRACKER_SPARQL_ERROR, TRACKER_SPARQL_ERROR_CONSTRAINT,
			             "Subject `%s' is not in domain `%s' of property `%s'",
			             data->resource_buffer->subject,
			             tracker_class_get_name (tracker_property_get_domain (property)),
			             tracker_property_get_name (property));
			return NULL;
		}

		if (tracker_property_get_fulltext_indexed (property)) {
			TrackerDBInterface *iface;

			iface = tracker_data_manager_get_writable_db_interface (data->manager);

			if (!data->resource_buffer->fts_updated && !data->resource_buffer->create) {
				TrackerOntologies *ontologies;
				guint i, n_props;
				TrackerProperty   **properties, *prop;
				GPtrArray *fts_props, *fts_text;

				/* first fulltext indexed property to be modified
				 * retrieve values of all fulltext indexed properties
				 */
				ontologies = tracker_data_manager_get_ontologies (data->manager);
				properties = tracker_ontologies_get_properties (ontologies, &n_props);

				fts_props = g_ptr_array_new ();
				fts_text = g_ptr_array_new_with_free_func (g_free);

				for (i = 0; i < n_props; i++) {
					prop = properties[i];

					if (tracker_property_get_fulltext_indexed (prop)
					    && check_property_domain (data, prop)) {
						const gchar *property_name;
						GString *str;
						gint i;

						old_values = get_property_values (data, prop);
						property_name = tracker_property_get_name (prop);
						str = g_string_new (NULL);

						/* delete old fts entries */
						for (i = 0; i < old_values->len; i++) {
							GValue *value = &g_array_index (old_values, GValue, i);
							if (i != 0)
								g_string_append_c (str, ',');
							g_string_append (str, g_value_get_string (value));
						}

						g_ptr_array_add (fts_props, (gpointer) property_name);
						g_ptr_array_add (fts_text, g_string_free (str, FALSE));
					}
				}

				g_ptr_array_add (fts_props, NULL);
				g_ptr_array_add (fts_text, NULL);

				tracker_db_interface_sqlite_fts_delete_text (iface,
				                                             database,
				                                             data->resource_buffer->id,
				                                             (const gchar **) fts_props->pdata,
				                                             (const gchar **) fts_text->pdata);

				g_ptr_array_unref (fts_props);
				g_ptr_array_unref (fts_text);

				data->update_buffer.fts_ever_updated = TRUE;

				old_values = g_hash_table_lookup (data->resource_buffer->predicates, property);
			} else {
				old_values = get_property_values (data, property);
			}

			data->resource_buffer->fts_updated = TRUE;
		} else {
			old_values = get_property_values (data, property);
		}
	}

	return old_values;
}

static void
bytes_to_gvalue (GBytes              *bytes,
                 TrackerPropertyType  type,
                 GValue              *gvalue,
                 TrackerData         *data,
                 GError             **error)
{
	gint object_id;
	gchar *datetime;
	const gchar *value;

	value = g_bytes_get_data (bytes, NULL);

	switch (type) {
	case TRACKER_PROPERTY_TYPE_STRING:
		g_value_init (gvalue, G_TYPE_STRING);
		g_value_set_string (gvalue, value);
		break;
	case TRACKER_PROPERTY_TYPE_LANGSTRING:
		g_value_init (gvalue, G_TYPE_BYTES);
		g_value_set_boxed (gvalue, bytes);
		break;
	case TRACKER_PROPERTY_TYPE_INTEGER:
		g_value_init (gvalue, G_TYPE_INT64);
		g_value_set_int64 (gvalue, atoll (value));
		break;
	case TRACKER_PROPERTY_TYPE_BOOLEAN:
		/* use G_TYPE_INT64 to be compatible with value stored in DB
		   (important for value_equal function) */
		g_value_init (gvalue, G_TYPE_INT64);
		g_value_set_int64 (gvalue, g_ascii_strncasecmp (value, "true", 4) == 0);
		break;
	case TRACKER_PROPERTY_TYPE_DOUBLE:
		g_value_init (gvalue, G_TYPE_DOUBLE);
		g_value_set_double (gvalue, g_ascii_strtod (value, NULL));
		break;
	case TRACKER_PROPERTY_TYPE_DATE:
		g_value_init (gvalue, G_TYPE_INT64);
		datetime = g_strdup_printf ("%sT00:00:00Z", value);
		g_value_set_int64 (gvalue, tracker_string_to_date (datetime, NULL, error));
		g_free (datetime);
		break;
	case TRACKER_PROPERTY_TYPE_DATETIME:
		g_value_init (gvalue, TRACKER_TYPE_DATE_TIME);
		tracker_date_time_set_from_string (gvalue, value, error);
		break;
	case TRACKER_PROPERTY_TYPE_RESOURCE:
		object_id = ensure_resource_id (data, value, NULL);
		g_value_init (gvalue, G_TYPE_INT64);
		g_value_set_int64 (gvalue, object_id);
		break;
	default:
		g_warn_if_reached ();
		break;
	}
}

static const gchar *
get_bnode_for_resource (GHashTable      *bnodes,
                        TrackerData     *data,
                        TrackerResource *resource)
{
	TrackerDBInterface *iface;
	const gchar *identifier;
	gchar *bnode;

	bnode = g_hash_table_lookup (bnodes, resource);
	if (bnode)
		return bnode;

	iface = tracker_data_manager_get_writable_db_interface (data->manager);
	bnode = tracker_data_query_unused_uuid (data->manager,
	                                        iface);
	identifier = tracker_resource_get_identifier (resource);
	g_hash_table_insert (bnodes, g_strdup (identifier), bnode);

	return bnode;
}

static void
bytes_from_gvalue (GValue       *gvalue,
                   GBytes      **bytes,
                   TrackerData  *data,
                   GHashTable   *bnodes)
{
	gchar *str;

	if (G_VALUE_HOLDS_BOOLEAN (gvalue)) {
		if (g_value_get_boolean (gvalue)) {
			*bytes = g_bytes_new_static ("true", strlen ("true") + 1);
		} else {
			*bytes = g_bytes_new_static ("false", strlen ("false") + 1);
		}
	} else if (G_VALUE_HOLDS_INT (gvalue)) {
		str = g_strdup_printf ("%d", g_value_get_int (gvalue));
		*bytes = g_bytes_new_take (str, strlen (str) + 1);
	} else if (G_VALUE_HOLDS_INT64 (gvalue)) {
		str = g_strdup_printf ("%" G_GINT64_FORMAT, g_value_get_int64 (gvalue));
		*bytes = g_bytes_new_take (str, strlen (str) + 1);
	} else if (G_VALUE_HOLDS_DOUBLE (gvalue)) {
		gchar buffer[G_ASCII_DTOSTR_BUF_SIZE];
		g_ascii_dtostr (buffer, G_ASCII_DTOSTR_BUF_SIZE,
		                g_value_get_double (gvalue));
		*bytes = g_bytes_new (buffer, strlen (buffer) + 1);
	} else if (g_strcmp0 (G_VALUE_TYPE_NAME (gvalue), "TrackerUri") == 0) {
		/* FIXME: We can't access TrackerUri GType here */
		const gchar *uri;
		gchar *expanded;

		uri = g_value_get_string (gvalue);

		if (g_str_has_prefix (uri, "_:")) {
			gchar *bnode;

			bnode = g_hash_table_lookup (bnodes, uri);

			if (!bnode) {
				TrackerDBInterface *iface;

				iface = tracker_data_manager_get_writable_db_interface (data->manager);
				bnode = tracker_data_query_unused_uuid (data->manager,
				                                        iface);
				g_hash_table_insert (bnodes, g_strdup (uri), bnode);
			}

			*bytes = g_bytes_new (bnode, strlen (bnode) + 1);
		} else if (tracker_data_manager_expand_prefix (data->manager,
		                                               g_value_get_string (gvalue),
		                                               NULL, NULL,
		                                               &expanded)) {
			*bytes = g_bytes_new_take (expanded, strlen (expanded) + 1);
		} else {
			*bytes = g_bytes_new (uri, strlen (uri) + 1);
		}
	} else if (G_VALUE_HOLDS_STRING (gvalue)) {
		const gchar *ptr;
		ptr = g_value_get_string (gvalue);
		*bytes = g_bytes_new (ptr, strlen (ptr) + 1);
	} else if (G_VALUE_HOLDS (gvalue, TRACKER_TYPE_RESOURCE)) {
		TrackerResource *res;
		const gchar *object;

		res = g_value_get_object (gvalue);
		object = tracker_resource_get_identifier (res);

		if (!object || g_str_has_prefix (object, "_:"))
			object = get_bnode_for_resource (bnodes, data, res);

		*bytes = g_bytes_new (object, strlen (object) + 1);
	}
}

static gboolean
resource_in_domain_index_class (TrackerData  *data,
                                TrackerClass *domain_index_class)
{
	guint i;
	for (i = 0; i < data->resource_buffer->types->len; i++) {
		if (g_ptr_array_index (data->resource_buffer->types, i) == domain_index_class) {
			return TRUE;
		}
	}
	return FALSE;
}

static void
process_domain_indexes (TrackerData     *data,
                        TrackerProperty *property,
                        GValue          *gvalue,
                        const gchar     *field_name)
{
	TrackerClass **domain_index_classes;

	domain_index_classes = tracker_property_get_domain_indexes (property);
	while (*domain_index_classes) {
		if (resource_in_domain_index_class (data, *domain_index_classes)) {
			cache_insert_value (data,
			                    tracker_class_get_name (*domain_index_classes),
			                    field_name,
			                    gvalue,
			                    FALSE,
			                    tracker_property_get_fulltext_indexed (property),
			                    tracker_property_get_data_type (property) == TRACKER_PROPERTY_TYPE_DATETIME);
		}
		domain_index_classes++;
	}
}

static gboolean
cache_insert_metadata_decomposed (TrackerData      *data,
                                  TrackerProperty  *property,
                                  GBytes           *object,
                                  GError          **error)
{
	gboolean            multiple_values;
	const gchar        *table_name;
	const gchar        *field_name;
	TrackerProperty   **super_properties;
	GArray             *old_values;
	GError             *new_error = NULL;
	gboolean            change = FALSE;
	GValue              value = G_VALUE_INIT;

	/* read existing property values */
	old_values = get_old_property_values (data, property, &new_error);
	if (new_error) {
		g_propagate_error (error, new_error);
		return FALSE;
	}

	/* also insert super property values */
	super_properties = tracker_property_get_super_properties (property);
	multiple_values = tracker_property_get_multiple_values (property);

	while (*super_properties) {
		gboolean super_is_multi;
		GArray *super_old_values;

		super_is_multi = tracker_property_get_multiple_values (*super_properties);
		super_old_values = get_old_property_values (data, *super_properties, &new_error);
		if (new_error) {
			g_propagate_error (error, new_error);
			return FALSE;
		}

		if (super_is_multi || super_old_values->len == 0) {
			change |= cache_insert_metadata_decomposed (data, *super_properties, object,
			                                            &new_error);
			if (new_error) {
				g_propagate_error (error, new_error);
				return FALSE;
			}
		}
		super_properties++;
	}

	bytes_to_gvalue (object, tracker_property_get_data_type (property),
	                 &value, data, &new_error);
	if (new_error) {
		g_propagate_error (error, new_error);
		return FALSE;
	}

	table_name = tracker_property_get_table_name (property);
	field_name = tracker_property_get_name (property);

	if (!value_set_add_value (old_values, &value)) {
		/* value already inserted */
	} else if (!multiple_values && old_values->len > 1) {
		/* trying to add second value to single valued property */
		GValue old_value = { 0 };
		GValue new_value = { 0 };
		GValue *v;
		gchar *old_value_str = NULL;
		gchar *new_value_str = NULL;

		g_value_init (&old_value, G_TYPE_STRING);
		g_value_init (&new_value, G_TYPE_STRING);

		/* Get both old and new values as strings letting glib do
		 * whatever transformation needed */
		v = &g_array_index (old_values, GValue, 0);
		if (g_value_transform (v, &old_value)) {
			old_value_str = tracker_utf8_truncate (g_value_get_string (&old_value), 255);
		}

		v = &g_array_index (old_values, GValue, 1);
		if (g_value_transform (v, &new_value)) {
			new_value_str = tracker_utf8_truncate (g_value_get_string (&new_value), 255);
		}

		g_set_error (error, TRACKER_SPARQL_ERROR, TRACKER_SPARQL_ERROR_CONSTRAINT,
		             "Unable to insert multiple values for subject `%s' and single valued property `%s' "
		             "(old_value: '%s', new value: '%s')",
		             data->resource_buffer->subject,
		             field_name,
		             old_value_str ? old_value_str : "<untransformable>",
		             new_value_str ? new_value_str : "<untransformable>");

		g_free (old_value_str);
		g_free (new_value_str);
		g_value_unset (&old_value);
		g_value_unset (&new_value);
	} else {
		cache_insert_value (data, table_name, field_name,
		                    &value,
		                    multiple_values,
		                    tracker_property_get_fulltext_indexed (property),
		                    tracker_property_get_data_type (property) == TRACKER_PROPERTY_TYPE_DATETIME);

		if (!multiple_values) {
			process_domain_indexes (data, property, &value, field_name);
		}

		change = TRUE;
	}

	g_value_unset (&value);

	return change;
}

static gboolean
delete_metadata_decomposed (TrackerData      *data,
                            TrackerProperty  *property,
                            GBytes           *object,
                            GError          **error)
{
	gboolean            multiple_values;
	const gchar        *table_name;
	const gchar        *field_name;
	TrackerProperty   **super_properties;
	GArray             *old_values;
	GError             *new_error = NULL;
	gboolean            change = FALSE;
	GValue              value = G_VALUE_INIT;

	bytes_to_gvalue (object, tracker_property_get_data_type (property),
	                 &value, data, &new_error);
	if (new_error) {
		g_propagate_error (error, new_error);
		return FALSE;
	}

	multiple_values = tracker_property_get_multiple_values (property);
	table_name = tracker_property_get_table_name (property);
	field_name = tracker_property_get_name (property);

	/* read existing property values */
	old_values = get_old_property_values (data, property, &new_error);
	if (new_error) {
		/* no need to error out if statement does not exist for any reason */
		g_value_unset (&value);
		g_clear_error (&new_error);
		return FALSE;
	}

	if (!value_set_remove_value (old_values, &value)) {
		/* value not found */
	} else {
		cache_delete_value (data, table_name, field_name,
		                    &value, multiple_values,
		                    tracker_property_get_fulltext_indexed (property),
		                    tracker_property_get_data_type (property) == TRACKER_PROPERTY_TYPE_DATETIME);

		if (!multiple_values) {
			TrackerClass **domain_index_classes;

			domain_index_classes = tracker_property_get_domain_indexes (property);

			while (*domain_index_classes) {
				if (resource_in_domain_index_class (data, *domain_index_classes)) {
					cache_delete_value (data,
					                    tracker_class_get_name (*domain_index_classes),
					                    field_name,
					                    &value, multiple_values,
					                    tracker_property_get_fulltext_indexed (property),
					                    tracker_property_get_data_type (property) == TRACKER_PROPERTY_TYPE_DATETIME);
				}
				domain_index_classes++;
			}
		}

		change = TRUE;
	}

	g_value_unset (&value);

	/* also delete super property values */
	super_properties = tracker_property_get_super_properties (property);
	while (*super_properties) {
		change |= delete_metadata_decomposed (data, *super_properties, object, error);
		super_properties++;
	}

	return change;
}

static void
cache_delete_resource_type_full (TrackerData  *data,
                                 TrackerClass *class,
                                 gboolean      single_type)
{
	TrackerDBInterface *iface;
	TrackerDBStatement *stmt;
	TrackerDBCursor    *cursor = NULL;
	TrackerProperty   **properties, *prop;
	gboolean            found;
	gint                i;
	guint               p, n_props;
	GError             *error = NULL;
	TrackerOntologies  *ontologies;
	const gchar        *database;

	iface = tracker_data_manager_get_writable_db_interface (data->manager);
	ontologies = tracker_data_manager_get_ontologies (data->manager);
	database = data->resource_buffer->graph->graph ?
		data->resource_buffer->graph->graph : "main";

	if (!single_type) {
		if (strcmp (tracker_class_get_uri (class), TRACKER_PREFIX_RDFS "Resource") == 0 &&
		    g_hash_table_size (data->resource_buffer->tables) == 0) {
			tracker_db_interface_sqlite_fts_delete_id (iface, database, data->resource_buffer->id);
			/* skip subclass query when deleting whole resource
			   to improve performance */

			while (data->resource_buffer->types->len > 0) {
				TrackerClass *type;

				type = g_ptr_array_index (data->resource_buffer->types,
				                          data->resource_buffer->types->len - 1);
				cache_delete_resource_type_full (data, type, TRUE);
			}

			return;
		}

		found = FALSE;
		for (i = 0; i < data->resource_buffer->types->len; i++) {
			if (g_ptr_array_index (data->resource_buffer->types, i) == class) {
				found = TRUE;
				break;
			}
		}

		if (!found) {
			/* type not found, nothing to do */
			return;
		}

		/* retrieve all subclasses we need to remove from the subject
		 * before we can remove the class specified as object of the statement */
		stmt = tracker_db_interface_create_statement (iface, TRACKER_DB_STATEMENT_CACHE_TYPE_SELECT, &error,
			                                      "SELECT (SELECT Uri FROM Resource WHERE ID = subclass.ID) "
			                                      "FROM \"%s\".\"rdfs:Resource_rdf:type\" AS type INNER JOIN \"%s\".\"rdfs:Class_rdfs:subClassOf\" AS subclass ON (type.\"rdf:type\" = subclass.ID) "
		                                              "WHERE type.ID = ? AND subclass.\"rdfs:subClassOf\" = (SELECT ID FROM Resource WHERE Uri = ?)",
		                                              database, database);

		if (stmt) {
			tracker_db_statement_bind_int (stmt, 0, data->resource_buffer->id);
			tracker_db_statement_bind_text (stmt, 1, tracker_class_get_uri (class));
			cursor = tracker_db_statement_start_cursor (stmt, &error);
			g_object_unref (stmt);
		}

		if (cursor) {
			while (tracker_db_cursor_iter_next (cursor, NULL, &error)) {
				const gchar *class_uri;

				class_uri = tracker_db_cursor_get_string (cursor, 0, NULL);
				cache_delete_resource_type_full (data, tracker_ontologies_get_class_by_uri (ontologies, class_uri),
					                         FALSE);
			}

			g_object_unref (cursor);
		}

		if (error) {
			g_warning ("Could not delete cache resource (selecting subclasses): %s", error->message);
			g_error_free (error);
			error = NULL;
		}
	}

	/* delete all property values */

	properties = tracker_ontologies_get_properties (ontologies, &n_props);

	for (p = 0; p < n_props; p++) {
		gboolean            multiple_values;
		const gchar        *table_name;
		const gchar        *field_name;
		GArray *old_values;
		gint                y;

		prop = properties[p];

		if (tracker_property_get_domain (prop) != class) {
			continue;
		}

		multiple_values = tracker_property_get_multiple_values (prop);
		table_name = tracker_property_get_table_name (prop);
		field_name = tracker_property_get_name (prop);

		old_values = get_old_property_values (data, prop, NULL);

		for (y = old_values->len - 1; y >= 0 ; y--) {
			GValue *old_gvalue, copy = G_VALUE_INIT;

			old_gvalue = &g_array_index (old_values, GValue, y);
			g_value_init (&copy, G_VALUE_TYPE (old_gvalue));
			g_value_copy (old_gvalue, &copy);

			value_set_remove_value (old_values, old_gvalue);
			cache_delete_value (data, table_name, field_name,
			                    &copy, multiple_values,
			                    tracker_property_get_fulltext_indexed (prop),
			                    tracker_property_get_data_type (prop) == TRACKER_PROPERTY_TYPE_DATETIME);

			if (!multiple_values) {
				TrackerClass **domain_index_classes;

				domain_index_classes = tracker_property_get_domain_indexes (prop);
				while (*domain_index_classes) {
					if (resource_in_domain_index_class (data, *domain_index_classes)) {
						cache_delete_value (data,
						                    tracker_class_get_name (*domain_index_classes),
						                    field_name,
						                    &copy, multiple_values,
						                    tracker_property_get_fulltext_indexed (prop),
						                    tracker_property_get_data_type (prop) == TRACKER_PROPERTY_TYPE_DATETIME);
					}
					domain_index_classes++;
				}
			}

			g_value_unset (&copy);
		}
	}

	cache_delete_row (data, class);

	tracker_data_dispatch_delete_statement_callbacks (data,
	                                                  tracker_property_get_id (tracker_ontologies_get_rdf_type (ontologies)),
	                                                  tracker_class_get_id (class),
	                                                  tracker_class_get_uri (class));

	g_ptr_array_remove (data->resource_buffer->types, class);
}

static void
cache_delete_resource_type (TrackerData  *data,
                            TrackerClass *class)
{
	cache_delete_resource_type_full (data, class, FALSE);
}

static TrackerDataUpdateBufferGraph *
ensure_graph_buffer (TrackerDataUpdateBuffer  *buffer,
                     TrackerData              *data,
                     const gchar              *name,
                     GError                  **error)
{
	TrackerDataUpdateBufferGraph *graph_buffer;
	gint i;

	for (i = 0; i < buffer->graphs->len; i++) {
		graph_buffer = g_ptr_array_index (buffer->graphs, i);
		if (g_strcmp0 (graph_buffer->graph, name) == 0)
			return graph_buffer;
	}

	if (name && !tracker_data_manager_find_graph (data->manager, name, TRUE)) {
		if (!tracker_data_manager_create_graph (data->manager, name, error))
			return NULL;
	}

	graph_buffer = g_slice_new0 (TrackerDataUpdateBufferGraph);
	graph_buffer->graph = g_strdup (name);
	if (graph_buffer->graph) {
		graph_buffer->id = tracker_data_manager_find_graph (data->manager,
		                                                    graph_buffer->graph,
		                                                    TRUE);
	}

	graph_buffer->resources =
		g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
		                       (GDestroyNotify) resource_buffer_free);
	g_ptr_array_add (buffer->graphs, graph_buffer);

	return graph_buffer;
}

static gboolean
resource_buffer_switch (TrackerData  *data,
                        const gchar  *graph,
                        const gchar  *subject,
                        gint          subject_id,
                        GError      **error)
{
	TrackerDataUpdateBufferGraph *graph_buffer;

	if (data->resource_buffer != NULL &&
	    g_strcmp0 (data->resource_buffer->graph->graph, graph) == 0 &&
	    strcmp (data->resource_buffer->subject, subject) == 0) {
		/* Resource buffer stays the same */
		return TRUE;
	}

	/* large INSERTs with thousands of resources could lead to
	   high peak memory usage due to the update buffer
	   flush the buffer if it already contains 1000 resources */
	tracker_data_update_buffer_might_flush (data, NULL);

	data->resource_buffer = NULL;

	graph_buffer = ensure_graph_buffer (&data->update_buffer, data, graph, error);
	if (!graph_buffer)
		return FALSE;

	data->resource_buffer =
		g_hash_table_lookup (graph_buffer->resources, subject);

	if (data->resource_buffer == NULL) {
		TrackerDataUpdateBufferResource *resource_buffer;
		gchar *subject_dup = NULL;

		/* subject not yet in cache, retrieve or create ID */
		resource_buffer = g_slice_new0 (TrackerDataUpdateBufferResource);
		if (subject != NULL) {
			subject_dup = g_strdup (subject);
			resource_buffer->subject = subject_dup;
		}
		if (subject_id > 0) {
			resource_buffer->id = subject_id;
		} else {
			resource_buffer->id = ensure_resource_id (data, resource_buffer->subject, &resource_buffer->create);
		}
		resource_buffer->fts_updated = FALSE;
		if (resource_buffer->create) {
			resource_buffer->types = g_ptr_array_new ();
		} else {
			resource_buffer->types = tracker_data_query_rdf_type (data->manager, graph, resource_buffer->id);
		}
		resource_buffer->predicates = g_hash_table_new_full (g_direct_hash, g_direct_equal, g_object_unref, (GDestroyNotify) g_array_unref);
		resource_buffer->tables = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) cache_table_free);
		resource_buffer->graph = graph_buffer;

		g_hash_table_insert (graph_buffer->resources, subject_dup, resource_buffer);

		data->resource_buffer = resource_buffer;
	}

	return TRUE;
}

void
tracker_data_delete_statement (TrackerData  *data,
                               const gchar  *graph,
                               const gchar  *subject,
                               const gchar  *predicate,
                               GBytes       *object,
                               GError      **error)
{
	TrackerClass       *class;
	gint                subject_id = 0;
	gboolean            change = FALSE;
	TrackerOntologies  *ontologies;
	const gchar *object_str;

	g_return_if_fail (subject != NULL);
	g_return_if_fail (predicate != NULL);
	g_return_if_fail (object != NULL);
	g_return_if_fail (data->in_transaction);

	subject_id = query_resource_id (data, subject);

	if (subject_id == 0) {
		/* subject not in database */
		return;
	}

	if (!resource_buffer_switch (data, graph, subject, subject_id, error))
		return;

	ontologies = tracker_data_manager_get_ontologies (data->manager);

	object_str = g_bytes_get_data (object, NULL);

	if (object && g_strcmp0 (predicate, TRACKER_PREFIX_RDF "type") == 0) {
		class = tracker_ontologies_get_class_by_uri (ontologies, object_str);
		if (class != NULL) {
			data->has_persistent = TRUE;
			cache_delete_resource_type (data, class);
		} else {
			g_set_error (error, TRACKER_SPARQL_ERROR, TRACKER_SPARQL_ERROR_UNKNOWN_CLASS,
			             "Class '%s' not found in the ontology", object_str);
		}
	} else {
		gint pred_id = 0, object_id = 0;
		TrackerProperty *field;

		field = tracker_ontologies_get_property_by_uri (ontologies, predicate);
		if (field != NULL) {
			pred_id = tracker_property_get_id (field);
			data->has_persistent = TRUE;
			change = delete_metadata_decomposed (data, field, object, error);
		} else {
			g_set_error (error, TRACKER_SPARQL_ERROR, TRACKER_SPARQL_ERROR_UNKNOWN_PROPERTY,
			             "Property '%s' not found in the ontology", predicate);
			return;
		}

		if (change) {
			tracker_data_dispatch_delete_statement_callbacks (data,
			                                                  pred_id,
			                                                  object_id,
			                                                  object_str);
		}
	}
}

static void
tracker_data_delete_all (TrackerData  *data,
                         const gchar  *graph,
                         const gchar  *subject,
                         const gchar  *predicate,
                         GError      **error)
{
	gint subject_id = 0;
	TrackerOntologies *ontologies;
	TrackerProperty *property;
	GArray *old_values;
	GError *inner_error = NULL;
	guint i;

	g_return_if_fail (subject != NULL);
	g_return_if_fail (predicate != NULL);
	g_return_if_fail (data->in_transaction);

	subject_id = query_resource_id (data, subject);

	if (subject_id == 0) {
		/* subject not in database */
		return;
	}

	if (!resource_buffer_switch (data, graph, subject, subject_id, error))
		return;

	ontologies = tracker_data_manager_get_ontologies (data->manager);
	property = tracker_ontologies_get_property_by_uri (ontologies,
	                                                   predicate);
	old_values = get_old_property_values (data, property, &inner_error);
	if (inner_error) {
		g_propagate_error (error, inner_error);
		return;
	}

	for (i = 0; i < old_values->len; i++) {
		GValue *value;
		GBytes *bytes;

		value = &g_array_index (old_values, GValue, i);
		bytes_from_gvalue (value,
		                   &bytes,
		                   data,
		                   NULL);

		tracker_data_delete_statement (data, graph, subject,
		                               predicate, bytes,
		                               &inner_error);
		g_bytes_unref (bytes);

		if (inner_error)
			break;
	}

	if (inner_error)
		g_propagate_error (error, inner_error);
}

static gboolean
delete_single_valued (TrackerData  *data,
                      const gchar  *graph,
                      const gchar  *subject,
                      const gchar  *predicate,
                      gboolean      super_is_single_valued,
                      GError      **error)
{
	TrackerProperty *field, **super_properties;
	TrackerOntologies *ontologies;
	gboolean multiple_values;

	ontologies = tracker_data_manager_get_ontologies (data->manager);
	field = tracker_ontologies_get_property_by_uri (ontologies, predicate);
	super_properties = tracker_property_get_super_properties (field);
	multiple_values = tracker_property_get_multiple_values (field);

	if (super_is_single_valued && multiple_values) {
		cache_delete_all_values (data,
		                         tracker_property_get_table_name (field),
		                         tracker_property_get_name (field),
		                         tracker_property_get_fulltext_indexed (field),
		                         tracker_property_get_data_type (field) == TRACKER_PROPERTY_TYPE_DATETIME);
	} else if (!multiple_values) {
		GError *inner_error = NULL;
		GArray *old_values;

		old_values = get_old_property_values (data, field, &inner_error);

		if (old_values && old_values->len == 1) {
			cache_delete_value (data,
			                    tracker_property_get_table_name (field),
			                    tracker_property_get_name (field),
			                    &g_array_index (old_values, GValue, 0),
			                    FALSE,
			                    tracker_property_get_fulltext_indexed (field),
			                    tracker_property_get_data_type (field) == TRACKER_PROPERTY_TYPE_DATETIME);
		} else {
			/* no need to error out if statement does not exist for any reason */
			g_clear_error (&inner_error);
		}
	}

	while (*super_properties) {
		if (!delete_single_valued (data, graph, subject,
		                           tracker_property_get_uri (*super_properties),
		                           super_is_single_valued,
		                           error))
			return FALSE;

		super_properties++;
	}

	return TRUE;
}

void
tracker_data_insert_statement (TrackerData  *data,
                               const gchar  *graph,
                               const gchar  *subject,
                               const gchar  *predicate,
                               GBytes       *object,
                               GError      **error)
{
	TrackerProperty *property;
	TrackerOntologies *ontologies;

	g_return_if_fail (subject != NULL);
	g_return_if_fail (predicate != NULL);
	g_return_if_fail (object != NULL);
	g_return_if_fail (data->in_transaction);

	ontologies = tracker_data_manager_get_ontologies (data->manager);

	property = tracker_ontologies_get_property_by_uri (ontologies, predicate);
	if (property != NULL) {
		if (tracker_property_get_data_type (property) == TRACKER_PROPERTY_TYPE_RESOURCE) {
			tracker_data_insert_statement_with_uri (data, graph, subject, predicate, object, error);
		} else {
			tracker_data_insert_statement_with_string (data, graph, subject, predicate, object, error);
		}
	} else {
		g_set_error (error, TRACKER_SPARQL_ERROR, TRACKER_SPARQL_ERROR_UNKNOWN_PROPERTY,
		             "Property '%s' not found in the ontology", predicate);
	}
}

void
tracker_data_insert_statement_with_uri (TrackerData  *data,
                                        const gchar  *graph,
                                        const gchar  *subject,
                                        const gchar  *predicate,
                                        GBytes       *object,
                                        GError      **error)
{
	GError          *actual_error = NULL;
	TrackerClass    *class;
	TrackerProperty *property;
	gint             prop_id = 0;
	gint             final_prop_id = 0, object_id = 0;
	gboolean change = FALSE;
	TrackerOntologies *ontologies;
	TrackerDBInterface *iface;
	const gchar *object_str;

	g_return_if_fail (subject != NULL);
	g_return_if_fail (predicate != NULL);
	g_return_if_fail (object != NULL);
	g_return_if_fail (data->in_transaction);

	ontologies = tracker_data_manager_get_ontologies (data->manager);
	iface = tracker_data_manager_get_writable_db_interface (data->manager);

	property = tracker_ontologies_get_property_by_uri (ontologies, predicate);
	if (property == NULL) {
		g_set_error (error, TRACKER_SPARQL_ERROR, TRACKER_SPARQL_ERROR_UNKNOWN_PROPERTY,
		             "Property '%s' not found in the ontology", predicate);
		return;
	} else {
		if (tracker_property_get_data_type (property) != TRACKER_PROPERTY_TYPE_RESOURCE) {
			g_set_error (error, TRACKER_SPARQL_ERROR, TRACKER_SPARQL_ERROR_TYPE,
			             "Property '%s' does not accept URIs", predicate);
			return;
		}
		prop_id = tracker_property_get_id (property);
	}

	data->has_persistent = TRUE;

	if (!resource_buffer_switch (data, graph, subject, 0, error))
		return;

	object_str = g_bytes_get_data (object, NULL);

	if (property == tracker_ontologies_get_rdf_type (ontologies)) {
		/* handle rdf:type statements specially to
		   cope with inference and insert blank rows */
		class = tracker_ontologies_get_class_by_uri (ontologies, object_str);
		if (class != NULL) {
			cache_create_service_decomposed (data, class);
		} else {
			g_set_error (error, TRACKER_SPARQL_ERROR, TRACKER_SPARQL_ERROR_UNKNOWN_CLASS,
			             "Class '%s' not found in the ontology", object_str);
			return;
		}

		final_prop_id = (prop_id != 0) ? prop_id : tracker_data_query_resource_id (data->manager, iface, predicate);
		object_id = query_resource_id (data, object_str);

		change = TRUE;
	} else {
		/* add value to metadata database */
		change = cache_insert_metadata_decomposed (data, property, object, &actual_error);

		if (actual_error) {
			g_propagate_error (error, actual_error);
			return;
		}

		if (change) {
			final_prop_id = (prop_id != 0) ? prop_id : tracker_data_query_resource_id (data->manager, iface, predicate);
			object_id = query_resource_id (data, object_str);

			tracker_data_dispatch_insert_statement_callbacks (data,
			                                                  final_prop_id,
			                                                  object_id,
			                                                  object_str);
		}
	}
}

void
tracker_data_insert_statement_with_string (TrackerData  *data,
                                           const gchar  *graph,
                                           const gchar  *subject,
                                           const gchar  *predicate,
                                           GBytes       *object,
                                           GError      **error)
{
	GError          *actual_error = NULL;
	TrackerProperty *property;
	gboolean         change;
	gint             pred_id = 0;
	TrackerOntologies *ontologies;
	TrackerDBInterface *iface;
	const gchar *object_str;

	g_return_if_fail (subject != NULL);
	g_return_if_fail (predicate != NULL);
	g_return_if_fail (object != NULL);
	g_return_if_fail (data->in_transaction);

	ontologies = tracker_data_manager_get_ontologies (data->manager);
	iface = tracker_data_manager_get_writable_db_interface (data->manager);

	property = tracker_ontologies_get_property_by_uri (ontologies, predicate);
	if (property == NULL) {
		g_set_error (error, TRACKER_SPARQL_ERROR, TRACKER_SPARQL_ERROR_UNKNOWN_PROPERTY,
		             "Property '%s' not found in the ontology", predicate);
		return;
	} else {
		if (tracker_property_get_data_type (property) == TRACKER_PROPERTY_TYPE_RESOURCE) {
			g_set_error (error, TRACKER_SPARQL_ERROR, TRACKER_SPARQL_ERROR_TYPE,
			             "Property '%s' only accepts URIs", predicate);
			return;
		}
		pred_id = tracker_property_get_id (property);
	}

	data->has_persistent = TRUE;

	if (!resource_buffer_switch (data, graph, subject, 0, error))
		return;

	/* add value to metadata database */
	change = cache_insert_metadata_decomposed (data, property, object, &actual_error);

	if (actual_error) {
		g_propagate_error (error, actual_error);
		return;
	}

	if (change) {
		pred_id = (pred_id != 0) ? pred_id : tracker_data_query_resource_id (data->manager, iface, predicate);
		object_str = g_bytes_get_data (object, NULL);

		tracker_data_dispatch_insert_statement_callbacks (data,
		                                                  pred_id,
		                                                  0, /* Always a literal */
		                                                  object_str);
	}
}

void
tracker_data_update_statement (TrackerData  *data,
                               const gchar  *graph,
                               const gchar  *subject,
                               const gchar  *predicate,
                               GBytes       *object,
                               GError      **error)
{
	TrackerProperty *property;
	TrackerOntologies *ontologies;
	GError *new_error = NULL;

	g_return_if_fail (subject != NULL);
	g_return_if_fail (predicate != NULL);
	g_return_if_fail (data->in_transaction);

	ontologies = tracker_data_manager_get_ontologies (data->manager);
	property = tracker_ontologies_get_property_by_uri (ontologies, predicate);

	if (property != NULL) {
		if (object == NULL) {
			if (property == tracker_ontologies_get_rdf_type (ontologies)) {
				g_set_error (error, TRACKER_SPARQL_ERROR, TRACKER_SPARQL_ERROR_UNSUPPORTED,
				             "Using 'null' with '%s' is not supported", predicate);
				return;
			}

			/* Flush upfront to make a null,x,null,y,z work: When x is set then
			 * if a null comes, we need to be flushed */

			tracker_data_update_buffer_flush (data, &new_error);
			if (new_error) {
				g_propagate_error (error, new_error);
				return;
			}

			if (!resource_buffer_switch (data, graph, subject,
			                             ensure_resource_id (data, subject, NULL),
			                             error))
				return;

			cache_delete_all_values (data,
			                         tracker_property_get_table_name (property),
			                         tracker_property_get_name (property),
			                         tracker_property_get_fulltext_indexed (property),
			                         tracker_property_get_data_type (property) == TRACKER_PROPERTY_TYPE_DATETIME);
		} else {
			if (!resource_buffer_switch (data, graph, subject,
			                             ensure_resource_id (data, subject, NULL),
			                             error))
				return;

			if (!delete_single_valued (data, graph, subject, predicate,
			                           !tracker_property_get_multiple_values (property),
			                           error))
				return;

			tracker_data_update_buffer_flush (data, &new_error);
			if (new_error) {
				g_propagate_error (error, new_error);
				return;
			}

			if (tracker_property_get_data_type (property) == TRACKER_PROPERTY_TYPE_RESOURCE) {
				tracker_data_insert_statement_with_uri (data, graph, subject, predicate, object, error);
			} else {
				tracker_data_insert_statement_with_string (data, graph, subject, predicate, object, error);
			}
		}

		tracker_data_update_buffer_flush (data, &new_error);
		if (new_error) {
			g_propagate_error (error, new_error);
		}
	} else {
		g_set_error (error, TRACKER_SPARQL_ERROR, TRACKER_SPARQL_ERROR_UNKNOWN_PROPERTY,
		             "Property '%s' not found in the ontology", predicate);
	}
}

void
tracker_data_begin_transaction (TrackerData  *data,
                                GError      **error)
{
	TrackerDBInterface *iface;
	TrackerDBManager *db_manager;

	g_return_if_fail (!data->in_transaction);

	db_manager = tracker_data_manager_get_db_manager (data->manager);

	if (!tracker_db_manager_has_enough_space (db_manager)) {
		g_set_error (error, TRACKER_SPARQL_ERROR, TRACKER_SPARQL_ERROR_NO_SPACE,
			"There is not enough space on the file system for update operations");
		return;
	}

	data->resource_time = time (NULL);

	data->has_persistent = FALSE;

	if (data->update_buffer.resource_cache == NULL) {
		data->update_buffer.resource_cache = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
		/* used for normal transactions */
		data->update_buffer.graphs = g_ptr_array_new_with_free_func ((GDestroyNotify) graph_buffer_free);
	}

	data->resource_buffer = NULL;

	iface = tracker_data_manager_get_writable_db_interface (data->manager);

	tracker_db_interface_execute_query (iface, NULL, "PRAGMA cache_size = %d", TRACKER_DB_CACHE_SIZE_UPDATE);

	tracker_db_interface_start_transaction (iface);

	data->in_transaction = TRUE;
}

void
tracker_data_begin_ontology_transaction (TrackerData  *data,
                                         GError      **error)
{
	data->in_ontology_transaction = TRUE;
	tracker_data_begin_transaction (data, error);
}

void
tracker_data_commit_transaction (TrackerData  *data,
                                 GError      **error)
{
	TrackerDBInterface *iface;
	GError *actual_error = NULL;

	g_return_if_fail (data->in_transaction);

	iface = tracker_data_manager_get_writable_db_interface (data->manager);

	tracker_data_update_buffer_flush (data, &actual_error);
	if (actual_error) {
		tracker_data_rollback_transaction (data);
		g_propagate_error (error, actual_error);
		return;
	}

	tracker_db_interface_end_db_transaction (iface,
	                                         &actual_error);

	if (actual_error) {
		tracker_data_rollback_transaction (data);
		g_propagate_error (error, actual_error);
		return;
	}

	get_transaction_modseq (data);
	if (data->has_persistent && !data->in_ontology_transaction) {
		data->transaction_modseq++;
	}

	data->resource_time = 0;
	data->in_transaction = FALSE;
	data->in_ontology_transaction = FALSE;

	if (data->update_buffer.fts_ever_updated) {
		data->update_buffer.fts_ever_updated = FALSE;
	}

	tracker_data_manager_commit_graphs (data->manager);

	tracker_db_interface_execute_query (iface, NULL, "PRAGMA cache_size = %d", TRACKER_DB_CACHE_SIZE_DEFAULT);

	g_ptr_array_set_size (data->update_buffer.graphs, 0);
	g_hash_table_remove_all (data->update_buffer.resource_cache);

	tracker_data_dispatch_commit_statement_callbacks (data);
}

void
tracker_data_rollback_transaction (TrackerData *data)
{
	TrackerDBInterface *iface;
	GError *ignorable = NULL;

	g_return_if_fail (data->in_transaction);

	data->in_transaction = FALSE;
	data->in_ontology_transaction = FALSE;

	iface = tracker_data_manager_get_writable_db_interface (data->manager);

	tracker_data_update_buffer_clear (data);

	tracker_db_interface_execute_query (iface, &ignorable, "ROLLBACK");

	if (ignorable) {
		g_warning ("Transaction rollback failed: %s\n", ignorable->message);
		g_clear_error (&ignorable);
	}

	tracker_data_manager_rollback_graphs (data->manager);

	tracker_db_interface_execute_query (iface, NULL, "PRAGMA cache_size = %d", TRACKER_DB_CACHE_SIZE_DEFAULT);

	tracker_data_dispatch_rollback_statement_callbacks (data);
}

static GVariant *
update_sparql (TrackerData  *data,
               const gchar  *update,
               gboolean      blank,
               GError      **error)
{
	GError *actual_error = NULL;
	TrackerSparql *sparql_query;
	GVariant *blank_nodes;

	g_return_val_if_fail (update != NULL, NULL);

#ifdef G_ENABLE_DEBUG
	if (TRACKER_DEBUG_CHECK (SPARQL)) {
		gchar *update_to_print;

		update_to_print = g_strdup (update);
		g_strdelimit (update_to_print, "\n", ' ');
		g_message ("[SPARQL] %s", update_to_print);
		g_free (update_to_print);
	}
#endif

	tracker_data_begin_transaction (data, &actual_error);
	if (actual_error) {
		g_propagate_error (error, actual_error);
		return NULL;
	}

	sparql_query = tracker_sparql_new_update (data->manager, update);
	blank_nodes = tracker_sparql_execute_update (sparql_query, blank, NULL, &actual_error);
	g_object_unref (sparql_query);

	if (actual_error) {
		tracker_data_rollback_transaction (data);
		g_propagate_error (error, actual_error);
		return NULL;
	}

	tracker_data_commit_transaction (data, &actual_error);
	if (actual_error) {
		g_propagate_error (error, actual_error);
		return NULL;
	}

	return blank_nodes;
}

void
tracker_data_update_sparql (TrackerData  *data,
                            const gchar  *update,
                            GError      **error)
{
	update_sparql (data, update, FALSE, error);
}

GVariant *
tracker_data_update_sparql_blank (TrackerData  *data,
                                  const gchar  *update,
                                  GError      **error)
{
	return update_sparql (data, update, TRUE, error);
}

void
tracker_data_load_turtle_file (TrackerData  *data,
                               GFile        *file,
                               const gchar  *graph,
                               GError      **error)
{
	TrackerTurtleReader *reader = NULL;
	GError *inner_error = NULL;
	const gchar *subject, *predicate, *object_str, *langtag;
	gboolean object_is_uri;

	reader = tracker_turtle_reader_new_for_file (file, &inner_error);
	if (inner_error)
		goto failed;

	while (tracker_turtle_reader_next (reader,
	                                   &subject,
	                                   &predicate,
	                                   &object_str,
	                                   &langtag,
	                                   &object_is_uri,
	                                   &inner_error)) {
		GBytes *object;

		object = tracker_sparql_make_langstring (object_str, langtag);

		if (object_is_uri) {
			tracker_data_insert_statement_with_uri (data, graph,
			                                        subject, predicate, object,
								&inner_error);
		} else {
			tracker_data_insert_statement_with_string (data, graph,
			                                           subject, predicate, object,
								   &inner_error);
		}

		g_bytes_unref (object);

		if (inner_error)
			goto failed;

		tracker_data_update_buffer_might_flush (data, &inner_error);

		if (inner_error)
			goto failed;
	}

	if (inner_error)
		goto failed;

	g_clear_object (&reader);

	return;

failed:
	g_clear_object (&reader);

	g_propagate_error (error, inner_error);
}

gint
tracker_data_ensure_graph (TrackerData  *data,
                           const gchar  *uri,
                           GError      **error)
{
	TrackerDBInterface *iface;
	TrackerDBStatement *stmt;
	gint id;

	id = ensure_resource_id (data, uri, NULL);
	iface = tracker_data_manager_get_writable_db_interface (data->manager);
	stmt = tracker_db_interface_create_statement (iface, TRACKER_DB_STATEMENT_CACHE_TYPE_UPDATE, error,
	                                              "INSERT OR IGNORE INTO Graph (ID) VALUES (?)");
	if (!stmt)
		return 0;

	tracker_db_statement_bind_int (stmt, 0, id);
	tracker_db_statement_execute (stmt, error);
	g_object_unref (stmt);

	return id;
}

gboolean
tracker_data_delete_graph (TrackerData  *data,
                           const gchar  *uri,
                           GError      **error)
{
	TrackerDBInterface *iface;
	TrackerDBStatement *stmt;
	gint id;

	id = query_resource_id (data, uri);
	iface = tracker_data_manager_get_writable_db_interface (data->manager);
	stmt = tracker_db_interface_create_statement (iface, TRACKER_DB_STATEMENT_CACHE_TYPE_UPDATE, error,
	                                              "DELETE FROM Graph WHERE ID = ?");
	if (!stmt)
		return FALSE;

	tracker_db_statement_bind_int (stmt, 0, id);
	tracker_db_statement_execute (stmt, error);
	g_object_unref (stmt);

	return TRUE;
}

static gboolean
resource_maybe_reset_property (TrackerData      *data,
                               const gchar      *graph,
                               TrackerResource  *resource,
                               const gchar      *subject_uri,
                               const gchar      *property_uri,
                               GHashTable       *bnodes,
                               GError          **error)
{
	GError *inner_error = NULL;
	const gchar *subject;

	/* If the subject is a blank node, this is a whole new insertion.
	 * We don't need deleting anything then.
	 */
	subject = tracker_resource_get_identifier (resource);
	if (g_str_has_prefix (subject, "_:"))
		return TRUE;

	tracker_data_delete_all (data,
	                         graph, subject_uri, property_uri,
	                         &inner_error);
	if (inner_error) {
		g_propagate_error (error, inner_error);
		return FALSE;
	}

	return TRUE;
}

static gboolean
update_resource_property (TrackerData      *data,
                          const gchar      *graph_uri,
                          TrackerResource  *resource,
                          const gchar      *subject,
                          const gchar      *property,
                          GHashTable       *visited,
                          GHashTable       *bnodes,
                          GError          **error)
{
	GList *values, *v;
	gchar *property_uri;
	GError *inner_error = NULL;

	values = tracker_resource_get_values (resource, property);
	tracker_data_manager_expand_prefix (data->manager,
	                                    property,
	                                    NULL, NULL,
	                                    &property_uri);

	if (tracker_resource_get_property_overwrite (resource, property) &&
	    !resource_maybe_reset_property (data, graph_uri, resource,
	                                    subject, property_uri,
	                                    bnodes, error)) {
		g_free (property_uri);
		return FALSE;
	}

	for (v = values; v && !inner_error; v = v->next) {
		GBytes *bytes = NULL;

		if (G_VALUE_HOLDS (v->data, TRACKER_TYPE_RESOURCE)) {
			update_resource_single (data,
			                        graph_uri,
			                        g_value_get_object (v->data),
			                        visited,
			                        bnodes,
			                        &inner_error);
			if (inner_error)
				break;
		}

		bytes_from_gvalue (v->data,
		                   &bytes,
		                   data,
		                   bnodes);

		tracker_data_insert_statement (data,
		                               graph_uri,
		                               subject,
		                               property_uri,
		                               bytes,
		                               &inner_error);
		g_bytes_unref (bytes);
	}

	g_list_free (values);
	g_free (property_uri);

	if (inner_error) {
		g_propagate_error (error, inner_error);
		return FALSE;
	}

	return TRUE;
}

static gboolean
update_resource_single (TrackerData      *data,
                        const gchar      *graph,
                        TrackerResource  *resource,
                        GHashTable       *visited,
                        GHashTable       *bnodes,
                        GError          **error)
{
	GList *properties, *l;
	GError *inner_error = NULL;
	const gchar *subject;
	gchar *graph_uri = NULL;

	if (g_hash_table_lookup (visited, resource))
		return TRUE;

	g_hash_table_add (visited, resource);

	properties = tracker_resource_get_properties (resource);

	subject = tracker_resource_get_identifier (resource);
	if (!subject || g_str_has_prefix (subject, "_:"))
		subject = get_bnode_for_resource (bnodes, data, resource);

	if (graph) {
		tracker_data_manager_expand_prefix (data->manager,
		                                    graph, NULL, NULL,
		                                    &graph_uri);
	}

	/* Handle rdf:type first */
	if (g_list_find_custom (properties, "rdf:type", (GCompareFunc) g_strcmp0)) {
		update_resource_property (data, graph_uri, resource,
		                          subject, "rdf:type",
		                          visited, bnodes,
		                          &inner_error);

		if (inner_error) {
			g_propagate_error (error, inner_error);
			return FALSE;
		}
	}

	for (l = properties; l; l = l->next) {
		if (g_str_equal (l->data, "rdf:type"))
			continue;

		if (!update_resource_property (data, graph_uri, resource,
		                               subject, l->data,
		                               visited, bnodes,
		                               &inner_error))
			break;
	}

	g_list_free (properties);

	if (inner_error) {
		g_propagate_error (error, inner_error);
		return FALSE;
	}

	return TRUE;
}

gboolean
tracker_data_update_resource (TrackerData      *data,
                              const gchar      *graph,
                              TrackerResource  *resource,
                              GHashTable       *bnodes,
                              GError          **error)
{
	GHashTable *visited;
	gboolean retval;

	visited = g_hash_table_new (NULL, NULL);

	if (bnodes)
		g_hash_table_ref (bnodes);
	else
		bnodes = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

	retval = update_resource_single (data, graph, resource, visited, bnodes, error);

	g_hash_table_unref (visited);
	g_hash_table_unref (bnodes);

	return retval;
}
