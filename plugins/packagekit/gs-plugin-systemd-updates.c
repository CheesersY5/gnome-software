/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 * vi:set noexpandtab tabstop=8 shiftwidth=8:
 *
 * Copyright (C) 2013 Richard Hughes <richard@hughsie.com>
 * Copyright (C) 2015-2016 Kalev Lember <klember@redhat.com>
 *
 * SPDX-License-Identifier: GPL-2.0+
 */

#include <config.h>

#include <packagekit-glib2/packagekit.h>

#include "packagekit-common.h"

#include <gnome-software.h>

/*
 * Mark previously downloaded packages as zero size, and also allow
 * scheduling the offline update.
 */

struct GsPluginData {
	GFileMonitor		*monitor;
	GFileMonitor		*monitor_trigger;
	GPermission		*permission;
	gboolean		 is_triggered;
	GHashTable		*hash_prepared;
	GMutex			 hash_prepared_mutex;
};

void
gs_plugin_initialize (GsPlugin *plugin)
{
	GsPluginData *priv = gs_plugin_alloc_data (plugin, sizeof(GsPluginData));
	gs_plugin_add_rule (plugin, GS_PLUGIN_RULE_RUN_AFTER, "packagekit-refresh");
	gs_plugin_add_rule (plugin, GS_PLUGIN_RULE_RUN_AFTER, "packagekit");
	gs_plugin_add_rule (plugin, GS_PLUGIN_RULE_RUN_BEFORE, "generic-updates");
	g_mutex_init (&priv->hash_prepared_mutex);
	priv->hash_prepared = g_hash_table_new_full (g_str_hash, g_str_equal,
						     g_free, NULL);
}

void
gs_plugin_destroy (GsPlugin *plugin)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	g_hash_table_unref (priv->hash_prepared);
	g_mutex_clear (&priv->hash_prepared_mutex);
	if (priv->monitor != NULL)
		g_object_unref (priv->monitor);
	if (priv->monitor_trigger != NULL)
		g_object_unref (priv->monitor_trigger);
}

static void
gs_plugin_systemd_updates_permission_cb (GPermission *permission,
					 GParamSpec *pspec,
					 gpointer data)
{
	GsPlugin *plugin = GS_PLUGIN (data);
	gboolean ret = g_permission_get_allowed (permission) ||
			g_permission_get_can_acquire (permission);
	gs_plugin_set_allow_updates (plugin, ret);
}

static gboolean
gs_plugin_systemd_update_cache (GsPlugin *plugin, GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	g_autoptr(GError) error_local = NULL;
	g_auto(GStrv) package_ids = NULL;
	g_autoptr(GMutexLocker) locker = g_mutex_locker_new (&priv->hash_prepared_mutex);

	/* invalidate */
	g_hash_table_remove_all (priv->hash_prepared);

	/* get new list of package-ids */
	package_ids = pk_offline_get_prepared_ids (&error_local);
	if (package_ids == NULL) {
		if (g_error_matches (error_local,
				     PK_OFFLINE_ERROR,
				     PK_OFFLINE_ERROR_NO_DATA)) {
			return TRUE;
		}
		gs_plugin_packagekit_error_convert (&error_local);
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_INVALID_FORMAT,
			     "Failed to get prepared IDs: %s",
			     error_local->message);
		return FALSE;
	}
	for (guint i = 0; package_ids[i] != NULL; i++) {
		g_hash_table_insert (priv->hash_prepared,
				     g_strdup (package_ids[i]),
				     GUINT_TO_POINTER (1));
	}
	return TRUE;
}

static void
gs_plugin_systemd_updates_changed_cb (GFileMonitor *monitor,
				      GFile *file, GFile *other_file,
				      GFileMonitorEvent event_type,
				      gpointer user_data)
{
	GsPlugin *plugin = GS_PLUGIN (user_data);

	/* update UI */
	gs_plugin_systemd_update_cache (plugin, NULL);
	gs_plugin_updates_changed (plugin);
}

