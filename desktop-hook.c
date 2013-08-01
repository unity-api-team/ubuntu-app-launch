/*
 * Copyright 2013 Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranties of
 * MERCHANTABILITY, SATISFACTORY QUALITY, or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *     Ted Gould <ted.gould@canonical.com>
 */

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>

#include "helpers.h"

typedef struct _app_state_t app_state_t;
struct _app_state_t {
	gchar * app_id;
	gboolean has_click;
	gboolean has_desktop;
	guint64 click_created;
	guint64 desktop_created;
};

/* Find an entry in the app array */
app_state_t *
find_app_entry (const gchar * name, GArray * app_array)
{
	int i;
	for (i = 0; i < app_array->len; i++) {
		app_state_t * state = &g_array_index(app_array, app_state_t, i);

		if (g_strcmp0(state->app_id, name) == 0) {
			return state;
		}
	}

	app_state_t newstate;
	newstate.has_click = FALSE;
	newstate.has_desktop = FALSE;
	newstate.click_created = 0;
	newstate.desktop_created = 0;
	newstate.app_id = g_strdup(name);

	g_array_append_val(app_array, newstate);

	/* Note: The pointer needs to be the entry in the array, not the
	   one that we have on the stack.  Criticaly important. */
	app_state_t * statepntr = &g_array_index(app_array, app_state_t, app_array->len - 1);
	return statepntr;
}

/* Looks up the file creation time, which seems harder with GLib
   than it should be */
guint64
creation_time (const gchar * dir, const gchar * filename)
{
	gchar * path = g_build_filename(dir, filename, NULL);
	GFile * file = g_file_new_for_path(path);
	GFileInfo * info = g_file_query_info(file, G_FILE_ATTRIBUTE_TIME_CREATED, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);

	guint64 time = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_CREATED);

	g_object_unref(info);
	g_object_unref(file);
	g_free(path);

	return time;
}

/* Look at an click package entry */
void
add_click_package (const gchar * dir, const gchar * name, GArray * app_array)
{
	app_state_t * state = find_app_entry(name, app_array);
	state->has_click = TRUE;
	state->click_created = creation_time(dir, name);

	return;
}

/* Look at an desktop file entry */
void
add_desktop_file (const gchar * dir, const gchar * name, GArray * app_array)
{
	if (!g_str_has_suffix(name, ".desktop")) {
		return;
	}

	if (!g_str_has_prefix(name, "click-")) {
		return;
	}

	gchar * appid = g_strdup(name + strlen("click-"));
	g_strstr_len(appid, -1, ".desktop")[0] = '\0';

	app_state_t * state = find_app_entry(appid, app_array);
	state->has_desktop = TRUE;
	state->desktop_created = creation_time(dir, name);

	g_free(appid);
	return;
}

/* Open a directory and look at all the entries */
void
dir_for_each (const gchar * dirname, void(*func)(const gchar * dir, const gchar * name, GArray * app_array), GArray * app_array)
{
	GError * error = NULL;
	GDir * directory = g_dir_open(dirname, 0, &error);

	if (error != NULL) {
		g_warning("Unable to read directory '%s': %s", dirname, error->message);
		g_error_free(error);
		return;
	}

	const gchar * filename = NULL;
	while ((filename = g_dir_read_name(directory)) != NULL) {
		func(dirname, filename, app_array);
	}

	g_dir_close(directory);
	return;
}

/* Function to take the source Desktop file and build a new
   one with similar, but not the same data in it */
static void
copy_desktop_file (const gchar * from, const gchar * to, const gchar * appdir, const gchar * app_id)
{
	GError * error = NULL;
	GKeyFile * keyfile = g_key_file_new();
	g_key_file_load_from_file(keyfile,
		from,
		G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS,
		&error);

	if (error != NULL) {
		g_warning("Unable to read the desktop file '%s' in the application directory: %s", from, error->message);
		g_error_free(error);
		g_key_file_unref(keyfile);
		return;
	}

	gchar * oldexec = desktop_to_exec(keyfile, from);
	if (oldexec == NULL) {
		g_key_file_unref(keyfile);
		return;
	}

	if (g_key_file_has_key(keyfile, "Desktop Entry", "Path", NULL)) {
		gchar * oldpath = g_key_file_get_string(keyfile, "Desktop Entry", "Path", NULL);
		g_debug("Desktop file '%s' has a Path set to '%s'.  Setting as XCanonicalOldPath.", from, oldpath);

		g_key_file_set_string(keyfile, "Desktop Entry", "XCanonicalOldPath", oldpath);

		g_free(oldpath);
	}

	gchar * path = g_build_filename(appdir, app_id, NULL);
	g_key_file_set_string(keyfile, "Desktop Entry", "Path", path);
	g_free(path);

	gchar * newexec = g_strdup_printf("aa-exec -p %s -- %s", app_id, oldexec);
	g_key_file_set_string(keyfile, "Desktop Entry", "Exec", newexec);
	g_free(newexec);
	g_free(oldexec);

	gsize datalen = 0;
	gchar * data = g_key_file_to_data(keyfile, &datalen, &error);
	g_key_file_unref(keyfile);

	if (error != NULL) {
		g_warning("Unable serialize keyfile built from '%s': %s", from, error->message);
		g_error_free(error);
		return;
	}

	g_file_set_contents(to, data, datalen, &error);
	g_free(data);

	if (error != NULL) {
		g_warning("Unable to write out desktop file to '%s': %s", to, error->message);
		g_error_free(error);
		return;
	}

	return;
}