static void
gs_plugin_systemd_updates_refresh_is_triggered (GsPlugin *plugin, GCancellable *cancellable)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	g_autoptr(GFile) file_trigger = NULL;
	file_trigger = g_file_new_for_path ("/system-update");
	priv->is_triggered = g_file_query_exists (file_trigger, NULL);
	g_debug ("offline trigger is now %s",
		 priv->is_triggered ? "enabled" : "disabled");
}

static void
gs_plugin_systemd_trigger_changed_cb (GFileMonitor *monitor,
				      GFile *file, GFile *other_file,
				      GFileMonitorEvent event_type,
				      gpointer user_data)
{
	GsPlugin *plugin = GS_PLUGIN (user_data);
	gs_plugin_systemd_updates_refresh_is_triggered (plugin, NULL);
}

static void
gs_plugin_systemd_refine_app (GsPlugin *plugin, GsApp *app)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	const gchar *package_id;
	g_autoptr(GMutexLocker) locker = NULL;

	/* only process this app if was created by this plugin */
	if (g_strcmp0 (gs_app_get_management_plugin (app), "packagekit") != 0)
		return;

	/* the package is already downloaded */
	package_id = gs_app_get_source_id_default (app);
	if (package_id == NULL)
		return;
	locker = g_mutex_locker_new (&priv->hash_prepared_mutex);
	if (g_hash_table_lookup (priv->hash_prepared, package_id) != NULL)
		gs_app_set_size_download (app, 0);
}

gboolean
gs_plugin_refine (GsPlugin *plugin,
                  GsAppList *list,
                  GsPluginRefineFlags flags,
                  GCancellable *cancellable,
                  GError **error)
{
	/* not now */
	if ((flags & GS_PLUGIN_REFINE_FLAGS_REQUIRE_SIZE) == 0)
		return TRUE;

	/* re-read /var/lib/PackageKit/prepared-update */
	if (!gs_plugin_systemd_update_cache (plugin, error))
		return FALSE;

	for (guint i = 0; i < gs_app_list_length (list); i++) {
		GsApp *app = gs_app_list_index (list, i);
		GsAppList *related = gs_app_get_related (app);
		/* refine the app itself */
		gs_plugin_systemd_refine_app (plugin, app);
		/* and anything related for proxy apps */
		for (guint j = 0; j < gs_app_list_length (related); j++) {
			GsApp *app_related = gs_app_list_index (related, j);
			gs_plugin_systemd_refine_app (plugin, app_related);
		}
	}

	return TRUE;
}

gboolean
gs_plugin_setup (GsPlugin *plugin, GCancellable *cancellable, GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	g_autoptr(GFile) file_trigger = NULL;

	/* watch the prepared file */
	priv->monitor = pk_offline_get_prepared_monitor (cancellable, error);
	if (priv->monitor == NULL) {
		gs_utils_error_convert_gio (error);
		return FALSE;
	}
	g_signal_connect (priv->monitor, "changed",
			  G_CALLBACK (gs_plugin_systemd_updates_changed_cb),
			  plugin);

	/* watch the trigger file */
	file_trigger = g_file_new_for_path ("/system-update");
	priv->monitor_trigger = g_file_monitor_file (file_trigger,
						     G_FILE_MONITOR_NONE,
						     NULL,
						     error);
	if (priv->monitor_trigger == NULL) {
		gs_utils_error_convert_gio (error);
		return FALSE;
	}
	g_signal_connect (priv->monitor_trigger, "changed",
			  G_CALLBACK (gs_plugin_systemd_trigger_changed_cb),
			  plugin);

	/* check if we have permission to trigger the update */
	priv->permission = gs_utils_get_permission (
		"org.freedesktop.packagekit.trigger-offline-update",
		NULL, NULL);
	if (priv->permission != NULL) {
		g_signal_connect (priv->permission, "notify",
				  G_CALLBACK (gs_plugin_systemd_updates_permission_cb),
				  plugin);
	}

	/* get the list of currently downloaded packages */
	return gs_plugin_systemd_update_cache (plugin, error);
}