/* Build a desktop file in the user's home directory */
static void
build_desktop_file (app_state_t * state, const gchar * symlinkdir, const gchar * desktopdir)
{
	/* 'Parse' the App ID */
	if (!app_id_to_triplet(state->app_id, NULL, NULL, NULL)) {
		return;
	}

	gchar * indesktop = manifest_to_desktop(symlinkdir, state->app_id);
	if (indesktop == NULL) {
		return;
	}

	/* Determine the desktop file name */
	gchar * desktopfile = g_strdup_printf("click-%s.desktop", state->app_id);
	gchar * desktoppath = g_build_filename(desktopdir, desktopfile, NULL);
	g_free(desktopfile);

	copy_desktop_file(indesktop, desktoppath, symlinkdir, state->app_id);

	g_free(desktoppath);
	g_free(indesktop);

	return;
}

/* Remove the desktop file from the user's home directory */
static void
remove_desktop_file (app_state_t * state, const gchar * desktopdir)
{
	gchar * desktopfile = g_strdup_printf("click-%s.desktop", state->app_id);
	gchar * desktoppath = g_build_filename(desktopdir, desktopfile, NULL);
	g_free(desktopfile);

	if (g_unlink(desktoppath) != 0) {
		g_warning("Unable to delete desktop file: %s", desktoppath);
	}

	g_free(desktoppath);

	return;
}

/* The main function */
int
main (int argc, char * argv[])
{
	if (argc != 1) {
		g_error("Shouldn't have arguments");
		return 1;
	}

	GArray * apparray = g_array_new(FALSE, FALSE, sizeof(app_state_t));

	/* Find all the symlinks of apps */
	gchar * symlinkdir = g_build_filename(g_get_user_cache_dir(), "upstart-app-launch", "desktop", NULL);
	if (!g_file_test(symlinkdir, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_DIR)) {
		g_warning("No installed click packages");
	} else {
		dir_for_each(symlinkdir, add_click_package, apparray);
	}

	/* Find all the click desktop files */
	gchar * desktopdir = g_build_filename(g_get_user_data_dir(), "applications", NULL);
	gboolean desktopdirexists = FALSE;
	if (!g_file_test(symlinkdir, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_DIR)) {
		g_warning("No applications defined");
	} else {
		dir_for_each(desktopdir, add_desktop_file, apparray);
		desktopdirexists = TRUE;
	}

	/* Process the merge */
	int i;
	for (i = 0; i < apparray->len; i++) {
		app_state_t * state = &g_array_index(apparray, app_state_t, i);
		g_debug("Processing App ID: %s", state->app_id);

		if (state->has_click && state->has_desktop) {
			if (state->click_created > state->desktop_created) {
				g_debug("\tClick updated more recently");
				g_debug("\tRemoving desktop file");
				remove_desktop_file(state, desktopdir);
				g_debug("\tBuilding desktop file");
				build_desktop_file(state, symlinkdir, desktopdir);
			} else {
				g_debug("\tAlready synchronized");
			}
		} else if (state->has_click) {
			if (!desktopdirexists) {
				if (g_mkdir_with_parents(desktopdir, 0755) == 0) {
					g_debug("\tCreated applications directory");
					desktopdirexists = TRUE;
				} else {
					g_warning("\tUnable to create applications directory");
				}
			}
			if (desktopdirexists) {
				g_debug("\tBuilding desktop file");
				build_desktop_file(state, symlinkdir, desktopdir);
			}
		} else if (state->has_desktop) {
			g_debug("\tRemoving desktop file");
			remove_desktop_file(state, desktopdir);
		}

		g_free(state->app_id);
	}

	g_array_free(apparray, TRUE);
	g_free(desktopdir);
	g_free(symlinkdir);

	return 0;
}