#ifdef HAVE_PK_OFFLINE_WITH_FLAGS

static PkOfflineFlags
gs_systemd_get_offline_flags (GsPlugin *plugin)
{
	if (gs_plugin_has_flags (plugin, GS_PLUGIN_FLAGS_INTERACTIVE))
		return PK_OFFLINE_FLAGS_INTERACTIVE;
	return PK_OFFLINE_FLAGS_NONE;
}

static gboolean
gs_systemd_call_trigger (GsPlugin *plugin,
			 PkOfflineAction action,
			 GCancellable *cancellable,
			 GError **error)
{
	return pk_offline_trigger_with_flags (action,
					      gs_systemd_get_offline_flags (plugin),
					      cancellable, error);
}

static gboolean
gs_systemd_call_cancel (GsPlugin *plugin,
			GCancellable *cancellable,
			GError **error)
{
	return pk_offline_cancel_with_flags (gs_systemd_get_offline_flags (plugin), cancellable, error);
}

static gboolean
gs_systemd_call_trigger_upgrade (GsPlugin *plugin,
				 PkOfflineAction action,
				 GCancellable *cancellable,
				 GError **error)
{
	return pk_offline_trigger_upgrade_with_flags (action,
						      gs_systemd_get_offline_flags (plugin),
						      cancellable, error);
}

#else /* HAVE_PK_OFFLINE_WITH_FLAGS */

static GDBusCallFlags
gs_systemd_get_gdbus_call_flags (GsPlugin *plugin)
{
	if (gs_plugin_has_flags (plugin, GS_PLUGIN_FLAGS_INTERACTIVE))
		return G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION;
	return G_DBUS_CALL_FLAGS_NONE;
}

static gboolean
gs_systemd_call_trigger (GsPlugin *plugin,
			 PkOfflineAction action,
			 GCancellable *cancellable,
			 GError **error)
{
	const gchar *tmp;
	g_autoptr(GDBusConnection) connection = NULL;
	g_autoptr(GVariant) res = NULL;

	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	connection = g_bus_get_sync (G_BUS_TYPE_SYSTEM, cancellable, error);
	if (connection == NULL)
		return FALSE;
	tmp = pk_offline_action_to_string (action);
	res = g_dbus_connection_call_sync (connection,
					   "org.freedesktop.PackageKit",
					   "/org/freedesktop/PackageKit",
					   "org.freedesktop.PackageKit.Offline",
					   "Trigger",
					   g_variant_new ("(s)", tmp),
					   NULL,
					   gs_systemd_get_gdbus_call_flags (plugin),
					   -1,
					   cancellable,
					   error);
	if (res == NULL)
		return FALSE;
	return TRUE;
}

static gboolean
gs_systemd_call_cancel (GsPlugin *plugin,
			GCancellable *cancellable,
			GError **error)
{
	g_autoptr(GDBusConnection) connection = NULL;
	g_autoptr(GVariant) res = NULL;

	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	connection = g_bus_get_sync (G_BUS_TYPE_SYSTEM, cancellable, error);
	if (connection == NULL)
		return FALSE;
	res = g_dbus_connection_call_sync (connection,
					   "org.freedesktop.PackageKit",
					   "/org/freedesktop/PackageKit",
					   "org.freedesktop.PackageKit.Offline",
					   "Cancel",
					   NULL,
					   NULL,
					   gs_systemd_get_gdbus_call_flags (plugin),
					   -1,
					   cancellable,
					   error);
	if (res == NULL)
		return FALSE;
	return TRUE;
}

static gboolean
gs_systemd_call_trigger_upgrade (GsPlugin *plugin,
				 PkOfflineAction action,
				 GCancellable *cancellable,
				 GError **error)
{
	const gchar *tmp;
	g_autoptr(GDBusConnection) connection = NULL;
	g_autoptr(GVariant) res = NULL;

	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	connection = g_bus_get_sync (G_BUS_TYPE_SYSTEM, cancellable, error);
	if (connection == NULL)
		return FALSE;
	tmp = pk_offline_action_to_string (action);
	res = g_dbus_connection_call_sync (connection,
					   "org.freedesktop.PackageKit",
					   "/org/freedesktop/PackageKit",
					   "org.freedesktop.PackageKit.Offline",
					   "TriggerUpgrade",
					   g_variant_new ("(s)", tmp),
					   NULL,
					   gs_systemd_get_gdbus_call_flags (plugin),
					   -1,
					   cancellable,
					   error);
	if (res == NULL)
		return FALSE;
	return TRUE;
}

#endif /* HAVE_PK_OFFLINE_WITH_FLAGS */

static gboolean
_systemd_trigger_app (GsPlugin *plugin,
		      GsApp *app,
		      GCancellable *cancellable,
		      GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);

	/* if we can process this online do not require a trigger */
	if (gs_app_get_state (app) != GS_APP_STATE_UPDATABLE)
		return TRUE;

	/* only process this app if was created by this plugin */
	if (g_strcmp0 (gs_app_get_management_plugin (app), "packagekit") != 0)
		return TRUE;

	/* already in correct state */
	if (priv->is_triggered)
		return TRUE;

	/* trigger offline update */
	if (!gs_systemd_call_trigger (plugin, PK_OFFLINE_ACTION_REBOOT, cancellable, error)) {
		gs_plugin_packagekit_error_convert (error);
		return FALSE;
	}

	/* don't rely on the file monitor */
	gs_plugin_systemd_updates_refresh_is_triggered (plugin, cancellable);

	/* success */
	return TRUE;
}

gboolean
gs_plugin_update (GsPlugin *plugin,
		  GsAppList *list,
		  GCancellable *cancellable,
		  GError **error)
{
	/* any are us? */
	for (guint i = 0; i < gs_app_list_length (list); i++) {
		GsApp *app = gs_app_list_index (list, i);
		GsAppList *related = gs_app_get_related (app);

		/* try to trigger this app */
		if (!gs_app_has_quirk (app, GS_APP_QUIRK_IS_PROXY)) {
			if (!_systemd_trigger_app (plugin, app, cancellable, error))
				return FALSE;
			continue;
		}

		/* try to trigger each related app */
		for (guint j = 0; j < gs_app_list_length (related); j++) {
			GsApp *app_tmp = gs_app_list_index (related, j);
			if (!_systemd_trigger_app (plugin, app_tmp, cancellable, error))
				return FALSE;
		}
	}

	/* success */
	return TRUE;
}

gboolean
gs_plugin_update_cancel (GsPlugin *plugin,
			 GsApp *app,
			 GCancellable *cancellable,
			 GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);

	/* only process this app if was created by this plugin */
	if (g_strcmp0 (gs_app_get_management_plugin (app), "packagekit") != 0)
		return TRUE;

	/* already in correct state */
	if (!priv->is_triggered)
		return TRUE;

	/* cancel offline update */
	if (!gs_systemd_call_cancel (plugin, cancellable, error)) {
		gs_plugin_packagekit_error_convert (error);
		return FALSE;
	}

	/* don't rely on the file monitor */
	gs_plugin_systemd_updates_refresh_is_triggered (plugin, cancellable);

	/* success! */
	return TRUE;
}

gboolean
gs_plugin_app_upgrade_trigger (GsPlugin *plugin,
                               GsApp *app,
                               GCancellable *cancellable,
                               GError **error)
{
	/* only process this app if was created by this plugin */
	if (g_strcmp0 (gs_app_get_management_plugin (app), "packagekit") != 0)
		return TRUE;
	if (!gs_systemd_call_trigger_upgrade (plugin, PK_OFFLINE_ACTION_REBOOT, cancellable, error)) {
		gs_plugin_packagekit_error_convert (error);
		return FALSE;
	}
	return TRUE;
}
