/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/*
 *  File-Roller
 *
 *  Copyright (C) 2007 Free Software Foundation, Inc.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <config.h>
#include <math.h>
#include <string.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <gdk/gdkkeysyms.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#ifdef ENABLE_NOTIFICATION
#  include <libnotify/notify.h>
#endif
#include "actions.h"
#include "dlg-batch-add.h"
#include "dlg-delete.h"
#include "dlg-extract.h"
#include "dlg-open-with.h"
#include "dlg-ask-password.h"
#include "dlg-package-installer.h"
#include "dlg-update.h"
#include "eggtreemultidnd.h"
#include "fr-marshal.h"
#include "fr-list-model.h"
#include "fr-archive.h"
#include "fr-command.h"
#include "fr-error.h"
#include "fr-new-archive-dialog.h"
#include "fr-stock.h"
#include "fr-window.h"
#include "file-data.h"
#include "file-utils.h"
#include "glib-utils.h"
#include "gth-icon-cache.h"
#include "gth-toggle-menu-action.h"
#include "fr-init.h"
#include "gtk-utils.h"
#include "open-file.h"
#include "typedefs.h"
#include "ui.h"

#define LAST_OUTPUT_SCHEMA_NAME "LastOutput"
#define MAX_HISTORY_LEN 5
#define ACTIVITY_DELAY 100
#define ACTIVITY_PULSE_STEP (0.033)
#define MAX_MESSAGE_LENGTH 50

#define PROGRESS_DIALOG_DEFAULT_WIDTH 500
#define PROGRESS_TIMEOUT_MSECS 1000
#define PROGRESS_BAR_HEIGHT 10
#undef  LOG_PROGRESS

#define HIDE_PROGRESS_TIMEOUT_MSECS 500
#define DEFAULT_NAME_COLUMN_WIDTH 250
#define OTHER_COLUMNS_WIDTH 100
#define RECENT_ITEM_MAX_WIDTH 25

#define DEF_WIN_WIDTH 600
#define DEF_WIN_HEIGHT 480
#define DEF_SIDEBAR_WIDTH 200

#define FILE_LIST_ICON_SIZE GTK_ICON_SIZE_LARGE_TOOLBAR
#define DIR_TREE_ICON_SIZE GTK_ICON_SIZE_MENU

#define BAD_CHARS "/\\*"

#define XDS_FILENAME "xds.txt"
#define MAX_XDS_ATOM_VAL_LEN 4096
#define XDS_ATOM   gdk_atom_intern  ("XdndDirectSave0", FALSE)
#define TEXT_ATOM  gdk_atom_intern  ("text/plain", FALSE)
#define OCTET_ATOM gdk_atom_intern  ("application/octet-stream", FALSE)
#define XFR_ATOM   gdk_atom_intern  ("XdndFileRoller0", FALSE)

#define FR_CLIPBOARD (gdk_atom_intern_static_string ("_FILE_ROLLER_SPECIAL_CLIPBOARD"))
#define FR_SPECIAL_URI_LIST (gdk_atom_intern_static_string ("application/file-roller-uri-list"))

static GtkTargetEntry clipboard_targets[] = {
	{ "application/file-roller-uri-list", 0, 1 }
};

static GtkTargetEntry target_table[] = {
	{ "XdndFileRoller0", 0, 0 },
	{ "text/uri-list", 0, 1 },
};

static GtkTargetEntry folder_tree_targets[] = {
	{ "XdndFileRoller0", 0, 0 },
	{ "XdndDirectSave0", 0, 2 }
};


typedef struct {
	FrBatchActionType type;
	void *            data;
	GFreeFunc         free_func;
} FrBatchAction;


typedef struct {
	FrWindow    *window;
	GList       *file_list;
	GFile       *destination;
	char        *base_dir;
	gboolean     skip_older;
	FrOverwrite  overwrite;
	gboolean     junk_paths;
	char        *password;
	gboolean     ask_to_open_destination;
} ExtractData;


typedef enum {
	FR_WINDOW_AREA_MENUBAR,
	FR_WINDOW_AREA_TOOLBAR,
	FR_WINDOW_AREA_LOCATIONBAR,
	FR_WINDOW_AREA_CONTENTS,
	FR_WINDOW_AREA_FILTERBAR,
	FR_WINDOW_AREA_STATUSBAR,
} FrWindowArea;


typedef enum {
	DIALOG_RESPONSE_NONE = 1,
	DIALOG_RESPONSE_OPEN_ARCHIVE,
	DIALOG_RESPONSE_OPEN_DESTINATION_FOLDER,
	DIALOG_RESPONSE_QUIT
} DialogResponse;


/* -- FrClipboardData -- */


typedef struct {
	int            refs;
	GFile         *file;
	char          *password;
	FrClipboardOp  op;
	char          *base_dir;
	GList         *files;
	GFile         *tmp_dir;
	char          *current_dir;
} FrClipboardData;


static FrClipboardData*
fr_clipboard_data_new (void)
{
	FrClipboardData *data;

	data = g_new0 (FrClipboardData, 1);
	data->refs = 1;

	return data;
}


static FrClipboardData *
fr_clipboard_data_ref (FrClipboardData *clipboard_data)
{
	clipboard_data->refs++;
	return clipboard_data;
}


static void
fr_clipboard_data_unref (FrClipboardData *clipboard_data)
{
	if (clipboard_data == NULL)
		return;
	if (--clipboard_data->refs > 0)
		return;

	_g_object_unref (clipboard_data->file);
	g_free (clipboard_data->password);
	g_free (clipboard_data->base_dir);
	_g_object_unref (clipboard_data->tmp_dir);
	g_free (clipboard_data->current_dir);
	g_list_foreach (clipboard_data->files, (GFunc) g_free, NULL);
	g_list_free (clipboard_data->files);
	g_free (clipboard_data);
}


static void
fr_clipboard_data_set_password (FrClipboardData *clipboard_data,
				const char      *password)
{
	if (clipboard_data->password != password)
		g_free (clipboard_data->password);
	if (password != NULL)
		clipboard_data->password = g_strdup (password);
}


/**/


G_DEFINE_TYPE (FrWindow, fr_window, GTK_TYPE_APPLICATION_WINDOW)


enum {
	ARCHIVE_LOADED,
	PROGRESS,
	READY,
	LAST_SIGNAL
};

static guint fr_window_signals[LAST_SIGNAL] = { 0 };

struct _FrWindowPrivate {
	GtkWidget         *layout;
	GtkWidget         *contents;
	GtkWidget         *list_view;
	GtkListStore      *list_store;
	GtkWidget         *tree_view;
	GtkTreeStore      *tree_store;
	GtkWidget         *toolbar;
	GtkWidget         *statusbar;
	GtkWidget         *progress_bar;
	GtkWidget         *location_bar;
	GtkWidget         *location_entry;
	GtkWidget         *location_label;
	GtkWidget         *filter_bar;
	GtkWidget         *filter_entry;
	GtkWidget         *paned;
	GtkWidget         *sidepane;
	GtkTreePath       *tree_hover_path;
	GtkTreePath       *list_hover_path;
	GtkTreeViewColumn *filename_column;
	GtkWindowGroup    *window_group;
	GHashTable        *named_dialogs;

	gboolean         filter_mode;
	gint             current_view_length;

	guint            help_message_cid;
	guint            list_info_cid;
	guint            progress_cid;

	GtkWidget *      up_arrows[5];
	GtkWidget *      down_arrows[5];

	FrAction         action;
	gboolean         archive_present;
	gboolean         archive_new;        /* A new archive has been created
					      * but it doesn't contain any
					      * file yet.  The real file will
					      * be created only when the user
					      * adds some file to the
					      * archive.*/
	gboolean         reload_archive;

	GFile *          archive_file;
	GFile *          open_default_dir;    /* default directory to be used
					       * in the Open dialog. */
	GFile *          add_default_dir;     /* default directory to be used
					       * in the Add dialog. */
	GFile *          extract_default_dir; /* default directory to be used
					       * in the Extract dialog. */
	gboolean         freeze_default_dir;
	gboolean         asked_for_password;
	gboolean         destroy_with_error_dialog;
	gboolean         quit_with_progress_dialog;

	FrBatchAction    current_batch_action;

	gboolean         give_focus_to_the_list;
	gboolean         single_click;
	GtkTreePath     *path_clicked;

	FrWindowSortMethod sort_method;
	GtkSortType      sort_type;

	char *           last_location;

	gboolean         view_folders;
	FrWindowListMode list_mode;
	FrWindowListMode last_list_mode;
	GList *          history;
	GList *          history_current;
	char *           password;
	char *           second_password;
	gboolean         encrypt_header;
	FrCompression    compression;
	guint            volume_size;

	guint            activity_timeout_handle;   /* activity timeout
						     * handle. */
	gint             activity_ref;              /* when > 0 some activity
						     * is present. */

	guint            update_timeout_handle;     /* update file list
						     * timeout handle. */

	gboolean         stoppable;
	gboolean         closing;
	gboolean         notify;
	gboolean         populating_file_list;

	FrClipboardData *clipboard_data;
	FrClipboardData *copy_data;

	FrArchive       *copy_from_archive;
	GFile           *saving_file;

	GtkActionGroup  *actions;

	GtkWidget        *file_popup_menu;
	GtkWidget        *folder_popup_menu;
	GtkWidget        *sidebar_folder_popup_menu;
	GtkWidget        *mitem_recents_menu;

	/* dragged files data */

	GFile            *drag_destination_folder;
	char             *drag_base_dir;
	GError           *drag_error;
	GList            *drag_file_list;        /* the list of files we are
					 	  * dragging*/

	/* progress dialog data */

	GtkWidget        *progress_dialog;
	GtkWidget        *pd_action;
	GtkWidget        *pd_message;
	GtkWidget        *pd_progress_bar;
	GtkWidget        *pd_progress_box;
	GtkWidget        *pd_cancel_button;
	GtkWidget        *pd_close_button;
	GtkWidget        *pd_open_archive_button;
	GtkWidget        *pd_open_destination_button;
	GtkWidget        *pd_quit_button;
	GtkWidget        *pd_icon;
	gboolean          progress_pulse;
	guint             progress_timeout;  /* Timeout to display the progress dialog. */
	guint             hide_progress_timeout;  /* Timeout to hide the progress dialog. */
	GFile            *pd_last_archive;
	GFile            *working_archive;
	double            pd_last_fraction;
	char             *pd_last_message;
	gboolean          use_progress_dialog;
	char             *custom_action_message;

	/* update dialog data */

	gpointer          update_dialog;
	GList            *open_files;

	/* batch mode data */

	gboolean          batch_mode;          /* whether we are in a non interactive
					 	* mode. */
	GList            *batch_action_list;   /* FrBatchAction * elements */
	GList            *batch_action;        /* current action. */
	char             *batch_title;

	/* misc */

	GCancellable     *cancellable;

	GSettings        *settings_listing;
	GSettings        *settings_ui;
	GSettings        *settings_general;
	GSettings        *settings_dialogs;
	GSettings        *settings_nautilus;

	gulong            theme_changed_handler_id;
	gboolean          extract_interact_use_default_dir;
	gboolean          update_dropped_files;
	gboolean          batch_adding_one_file;

	GtkWindow        *load_error_parent_window;
	gboolean          showing_error_dialog;

	GthIconCache     *list_icon_cache;
	GthIconCache     *tree_icon_cache;

	GFile            *last_extraction_destination;
};


/* -- fr_window_free_private_data -- */


static void
fr_window_free_batch_data (FrWindow *window)
{
	GList *scan;

	for (scan = window->priv->batch_action_list; scan; scan = scan->next) {
		FrBatchAction *adata = scan->data;

		if ((adata->data != NULL) && (adata->free_func != NULL))
			(*adata->free_func) (adata->data);
		g_free (adata);
	}

	g_list_free (window->priv->batch_action_list);
	window->priv->batch_action_list = NULL;
	window->priv->batch_action = NULL;

	fr_window_reset_current_batch_action (window);
}


static void
fr_window_clipboard_remove_file_list (FrWindow *window,
				      GList    *file_list)
{
	GList *scan1;

	if (window->priv->copy_data == NULL)
		return;

	if (file_list == NULL) {
		fr_clipboard_data_unref	 (window->priv->copy_data);
		window->priv->copy_data = NULL;
		return;
	}

	for (scan1 = file_list; scan1; scan1 = scan1->next) {
		const char *name1 = scan1->data;
		GList      *scan2;

		for (scan2 = window->priv->copy_data->files; scan2;) {
			const char *name2 = scan2->data;

			if (strcmp (name1, name2) == 0) {
				GList *tmp = scan2->next;
				window->priv->copy_data->files = g_list_remove_link (window->priv->copy_data->files, scan2);
				g_free (scan2->data);
				g_list_free (scan2);
				scan2 = tmp;
			}
			else
				scan2 = scan2->next;
		}
	}

	if (window->priv->copy_data->files == NULL) {
		fr_clipboard_data_unref (window->priv->copy_data);
		window->priv->copy_data = NULL;
		gtk_clipboard_clear (gtk_widget_get_clipboard (GTK_WIDGET (window), FR_CLIPBOARD));
	}
}


static void
fr_window_history_clear (FrWindow *window)
{
	if (window->priv->history != NULL)
		_g_string_list_free (window->priv->history);
	window->priv->history = NULL;
	window->priv->history_current = NULL;
	g_free (window->priv->last_location);
	window->priv->last_location = NULL;
}


static void
fr_window_free_open_files (FrWindow *window)
{
	GList *scan;

	for (scan = window->priv->open_files; scan; scan = scan->next) {
		OpenFile *file = scan->data;

		if (file->monitor != NULL)
			g_file_monitor_cancel (file->monitor);
		open_file_free (file);
	}
	g_list_free (window->priv->open_files);
	window->priv->open_files = NULL;
}


static void
fr_window_free_private_data (FrWindow *window)
{
	if (window->priv->update_timeout_handle != 0) {
		g_source_remove (window->priv->update_timeout_handle);
		window->priv->update_timeout_handle = 0;
	}

	if (window->priv->activity_timeout_handle != 0) {
		g_source_remove (window->priv->activity_timeout_handle);
		window->priv->activity_timeout_handle = 0;
	}

	if (window->priv->progress_timeout != 0) {
		g_source_remove (window->priv->progress_timeout);
		window->priv->progress_timeout = 0;
	}

	if (window->priv->hide_progress_timeout != 0) {
		g_source_remove (window->priv->hide_progress_timeout);
		window->priv->hide_progress_timeout = 0;
	}

	if (window->priv->theme_changed_handler_id != 0)
		g_signal_handler_disconnect (gtk_icon_theme_get_default (), window->priv->theme_changed_handler_id);

	fr_window_history_clear (window);

	_g_object_unref (window->priv->open_default_dir);
	_g_object_unref (window->priv->add_default_dir);
	_g_object_unref (window->priv->extract_default_dir);
	_g_object_unref (window->priv->archive_file);
	_g_object_unref (window->priv->last_extraction_destination);

	g_free (window->priv->password);
	g_free (window->priv->second_password);
	g_free (window->priv->custom_action_message);

	g_object_unref (window->priv->list_store);

	if (window->priv->clipboard_data != NULL) {
		fr_clipboard_data_unref (window->priv->clipboard_data);
		window->priv->clipboard_data = NULL;
	}
	if (window->priv->copy_data != NULL) {
		fr_clipboard_data_unref (window->priv->copy_data);
		window->priv->copy_data = NULL;
	}
	if (window->priv->copy_from_archive != NULL) {
		g_object_unref (window->priv->copy_from_archive);
		window->priv->copy_from_archive = NULL;
	}

	_g_object_unref (window->priv->saving_file);

	fr_window_free_open_files (window);

	g_clear_error (&window->priv->drag_error);
	_g_string_list_free (window->priv->drag_file_list);
	window->priv->drag_file_list = NULL;

	if (window->priv->file_popup_menu != NULL) {
		gtk_widget_destroy (window->priv->file_popup_menu);
		window->priv->file_popup_menu = NULL;
	}

	if (window->priv->folder_popup_menu != NULL) {
		gtk_widget_destroy (window->priv->folder_popup_menu);
		window->priv->folder_popup_menu = NULL;
	}

	if (window->priv->sidebar_folder_popup_menu != NULL) {
		gtk_widget_destroy (window->priv->sidebar_folder_popup_menu);
		window->priv->sidebar_folder_popup_menu = NULL;
	}

	g_free (window->priv->last_location);

	fr_window_free_batch_data (window);
	g_free (window->priv->batch_title);

	_g_object_unref (window->priv->pd_last_archive);
	g_free (window->priv->pd_last_message);

	g_settings_set_enum (window->priv->settings_listing, PREF_LISTING_LIST_MODE, window->priv->last_list_mode);

	_g_object_unref (window->priv->settings_listing);
	_g_object_unref (window->priv->settings_ui);
	_g_object_unref (window->priv->settings_general);
	_g_object_unref (window->priv->settings_dialogs);
	_g_object_unref (window->priv->settings_nautilus);

	_g_object_unref (window->priv->cancellable);
	g_hash_table_unref (window->priv->named_dialogs);
	g_object_unref (window->priv->window_group);
}


void
fr_window_close (FrWindow *window)
{
	if (window->priv->activity_ref > 0)
		return;

	if (window->priv->closing)
		return;

	window->priv->closing = TRUE;

	if (gtk_widget_get_realized (GTK_WIDGET (window))) {
		int width, height;

		width = gtk_widget_get_allocated_width (GTK_WIDGET (window));
		height = gtk_widget_get_allocated_height (GTK_WIDGET (window));
		g_settings_set_int (window->priv->settings_ui, PREF_UI_WINDOW_WIDTH, width);
		g_settings_set_int (window->priv->settings_ui, PREF_UI_WINDOW_HEIGHT, height);

		width = gtk_paned_get_position (GTK_PANED (window->priv->paned));
		if (width > 0)
			g_settings_set_int (window->priv->settings_ui, PREF_UI_SIDEBAR_WIDTH, width);

		width = gtk_tree_view_column_get_width (window->priv->filename_column);
		if (width > 0)
			g_settings_set_int (window->priv->settings_listing, PREF_LISTING_NAME_COLUMN_WIDTH, width);
	}

	gtk_widget_destroy (GTK_WIDGET (window));
}


#define DIALOG_NAME_KEY "fr_dialog_name"


static void
unset_dialog (GtkWidget *object,
              gpointer   user_data)
{
	FrWindow   *window = user_data;
	const char *dialog_name;

	dialog_name = g_object_get_data (G_OBJECT (object), DIALOG_NAME_KEY);
	if (dialog_name != NULL)
		g_hash_table_remove (window->priv->named_dialogs, dialog_name);
}


void
fr_window_set_dialog (FrWindow   *window,
		      const char *dialog_name,
		      GtkWidget  *dialog)
{
	g_object_set_data (G_OBJECT (dialog), DIALOG_NAME_KEY, (gpointer) _g_str_get_static (dialog_name));
	g_hash_table_insert (window->priv->named_dialogs, (gpointer) dialog_name, dialog);
	g_signal_connect (dialog,
			  "destroy",
			  G_CALLBACK (unset_dialog),
			  window);
}


gboolean
fr_window_present_dialog_if_created (FrWindow   *window,
				     const char *dialog_name)
{
	GtkWidget *dialog;

	dialog = g_hash_table_lookup (window->priv->named_dialogs, dialog_name);
	if (dialog != NULL) {
		gtk_window_present (GTK_WINDOW (dialog));
		return TRUE;
	}

	return FALSE;
}


static void
fr_window_finalize (GObject *object)
{
	FrWindow *window = FR_WINDOW (object);

	fr_window_free_open_files (window);

	if (window->archive != NULL) {
		g_object_unref (window->archive);
		window->archive = NULL;
	}

	if (window->priv != NULL) {
		fr_window_free_private_data (window);
		g_free (window->priv);
		window->priv = NULL;
	}

	G_OBJECT_CLASS (fr_window_parent_class)->finalize (object);
}



static void fr_window_update_file_list (FrWindow *window,
					gboolean  update_view);
static void fr_window_update_dir_tree  (FrWindow *window);


static void
set_sensitive (FrWindow   *window,
	       const char *action_name,
	       gboolean    sensitive)
{
	GtkAction *action;

	action = gtk_action_group_get_action (window->priv->actions, action_name);
	g_object_set (action, "sensitive", sensitive, NULL);
}


static void
fr_window_update_paste_command_sensitivity (FrWindow     *window,
					    GtkClipboard *clipboard)
{
	gboolean running;
	gboolean no_archive;
	gboolean ro;

	if (window->priv->closing)
		return;

	if (clipboard == NULL)
		clipboard = gtk_widget_get_clipboard (GTK_WIDGET (window), FR_CLIPBOARD);
	running    = window->priv->activity_ref > 0;
	no_archive = (window->archive == NULL) || ! window->priv->archive_present;
	ro         = ! no_archive && window->archive->read_only;

	set_sensitive (window, "Paste",
		       ! no_archive
		       && ! ro
		       && ! running
		       && fr_archive_is_capable_of (window->archive, FR_ARCHIVE_CAN_STORE_MANY_FILES)
		       && (window->priv->list_mode != FR_WINDOW_LIST_MODE_FLAT)
		       && gtk_clipboard_wait_is_target_available (clipboard, FR_SPECIAL_URI_LIST));
}


static void
clipboard_owner_change_cb (GtkClipboard *clipboard,
			   GdkEvent     *event,
			   gpointer      user_data)
{
	fr_window_update_paste_command_sensitivity ((FrWindow *) user_data, clipboard);
}


static void
fr_window_realize (GtkWidget *widget)
{
	FrWindow     *window = FR_WINDOW (widget);
	GIcon        *icon;
	GtkClipboard *clipboard;

	GTK_WIDGET_CLASS (fr_window_parent_class)->realize (widget);

	window->priv->list_icon_cache = gth_icon_cache_new_for_widget (GTK_WIDGET (window), GTK_ICON_SIZE_LARGE_TOOLBAR);
	window->priv->tree_icon_cache = gth_icon_cache_new_for_widget (GTK_WIDGET (window), GTK_ICON_SIZE_MENU);

	icon = g_content_type_get_icon ("text/plain");
	gth_icon_cache_set_fallback (window->priv->list_icon_cache, icon);
	gth_icon_cache_set_fallback (window->priv->tree_icon_cache, icon);
	g_object_unref (icon);

	clipboard = gtk_widget_get_clipboard (widget, FR_CLIPBOARD);
	g_signal_connect (clipboard,
			  "owner_change",
			  G_CALLBACK (clipboard_owner_change_cb),
			  window);

	gtk_tree_sortable_set_sort_column_id (GTK_TREE_SORTABLE (window->priv->list_store),
					      g_settings_get_enum (window->priv->settings_listing, PREF_LISTING_SORT_METHOD),
					      g_settings_get_enum (window->priv->settings_listing, PREF_LISTING_SORT_TYPE));

	fr_window_update_dir_tree (window);
	fr_window_update_file_list (window, TRUE);
}


static void
fr_window_unrealize (GtkWidget *widget)
{
	FrWindow     *window = FR_WINDOW (widget);
	GtkClipboard *clipboard;

	gth_icon_cache_free (window->priv->list_icon_cache);
	window->priv->list_icon_cache = NULL;

	gth_icon_cache_free (window->priv->tree_icon_cache);
	window->priv->tree_icon_cache = NULL;

	clipboard = gtk_widget_get_clipboard (widget, FR_CLIPBOARD);
	g_signal_handlers_disconnect_by_func (clipboard,
					      G_CALLBACK (clipboard_owner_change_cb),
					      window);

	GTK_WIDGET_CLASS (fr_window_parent_class)->unrealize (widget);
}


static void
fr_window_unmap (GtkWidget *widget)
{
	FrWindow    *window = FR_WINDOW (widget);
	GtkSortType  order;
	int          column_id;

	if (gtk_tree_sortable_get_sort_column_id (GTK_TREE_SORTABLE (window->priv->list_store),
						  &column_id,
						  &order))
	{
		g_settings_set_enum (window->priv->settings_listing, PREF_LISTING_SORT_METHOD, column_id);
		g_settings_set_enum (window->priv->settings_listing, PREF_LISTING_SORT_TYPE, order);
	}

	GTK_WIDGET_CLASS (fr_window_parent_class)->unmap (widget);
}


static void
fr_window_class_init (FrWindowClass *klass)
{
	GObjectClass   *gobject_class;
	GtkWidgetClass *widget_class;

	fr_window_parent_class = g_type_class_peek_parent (klass);

	fr_window_signals[ARCHIVE_LOADED] =
		g_signal_new ("archive-loaded",
			      G_TYPE_FROM_CLASS (klass),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (FrWindowClass, archive_loaded),
			      NULL, NULL,
			      fr_marshal_VOID__BOOLEAN,
			      G_TYPE_NONE, 1,
			      G_TYPE_BOOLEAN);
	fr_window_signals[PROGRESS] =
		g_signal_new ("progress",
			      G_TYPE_FROM_CLASS (klass),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (FrWindowClass, progress),
			      NULL, NULL,
			      fr_marshal_VOID__DOUBLE_STRING,
			      G_TYPE_NONE, 2,
			      G_TYPE_DOUBLE,
			      G_TYPE_STRING);
	fr_window_signals[READY] =
		g_signal_new ("ready",
			      G_TYPE_FROM_CLASS (klass),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (FrWindowClass, ready),
			      NULL, NULL,
			      fr_marshal_VOID__POINTER,
			      G_TYPE_NONE, 1,
			      G_TYPE_POINTER);

	gobject_class = G_OBJECT_CLASS (klass);
	gobject_class->finalize = fr_window_finalize;

	widget_class = GTK_WIDGET_CLASS (klass);
	widget_class->realize = fr_window_realize;
	widget_class->unrealize = fr_window_unrealize;
	widget_class->unmap = fr_window_unmap;
}


static void
fr_window_init (FrWindow *window)
{
	window->priv = g_new0 (FrWindowPrivate, 1);
	window->priv->update_dropped_files = FALSE;
	window->priv->filter_mode = FALSE;
	window->priv->use_progress_dialog = TRUE;
	window->priv->batch_title = NULL;
	window->priv->cancellable = g_cancellable_new ();
	window->priv->compression = FR_COMPRESSION_NORMAL;
	window->priv->window_group = gtk_window_group_new ();
	window->priv->populating_file_list = FALSE;
	window->priv->named_dialogs = g_hash_table_new (g_str_hash, g_str_equal);
	gtk_window_group_add_window (window->priv->window_group, GTK_WINDOW (window));

	window->archive = NULL;
}



/* -- window history -- */


#if 0
static void
fr_window_history_print (FrWindow *window)
{
	GList *list;

	debug (DEBUG_INFO, "history:\n");
	for (list = window->priv->history; list; list = list->next)
		g_print ("\t%s %s\n",
			 (char*) list->data,
			 (list == window->priv->history_current)? "<-": "");
	g_print ("\n");
}
#endif


static gboolean
fr_window_dir_exists_in_archive (FrWindow   *window,
				 const char *dir_name)
{
	int dir_name_len;
	int i;

	if (dir_name == NULL)
		return FALSE;

	dir_name_len = strlen (dir_name);
	if (dir_name_len == 0)
		return TRUE;

	if (strcmp (dir_name, "/") == 0)
		return TRUE;

	for (i = 0; i < window->archive->files->len; i++) {
		FileData *fdata = g_ptr_array_index (window->archive->files, i);

		if (strncmp (dir_name, fdata->full_path, dir_name_len) == 0) {
			return TRUE;
		}
		else if (fdata->dir
			 && (fdata->full_path[strlen (fdata->full_path) - 1] != '/')
			 && (strncmp (dir_name, fdata->full_path, dir_name_len - 1) == 0))
		{
			return TRUE;
		}
	}

	return FALSE;
}


static void
fr_window_update_history (FrWindow *window)
{
	GList *scan;

	/* remove the paths not present in the archive */

	for (scan = window->priv->history; scan; /* void */) {
		GList *next = scan->next;
		char  *path = scan->data;

		if (! fr_window_dir_exists_in_archive (window, path)) {
			if (scan == window->priv->history_current)
				window->priv->history_current = NULL;
			window->priv->history = g_list_remove_link (window->priv->history, scan);
			_g_string_list_free (scan);
		}

		scan = next;
	}

	if (window->priv->history_current == NULL)
		window->priv->history_current = window->priv->history;
}


static void
fr_window_history_add (FrWindow   *window,
		       const char *path)
{
	if ((window->priv->history_current == NULL) || (g_strcmp0 (path, window->priv->history_current->data) != 0)) {
		GList *scan;
		GList *new_current = NULL;

                /* search the path in the history */
                for (scan = window->priv->history_current; scan; scan = scan->next) {
                        char *path_in_history = scan->data;

                        if (g_strcmp0 (path, path_in_history) == 0) {
                        	new_current = scan;
                        	break;
                        }
                }

                if (new_current != NULL) {
                	window->priv->history_current = new_current;
                }
                else {
			/* remove all the paths after the current position */
			for (scan = window->priv->history; scan && (scan != window->priv->history_current); /* void */) {
				GList *next = scan->next;

				window->priv->history = g_list_remove_link (window->priv->history, scan);
				_g_string_list_free (scan);

				scan = next;
			}

			window->priv->history = g_list_prepend (window->priv->history, g_strdup (path));
			window->priv->history_current = window->priv->history;
                }
	}
}


static void
fr_window_history_pop (FrWindow *window)
{
	GList *first;

	if (window->priv->history == NULL)
		return;

	first = window->priv->history;
	window->priv->history = g_list_remove_link (window->priv->history, first);
	if (window->priv->history_current == first)
		window->priv->history_current = window->priv->history;
	g_free (first->data);
	g_list_free (first);
}


/* -- activity mode -- */



static void
check_whether_has_a_dir (GtkTreeModel *model,
			 GtkTreePath  *path,
			 GtkTreeIter  *iter,
			 gpointer      data)
{
	gboolean *has_a_dir = data;
	FileData *fdata;

	gtk_tree_model_get (model, iter,
			    COLUMN_FILE_DATA, &fdata,
			    -1);
	if (file_data_is_dir (fdata))
		*has_a_dir = TRUE;
}


static gboolean
selection_has_a_dir (FrWindow *window)
{
	GtkTreeSelection *selection;
	gboolean          has_a_dir = FALSE;

	if (! gtk_widget_get_realized (window->priv->list_view))
		return FALSE;

	selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (window->priv->list_view));
	if (selection == NULL)
		return FALSE;

	gtk_tree_selection_selected_foreach (selection,
					     check_whether_has_a_dir,
					     &has_a_dir);

	return has_a_dir;
}


static void
fr_window_update_sensitivity (FrWindow *window)
{
	gboolean no_archive;
	gboolean ro;
	gboolean file_op;
	gboolean running;
	gboolean can_store_many_files;
	gboolean sel_not_null;
	gboolean one_file_selected;
	gboolean dir_selected;
	int      n_selected;

	if (window->priv->batch_mode)
		return;

	running              = window->priv->activity_ref > 0;
	no_archive           = (window->archive == NULL) || ! window->priv->archive_present;
	ro                   = ! no_archive && window->archive->read_only;
	file_op              = ! no_archive && ! window->priv->archive_new  && ! running;
	can_store_many_files = (window->archive != NULL) && fr_archive_is_capable_of (window->archive, FR_ARCHIVE_CAN_STORE_MANY_FILES);
	n_selected           = fr_window_get_n_selected_files (window);
	sel_not_null         = n_selected > 0;
	one_file_selected    = n_selected == 1;
	dir_selected         = selection_has_a_dir (window);

	set_sensitive (window, "Add", ! no_archive && ! ro && ! running && can_store_many_files);
	set_sensitive (window, "Add_Toolbar", ! no_archive && ! ro && ! running && can_store_many_files);
	set_sensitive (window, "Copy", ! no_archive && ! ro && ! running && can_store_many_files && sel_not_null && (window->priv->list_mode != FR_WINDOW_LIST_MODE_FLAT));
	set_sensitive (window, "Cut", ! no_archive && ! ro && ! running && can_store_many_files && sel_not_null && (window->priv->list_mode != FR_WINDOW_LIST_MODE_FLAT));
	set_sensitive (window, "Delete", ! no_archive && ! ro && ! window->priv->archive_new && ! running && can_store_many_files);
	set_sensitive (window, "DeselectAll", ! no_archive && sel_not_null);
	set_sensitive (window, "Extract", file_op);
	set_sensitive (window, "Extract_Toolbar", file_op);
	set_sensitive (window, "Find", ! no_archive);
	set_sensitive (window, "New", ! running);
	set_sensitive (window, "Open", ! running);
	set_sensitive (window, "Open_Toolbar", ! running);
	set_sensitive (window, "OpenSelection", file_op && sel_not_null && ! dir_selected);
	set_sensitive (window, "OpenFolder", file_op && one_file_selected && dir_selected);
	set_sensitive (window, "Password", ! running && (window->priv->asked_for_password || (! no_archive && window->archive->propPassword)));
	set_sensitive (window, "Properties", file_op);
	set_sensitive (window, "Close", !running || window->priv->stoppable);
	set_sensitive (window, "Reload", ! (no_archive || running));
	set_sensitive (window, "Rename", ! no_archive && ! ro && ! running && can_store_many_files && one_file_selected);
	set_sensitive (window, "SaveAs", ! no_archive && can_store_many_files && ! running);
	set_sensitive (window, "SelectAll", ! no_archive);
	set_sensitive (window, "TestArchive", ! no_archive && ! running && window->archive->propTest);
	set_sensitive (window, "ViewSelection", file_op && one_file_selected && ! dir_selected);
	set_sensitive (window, "ViewSelection_Toolbar", file_op && one_file_selected && ! dir_selected);

	if (window->priv->progress_dialog != NULL)
		gtk_dialog_set_response_sensitive (GTK_DIALOG (window->priv->progress_dialog),
						   GTK_RESPONSE_OK,
						   running && window->priv->stoppable);

	fr_window_update_paste_command_sensitivity (window, NULL);

	set_sensitive (window, "SelectAll", (window->priv->current_view_length > 0) && (window->priv->current_view_length != n_selected));
	set_sensitive (window, "DeselectAll", n_selected > 0);
	set_sensitive (window, "OpenRecent", ! running);
	set_sensitive (window, "OpenRecent_Toolbar", ! running);

	set_sensitive (window, "ViewFolders", (window->priv->list_mode == FR_WINDOW_LIST_MODE_AS_DIR));

	set_sensitive (window, "ViewAllFiles", ! window->priv->filter_mode);
	set_sensitive (window, "ViewAsFolder", ! window->priv->filter_mode);
}


static int
activity_cb (gpointer data)
{
	FrWindow *window = data;

	if ((window->priv->pd_progress_bar != NULL) && window->priv->progress_pulse)
		gtk_progress_bar_pulse (GTK_PROGRESS_BAR (window->priv->pd_progress_bar));
	if (window->priv->progress_pulse)
		gtk_progress_bar_pulse (GTK_PROGRESS_BAR (window->priv->progress_bar));

	return TRUE;
}


static void
_fr_window_start_activity_mode (FrWindow *window)
{
	g_return_if_fail (window != NULL);

	if (window->priv->activity_ref++ > 0)
		return;

	window->priv->activity_timeout_handle = g_timeout_add (ACTIVITY_DELAY,
							       activity_cb,
							       window);
	fr_window_update_sensitivity (window);
}


static void
fr_window_pop_message (FrWindow *window)
{
	if (! gtk_widget_get_mapped (GTK_WIDGET (window)))
		return;

	gtk_statusbar_pop (GTK_STATUSBAR (window->priv->statusbar), window->priv->progress_cid);
	if (window->priv->progress_dialog != NULL)
		gtk_label_set_text (GTK_LABEL (window->priv->pd_message), _("Operation completed"));
}


static void
add_selected_fd (GtkTreeModel *model,
		 GtkTreePath  *path,
		 GtkTreeIter  *iter,
		 gpointer      data)
{
	GList    **list = data;
	FileData  *fdata;

	gtk_tree_model_get (model, iter,
			    COLUMN_FILE_DATA, &fdata,
			    -1);
	if (! fdata->list_dir)
		*list = g_list_prepend (*list, fdata);
}


static GList *
get_selection_as_fd (FrWindow *window)
{
	GtkTreeSelection *selection;
	GList            *list = NULL;

	if (! gtk_widget_get_realized (window->priv->list_view))
		return NULL;

	selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (window->priv->list_view));
	if (selection == NULL)
		return NULL;
	gtk_tree_selection_selected_foreach (selection, add_selected_fd, &list);

	return list;
}


static GPtrArray *
fr_window_get_current_dir_list (FrWindow *window)
{
	GPtrArray *files;
	int        i;

	files = g_ptr_array_sized_new (128);

	for (i = 0; i < window->archive->files->len; i++) {
		FileData *fdata = g_ptr_array_index (window->archive->files, i);

		if (fdata->list_name == NULL)
			continue;
		g_ptr_array_add (files, fdata);
	}

	return files;
}


static void
fr_window_update_statusbar_list_info (FrWindow *window)
{
	char    *info, *archive_info, *selected_info;
	char    *size_txt, *sel_size_txt;
	int      tot_n, sel_n;
	goffset  tot_size, sel_size;
	GList   *scan;

	if ((window == NULL) || window->priv->batch_mode)
		return;

	if (window->archive == NULL) {
		gtk_statusbar_pop (GTK_STATUSBAR (window->priv->statusbar), window->priv->list_info_cid);
		return;
	}

	tot_n = 0;
	tot_size = 0;

	if (window->priv->archive_present) {
		GPtrArray *files = fr_window_get_current_dir_list (window);
		int        i;

		for (i = 0; i < files->len; i++) {
			FileData *fd = g_ptr_array_index (files, i);

			tot_n++;
			if (! file_data_is_dir (fd))
				tot_size += fd->size;
			else
				tot_size += fd->dir_size;
		}
		g_ptr_array_free (files, TRUE);
	}

	sel_n = 0;
	sel_size = 0;

	if (window->priv->archive_present) {
		GList *selection = get_selection_as_fd (window);

		for (scan = selection; scan; scan = scan->next) {
			FileData *fd = scan->data;

			sel_n++;
			if (! file_data_is_dir (fd))
				sel_size += fd->size;
		}
		g_list_free (selection);
	}

	size_txt = g_format_size (tot_size);
	sel_size_txt = g_format_size (sel_size);

	if (tot_n == 0)
		archive_info = g_strdup ("");
	else
		archive_info = g_strdup_printf (ngettext ("%d object (%s)", "%d objects (%s)", tot_n), tot_n, size_txt);

	if (sel_n == 0)
		selected_info = g_strdup ("");
	else
		selected_info = g_strdup_printf (ngettext ("%d object selected (%s)", "%d objects selected (%s)", sel_n), sel_n, sel_size_txt);

	info = g_strconcat (archive_info,
			    ((sel_n == 0) ? NULL : ", "),
			    selected_info,
			    NULL);

	gtk_statusbar_push (GTK_STATUSBAR (window->priv->statusbar), window->priv->list_info_cid, info);

	g_free (size_txt);
	g_free (sel_size_txt);
	g_free (archive_info);
	g_free (selected_info);
	g_free (info);
}


static void
_fr_window_stop_activity_mode (FrWindow *window)
{
	g_return_if_fail (window != NULL);

	if (window->priv->activity_ref == 0)
		return;

	fr_window_pop_message (window);

	if (--window->priv->activity_ref > 0)
		return;

	if (window->priv->activity_timeout_handle != 0) {
		g_source_remove (window->priv->activity_timeout_handle);
		window->priv->activity_timeout_handle = 0;
	}

	if (window->priv->progress_dialog != NULL)
		gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (window->priv->pd_progress_bar), 0.0);

	if (! window->priv->batch_mode) {
		if (window->priv->progress_bar != NULL)
			gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (window->priv->progress_bar), 0.0);
	}

	fr_window_update_sensitivity (window);
	fr_window_update_statusbar_list_info (window);
}


/* -- window_update_file_list -- */


static guint64
get_dir_size (FrWindow   *window,
	      const char *current_dir,
	      const char *name)
{
	guint64  size;
	char    *dirname;
	int      dirname_l;
	int      i;

	dirname = g_strconcat (current_dir, name, "/", NULL);
	dirname_l = strlen (dirname);

	size = 0;
	for (i = 0; i < window->archive->files->len; i++) {
		FileData *fd = g_ptr_array_index (window->archive->files, i);

		if (strncmp (dirname, fd->full_path, dirname_l) == 0)
			size += fd->size;
	}

	g_free (dirname);

	return size;
}


static gboolean
file_data_respects_filter (FrWindow *window,
			   FileData *fdata)
{
	const char *filter;

	filter = gtk_entry_get_text (GTK_ENTRY (window->priv->filter_entry));
	if ((fdata == NULL) || (filter == NULL) || (*filter == '\0'))
		return TRUE;

	if (fdata->dir || (fdata->name == NULL))
		return FALSE;

	return strncasecmp (fdata->name, filter, strlen (filter)) == 0;
}


static gboolean
compute_file_list_name (FrWindow   *window,
			FileData   *fdata,
			const char *current_dir,
			int         current_dir_len,
			GHashTable *names_hash,
			gboolean   *different_name)
{
	register char *scan, *end;

	*different_name = FALSE;

	if (! file_data_respects_filter (window, fdata))
		return FALSE;

	if (window->priv->list_mode == FR_WINDOW_LIST_MODE_FLAT) {
		file_data_set_list_name (fdata, fdata->name);
		if (fdata->dir)
			fdata->dir_size = 0;
		return FALSE;
	}

	if (strncmp (fdata->full_path, current_dir, current_dir_len) != 0) {
		*different_name = TRUE;
		return FALSE;
	}

	if (strlen (fdata->full_path) == current_dir_len)
		return FALSE;

	scan = fdata->full_path + current_dir_len;
	end = strchr (scan, '/');
	if ((end == NULL) && ! fdata->dir) { /* file */
		file_data_set_list_name (fdata, scan);
	}
	else { /* folder */
		char *dir_name;

		if (end != NULL)
			dir_name = g_strndup (scan, end - scan);
		else
			dir_name = g_strdup (scan);

		/* avoid to insert duplicated folders */
		if (g_hash_table_lookup (names_hash, dir_name) != NULL) {
			g_free (dir_name);
			return FALSE;
		}
		g_hash_table_insert (names_hash, dir_name, GINT_TO_POINTER (1));

		if ((end != NULL) && (*(end + 1) != '\0'))
			fdata->list_dir = TRUE;
		file_data_set_list_name (fdata, dir_name);
		fdata->dir_size = get_dir_size (window, current_dir, dir_name);
	}

	return TRUE;
}


static void
fr_window_compute_list_names (FrWindow  *window,
			      GPtrArray *files)
{
	const char *current_dir;
	int         current_dir_len;
	GHashTable *names_hash;
	int         i;
	gboolean    visible_list_started = FALSE;
	gboolean    visible_list_completed = FALSE;
	gboolean    different_name;

	current_dir = fr_window_get_current_location (window);
	current_dir_len = strlen (current_dir);
	names_hash = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

	for (i = 0; i < files->len; i++) {
		FileData *fdata = g_ptr_array_index (files, i);

		file_data_set_list_name (fdata, NULL);
		fdata->list_dir = FALSE;

		/* the files array is sorted by path, when the visible list
		 * is started and we find a path that doesn't match the
		 * current_dir path, the following files can't match
		 * the current_dir path. */

		if (visible_list_completed)
			continue;

		if (compute_file_list_name (window, fdata, current_dir, current_dir_len, names_hash, &different_name)) {
			visible_list_started = TRUE;
		}
		else if (visible_list_started && different_name)
			visible_list_completed = TRUE;
	}

	g_hash_table_destroy (names_hash);
}


static char *
get_parent_dir (const char *current_dir)
{
	char *dir;
	char *new_dir;
	char *retval;

	if (current_dir == NULL)
		return NULL;
	if (strcmp (current_dir, "/") == 0)
		return g_strdup ("/");

	dir = g_strdup (current_dir);
	dir[strlen (dir) - 1] = 0;
	new_dir = _g_path_remove_level (dir);
	g_free (dir);

	if (new_dir[strlen (new_dir) - 1] == '/')
		retval = new_dir;
	else {
		retval = g_strconcat (new_dir, "/", NULL);
		g_free (new_dir);
	}

	return retval;
}


static GdkPixbuf *
get_mime_type_icon (FrWindow   *window,
		    const char *mime_type)
{
	GIcon     *icon;
	GdkPixbuf *pixbuf;

	icon = g_content_type_get_icon (mime_type);
	pixbuf = gth_icon_cache_get_pixbuf (window->priv->tree_icon_cache, icon);

	g_object_unref (icon);

	return pixbuf;
}


static GdkPixbuf *
get_icon (FrWindow  *window,
	  FileData  *fdata)
{
	GIcon     *icon = NULL;
	GdkPixbuf *pixbuf = NULL;

	if (fdata->link != NULL)
		icon = g_themed_icon_new ("emblem-symbolic-link");
	else {
		const char *content_type;

		if (file_data_is_dir (fdata))
			content_type = MIME_TYPE_DIRECTORY;
		else
			content_type = fdata->content_type;
		icon = g_content_type_get_icon (content_type);
	}

	pixbuf = gth_icon_cache_get_pixbuf (window->priv->list_icon_cache, icon);
	g_object_unref (icon);

	return pixbuf;
}


static GdkPixbuf *
get_emblem (FrWindow *window,
	    FileData *fdata)
{
	const char *emblem_name;
	GIcon      *icon;
	GdkPixbuf  *pixbuf;

	emblem_name = NULL;
	if (fdata->encrypted)
		emblem_name = "emblem-nowrite";

	if (emblem_name == NULL)
		return NULL;

	icon = g_themed_icon_new (emblem_name);
	pixbuf = gth_icon_cache_get_pixbuf (window->priv->list_icon_cache, icon);
	g_object_unref (icon);

	return pixbuf;
}


static void
add_selected_from_list_view (GtkTreeModel *model,
			     GtkTreePath  *path,
			     GtkTreeIter  *iter,
			     gpointer      data)
{
	GList    **list = data;
	FileData  *fdata;

	gtk_tree_model_get (model, iter,
			    COLUMN_FILE_DATA, &fdata,
			    -1);
	*list = g_list_prepend (*list, fdata);
}


static void
add_selected_from_tree_view (GtkTreeModel *model,
			     GtkTreePath  *path,
			     GtkTreeIter  *iter,
			     gpointer      data)
{
	GList **list = data;
	char   *dir_path;

	gtk_tree_model_get (model, iter,
			    TREE_COLUMN_PATH, &dir_path,
			    -1);
	*list = g_list_prepend (*list, dir_path);
}


static void
fr_window_populate_file_list (FrWindow  *window,
			      GPtrArray *files)
{
	int         sort_column_id;
	GtkSortType order;
	int         i;

	if (! gtk_widget_get_realized (GTK_WIDGET (window))) {
		_fr_window_stop_activity_mode (window);
		return;
	}

	window->priv->populating_file_list = TRUE;
	gtk_list_store_clear (window->priv->list_store);

	gtk_tree_sortable_get_sort_column_id (GTK_TREE_SORTABLE (window->priv->list_store),
					      &sort_column_id,
					      &order);
	gtk_tree_sortable_set_sort_column_id (GTK_TREE_SORTABLE (window->priv->list_store),
	 				      GTK_TREE_SORTABLE_UNSORTED_SORT_COLUMN_ID,
	 				      0);

	for (i = 0; i < files->len; i++) {
		FileData    *fdata = g_ptr_array_index (files, i);
		GtkTreeIter  iter;
		GdkPixbuf   *icon, *emblem;
		char        *utf8_name;

		if (fdata->list_name == NULL)
			continue;

		gtk_list_store_append (window->priv->list_store, &iter);

		icon = get_icon (window, fdata);
		emblem = get_emblem (window, fdata);
		utf8_name = g_filename_display_name (fdata->list_name);

		if (file_data_is_dir (fdata)) {
			char *utf8_path;
			char *tmp;
			char *s_size;
			char *s_time;

			if (fdata->list_dir)
				tmp = _g_path_remove_ending_separator (fr_window_get_current_location (window));

			else
				tmp = _g_path_remove_level (fdata->path);
			utf8_path = g_filename_display_name (tmp);
			g_free (tmp);

			s_size = g_format_size (fdata->dir_size);

			if (fdata->list_dir)
				s_time = g_strdup ("");
			else
				s_time = _g_time_to_string (fdata->modified);

			gtk_list_store_set (window->priv->list_store, &iter,
					    COLUMN_FILE_DATA, fdata,
					    COLUMN_ICON, icon,
					    COLUMN_NAME, utf8_name,
					    COLUMN_EMBLEM, emblem,
					    COLUMN_TYPE, _("Folder"),
					    COLUMN_SIZE, s_size,
					    COLUMN_TIME, s_time,
					    COLUMN_PATH, utf8_path,
					    -1);
			g_free (utf8_path);
			g_free (s_size);
			g_free (s_time);
		}
		else {
			char       *utf8_path;
			char       *s_size;
			char       *s_time;
			const char *desc;

			utf8_path = g_filename_display_name (fdata->path);

			s_size = g_format_size (fdata->size);
			s_time = _g_time_to_string (fdata->modified);
			desc = g_content_type_get_description (fdata->content_type);

			gtk_list_store_set (window->priv->list_store, &iter,
					    COLUMN_FILE_DATA, fdata,
					    COLUMN_ICON, icon,
					    COLUMN_NAME, utf8_name,
					    COLUMN_EMBLEM, emblem,
					    COLUMN_TYPE, desc,
					    COLUMN_SIZE, s_size,
					    COLUMN_TIME, s_time,
					    COLUMN_PATH, utf8_path,
					    -1);
			g_free (utf8_path);
			g_free (s_size);
			g_free (s_time);
		}

		g_free (utf8_name);
		_g_object_unref (icon);
		_g_object_unref (emblem);
	}

	gtk_tree_sortable_set_sort_column_id (GTK_TREE_SORTABLE (window->priv->list_store),
					      sort_column_id,
					      order);

	window->priv->populating_file_list = FALSE;

	_fr_window_stop_activity_mode (window);
}


static int
path_compare (gconstpointer a,
	      gconstpointer b)
{
	char *path_a = *((char**) a);
	char *path_b = *((char**) b);

	return strcmp (path_a, path_b);
}


static gboolean
get_tree_iter_from_path (FrWindow    *window,
			 const char  *path,
			 GtkTreeIter *parent,
			 GtkTreeIter *iter)
{
	gboolean    result = FALSE;

	if (! gtk_tree_model_iter_children (GTK_TREE_MODEL (window->priv->tree_store), iter, parent))
		return FALSE;

	do {
		GtkTreeIter  tmp;
		char        *iter_path;

		if (get_tree_iter_from_path (window, path, iter, &tmp)) {
			*iter = tmp;
			return TRUE;
		}

		gtk_tree_model_get (GTK_TREE_MODEL (window->priv->tree_store),
				    iter,
				    TREE_COLUMN_PATH, &iter_path,
				    -1);

		if ((iter_path != NULL) && (strcmp (path, iter_path) == 0)) {
			result = TRUE;
			g_free (iter_path);
			break;
		}
		g_free (iter_path);
	} while (gtk_tree_model_iter_next (GTK_TREE_MODEL (window->priv->tree_store), iter));

	return result;
}


static void
fr_window_deactivate_filter (FrWindow *window)
{
	GtkAction *action;

	action = gtk_action_group_get_action (window->priv->actions, "Find");
	g_object_set (action, "active", FALSE, NULL);
}


static void
fr_window_update_current_location (FrWindow *window)
{
	const char *current_dir = fr_window_get_current_location (window);
	char       *path;
	GtkTreeIter iter;

	if (window->priv->list_mode == FR_WINDOW_LIST_MODE_FLAT) {
		gtk_widget_hide (window->priv->location_bar);
		return;
	}

	gtk_widget_show (window->priv->location_bar);

	gtk_entry_set_text (GTK_ENTRY (window->priv->location_entry), window->priv->archive_present? current_dir: "");

	set_sensitive (window, "GoBack", window->priv->archive_present && (current_dir != NULL) && (window->priv->history_current != NULL) && (window->priv->history_current->next != NULL));
	set_sensitive (window, "GoForward", window->priv->archive_present && (current_dir != NULL) && (window->priv->history_current != NULL) && (window->priv->history_current->prev != NULL));
	set_sensitive (window, "GoUp", window->priv->archive_present && (current_dir != NULL) && (strcmp (current_dir, "/") != 0));
	set_sensitive (window, "GoHome", window->priv->archive_present);
	gtk_widget_set_sensitive (window->priv->location_entry, window->priv->archive_present);
	gtk_widget_set_sensitive (window->priv->location_label, window->priv->archive_present);
	gtk_widget_set_sensitive (window->priv->filter_entry, window->priv->archive_present);

	fr_window_deactivate_filter (window);

#if 0
	fr_window_history_print (window);
#endif

	path = _g_path_remove_ending_separator (current_dir);
	if (get_tree_iter_from_path (window, path, NULL, &iter)) {
		GtkTreeSelection *selection;
		GtkTreePath      *t_path;

		t_path = gtk_tree_model_get_path (GTK_TREE_MODEL (window->priv->tree_store), &iter);
		gtk_tree_view_expand_to_path (GTK_TREE_VIEW (window->priv->tree_view), t_path);
		gtk_tree_path_free (t_path);

		selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (window->priv->tree_view));
		gtk_tree_selection_select_iter (selection, &iter);
	}
	g_free (path);
}


static void
fr_window_update_dir_tree (FrWindow *window)
{
	GPtrArray  *dirs;
	GHashTable *dir_cache;
	int         i;
	GdkPixbuf  *icon;

	if (! gtk_widget_get_realized (GTK_WIDGET (window)))
		return;

	gtk_tree_store_clear (window->priv->tree_store);

	if (! window->priv->view_folders
	    || ! window->priv->archive_present
	    || (window->priv->list_mode == FR_WINDOW_LIST_MODE_FLAT))
	{
		gtk_widget_set_sensitive (window->priv->tree_view, FALSE);
		gtk_widget_hide (window->priv->sidepane);
		return;
	}
	else {
		gtk_widget_set_sensitive (window->priv->tree_view, TRUE);
		if (! gtk_widget_get_visible (window->priv->sidepane))
			gtk_widget_show_all (window->priv->sidepane);
	}

	if (gtk_widget_get_realized (window->priv->tree_view))
		gtk_tree_view_scroll_to_point (GTK_TREE_VIEW (window->priv->tree_view), 0, 0);

	/**/

	dirs = g_ptr_array_sized_new (128);

	dir_cache = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, NULL);
	for (i = 0; i < window->archive->files->len; i++) {
		FileData *fdata = g_ptr_array_index (window->archive->files, i);
		char     *dir;

		if (gtk_entry_get_text (GTK_ENTRY (window->priv->filter_entry)) != NULL) {
			if (! file_data_respects_filter (window, fdata))
				continue;
		}

		if (fdata->dir)
			dir = _g_path_remove_ending_separator (fdata->full_path);
		else
			dir = _g_path_remove_level (fdata->full_path);

		while ((dir != NULL) && (strcmp (dir, "/") != 0)) {
			char *new_dir;

			if (g_hash_table_lookup (dir_cache, dir) != NULL)
				break;

			new_dir = dir;
			g_ptr_array_add (dirs, new_dir);
			g_hash_table_replace (dir_cache, new_dir, "1");

			dir = _g_path_remove_level (new_dir);
		}

		g_free (dir);
	}
	g_hash_table_destroy (dir_cache);

	g_ptr_array_sort (dirs, path_compare);
	dir_cache = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, (GDestroyNotify) gtk_tree_path_free);

	/**/

	icon = get_mime_type_icon (window, MIME_TYPE_ARCHIVE);
	{
		GtkTreeIter  node;
		char        *name;

		name = _g_file_get_display_basename (fr_archive_get_file (window->archive));

		gtk_tree_store_append (window->priv->tree_store, &node, NULL);
		gtk_tree_store_set (window->priv->tree_store, &node,
				    TREE_COLUMN_ICON, icon,
				    TREE_COLUMN_NAME, name,
				    TREE_COLUMN_PATH, "/",
				    TREE_COLUMN_WEIGHT, PANGO_WEIGHT_BOLD,
				    -1);
		g_hash_table_replace (dir_cache, "/", gtk_tree_model_get_path (GTK_TREE_MODEL (window->priv->tree_store), &node));

		g_free (name);
	}
	g_object_unref (icon);

	/**/

	icon = get_mime_type_icon (window, MIME_TYPE_DIRECTORY);
	for (i = 0; i < dirs->len; i++) {
		char        *dir = g_ptr_array_index (dirs, i);
		char        *parent_dir;
		GtkTreePath *parent_path;
		GtkTreeIter  parent;
		GtkTreeIter  node;

		parent_dir = _g_path_remove_level (dir);
		if (parent_dir == NULL)
			continue;

		parent_path = g_hash_table_lookup (dir_cache, parent_dir);
		gtk_tree_model_get_iter (GTK_TREE_MODEL (window->priv->tree_store),
					 &parent,
					 parent_path);
		gtk_tree_store_append (window->priv->tree_store, &node, &parent);
		gtk_tree_store_set (window->priv->tree_store, &node,
				    TREE_COLUMN_ICON, icon,
				    TREE_COLUMN_NAME, _g_path_get_basename (dir),
				    TREE_COLUMN_PATH, dir,
				    TREE_COLUMN_WEIGHT, PANGO_WEIGHT_NORMAL,
				    -1);
		g_hash_table_replace (dir_cache, dir, gtk_tree_model_get_path (GTK_TREE_MODEL (window->priv->tree_store), &node));

		g_free (parent_dir);
	}
	g_hash_table_destroy (dir_cache);
	if (icon != NULL)
		g_object_unref (icon);

	g_ptr_array_free (dirs, TRUE);

	fr_window_update_current_location (window);
}


static void
fr_window_update_file_list (FrWindow *window,
			    gboolean  update_view)
{
	GPtrArray  *files;
	gboolean    free_files = FALSE;

	if (! gtk_widget_get_realized (GTK_WIDGET (window)))
		return;

	if (gtk_widget_get_realized (window->priv->list_view))
		gtk_tree_view_scroll_to_point (GTK_TREE_VIEW (window->priv->list_view), 0, 0);

	if (! window->priv->archive_present || window->priv->archive_new) {
		if (update_view)
			gtk_list_store_clear (window->priv->list_store);

		window->priv->current_view_length = 0;

		if (window->priv->archive_new) {
			gtk_widget_set_sensitive (window->priv->list_view, TRUE);
			gtk_widget_show_all (gtk_widget_get_parent (window->priv->list_view));
		}
		else {
			gtk_widget_set_sensitive (window->priv->list_view, FALSE);
			gtk_widget_hide (gtk_widget_get_parent (window->priv->list_view));
		}

		return;
	}
	else {
		gtk_widget_set_sensitive (window->priv->list_view, TRUE);
		gtk_widget_show_all (gtk_widget_get_parent (window->priv->list_view));
	}

	if (window->priv->give_focus_to_the_list) {
		gtk_widget_grab_focus (window->priv->list_view);
		window->priv->give_focus_to_the_list = FALSE;
	}

	/**/

	_fr_window_start_activity_mode (window);

	if (window->priv->list_mode == FR_WINDOW_LIST_MODE_FLAT) {
		fr_window_compute_list_names (window, window->archive->files);
		files = window->archive->files;
		free_files = FALSE;
	}
	else {
		char *current_dir = g_strdup (fr_window_get_current_location (window));

		while (! fr_window_dir_exists_in_archive (window, current_dir)) {
			char *tmp;

			fr_window_history_pop (window);

			tmp = get_parent_dir (current_dir);
			g_free (current_dir);
			current_dir = tmp;

			fr_window_history_add (window, current_dir);
		}
		g_free (current_dir);

		fr_window_compute_list_names (window, window->archive->files);
		files = fr_window_get_current_dir_list (window);
		free_files = TRUE;
	}

	if (files != NULL)
		window->priv->current_view_length = files->len;
	else
		window->priv->current_view_length = 0;

	if (update_view)
		fr_window_populate_file_list (window, files);

	if (free_files)
		g_ptr_array_free (files, TRUE);
}


static void
fr_window_update_title (FrWindow *window)
{
	if (! window->priv->archive_present)
		gtk_window_set_title (GTK_WINDOW (window), _("Archive Manager"));
	else {
		char *title;
		char *name;

		name = _g_file_get_display_basename (fr_window_get_archive_file (window));
		title = g_strdup_printf ("%s %s",
					 name,
					 window->archive->read_only ? _("[read only]") : "");

		gtk_window_set_title (GTK_WINDOW (window), title);
		g_free (title);
		g_free (name);
	}
}


static gboolean
location_entry_key_press_event_cb (GtkWidget   *widget,
				   GdkEventKey *event,
				   FrWindow    *window)
{
	if ((event->keyval == GDK_KEY_Return)
	    || (event->keyval == GDK_KEY_KP_Enter)
	    || (event->keyval == GDK_KEY_ISO_Enter))
	{
		fr_window_go_to_location (window, gtk_entry_get_text (GTK_ENTRY (window->priv->location_entry)), FALSE);
	}

	return FALSE;
}


static void
_fr_window_close_after_notification (FrWindow *window)
{
	fr_window_set_current_batch_action (window, FR_BATCH_ACTION_QUIT, NULL, NULL);
	fr_window_restart_current_batch_action (window);
}


static gboolean
real_close_progress_dialog (gpointer data)
{
	FrWindow *window = data;

	if (window->priv->hide_progress_timeout != 0) {
		g_source_remove (window->priv->hide_progress_timeout);
		window->priv->hide_progress_timeout = 0;
	}

	if (window->priv->progress_dialog != NULL)
		gtk_widget_hide (window->priv->progress_dialog);

	if (window->priv->batch_mode && window->priv->quit_with_progress_dialog)
		_fr_window_close_after_notification (window);

	return FALSE;
}


static void
close_progress_dialog (FrWindow *window,
		       gboolean  close_now)
{
	if (window->priv->progress_timeout != 0) {
		g_source_remove (window->priv->progress_timeout);
		window->priv->progress_timeout = 0;
	}

	if (! window->priv->batch_mode)
		gtk_widget_hide (window->priv->progress_bar);

	if (window->priv->progress_dialog == NULL)
		return;

	if (close_now) {
		if (window->priv->hide_progress_timeout != 0) {
			g_source_remove (window->priv->hide_progress_timeout);
			window->priv->hide_progress_timeout = 0;
		}
		real_close_progress_dialog (window);
	}
	else {
		if (window->priv->hide_progress_timeout != 0)
			return;
		window->priv->hide_progress_timeout = g_timeout_add (HIDE_PROGRESS_TIMEOUT_MSECS,
								     real_close_progress_dialog,
								     window);
	}
}


static gboolean
progress_dialog_delete_event (GtkWidget *caller,
			      GdkEvent  *event,
			      FrWindow  *window)
{
	if (window->priv->stoppable) {
		activate_action_stop (NULL, window);
		close_progress_dialog (window, TRUE);
	}

	return TRUE;
}


static void
open_folder (GtkWindow *parent_window,
	     GFile     *folder)
{
	GError *error = NULL;
	char   *uri;

	if (folder == NULL)
		return;

	uri = g_file_get_uri (folder);
	if (! gtk_show_uri (parent_window != NULL ? gtk_window_get_screen (parent_window) : NULL,
			    uri,
			    GDK_CURRENT_TIME, &error))
	{
		GtkWidget *d;
		char      *utf8_name;
		char      *message;

		utf8_name = _g_file_get_display_basename (folder);
		message = g_strdup_printf (_("Could not display the folder \"%s\""), utf8_name);
		g_free (utf8_name);

		d = _gtk_error_dialog_new (parent_window,
					   GTK_DIALOG_MODAL,
					   NULL,
					   message,
					   "%s",
					   error->message);
		gtk_dialog_run (GTK_DIALOG (d));
		gtk_widget_destroy (d);

		g_free (message);
		g_clear_error (&error);
	}

	g_free (uri);
}


static void
fr_window_view_extraction_destination_folder (FrWindow *window)
{
	open_folder (GTK_WINDOW (window), window->priv->last_extraction_destination);
}


static gboolean
close_window_cb (gpointer data)
{
	fr_window_close (FR_WINDOW (data));
	return FALSE;
}


static void
progress_dialog_response (GtkDialog *dialog,
			  int        response_id,
			  FrWindow  *window)
{
	GtkWidget *new_window;
	GFile     *saved_file;

	saved_file = window->priv->saving_file;
	window->priv->saving_file = NULL;

	switch (response_id) {
	case GTK_RESPONSE_CANCEL:
		if (window->priv->stoppable) {
			activate_action_stop (NULL, window);
			close_progress_dialog (window, TRUE);
		}
		break;
	case GTK_RESPONSE_CLOSE:
		close_progress_dialog (window, TRUE);
		break;
	case DIALOG_RESPONSE_OPEN_ARCHIVE:
		new_window = fr_window_new ();
		gtk_widget_show (new_window);
		fr_window_archive_open (FR_WINDOW (new_window), saved_file, GTK_WINDOW (new_window));
		close_progress_dialog (window, TRUE);
		break;
	case DIALOG_RESPONSE_OPEN_DESTINATION_FOLDER:
		fr_window_view_extraction_destination_folder (window);
		close_progress_dialog (window, TRUE);
		break;
	case DIALOG_RESPONSE_QUIT:
		g_idle_add (close_window_cb, window);
		break;
	default:
		break;
	}

	_g_object_unref (saved_file);
}


static char *
get_action_description (FrWindow *window,
			FrAction  action,
			GFile    *file)
{
	char *basename;
	char *message;

	basename = _g_file_get_display_basename (file);

	message = NULL;
	switch (action) {
	case FR_ACTION_CREATING_NEW_ARCHIVE:
		/* Translators: %s is a filename */
		message = g_strdup_printf (_("Creating \"%s\""), basename);
		break;
	case FR_ACTION_LOADING_ARCHIVE:
		/* Translators: %s is a filename */
		message = g_strdup_printf (_("Loading \"%s\""), basename);
		break;
	case FR_ACTION_LISTING_CONTENT:
		/* Translators: %s is a filename */
		message = g_strdup_printf (_("Reading \"%s\""), basename);
		break;
	case FR_ACTION_DELETING_FILES:
		/* Translators: %s is a filename */
		message = g_strdup_printf (_("Deleting the files from \"%s\""), basename);
		break;
	case FR_ACTION_TESTING_ARCHIVE:
		/* Translators: %s is a filename */
		message = g_strdup_printf (_("Testing \"%s\""), basename);
		break;
	case FR_ACTION_GETTING_FILE_LIST:
		message = g_strdup (_("Getting the file list"));
		break;
	case FR_ACTION_COPYING_FILES_FROM_REMOTE:
		/* Translators: %s is a filename */
		message = g_strdup_printf (_("Copying the files to add to \"%s\""), basename);
		break;
	case FR_ACTION_ADDING_FILES:
		/* Translators: %s is a filename */
		message = g_strdup_printf (_("Adding the files to \"%s\""), basename);
		break;
	case FR_ACTION_EXTRACTING_FILES:
		/* Translators: %s is a filename */
		message = g_strdup_printf (_("Extracting the files from \"%s\""), basename);
		break;
	case FR_ACTION_COPYING_FILES_TO_REMOTE:
		message = g_strdup (_("Copying the extracted files to the destination"));
		break;
	case FR_ACTION_CREATING_ARCHIVE:
		/* Translators: %s is a filename */
		message = g_strdup_printf (_("Creating \"%s\""), basename);
		break;
	case FR_ACTION_SAVING_REMOTE_ARCHIVE:
	case FR_ACTION_ENCRYPTING_ARCHIVE:
		/* Translators: %s is a filename */
		message = g_strdup_printf (_("Saving \"%s\""), basename);
		break;
	case FR_ACTION_PASTING_FILES:
		message = g_strdup (window->priv->custom_action_message);
		break;
	case FR_ACTION_RENAMING_FILES:
		/* Translators: %s is a filename */
		message = g_strdup_printf (_("Renaming the files in \"%s\""), basename);
		break;
	case FR_ACTION_UPDATING_FILES:
		/* Translators: %s is a filename */
		message = g_strdup_printf (_("Updating the files in \"%s\""), basename);
		break;
	case FR_ACTION_NONE:
		break;
	}

	g_free (basename);

	return message;
}


static void
progress_dialog_set_action_description (FrWindow   *window,
					const char *description)
{
	char *description_markup;

	description_markup = g_markup_printf_escaped ("<span weight=\"bold\" size=\"larger\">%s</span>", description);
	gtk_label_set_markup (GTK_LABEL (window->priv->pd_action), description_markup);

	g_free (description_markup);
}


static void
progress_dialog_update_action_description (FrWindow *window)
{
	GFile *current_archive;
	char  *description;


	if (window->priv->progress_dialog == NULL)
		return;

	if (window->priv->saving_file != NULL)
		current_archive = window->priv->saving_file;
	else if (window->priv->working_archive != NULL)
		current_archive = window->priv->working_archive;
	else
		current_archive = window->priv->archive_file;

	_g_clear_object (&window->priv->pd_last_archive);
	if (current_archive != NULL)
		window->priv->pd_last_archive = g_object_ref (current_archive);

	description = get_action_description (window, window->priv->action, window->priv->pd_last_archive);
	progress_dialog_set_action_description (window, description);

	g_free (description);
}


static gboolean
fr_window_working_archive_cb (FrArchive  *archive,
			      const char *archive_uri,
			      FrWindow   *window)
{
	_g_clear_object (&window->priv->working_archive);
	if (archive_uri != NULL)
		window->priv->working_archive = g_file_new_for_uri (archive_uri);
	progress_dialog_update_action_description (window);

	return TRUE;
}


static gboolean
fr_archive_message_cb (FrArchive  *archive,
		       const char *msg,
		       FrWindow   *window)
{
	if (window->priv->pd_last_message != msg) {
		g_free (window->priv->pd_last_message);
		window->priv->pd_last_message = g_strdup (msg);
	}

	if (window->priv->progress_dialog == NULL)
		return TRUE;

	if (msg != NULL) {
		while (*msg == ' ')
			msg++;
		if (*msg == 0)
			msg = NULL;
	}

	if (msg != NULL) {
		char *utf8_msg;

		if (! g_utf8_validate (msg, -1, NULL))
			utf8_msg = g_locale_to_utf8 (msg, -1 , 0, 0, 0);
		else
			utf8_msg = g_strdup (msg);
		if (utf8_msg == NULL)
			return TRUE;

		if (g_utf8_validate (utf8_msg, -1, NULL)) {
			gtk_label_set_text (GTK_LABEL (window->priv->pd_message), utf8_msg);
			gtk_widget_show (window->priv->pd_message);
		}

		g_free (window->priv->pd_last_message);
		window->priv->pd_last_message = g_strdup (utf8_msg);

		g_signal_emit (G_OBJECT (window),
			       fr_window_signals[PROGRESS],
			       0,
			       window->priv->pd_last_fraction,
			       window->priv->pd_last_message);

#ifdef LOG_PROGRESS
		g_print ("message > %s\n", utf8_msg);
#endif

		g_free (utf8_msg);
	}
	else
		gtk_widget_hide (window->priv->pd_message);

	progress_dialog_update_action_description (window);

	return TRUE;
}


static void
fr_archive_start_cb (FrArchive *archive,
		     FrAction   action,
		     FrWindow  *window)
{
	char *description;

	description = get_action_description (window, action, fr_archive_get_file (archive));
	fr_archive_message_cb (archive, description, window);

	g_free (description);
}


static void
create_the_progress_dialog (FrWindow *window)
{
	GtkWindow      *parent;
	GtkDialogFlags  flags;
	const char     *title;
	GtkBuilder     *builder;
	GtkWidget      *dialog;

	if (window->priv->progress_dialog != NULL)
		return;

	if (window->priv->batch_mode) {
		parent = NULL;
		flags = 0;
		title = window->priv->batch_title;
	}
	else {
		parent = GTK_WINDOW (window);
		flags = GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL;
		title = NULL;
	}

	builder = _gtk_builder_new_from_resource ("progress-dialog.ui");
	dialog = _gtk_builder_get_widget (builder, "progress_dialog");
	gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_OK);
	gtk_window_set_title (GTK_WINDOW (dialog), title);
	gtk_window_set_transient_for (GTK_WINDOW (dialog), parent);
	gtk_window_set_modal (GTK_WINDOW (dialog), (flags & GTK_DIALOG_MODAL));
	gtk_window_set_destroy_with_parent (GTK_WINDOW (dialog), (flags & GTK_DIALOG_DESTROY_WITH_PARENT));
	g_object_weak_ref (G_OBJECT (dialog), (GWeakNotify) g_object_unref, builder);

	_gtk_dialog_add_to_window_group (GTK_DIALOG (dialog));

	window->priv->pd_quit_button = gtk_dialog_add_button (GTK_DIALOG (dialog), GTK_STOCK_QUIT, DIALOG_RESPONSE_QUIT);
	window->priv->pd_open_archive_button = gtk_dialog_add_button (GTK_DIALOG (dialog), _("_Open the Archive"), DIALOG_RESPONSE_OPEN_ARCHIVE);
	window->priv->pd_open_destination_button = gtk_dialog_add_button (GTK_DIALOG (dialog), _("_Show the Files"), DIALOG_RESPONSE_OPEN_DESTINATION_FOLDER);
	window->priv->pd_close_button = gtk_dialog_add_button (GTK_DIALOG (dialog), GTK_STOCK_CLOSE, GTK_RESPONSE_CLOSE);
	window->priv->pd_cancel_button = gtk_dialog_add_button (GTK_DIALOG (dialog), GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL);
	gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_OK);

	window->priv->progress_dialog = dialog;
	window->priv->pd_icon = _gtk_builder_get_widget (builder, "icon_image");
	window->priv->pd_action = _gtk_builder_get_widget (builder, "action_label");
	window->priv->pd_progress_bar = _gtk_builder_get_widget (builder, "progress_progressbar");
	window->priv->pd_message = _gtk_builder_get_widget (builder, "message_label");
	window->priv->pd_progress_box = _gtk_builder_get_widget (builder, "progress_box");

	_g_clear_object (&window->priv->pd_last_archive);
	window->priv->pd_last_archive = _g_object_ref (window->priv->archive_file);

	progress_dialog_update_action_description (window);

	/* signals */

	g_signal_connect (G_OBJECT (window->priv->progress_dialog),
			  "response",
			  G_CALLBACK (progress_dialog_response),
			  window);
	g_signal_connect (G_OBJECT (window->priv->progress_dialog),
			  "delete_event",
			  G_CALLBACK (progress_dialog_delete_event),
			  window);
}


static gboolean
display_progress_dialog (gpointer data)
{
	FrWindow *window = data;

	if (window->priv->progress_timeout != 0)
		g_source_remove (window->priv->progress_timeout);

	if (window->priv->use_progress_dialog && (window->priv->progress_dialog != NULL)) {
		gtk_dialog_set_response_sensitive (GTK_DIALOG (window->priv->progress_dialog),
						   GTK_RESPONSE_OK,
						   window->priv->stoppable);
		if (! window->priv->batch_mode)
			gtk_window_present (GTK_WINDOW (window));
		gtk_widget_hide (window->priv->progress_bar);
		gtk_window_present (GTK_WINDOW (window->priv->progress_dialog));
		fr_archive_message_cb (NULL, window->priv->pd_last_message, window);
	}

	window->priv->progress_timeout = 0;

	return FALSE;
}


static void
open_progress_dialog (FrWindow *window,
		      gboolean  open_now)
{
	if (window->priv->hide_progress_timeout != 0) {
		g_source_remove (window->priv->hide_progress_timeout);
		window->priv->hide_progress_timeout = 0;
	}

	if (open_now) {
		if (window->priv->progress_timeout != 0)
			g_source_remove (window->priv->progress_timeout);
		window->priv->progress_timeout = 0;
	}

	if ((window->priv->progress_timeout != 0)
	    || ((window->priv->progress_dialog != NULL) && gtk_widget_get_visible (window->priv->progress_dialog)))
		return;

	if (! window->priv->batch_mode && ! open_now)
		gtk_widget_show (window->priv->progress_bar);

	create_the_progress_dialog (window);
	gtk_widget_show (window->priv->pd_cancel_button);
	gtk_widget_hide (window->priv->pd_open_archive_button);
	gtk_widget_hide (window->priv->pd_open_destination_button);
	gtk_widget_hide (window->priv->pd_quit_button);
	gtk_widget_hide (window->priv->pd_close_button);

	if (open_now)
		display_progress_dialog (window);
	else
		window->priv->progress_timeout = g_timeout_add (PROGRESS_TIMEOUT_MSECS,
								display_progress_dialog,
								window);
}


static gboolean
fr_archive_progress_cb (FrArchive *archive,
		        double     fraction,
		        FrWindow  *window)
{
	window->priv->progress_pulse = (fraction < 0.0);
	if (! window->priv->progress_pulse) {
		fraction = CLAMP (fraction, 0.0, 1.0);
		if (window->priv->progress_dialog != NULL)
			gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (window->priv->pd_progress_bar), fraction);
		gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (window->priv->progress_bar), fraction);

		if ((archive != NULL) && (fr_archive_progress_get_total_files (archive) > 0)) {
			char *message = NULL;
			int   remaining_files;

			remaining_files = fr_archive_progress_get_total_files (archive) - fr_archive_progress_get_completed_files (archive);

			switch (window->priv->action) {
			case FR_ACTION_ADDING_FILES:
			case FR_ACTION_EXTRACTING_FILES:
			case FR_ACTION_DELETING_FILES:
			case FR_ACTION_UPDATING_FILES:
				if (remaining_files > 0)
					message = g_strdup_printf (ngettext ("%d file remaining",
									     "%'d files remaining",
									     remaining_files), remaining_files);
				else
					message = g_strdup (_("Please wait…"));
				break;
			default:
				break;
			}

			if (message != NULL) {
				fr_archive_message (archive, message);
				g_free (message);
			}
		}

		if (fraction == 1.0)
			gtk_widget_hide (window->priv->pd_progress_box);
		else
			gtk_widget_show (window->priv->pd_progress_box);

		window->priv->pd_last_fraction = fraction;

		g_signal_emit (G_OBJECT (window),
			       fr_window_signals[PROGRESS],
			       0,
			       window->priv->pd_last_fraction,
			       window->priv->pd_last_message);

#ifdef LOG_PROGRESS
		g_print ("progress > %2.2f\n", fraction);
#endif
	}
	return TRUE;
}


static void
open_progress_dialog_with_open_destination (FrWindow *window)
{
	if (window->priv->hide_progress_timeout != 0) {
		g_source_remove (window->priv->hide_progress_timeout);
		window->priv->hide_progress_timeout = 0;
	}
	if (window->priv->progress_timeout != 0) {
		g_source_remove (window->priv->progress_timeout);
		window->priv->progress_timeout = 0;
	}

	create_the_progress_dialog (window);
	gtk_widget_hide (window->priv->pd_cancel_button);
	gtk_widget_hide (window->priv->pd_open_archive_button);
	gtk_widget_show (window->priv->pd_open_destination_button);
	gtk_widget_set_visible (window->priv->pd_quit_button, ! window->priv->quit_with_progress_dialog);
	gtk_widget_show (window->priv->pd_close_button);
	display_progress_dialog (window);

	fr_archive_progress_cb (NULL, 1.0, window);
	fr_archive_message_cb (NULL, NULL, window);

	progress_dialog_set_action_description (window, _("Extraction completed successfully"));
}


static void
open_progress_dialog_with_open_archive (FrWindow *window)
{
	char *basename;
	char *description;

	if (window->priv->hide_progress_timeout != 0) {
		g_source_remove (window->priv->hide_progress_timeout);
		window->priv->hide_progress_timeout = 0;
	}
	if (window->priv->progress_timeout != 0) {
		g_source_remove (window->priv->progress_timeout);
		window->priv->progress_timeout = 0;
	}

	create_the_progress_dialog (window);
	gtk_widget_hide (window->priv->pd_cancel_button);
	gtk_widget_hide (window->priv->pd_open_destination_button);
	gtk_widget_show (window->priv->pd_open_archive_button);
	gtk_widget_set_visible (window->priv->pd_quit_button, ! window->priv->quit_with_progress_dialog);
	gtk_widget_show (window->priv->pd_close_button);
	display_progress_dialog (window);

	fr_archive_progress_cb (NULL, 1.0, window);
	fr_archive_message_cb (NULL, NULL, window);

	basename = _g_file_get_display_basename (window->priv->saving_file);
	/* Translators: %s is a filename */
	description = g_strdup_printf (_("\"%s\" created successfully"), basename);
	progress_dialog_set_action_description (window, description);

	g_free (description);
	g_free (basename);
}


static void
fr_window_push_message (FrWindow   *window,
			const char *msg)
{
	if (! gtk_widget_get_mapped (GTK_WIDGET (window)))
		return;

	gtk_statusbar_push (GTK_STATUSBAR (window->priv->statusbar),
			    window->priv->progress_cid,
			    msg);
}


static void
fr_window_add_to_recent_list (FrWindow *window,
			      GFile    *file)
{
	char *uri;

	if (_g_file_is_temp_dir (file))
		return;

	uri = g_file_get_uri (file);

	if (window->archive->mime_type != NULL) {
		GtkRecentData *recent_data;

		recent_data = g_new0 (GtkRecentData, 1);
		recent_data->mime_type = g_content_type_get_mime_type (window->archive->mime_type);
		recent_data->app_name = "File Roller";
		recent_data->app_exec = "file-roller";
		gtk_recent_manager_add_full (gtk_recent_manager_get_default (), uri, recent_data);

		g_free (recent_data);
	}
	else
		gtk_recent_manager_add_item (gtk_recent_manager_get_default (), uri);

	g_free (uri);
}


static void
fr_window_remove_from_recent_list (FrWindow *window,
				   GFile    *file)
{
	char *uri;

	if (file == NULL)
		return;

	uri = g_file_get_uri (file);
	gtk_recent_manager_remove_item (gtk_recent_manager_get_default (), uri, NULL);

	g_free (uri);
}


static void
error_dialog_response_cb (GtkDialog *dialog,
			  gint       arg1,
			  gpointer   user_data)
{
	FrWindow  *window = user_data;

	window->priv->showing_error_dialog = FALSE;
	gtk_widget_destroy (GTK_WIDGET (dialog));

	if (window->priv->destroy_with_error_dialog)
		gtk_widget_destroy (GTK_WIDGET (window));
}


static void
fr_window_show_error_dialog (FrWindow   *window,
			     GtkWidget  *dialog,
			     GtkWindow  *dialog_parent,
			     const char *details)
{
	if (window->priv->batch_mode && ! window->priv->use_progress_dialog) {
		GError *error;

		error = g_error_new_literal (FR_ERROR, FR_ERROR_GENERIC, details ? details : _("Command exited abnormally."));
		g_signal_emit (window,
			       fr_window_signals[READY],
			       0,
			       error);

		gtk_widget_destroy (GTK_WIDGET (window));

		return;
	}

	close_progress_dialog (window, TRUE);

	if (window->priv->batch_mode) {
		gtk_window_set_title (GTK_WINDOW (dialog), window->priv->batch_title);
		fr_window_destroy_with_error_dialog (window);
	}

	g_signal_connect (dialog,
			  "response",
			  G_CALLBACK (error_dialog_response_cb),
			  window);
	if (dialog_parent != NULL)
		gtk_window_set_modal (GTK_WINDOW (dialog), TRUE);
	gtk_widget_show (dialog);

	window->priv->showing_error_dialog = TRUE;
}


void
fr_window_destroy_with_error_dialog (FrWindow *window)
{
	window->priv->destroy_with_error_dialog = TRUE;
}


void
fr_window_set_notify (FrWindow   *window,
		      gboolean    notify)
{
	window->priv->notify = notify;
}


static void
_handle_archive_operation_error (FrWindow  *window,
				 FrArchive *archive,
				 FrAction   action,
				 GError    *error,
				 gboolean  *continue_batch,
				 gboolean  *opens_dialog)
{
	GtkWindow *dialog_parent;
	char      *msg;
	char      *utf8_name;
	char      *details;
	GList     *output;
	GtkWidget *dialog;

	if (continue_batch) *continue_batch = (error == NULL);
	if (opens_dialog) *opens_dialog = FALSE;

	if (error == NULL)
		return;

	if ((error != NULL) && (error->code == FR_ERROR_STOPPED))
		g_cancellable_reset (window->priv->cancellable);

	switch (error->code) {
	case FR_ERROR_ASK_PASSWORD:
		close_progress_dialog (window, TRUE);
		dlg_ask_password (window);
		if (opens_dialog) *opens_dialog = TRUE;
		break;

	case FR_ERROR_UNSUPPORTED_FORMAT:
		close_progress_dialog (window, TRUE);
		dlg_package_installer (window,
				       window->priv->archive_file,
				       action,
				       window->priv->cancellable);
		if (opens_dialog) *opens_dialog = TRUE;
		break;

#if 0
	case FR_PROC_ERROR_BAD_CHARSET:
		close_progress_dialog (window, TRUE);
		/* dlg_ask_archive_charset (window); FIXME: implement after feature freeze */
		break;
#endif

	case FR_ERROR_STOPPED:
		/* nothing */
		break;

	default:
		/* generic error => show an error dialog */

		msg = NULL;
		details = NULL;
		output = NULL;

		if (window->priv->batch_mode) {
			dialog_parent = NULL;
			window->priv->load_error_parent_window = NULL;
		}
		else {
			dialog_parent = (GtkWindow *) window;
			if (window->priv->load_error_parent_window == NULL)
				window->priv->load_error_parent_window = (GtkWindow *) window;
		}

		switch (action) {
		case FR_ACTION_CREATING_NEW_ARCHIVE:
			dialog_parent = window->priv->load_error_parent_window;
			msg = _("Could not create the archive");
			break;

		case FR_ACTION_EXTRACTING_FILES:
		case FR_ACTION_COPYING_FILES_TO_REMOTE:
			msg = _("An error occurred while extracting files.");
			break;

		case FR_ACTION_LOADING_ARCHIVE:
			dialog_parent = window->priv->load_error_parent_window;
			utf8_name = _g_file_get_display_basename (window->priv->archive_file);
			msg = g_strdup_printf (_("Could not open \"%s\""), utf8_name);
			g_free (utf8_name);
			break;

		case FR_ACTION_LISTING_CONTENT:
			msg = _("An error occurred while loading the archive.");
			break;

		case FR_ACTION_DELETING_FILES:
			msg = _("An error occurred while deleting files from the archive.");
			break;

		case FR_ACTION_ADDING_FILES:
		case FR_ACTION_GETTING_FILE_LIST:
		case FR_ACTION_COPYING_FILES_FROM_REMOTE:
			msg = _("An error occurred while adding files to the archive.");
			break;

		case FR_ACTION_TESTING_ARCHIVE:
			msg = _("An error occurred while testing archive.");
			break;

		case FR_ACTION_SAVING_REMOTE_ARCHIVE:
		case FR_ACTION_ENCRYPTING_ARCHIVE:
			msg = _("An error occurred while saving the archive.");
			break;

		case FR_ACTION_RENAMING_FILES:
			msg = _("An error occurred while renaming the files.");
			break;

		case FR_ACTION_UPDATING_FILES:
			msg = _("An error occurred while updating the files.");
			break;

		default:
			msg = _("An error occurred.");
			break;
		}

		switch (error->code) {
		case FR_ERROR_COMMAND_NOT_FOUND:
			details = _("Command not found.");
			break;
		case FR_ERROR_EXITED_ABNORMALLY:
			details = _("Command exited abnormally.");
			break;
		case FR_ERROR_SPAWN:
			details = error->message;
			break;
		default:
			details = error->message;
			break;
		}

		if ((error->code != FR_ERROR_GENERIC) && FR_IS_COMMAND (archive))
			output = fr_command_get_last_output (FR_COMMAND (archive));

		dialog = _gtk_error_dialog_new (dialog_parent,
						0,
						output,
						msg,
						((details != NULL) ? "%s" : NULL),
						details);
		fr_window_show_error_dialog (window, dialog, dialog_parent, details);
		break;
	}
}


static void fr_window_exec_next_batch_action (FrWindow *window);
static void fr_window_archive_list (FrWindow *window);


static void
_archive_operation_completed (FrWindow *window,
			      FrAction  action,
			      GError   *error)
{
	gboolean  continue_batch = FALSE;
	gboolean  opens_dialog;
	gboolean  operation_canceled;
	GFile    *archive_dir;
	gboolean  is_temp_dir;

#ifdef DEBUG
	debug (DEBUG_INFO, "%s [DONE] (FR::Window)\n", action_names[action]);
#endif

	_fr_window_stop_activity_mode (window);
	_handle_archive_operation_error (window, window->archive, action, error, &continue_batch, &opens_dialog);
	if (opens_dialog)
		return;

	operation_canceled = g_error_matches (error, FR_ERROR, FR_ERROR_STOPPED);

	switch (action) {
	case FR_ACTION_CREATING_NEW_ARCHIVE:
	case FR_ACTION_CREATING_ARCHIVE:
		close_progress_dialog (window, FALSE);
		if (! operation_canceled) {
			fr_window_history_clear (window);
			fr_window_go_to_location (window, "/", TRUE);
			fr_window_update_dir_tree (window);
			fr_window_update_title (window);
			fr_window_update_sensitivity (window);
		}
		break;

	case FR_ACTION_LOADING_ARCHIVE:
		close_progress_dialog (window, FALSE);
		if (error != NULL) {
			fr_window_remove_from_recent_list (window, window->priv->archive_file);
			fr_window_archive_close (window);
		}
		else {
			fr_window_archive_list (window);
			return;
		}
		break;

	case FR_ACTION_LISTING_CONTENT:
		/* update the file because multi-volume archives can have
		 * a different name after loading. */
		_g_object_unref (window->priv->archive_file);
		window->priv->archive_file = _g_object_ref (fr_archive_get_file (window->archive));

		window->priv->reload_archive = FALSE;

		close_progress_dialog (window, FALSE);
		if (error != NULL) {
			fr_window_remove_from_recent_list (window, window->priv->archive_file);
			fr_window_archive_close (window);
			fr_window_set_password (window, NULL);
			break;
		}

		/* error == NULL */

		archive_dir = g_file_get_parent (window->priv->archive_file);
		is_temp_dir = _g_file_is_temp_dir (archive_dir);
		if (! window->priv->archive_present) {
			window->priv->archive_present = TRUE;

			fr_window_history_clear (window);
			fr_window_history_add (window, "/");

			if (! is_temp_dir) {
				fr_window_set_open_default_dir (window, archive_dir);
				fr_window_set_add_default_dir (window, archive_dir);
				if (! window->priv->freeze_default_dir)
					fr_window_set_extract_default_dir (window, archive_dir, FALSE);
			}

			window->priv->archive_new = FALSE;
		}
		g_object_unref (archive_dir);

		if (! is_temp_dir)
			fr_window_add_to_recent_list (window, window->priv->archive_file);

		fr_window_update_history (window);
		fr_window_update_title (window);
		fr_window_go_to_location (window, fr_window_get_current_location (window), TRUE);
		fr_window_update_dir_tree (window);

		if (! window->priv->batch_mode)
			gtk_window_present (GTK_WINDOW (window));
		break;

	case FR_ACTION_DELETING_FILES:
	case FR_ACTION_ADDING_FILES:
		close_progress_dialog (window, FALSE);

		/* update the file because multi-volume archives can have
		 * a different name after creation. */
		_g_object_unref (window->priv->archive_file);
		window->priv->archive_file = _g_object_ref (fr_archive_get_file (window->archive));

		if (window->priv->notify) {
			_g_object_unref (window->priv->saving_file);
			window->priv->saving_file = g_object_ref (window->priv->archive_file);
		}

		if (error == NULL) {
			if (window->priv->archive_new)
				window->priv->archive_new = FALSE;
			fr_window_add_to_recent_list (window, window->priv->archive_file);
		}

		if (! window->priv->batch_mode && ! operation_canceled)
			window->priv->reload_archive = TRUE;

		break;

	case FR_ACTION_TESTING_ARCHIVE:
		close_progress_dialog (window, FALSE);
		if (error == NULL)
			fr_window_view_last_output (window, _("Test Result"));
		return;

	case FR_ACTION_EXTRACTING_FILES:
		close_progress_dialog (window, FALSE);
		break;

	case FR_ACTION_RENAMING_FILES:
	case FR_ACTION_UPDATING_FILES:
		close_progress_dialog (window, FALSE);
		if (! window->priv->batch_mode && ! operation_canceled)
			window->priv->reload_archive = TRUE;
		break;

	default:
		close_progress_dialog (window, FALSE);
		continue_batch = FALSE;
		break;
	}

	if (continue_batch)
		fr_window_exec_next_batch_action (window);
	else
		fr_window_stop_batch (window);
}


static void
_archive_operation_cancelled (FrWindow *window,
			      FrAction  action)
{
	GError *error;

	error = g_error_new_literal (FR_ERROR, FR_ERROR_STOPPED, "");
	_archive_operation_completed (window, action, error);

	g_error_free (error);
}


static void
_archive_operation_started (FrWindow *window,
			    FrAction  action)
{
	GFile *archive;
	char  *message;

	window->priv->action = action;
	_fr_window_start_activity_mode (window);

#ifdef DEBUG
	debug (DEBUG_INFO, "%s [START] (FR::Window)\n", action_names[action]);
#endif

	archive = window->priv->pd_last_archive;
	if (archive == NULL)
		archive =  window->priv->archive_file;
	message = get_action_description (window, action, archive);
	fr_window_push_message (window, message);
	g_free (message);

	switch (action) {
	case FR_ACTION_EXTRACTING_FILES:
		open_progress_dialog (window, ((window->priv->saving_file != NULL)
					       || window->priv->batch_mode));
		break;
	default:
		open_progress_dialog (window, window->priv->batch_mode);
		break;
	}

	fr_archive_progress_cb (NULL, -1.0, window);
	fr_archive_message_cb (NULL, _("Please wait…"), window);
}


/* -- selections -- */


#undef DEBUG_GET_DIR_LIST_FROM_PATH


static GList *
get_dir_list_from_path (FrWindow *window,
	      		char     *path)
{
	char  *dirname;
	int    dirname_l;
	GList *list = NULL;
	int    i;

	if (path[strlen (path) - 1] != '/')
		dirname = g_strconcat (path, "/", NULL);
	else
		dirname = g_strdup (path);
	dirname_l = strlen (dirname);
	for (i = 0; i < window->archive->files->len; i++) {
		FileData *fd = g_ptr_array_index (window->archive->files, i);
		gboolean  matches = FALSE;

#ifdef DEBUG_GET_DIR_LIST_FROM_PATH
		g_print ("%s <=> %s (%d)\n", dirname, fd->full_path, dirname_l);
#endif

		if (fd->dir) {
			int full_path_l = strlen (fd->full_path);
			if ((full_path_l == dirname_l - 1) && (strncmp (dirname, fd->full_path, full_path_l) == 0))
				/* example: dirname is '/path/to/dir/' and fd->full_path is '/path/to/dir' */
				matches = TRUE;
			else if (strcmp (dirname, fd->full_path) == 0)
				matches = TRUE;
		}

		if (! matches && strncmp (dirname, fd->full_path, dirname_l) == 0) {
			matches = TRUE;
		}

		if (matches) {
#ifdef DEBUG_GET_DIR_LIST_FROM_PATH
			g_print ("`-> OK\n");
#endif
			list = g_list_prepend (list, g_strdup (fd->original_path));
		}
	}
	g_free (dirname);

	return g_list_reverse (list);
}


static GList *
get_dir_list_from_file_data (FrWindow *window,
			     FileData *fdata)
{
	char  *dirname;
	GList *list;

	dirname = g_strconcat (fr_window_get_current_location (window),
			       fdata->list_name,
			       NULL);
	list = get_dir_list_from_path (window, dirname);
	g_free (dirname);

	return list;
}


GList *
fr_window_get_file_list_selection (FrWindow *window,
				   gboolean  recursive,
				   gboolean *has_dirs)
{
	GtkTreeSelection *selection;
	GList            *selections = NULL, *list, *scan;

	g_return_val_if_fail (window != NULL, NULL);

	if (has_dirs != NULL)
		*has_dirs = FALSE;

	selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (window->priv->list_view));
	if (selection == NULL)
		return NULL;
	gtk_tree_selection_selected_foreach (selection, add_selected_from_list_view, &selections);

	list = NULL;
	for (scan = selections; scan; scan = scan->next) {
		FileData *fd = scan->data;

		if (!fd)
			continue;

		if (file_data_is_dir (fd)) {
			if (has_dirs != NULL)
				*has_dirs = TRUE;

			if (recursive)
				list = g_list_concat (list, get_dir_list_from_file_data (window, fd));
		}
		else
			list = g_list_prepend (list, g_strdup (fd->original_path));
	}
	if (selections)
		g_list_free (selections);

	return g_list_reverse (list);
}


GList *
fr_window_get_folder_tree_selection (FrWindow *window,
				     gboolean  recursive,
				     gboolean *has_dirs)
{
	GtkTreeSelection *tree_selection;
	GList            *selections, *list, *scan;

	g_return_val_if_fail (window != NULL, NULL);

	if (has_dirs != NULL)
		*has_dirs = FALSE;

	tree_selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (window->priv->tree_view));
	if (tree_selection == NULL)
		return NULL;

	selections = NULL;
	gtk_tree_selection_selected_foreach (tree_selection, add_selected_from_tree_view, &selections);
	if (selections == NULL)
		return NULL;

	if (has_dirs != NULL)
		*has_dirs = TRUE;

	list = NULL;
	for (scan = selections; scan; scan = scan->next) {
		char *path = scan->data;

		if (recursive)
			list = g_list_concat (list, get_dir_list_from_path (window, path));
	}
	_g_string_list_free (selections);

	return g_list_reverse (list);
}


GList *
fr_window_get_file_list_from_path_list (FrWindow *window,
					GList    *path_list,
					gboolean *has_dirs)
{
	GtkTreeModel *model;
	GList        *selections, *list, *scan;

	g_return_val_if_fail (window != NULL, NULL);

	model = GTK_TREE_MODEL (window->priv->list_store);
	selections = NULL;

	if (has_dirs != NULL)
		*has_dirs = FALSE;

	for (scan = path_list; scan; scan = scan->next) {
		GtkTreeRowReference *reference = scan->data;
		GtkTreePath         *path;
		GtkTreeIter          iter;
		FileData            *fdata;

		path = gtk_tree_row_reference_get_path (reference);
		if (path == NULL)
			continue;

		if (! gtk_tree_model_get_iter (model, &iter, path))
			continue;

		gtk_tree_model_get (model, &iter,
				    COLUMN_FILE_DATA, &fdata,
				    -1);

		selections = g_list_prepend (selections, fdata);
	}

	list = NULL;
	for (scan = selections; scan; scan = scan->next) {
		FileData *fd = scan->data;

		if (!fd)
			continue;

		if (file_data_is_dir (fd)) {
			if (has_dirs != NULL)
				*has_dirs = TRUE;
			list = g_list_concat (list, get_dir_list_from_file_data (window, fd));
		}
		else
			list = g_list_prepend (list, g_strdup (fd->original_path));
	}

	if (selections != NULL)
		g_list_free (selections);

	return g_list_reverse (list);
}


GList *
fr_window_get_file_list_pattern (FrWindow    *window,
				 const char  *pattern)
{
	GRegex **regexps;
	GList   *list;
	int      i;

	g_return_val_if_fail (window != NULL, NULL);

	regexps = _g_regexp_split_from_patterns (pattern, G_REGEX_CASELESS);
	list = NULL;
	for (i = 0; i < window->archive->files->len; i++) {
		FileData *fd = g_ptr_array_index (window->archive->files, i);
		char     *utf8_name;

		if (fd == NULL)
			continue;

		utf8_name = g_filename_to_utf8 (fd->name, -1, NULL, NULL, NULL);
		if (_g_regexp_matchv (regexps, utf8_name, 0))
			list = g_list_prepend (list, g_strdup (fd->original_path));
		g_free (utf8_name);
	}
	_g_regexp_freev (regexps);

	return g_list_reverse (list);
}


static GList *
fr_window_get_file_list (FrWindow *window)
{
	GList *list;
	int    i;

	g_return_val_if_fail (window != NULL, NULL);

	list = NULL;
	for (i = 0; i < window->archive->files->len; i++) {
		FileData *fd = g_ptr_array_index (window->archive->files, i);
		list = g_list_prepend (list, g_strdup (fd->original_path));
	}

	return g_list_reverse (list);
}


int
fr_window_get_n_selected_files (FrWindow *window)
{
	return _gtk_tree_selection_count_selected (gtk_tree_view_get_selection (GTK_TREE_VIEW (window->priv->list_view)));
}


/**/


static int
dir_tree_button_press_cb (GtkWidget      *widget,
			  GdkEventButton *event,
			  gpointer        data)
{
	FrWindow         *window = data;
	GtkTreeSelection *selection;

	if (event->window != gtk_tree_view_get_bin_window (GTK_TREE_VIEW (window->priv->tree_view)))
		return FALSE;

	selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (window->priv->tree_view));
	if (selection == NULL)
		return FALSE;

	if ((event->type == GDK_BUTTON_PRESS) && (event->button == 3)) {
		GtkTreePath *path;
		GtkTreeIter  iter;

		if (gtk_tree_view_get_path_at_pos (GTK_TREE_VIEW (window->priv->tree_view),
						   event->x, event->y,
						   &path, NULL, NULL, NULL)) {

			if (! gtk_tree_model_get_iter (GTK_TREE_MODEL (window->priv->tree_store), &iter, path)) {
				gtk_tree_path_free (path);
				return FALSE;
			}
			gtk_tree_path_free (path);

			if (! gtk_tree_selection_iter_is_selected (selection, &iter)) {
				gtk_tree_selection_unselect_all (selection);
				gtk_tree_selection_select_iter (selection, &iter);
			}

			gtk_menu_popup (GTK_MENU (window->priv->sidebar_folder_popup_menu),
					NULL, NULL, NULL,
					window,
					event->button,
					event->time);
		}
		else
			gtk_tree_selection_unselect_all (selection);

		return TRUE;
	}
	else if ((event->type == GDK_BUTTON_PRESS) && (event->button == 8)) {
		fr_window_go_back (window);
		return TRUE;
	}
	else if ((event->type == GDK_BUTTON_PRESS) && (event->button == 9)) {
		fr_window_go_forward (window);
		return TRUE;
	}

	return FALSE;
}


static FileData *
fr_window_get_selected_item_from_file_list (FrWindow *window)
{
	GtkTreeSelection *tree_selection;
	GList            *selection;
	FileData         *fdata = NULL;

	g_return_val_if_fail (window != NULL, NULL);

	tree_selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (window->priv->list_view));
	if (tree_selection == NULL)
		return NULL;

	selection = NULL;
	gtk_tree_selection_selected_foreach (tree_selection, add_selected_from_list_view, &selection);
	if ((selection == NULL) || (selection->next != NULL)) {
		/* return NULL if the selection contains more than one entry. */
		g_list_free (selection);
		return NULL;
	}

	fdata = file_data_copy (selection->data);
	g_list_free (selection);

	return fdata;
}


static char *
fr_window_get_selected_folder_in_tree_view (FrWindow *window)
{
	GtkTreeSelection *tree_selection;
	GList            *selections;
	char             *path = NULL;

	g_return_val_if_fail (window != NULL, NULL);

	tree_selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (window->priv->tree_view));
	if (tree_selection == NULL)
		return NULL;

	selections = NULL;
	gtk_tree_selection_selected_foreach (tree_selection, add_selected_from_tree_view, &selections);

	if (selections != NULL) {
		path = selections->data;
		g_list_free (selections);
	}

	return path;
}


void
fr_window_current_folder_activated (FrWindow *window,
				    gboolean   from_sidebar)
{
	char *dir_path;

	if (! from_sidebar) {
		FileData *fdata;
		char     *dir_name;

		fdata = fr_window_get_selected_item_from_file_list (window);
		if ((fdata == NULL) || ! file_data_is_dir (fdata)) {
			file_data_free (fdata);
			return;
		}
		dir_name = g_strdup (fdata->list_name);
		dir_path = g_strconcat (fr_window_get_current_location (window),
					dir_name,
					"/",
					NULL);
		g_free (dir_name);
		file_data_free (fdata);
	}
	else
		dir_path = fr_window_get_selected_folder_in_tree_view (window);

	fr_window_go_to_location (window, dir_path, FALSE);

	g_free (dir_path);
}


static gboolean
row_activated_cb (GtkTreeView       *tree_view,
		  GtkTreePath       *path,
		  GtkTreeViewColumn *column,
		  gpointer           data)
{
	FrWindow    *window = data;
	FileData    *fdata;
	GtkTreeIter  iter;

	if (! gtk_tree_model_get_iter (GTK_TREE_MODEL (window->priv->list_store),
				       &iter,
				       path))
		return FALSE;

	gtk_tree_model_get (GTK_TREE_MODEL (window->priv->list_store), &iter,
			    COLUMN_FILE_DATA, &fdata,
			    -1);

	if (! file_data_is_dir (fdata)) {
		GList *list = g_list_prepend (NULL, fdata->original_path);
		fr_window_open_files (window, list, FALSE);
		g_list_free (list);
	}
	else if (window->priv->list_mode == FR_WINDOW_LIST_MODE_AS_DIR) {
		char *new_dir;
		new_dir = g_strconcat (fr_window_get_current_location (window),
				       fdata->list_name,
				       "/",
				       NULL);
		fr_window_go_to_location (window, new_dir, FALSE);
		g_free (new_dir);
	}

	return FALSE;
}


static int
file_button_press_cb (GtkWidget      *widget,
		      GdkEventButton *event,
		      gpointer        data)
{
	FrWindow         *window = data;
	GtkTreeSelection *selection;

	if (event->window != gtk_tree_view_get_bin_window (GTK_TREE_VIEW (window->priv->list_view)))
		return FALSE;

	selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (window->priv->list_view));
	if (selection == NULL)
		return FALSE;

	if (window->priv->path_clicked != NULL) {
		gtk_tree_path_free (window->priv->path_clicked);
		window->priv->path_clicked = NULL;
	}

	if ((event->type == GDK_BUTTON_PRESS) && (event->button == 3)) {
		GtkTreePath *path;
		GtkTreeIter  iter;
		int          n_selected;

		if (gtk_tree_view_get_path_at_pos (GTK_TREE_VIEW (window->priv->list_view),
						   event->x, event->y,
						   &path, NULL, NULL, NULL)) {

			if (! gtk_tree_model_get_iter (GTK_TREE_MODEL (window->priv->list_store), &iter, path)) {
				gtk_tree_path_free (path);
				return FALSE;
			}
			gtk_tree_path_free (path);

			if (! gtk_tree_selection_iter_is_selected (selection, &iter)) {
				gtk_tree_selection_unselect_all (selection);
				gtk_tree_selection_select_iter (selection, &iter);
			}
		}
		else
			gtk_tree_selection_unselect_all (selection);

		n_selected = fr_window_get_n_selected_files (window);
		if ((n_selected == 1) && selection_has_a_dir (window))
			gtk_menu_popup (GTK_MENU (window->priv->folder_popup_menu),
					NULL, NULL, NULL,
					window,
					event->button,
					event->time);
		else
			gtk_menu_popup (GTK_MENU (window->priv->file_popup_menu),
					NULL, NULL, NULL,
					window,
					event->button,
					event->time);
		return TRUE;
	}
	else if ((event->type == GDK_BUTTON_PRESS) && (event->button == 1)) {
		GtkTreePath *path = NULL;

		if (! gtk_tree_view_get_path_at_pos (GTK_TREE_VIEW (window->priv->list_view),
						     event->x, event->y,
						     &path, NULL, NULL, NULL)) {
			gtk_tree_selection_unselect_all (selection);
		}

		if (window->priv->path_clicked != NULL) {
			gtk_tree_path_free (window->priv->path_clicked);
			window->priv->path_clicked = NULL;
		}

		if (path != NULL) {
			window->priv->path_clicked = gtk_tree_path_copy (path);
			gtk_tree_path_free (path);
		}

		return FALSE;
	}
	else if ((event->type == GDK_BUTTON_PRESS) && (event->button == 8)) {
		// go back
		fr_window_go_back (window);
		return TRUE;
	}
	else if ((event->type == GDK_BUTTON_PRESS) && (event->button == 9)) {
		// go forward
		fr_window_go_forward (window);
		return TRUE;
	}

	return FALSE;
}


static int
file_button_release_cb (GtkWidget      *widget,
			GdkEventButton *event,
			gpointer        data)
{
	FrWindow         *window = data;
	GtkTreeSelection *selection;

	if (event->window != gtk_tree_view_get_bin_window (GTK_TREE_VIEW (window->priv->list_view)))
		return FALSE;

	selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (window->priv->list_view));
	if (selection == NULL)
		return FALSE;

	if (window->priv->path_clicked == NULL)
		return FALSE;

	if ((event->type == GDK_BUTTON_RELEASE)
	    && (event->button == 1)
	    && (window->priv->path_clicked != NULL)) {
		GtkTreePath *path = NULL;

		if (gtk_tree_view_get_path_at_pos (GTK_TREE_VIEW (window->priv->list_view),
						   event->x, event->y,
						   &path, NULL, NULL, NULL)) {

			if ((gtk_tree_path_compare (window->priv->path_clicked, path) == 0)
			    && window->priv->single_click
			    && ! ((event->state & GDK_CONTROL_MASK) || (event->state & GDK_SHIFT_MASK))) {
				gtk_tree_view_set_cursor (GTK_TREE_VIEW (widget),
							  path,
							  NULL,
							  FALSE);
				gtk_tree_view_row_activated (GTK_TREE_VIEW (widget),
							     path,
							     NULL);
			}
		}

		if (path != NULL)
			gtk_tree_path_free (path);
	}

	if (window->priv->path_clicked != NULL) {
		gtk_tree_path_free (window->priv->path_clicked);
		window->priv->path_clicked = NULL;
	}

	return FALSE;
}


static gboolean
file_motion_notify_callback (GtkWidget *widget,
			     GdkEventMotion *event,
			     gpointer user_data)
{
	FrWindow    *window = user_data;
	GdkCursor   *cursor;
	GtkTreePath *last_hover_path;
	GtkTreeIter  iter;

	if (! window->priv->single_click)
		return FALSE;

	if (event->window != gtk_tree_view_get_bin_window (GTK_TREE_VIEW (window->priv->list_view)))
		return FALSE;

	last_hover_path = window->priv->list_hover_path;

	gtk_tree_view_get_path_at_pos (GTK_TREE_VIEW (widget),
				       event->x, event->y,
				       &window->priv->list_hover_path,
				       NULL, NULL, NULL);

	if (window->priv->list_hover_path != NULL)
		cursor = gdk_cursor_new (GDK_HAND2);
	else
		cursor = NULL;

	gdk_window_set_cursor (event->window, cursor);

	/* only redraw if the hover row has changed */
	if (!(last_hover_path == NULL && window->priv->list_hover_path == NULL) &&
	    (!(last_hover_path != NULL && window->priv->list_hover_path != NULL) ||
	     gtk_tree_path_compare (last_hover_path, window->priv->list_hover_path)))
	{
		if (last_hover_path) {
			gtk_tree_model_get_iter (GTK_TREE_MODEL (window->priv->list_store),
						 &iter, last_hover_path);
			gtk_tree_model_row_changed (GTK_TREE_MODEL (window->priv->list_store),
						    last_hover_path, &iter);
		}

		if (window->priv->list_hover_path) {
			gtk_tree_model_get_iter (GTK_TREE_MODEL (window->priv->list_store),
						 &iter, window->priv->list_hover_path);
			gtk_tree_model_row_changed (GTK_TREE_MODEL (window->priv->list_store),
						    window->priv->list_hover_path, &iter);
		}
	}

	gtk_tree_path_free (last_hover_path);

 	return FALSE;
}


static gboolean
file_leave_notify_callback (GtkWidget *widget,
			    GdkEventCrossing *event,
			    gpointer user_data)
{
	FrWindow    *window = user_data;
	GtkTreeIter  iter;

	if (window->priv->single_click && (window->priv->list_hover_path != NULL)) {
		gtk_tree_model_get_iter (GTK_TREE_MODEL (window->priv->list_store),
					 &iter,
					 window->priv->list_hover_path);
		gtk_tree_model_row_changed (GTK_TREE_MODEL (window->priv->list_store),
					    window->priv->list_hover_path,
					    &iter);

		gtk_tree_path_free (window->priv->list_hover_path);
		window->priv->list_hover_path = NULL;
	}

	return FALSE;
}


/* -- drag and drop -- */


static GList *
get_file_list_from_selection_data (char *uri_list)
{
	GList  *list = NULL;
	char  **uris;
	int     i;

	if (uri_list == NULL)
		return NULL;

	uris = g_uri_list_extract_uris (uri_list);
	for (i = 0; uris[i] != NULL; i++)
		list = g_list_prepend (list, g_file_new_for_uri (uris[i]));
	g_strfreev (uris);

	return g_list_reverse (list);
}


static gboolean
fr_window_drag_motion (GtkWidget      *widget,
		       GdkDragContext *context,
		       gint            x,
		       gint            y,
		       guint           time,
		       gpointer        user_data)
{
	FrWindow  *window = user_data;

	if ((gtk_drag_get_source_widget (context) == window->priv->list_view)
	    || (gtk_drag_get_source_widget (context) == window->priv->tree_view))
	{
		gdk_drag_status (context, 0, time);
		return FALSE;
	}

	return TRUE;
}


static void fr_window_paste_from_clipboard_data (FrWindow *window, FrClipboardData *data);


static FrClipboardData*
get_clipboard_data_from_selection_data (FrWindow   *window,
					const char *data)
{
	FrClipboardData  *clipboard_data;
	char            **uris;
	int               i;

	clipboard_data = fr_clipboard_data_new ();

	uris = g_strsplit (data, "\r\n", -1);

	clipboard_data->file = g_file_new_for_uri (uris[0]);
	if (window->priv->second_password != NULL)
		clipboard_data->password = g_strdup (window->priv->second_password);
	else if (strcmp (uris[1], "") != 0)
		clipboard_data->password = g_strdup (uris[1]);
	clipboard_data->op = (strcmp (uris[2], "copy") == 0) ? FR_CLIPBOARD_OP_COPY : FR_CLIPBOARD_OP_CUT;
	clipboard_data->base_dir = g_strdup (uris[3]);
	for (i = 4; uris[i] != NULL; i++)
		if (uris[i][0] != '\0')
			clipboard_data->files = g_list_prepend (clipboard_data->files, g_strdup (uris[i]));
	clipboard_data->files = g_list_reverse (clipboard_data->files);

	g_strfreev (uris);

	return clipboard_data;
}


gboolean
fr_window_create_archive_and_continue (FrWindow   *window,
			  	       GFile      *file,
			  	       const char *mime_type,
			  	       GtkWindow  *error_dialog_parent)
{
	gboolean result = FALSE;

	if (fr_window_archive_new (FR_WINDOW (window), file, mime_type)) {
		if (! fr_window_is_batch_mode (FR_WINDOW (window)))
			gtk_window_present (GTK_WINDOW (window));
		_archive_operation_completed (window, FR_ACTION_CREATING_NEW_ARCHIVE, NULL);

		result = TRUE;
	}
	else {
		GError *error;

		error = g_error_new_literal (FR_ERROR, FR_ERROR_GENERIC, _("Archive type not supported."));
		window->priv->load_error_parent_window = error_dialog_parent;
		_archive_operation_completed (window, FR_ACTION_CREATING_NEW_ARCHIVE, error);

		g_error_free (error);

		result = FALSE;
	}

	return result;
}


static void
new_archive_dialog_response_cb (GtkDialog *dialog,
				int        response,
				gpointer   user_data)
{
	FrWindow   *window = user_data;
	GFile      *file;
	const char *mime_type;
	GtkWidget  *archive_window;
	gboolean    new_window;
	const char *password;
	gboolean    encrypt_header;
	int         volume_size;

	if ((response == GTK_RESPONSE_CANCEL) || (response == GTK_RESPONSE_DELETE_EVENT)) {
		gtk_widget_destroy (GTK_WIDGET (dialog));
		_archive_operation_cancelled (window, FR_ACTION_CREATING_NEW_ARCHIVE);
		return;
	}

	file = fr_new_archive_dialog_get_file (FR_NEW_ARCHIVE_DIALOG (dialog), &mime_type);
	if (file == NULL)
		return;

	new_window = fr_window_archive_is_present (window) && ! fr_window_is_batch_mode (window);
	if (new_window)
		archive_window = fr_window_new ();
	else
		archive_window = (GtkWidget *) window;

	password = fr_new_archive_dialog_get_password (FR_NEW_ARCHIVE_DIALOG (dialog));
	encrypt_header = fr_new_archive_dialog_get_encrypt_header (FR_NEW_ARCHIVE_DIALOG (dialog));
	volume_size = fr_new_archive_dialog_get_volume_size (FR_NEW_ARCHIVE_DIALOG (dialog));

	fr_window_set_password (FR_WINDOW (archive_window), password);
	fr_window_set_encrypt_header (FR_WINDOW (archive_window), encrypt_header);
	fr_window_set_volume_size (FR_WINDOW (archive_window), volume_size);

	if (fr_window_create_archive_and_continue (FR_WINDOW (archive_window),
						   file,
						   mime_type,
						   GTK_WINDOW (dialog)))
	{
		gtk_widget_destroy (GTK_WIDGET (dialog));
	}
	else if (new_window)
		gtk_widget_destroy (archive_window);

	g_object_unref (file);
}


static void
fr_window_drag_data_received  (GtkWidget          *widget,
			       GdkDragContext     *context,
			       gint                x,
			       gint                y,
			       GtkSelectionData   *data,
			       guint               info,
			       guint               time,
			       gpointer            extra_data)
{
	FrWindow  *window = extra_data;
	GList     *list;
	gboolean   one_file;
	gboolean   is_an_archive;

	debug (DEBUG_INFO, "::DragDataReceived -->\n");

	if ((gtk_drag_get_source_widget (context) == window->priv->list_view)
	    || (gtk_drag_get_source_widget (context) == window->priv->tree_view))
	{
		gtk_drag_finish (context, FALSE, FALSE, time);
		return;
	}

	if (! ((gtk_selection_data_get_length (data) >= 0) && (gtk_selection_data_get_format (data) == 8))) {
		gtk_drag_finish (context, FALSE, FALSE, time);
		return;
	}

	if (window->priv->activity_ref > 0) {
		gtk_drag_finish (context, FALSE, FALSE, time);
		return;
	}

	gtk_drag_finish (context, TRUE, FALSE, time);

	if (gtk_selection_data_get_target (data) == XFR_ATOM) {
		FrClipboardData *dnd_data;

		dnd_data = get_clipboard_data_from_selection_data (window, (char*) gtk_selection_data_get_data (data));
		dnd_data->current_dir = g_strdup (fr_window_get_current_location (window));
		fr_window_paste_from_clipboard_data (window, dnd_data);

		return;
	}

	list = get_file_list_from_selection_data ((char*) gtk_selection_data_get_data (data));
	if (list == NULL) {
		GtkWidget *d;

		d = _gtk_error_dialog_new (GTK_WINDOW (window),
					   GTK_DIALOG_MODAL,
					   NULL,
					   _("Could not perform the operation"),
					   NULL);
		gtk_dialog_run (GTK_DIALOG (d));
		gtk_widget_destroy(d);

 		return;
	}

	one_file = (list->next == NULL);
	if (one_file)
		is_an_archive = _g_file_is_archive (G_FILE (list->data));
	else
		is_an_archive = FALSE;

	if (window->priv->archive_present
	    && (window->archive != NULL)
	    && ! window->archive->read_only
	    && fr_archive_is_capable_of (window->archive, FR_ARCHIVE_CAN_STORE_MANY_FILES))
	{
		if (one_file && is_an_archive) {
			GtkWidget *d;
			gint       r;

			d = _gtk_message_dialog_new (GTK_WINDOW (window),
						     GTK_DIALOG_MODAL,
						     GTK_STOCK_DIALOG_QUESTION,
						     _("Do you want to add this file to the current archive or open it as a new archive?"),
						     NULL,
						     GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
						     GTK_STOCK_ADD, 0,
						     GTK_STOCK_OPEN, 1,
						     NULL);

			gtk_dialog_set_default_response (GTK_DIALOG (d), 2);

			r = gtk_dialog_run (GTK_DIALOG (d));
			gtk_widget_destroy (GTK_WIDGET (d));

			if (r == 0)  /* Add */
				fr_window_archive_add_dropped_items (window, list);
			else if (r == 1)  /* Open */
				fr_window_archive_open (window, G_FILE (list->data), GTK_WINDOW (window));
 		}
 		else
			fr_window_archive_add_dropped_items (window, list);
	}
	else {
		if (one_file && is_an_archive)
			fr_window_archive_open (window, G_FILE (list->data), GTK_WINDOW (window));
		else {
			GtkWidget *d;
			int        r;

			d = _gtk_message_dialog_new (GTK_WINDOW (window),
						     GTK_DIALOG_MODAL,
						     GTK_STOCK_DIALOG_QUESTION,
						     _("Do you want to create a new archive with these files?"),
						     NULL,
						     GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
						     _("Create _Archive"), GTK_RESPONSE_YES,
						     NULL);

			gtk_dialog_set_default_response (GTK_DIALOG (d), GTK_RESPONSE_YES);
			r = gtk_dialog_run (GTK_DIALOG (d));
			gtk_widget_destroy (GTK_WIDGET (d));

			if (r == GTK_RESPONSE_YES) {
				GFile     *first_file;
				GFile     *folder;
				char      *archive_name;
				GtkWidget *dialog;

				fr_window_free_batch_data (window);
				fr_window_append_batch_action (window,
							       FR_BATCH_ACTION_ADD,
							       _g_object_list_ref (list),
							       (GFreeFunc) _g_object_list_unref);

				first_file = G_FILE (list->data);
				folder = g_file_get_parent (first_file);
				if (folder != NULL)
					fr_window_set_open_default_dir (window, folder);

				if ((list->next != NULL) && (folder != NULL))
					archive_name = g_file_get_basename (folder);
				else
					archive_name = g_file_get_basename (first_file);

				dialog = fr_new_archive_dialog_new (_("New Archive"),
								    GTK_WINDOW (window),
								    FR_NEW_ARCHIVE_ACTION_SAVE_AS,
								    fr_window_get_open_default_dir (window),
								    archive_name,
								    NULL);
				gtk_window_set_modal (GTK_WINDOW (dialog), TRUE);
				g_signal_connect (G_OBJECT (dialog),
						  "response",
						  G_CALLBACK (new_archive_dialog_response_cb),
						  window);
				gtk_window_present (GTK_WINDOW (dialog));

				g_free (archive_name);
				_g_object_unref (folder);
			}
		}
	}

	_g_object_list_unref (list);

	debug (DEBUG_INFO, "::DragDataReceived <--\n");
}


static gboolean
file_list_drag_begin (GtkWidget          *widget,
		      GdkDragContext     *context,
		      gpointer            data)
{
	FrWindow *window = data;

	debug (DEBUG_INFO, "::DragBegin -->\n");

	if (window->priv->activity_ref > 0)
		return FALSE;

	_g_clear_object (&window->priv->drag_destination_folder);

	g_free (window->priv->drag_base_dir);
	window->priv->drag_base_dir = NULL;

	gdk_property_change (gdk_drag_context_get_source_window (context),
			     XDS_ATOM, TEXT_ATOM,
			     8, GDK_PROP_MODE_REPLACE,
			     (guchar *) XDS_FILENAME,
			     strlen (XDS_FILENAME));

	return TRUE;
}


static void
file_list_drag_end (GtkWidget      *widget,
		    GdkDragContext *context,
		    gpointer        data)
{
	FrWindow *window = data;

	debug (DEBUG_INFO, "::DragEnd -->\n");

	gdk_property_delete (gdk_drag_context_get_source_window (context), XDS_ATOM);

	if (window->priv->drag_error != NULL) {
		_gtk_error_dialog_run (GTK_WINDOW (window),
				       _("Extraction not performed"),
				       "%s",
				       window->priv->drag_error->message);
		g_clear_error (&window->priv->drag_error);
	}
	else if (window->priv->drag_destination_folder != NULL) {
		fr_window_archive_extract (window,
					   window->priv->drag_file_list,
					   window->priv->drag_destination_folder,
					   window->priv->drag_base_dir,
					   FALSE,
					   FR_OVERWRITE_ASK,
					   FALSE,
					   FALSE);
		_g_string_list_free (window->priv->drag_file_list);
		window->priv->drag_file_list = NULL;
	}

	debug (DEBUG_INFO, "::DragEnd <--\n");
}


/* The following three functions taken from bugzilla
 * (http://bugzilla.gnome.org/attachment.cgi?id=49362&action=view)
 * Author: Christian Neumair
 * Copyright: 2005 Free Software Foundation, Inc
 * License: GPL */
static char *
get_xds_atom_value (GdkDragContext *context)
{
	char *ret;

	g_return_val_if_fail (context != NULL, NULL);
	g_return_val_if_fail (gdk_drag_context_get_source_window (context) != NULL, NULL);

	if (gdk_property_get (gdk_drag_context_get_source_window (context),
			      XDS_ATOM, TEXT_ATOM,
			      0, MAX_XDS_ATOM_VAL_LEN,
			      FALSE, NULL, NULL, NULL,
			      (unsigned char **) &ret))
		return ret;

	return NULL;
}


static gboolean
context_offers_target (GdkDragContext *context,
		       GdkAtom target)
{
	return (g_list_find (gdk_drag_context_list_targets (context), target) != NULL);
}


static gboolean
nautilus_xds_dnd_is_valid_xds_context (GdkDragContext *context)
{
	char *tmp;
	gboolean ret;

	g_return_val_if_fail (context != NULL, FALSE);

	tmp = NULL;
	if (context_offers_target (context, XDS_ATOM)) {
		tmp = get_xds_atom_value (context);
	}

	ret = (tmp != NULL);
	g_free (tmp);

	return ret;
}


static char *
get_selection_data_from_clipboard_data (FrWindow        *window,
		      			FrClipboardData *data)
{
	GString *list;
	char    *uri;
	GList   *scan;

	if (data == NULL)
		return NULL;

	list = g_string_new (NULL);

	uri = g_file_get_uri (fr_archive_get_file (window->archive));
	g_string_append (list, uri);
	g_free (uri);

	g_string_append (list, "\r\n");
	if (window->priv->password != NULL)
		g_string_append (list, window->priv->password);
	g_string_append (list, "\r\n");
	g_string_append (list, (data->op == FR_CLIPBOARD_OP_COPY) ? "copy" : "cut");
	g_string_append (list, "\r\n");
	g_string_append (list, data->base_dir);
	g_string_append (list, "\r\n");
	for (scan = data->files; scan; scan = scan->next) {
		g_string_append (list, scan->data);
		g_string_append (list, "\r\n");
	}

	return g_string_free (list, FALSE);
}


static gboolean
fr_window_folder_tree_drag_data_get (GtkWidget        *widget,
				     GdkDragContext   *context,
				     GtkSelectionData *selection_data,
				     guint             info,
				     guint             time,
				     gpointer          user_data)
{
	FrWindow *window = user_data;
	GList    *file_list;
	char     *uri;
	GFile    *destination;
	GFile    *destination_folder;

	debug (DEBUG_INFO, "::DragDataGet -->\n");

	if (window->priv->activity_ref > 0)
		return FALSE;

	file_list = fr_window_get_folder_tree_selection (window, TRUE, NULL);
	if (file_list == NULL)
		return FALSE;

	if (gtk_selection_data_get_target (selection_data) == XFR_ATOM) {
		FrClipboardData *tmp;
		char            *data;

		tmp = fr_clipboard_data_new ();
		tmp->files = file_list;
		tmp->op = FR_CLIPBOARD_OP_COPY;
		tmp->base_dir = g_strdup (fr_window_get_current_location (window));

		data = get_selection_data_from_clipboard_data (window, tmp);
		gtk_selection_data_set (selection_data, XFR_ATOM, 8, (guchar *) data, strlen (data));

		fr_clipboard_data_unref (tmp);
		g_free (data);

		return TRUE;
	}

	if (! nautilus_xds_dnd_is_valid_xds_context (context))
		return FALSE;

	uri  = get_xds_atom_value (context);
	g_return_val_if_fail (uri != NULL, FALSE);

	destination = g_file_new_for_uri (uri);
	destination_folder = g_file_get_parent (destination);

	g_object_unref (destination);

	/* check whether the extraction can be performed in the destination
	 * folder */

	g_clear_error (&window->priv->drag_error);

	if (! _g_file_check_permissions (destination_folder, R_OK | W_OK)) {
		char *display_name;

		display_name = _g_file_get_display_basename (destination_folder);
		window->priv->drag_error = g_error_new (FR_ERROR, 0, _("You don't have the right permissions to extract archives in the folder \"%s\""), display_name);

		g_free (display_name);
	}

	if (window->priv->drag_error == NULL) {
		_g_object_unref (window->priv->drag_destination_folder);
		g_free (window->priv->drag_base_dir);
		_g_string_list_free (window->priv->drag_file_list);
		window->priv->drag_destination_folder = g_object_ref (destination_folder);
		window->priv->drag_base_dir = fr_window_get_selected_folder_in_tree_view (window);
		window->priv->drag_file_list = file_list;
	}

	g_object_unref (destination_folder);

	/* sends back the response */

	gtk_selection_data_set (selection_data, gtk_selection_data_get_target (selection_data), 8, (guchar *) ((window->priv->drag_error == NULL) ? "S" : "E"), 1);

	debug (DEBUG_INFO, "::DragDataGet <--\n");

	return TRUE;
}


gboolean
fr_window_file_list_drag_data_get (FrWindow         *window,
				   GdkDragContext   *context,
				   GtkSelectionData *selection_data,
				   GList            *path_list)
{
	char  *uri;
	GFile *destination;
	GFile *destination_folder;

	debug (DEBUG_INFO, "::DragDataGet -->\n");

	if (window->priv->path_clicked != NULL) {
		gtk_tree_path_free (window->priv->path_clicked);
		window->priv->path_clicked = NULL;
	}

	if (window->priv->activity_ref > 0)
		return FALSE;

	if (gtk_selection_data_get_target (selection_data) == XFR_ATOM) {
		FrClipboardData *tmp;
		char            *data;

		tmp = fr_clipboard_data_new ();
		tmp->files = fr_window_get_file_list_selection (window, TRUE, NULL);
		tmp->op = FR_CLIPBOARD_OP_COPY;
		tmp->base_dir = g_strdup (fr_window_get_current_location (window));

		data = get_selection_data_from_clipboard_data (window, tmp);
		gtk_selection_data_set (selection_data, XFR_ATOM, 8, (guchar *) data, strlen (data));

		fr_clipboard_data_unref (tmp);
		g_free (data);

		return TRUE;
	}

	if (! nautilus_xds_dnd_is_valid_xds_context (context))
		return FALSE;

	uri = get_xds_atom_value (context);
	g_return_val_if_fail (uri != NULL, FALSE);

	destination = g_file_new_for_uri (uri);
	destination_folder = g_file_get_parent (destination);

	g_object_unref (destination);

	/* check whether the extraction can be performed in the destination
	 * folder */

	g_clear_error (&window->priv->drag_error);

	if (! _g_file_check_permissions (destination_folder, R_OK | W_OK)) {
		char *display_name;

		display_name = _g_file_get_display_basename (destination_folder);
		window->priv->drag_error = g_error_new (FR_ERROR, 0, _("You don't have the right permissions to extract archives in the folder \"%s\""), display_name);

		g_free (display_name);
	}

	if (window->priv->drag_error == NULL) {
		_g_object_unref (window->priv->drag_destination_folder);
		g_free (window->priv->drag_base_dir);
		_g_string_list_free (window->priv->drag_file_list);
		window->priv->drag_destination_folder = g_object_ref (destination_folder);
		window->priv->drag_base_dir = g_strdup (fr_window_get_current_location (window));
		window->priv->drag_file_list = fr_window_get_file_list_from_path_list (window, path_list, NULL);
	}

	g_object_unref (destination_folder);

	/* sends back the response */

	gtk_selection_data_set (selection_data, gtk_selection_data_get_target (selection_data), 8, (guchar *) ((window->priv->drag_error == NULL) ? "S" : "E"), 1);

	debug (DEBUG_INFO, "::DragDataGet <--\n");

	return TRUE;
}


/* -- window_new -- */


static void
fr_window_update_columns_visibility (FrWindow *window)
{
	GtkTreeView       *tree_view = GTK_TREE_VIEW (window->priv->list_view);
	GtkTreeViewColumn *column;

	column = gtk_tree_view_get_column (tree_view, 1);
	gtk_tree_view_column_set_visible (column, g_settings_get_boolean (window->priv->settings_listing, PREF_LISTING_SHOW_SIZE));

	column = gtk_tree_view_get_column (tree_view, 2);
	gtk_tree_view_column_set_visible (column, g_settings_get_boolean (window->priv->settings_listing, PREF_LISTING_SHOW_TYPE));

	column = gtk_tree_view_get_column (tree_view, 3);
	gtk_tree_view_column_set_visible (column, g_settings_get_boolean (window->priv->settings_listing, PREF_LISTING_SHOW_TIME));

	column = gtk_tree_view_get_column (tree_view, 4);
	gtk_tree_view_column_set_visible (column, g_settings_get_boolean (window->priv->settings_listing, PREF_LISTING_SHOW_PATH));
}


static gboolean
key_press_cb (GtkWidget   *widget,
	      GdkEventKey *event,
	      gpointer     data)
{
	FrWindow *window = data;
	gboolean  retval = FALSE;
	gboolean  alt;

	if (gtk_widget_has_focus (window->priv->location_entry))
		return FALSE;

	if (gtk_widget_has_focus (window->priv->filter_entry)) {
		switch (event->keyval) {
		case GDK_KEY_Escape:
			fr_window_deactivate_filter (window);
			retval = TRUE;
			break;
		default:
			break;
		}
		return retval;
	}

	alt = (event->state & GDK_MOD1_MASK) == GDK_MOD1_MASK;

	switch (event->keyval) {
	case GDK_KEY_Escape:
		activate_action_stop (NULL, window);
		if (window->priv->filter_mode)
			fr_window_deactivate_filter (window);
		retval = TRUE;
		break;

	case GDK_KEY_F10:
		if (event->state & GDK_SHIFT_MASK) {
			GtkTreeSelection *selection;

			selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (window->priv->list_view));
			if (selection == NULL)
				return FALSE;

			gtk_menu_popup (GTK_MENU (window->priv->file_popup_menu),
					NULL, NULL, NULL,
					window,
					3,
					GDK_CURRENT_TIME);
			retval = TRUE;
		}
		break;

	case GDK_KEY_Up:
	case GDK_KEY_KP_Up:
		if (alt) {
			fr_window_go_up_one_level (window);
			retval = TRUE;
		}
		break;

	case GDK_KEY_BackSpace:
		fr_window_go_up_one_level (window);
		retval = TRUE;
		break;

	case GDK_KEY_Right:
	case GDK_KEY_KP_Right:
		if (alt) {
			fr_window_go_forward (window);
			retval = TRUE;
		}
		break;

	case GDK_KEY_Left:
	case GDK_KEY_KP_Left:
		if (alt) {
			fr_window_go_back (window);
			retval = TRUE;
		}
		break;

	case GDK_KEY_Home:
	case GDK_KEY_KP_Home:
		if (alt) {
			fr_window_go_to_location (window, "/", FALSE);
			retval = TRUE;
		}
		break;

	default:
		break;
	}

	return retval;
}


static gboolean
dir_tree_selection_changed_cb (GtkTreeSelection *selection,
			       gpointer          user_data)
{
	FrWindow    *window = user_data;
	GtkTreeIter  iter;

	if (gtk_tree_selection_get_selected (selection, NULL, &iter)) {
		char *path;

		gtk_tree_model_get (GTK_TREE_MODEL (window->priv->tree_store),
				    &iter,
				    TREE_COLUMN_PATH, &path,
				    -1);
		fr_window_go_to_location (window, path, FALSE);
		g_free (path);
	}

	return FALSE;
}


static gboolean
selection_changed_cb (GtkTreeSelection *selection,
		      gpointer          user_data)
{
	FrWindow *window = user_data;

	fr_window_update_statusbar_list_info (window);
	fr_window_update_sensitivity (window);

	return FALSE;
}


static gboolean
fr_window_delete_event_cb (GtkWidget *caller,
			   GdkEvent  *event,
			   FrWindow  *window)
{
	fr_window_close (window);
	return TRUE;
}


static gboolean
is_single_click_policy (FrWindow *window)
{
	char     *value;
	gboolean  result;

	if (window->priv->settings_nautilus == NULL)
		return FALSE;

	value = g_settings_get_string (window->priv->settings_nautilus, NAUTILUS_CLICK_POLICY);
	result = (value != NULL) && (strncmp (value, "single", 6) == 0);
	g_free (value);

	return result;
}


static void
filename_cell_data_func (GtkTreeViewColumn *column,
			 GtkCellRenderer   *renderer,
			 GtkTreeModel      *model,
			 GtkTreeIter       *iter,
			 FrWindow          *window)
{
	char           *text;
	GtkTreePath    *path;
	PangoUnderline  underline;

	gtk_tree_model_get (model, iter,
			    COLUMN_NAME, &text,
			    -1);

	if (window->priv->single_click) {
		path = gtk_tree_model_get_path (model, iter);

		if ((window->priv->list_hover_path == NULL)
		    || gtk_tree_path_compare (path, window->priv->list_hover_path))
			underline = PANGO_UNDERLINE_NONE;
		else
			underline = PANGO_UNDERLINE_SINGLE;

		gtk_tree_path_free (path);
	}
	else
		underline = PANGO_UNDERLINE_NONE;

	g_object_set (G_OBJECT (renderer),
		      "text", text,
		      "underline", underline,
		      NULL);

	g_free (text);
}


static void
add_dir_tree_columns (FrWindow    *window,
		      GtkTreeView *treeview)
{
	GtkCellRenderer   *renderer;
	GtkTreeViewColumn *column;
	GValue             value = { 0, };

	/* First column. */

	column = gtk_tree_view_column_new ();
	gtk_tree_view_column_set_title (column, _("Folders"));

	/* icon */

	renderer = gtk_cell_renderer_pixbuf_new ();
	gtk_tree_view_column_pack_start (column, renderer, FALSE);
	gtk_tree_view_column_set_attributes (column, renderer,
					     "pixbuf", TREE_COLUMN_ICON,
					     NULL);

	/* name */

	renderer = gtk_cell_renderer_text_new ();

	g_value_init (&value, PANGO_TYPE_ELLIPSIZE_MODE);
	g_value_set_enum (&value, PANGO_ELLIPSIZE_END);
	g_object_set_property (G_OBJECT (renderer), "ellipsize", &value);
	g_value_unset (&value);

	gtk_tree_view_column_pack_start (column,
					 renderer,
					 TRUE);
	gtk_tree_view_column_set_attributes (column, renderer,
					     "text", TREE_COLUMN_NAME,
					     "weight", TREE_COLUMN_WEIGHT,
					     NULL);

	gtk_tree_view_column_set_sizing (column, GTK_TREE_VIEW_COLUMN_AUTOSIZE);
	gtk_tree_view_column_set_sort_column_id (column, TREE_COLUMN_NAME);

	gtk_tree_view_append_column (GTK_TREE_VIEW (treeview), column);
}


static void
add_file_list_columns (FrWindow    *window,
		       GtkTreeView *treeview)
{
	static char       *titles[] = {NC_("File", "Size"),
				       NC_("File", "Type"),
				       NC_("File", "Modified"),
				       NC_("File", "Location")};
	GtkCellRenderer   *renderer;
	GtkTreeViewColumn *column;
	GValue             value = { 0, };
	int                i, j, w;

	/* First column. */

	window->priv->filename_column = column = gtk_tree_view_column_new ();
	gtk_tree_view_column_set_title (column, C_("File", "Name"));

	/* emblem */

	renderer = gtk_cell_renderer_pixbuf_new ();
	gtk_tree_view_column_pack_end (column, renderer, FALSE);
	gtk_tree_view_column_set_attributes (column, renderer,
					     "pixbuf", COLUMN_EMBLEM,
					     NULL);

	/* icon */

	renderer = gtk_cell_renderer_pixbuf_new ();
	gtk_tree_view_column_pack_start (column, renderer, FALSE);
	gtk_tree_view_column_set_attributes (column, renderer,
					     "pixbuf", COLUMN_ICON,
					     NULL);

	/* name */

	window->priv->single_click = is_single_click_policy (window);

	renderer = gtk_cell_renderer_text_new ();

	g_value_init (&value, PANGO_TYPE_ELLIPSIZE_MODE);
	g_value_set_enum (&value, PANGO_ELLIPSIZE_END);
	g_object_set_property (G_OBJECT (renderer), "ellipsize", &value);
	g_value_unset (&value);

	gtk_tree_view_column_pack_start (column,
					 renderer,
					 TRUE);
	gtk_tree_view_column_set_attributes (column, renderer,
					     "text", COLUMN_NAME,
					     NULL);

	gtk_tree_view_column_set_sizing (column, GTK_TREE_VIEW_COLUMN_FIXED);
	w = g_settings_get_int (window->priv->settings_listing, PREF_LISTING_NAME_COLUMN_WIDTH);
	if (w <= 0)
		w = DEFAULT_NAME_COLUMN_WIDTH;
	gtk_tree_view_column_set_fixed_width (column, w);
	gtk_tree_view_column_set_resizable (column, TRUE);
	gtk_tree_view_column_set_sort_column_id (column, FR_WINDOW_SORT_BY_NAME);
	gtk_tree_view_column_set_cell_data_func (column, renderer,
						 (GtkTreeCellDataFunc) filename_cell_data_func,
						 window, NULL);

	gtk_tree_view_append_column (GTK_TREE_VIEW (treeview), column);

	/* Other columns */

	for (j = 0, i = COLUMN_SIZE; i < NUMBER_OF_COLUMNS; i++, j++) {
		GValue  value = { 0, };

		renderer = gtk_cell_renderer_text_new ();
		column = gtk_tree_view_column_new_with_attributes (g_dpgettext2 (NULL, "File", titles[j]),
								   renderer,
								   "text", i,
								   NULL);

		gtk_tree_view_column_set_sizing (column, GTK_TREE_VIEW_COLUMN_FIXED);
		gtk_tree_view_column_set_fixed_width (column, OTHER_COLUMNS_WIDTH);
		gtk_tree_view_column_set_resizable (column, TRUE);

		gtk_tree_view_column_set_sort_column_id (column, FR_WINDOW_SORT_BY_NAME + 1 + j);

		g_value_init (&value, PANGO_TYPE_ELLIPSIZE_MODE);
		g_value_set_enum (&value, PANGO_ELLIPSIZE_END);
		g_object_set_property (G_OBJECT (renderer), "ellipsize", &value);
		g_value_unset (&value);

		gtk_tree_view_append_column (treeview, column);
	}
}


static int
name_column_sort_func (GtkTreeModel *model,
		       GtkTreeIter  *a,
		       GtkTreeIter  *b,
		       gpointer      user_data)
{
	FileData    *fdata1;
	FileData    *fdata2;
	GtkSortType  sort_order;
	int          result;

	gtk_tree_sortable_get_sort_column_id (GTK_TREE_SORTABLE (model), NULL, &sort_order);

	gtk_tree_model_get (model, a, COLUMN_FILE_DATA, &fdata1, -1);
	gtk_tree_model_get (model, b, COLUMN_FILE_DATA, &fdata2, -1);

	if (file_data_is_dir (fdata1) == file_data_is_dir (fdata2)) {
		result = strcmp (fdata1->sort_key, fdata2->sort_key);
	}
	else {
        	result = file_data_is_dir (fdata1) ? -1 : 1;
        	if (sort_order == GTK_SORT_DESCENDING)
        		result = -1 * result;
	}

	return result;
}


static int
size_column_sort_func (GtkTreeModel *model,
		       GtkTreeIter  *a,
		       GtkTreeIter  *b,
		       gpointer      user_data)
{
	FileData    *fdata1;
	FileData    *fdata2;
	GtkSortType  sort_order;
	int          result;

	gtk_tree_sortable_get_sort_column_id (GTK_TREE_SORTABLE (model), NULL, &sort_order);

	gtk_tree_model_get (model, a, COLUMN_FILE_DATA, &fdata1, -1);
	gtk_tree_model_get (model, b, COLUMN_FILE_DATA, &fdata2, -1);

	if (file_data_is_dir (fdata1) == file_data_is_dir (fdata2)) {
        	if (file_data_is_dir (fdata1))
                	result = fdata1->dir_size - fdata2->dir_size;
        	else
        		result = fdata1->size - fdata2->size;
        }
        else {
        	result = file_data_is_dir (fdata1) ? -1 : 1;
        	if (sort_order == GTK_SORT_DESCENDING)
        		result = -1 * result;
        }

	return result;
}


static int
type_column_sort_func (GtkTreeModel *model,
		       GtkTreeIter  *a,
		       GtkTreeIter  *b,
		       gpointer      user_data)
{
	FileData    *fdata1;
	FileData    *fdata2;
	GtkSortType  sort_order;
	int          result;

	gtk_tree_sortable_get_sort_column_id (GTK_TREE_SORTABLE (model), NULL, &sort_order);

	gtk_tree_model_get (model, a, COLUMN_FILE_DATA, &fdata1, -1);
	gtk_tree_model_get (model, b, COLUMN_FILE_DATA, &fdata2, -1);

	if (file_data_is_dir (fdata1) == file_data_is_dir (fdata2)) {
        	if (file_data_is_dir (fdata1)) {
                	result = strcmp (fdata1->sort_key, fdata2->sort_key);
                	if (sort_order == GTK_SORT_DESCENDING)
                		result = -1 * result;
        	}
        	else {
        		const char  *desc1, *desc2;

        		desc1 = g_content_type_get_description (fdata1->content_type);
        		desc2 = g_content_type_get_description (fdata2->content_type);
        		result = strcasecmp (desc1, desc2);
        		if (result == 0)
        			result = strcmp (fdata1->sort_key, fdata2->sort_key);
        	}
        }
        else {
        	result = file_data_is_dir (fdata1) ? -1 : 1;
        	if (sort_order == GTK_SORT_DESCENDING)
        		result = -1 * result;
        }

	return result;
}


static int
time_column_sort_func (GtkTreeModel *model,
		       GtkTreeIter  *a,
		       GtkTreeIter  *b,
		       gpointer      user_data)
{
	FileData    *fdata1;
	FileData    *fdata2;
	GtkSortType  sort_order;
	int          result;

	gtk_tree_sortable_get_sort_column_id (GTK_TREE_SORTABLE (model), NULL, &sort_order);

	gtk_tree_model_get (model, a, COLUMN_FILE_DATA, &fdata1, -1);
	gtk_tree_model_get (model, b, COLUMN_FILE_DATA, &fdata2, -1);

	if (file_data_is_dir (fdata1) == file_data_is_dir (fdata2)) {
        	if (file_data_is_dir (fdata1)) {
                	result = strcmp (fdata1->sort_key, fdata2->sort_key);
                	if (sort_order == GTK_SORT_DESCENDING)
                		result = -1 * result;
        	}
        	else
        		result = fdata1->modified - fdata2->modified;
        }
        else {
        	result = file_data_is_dir (fdata1) ? -1 : 1;
        	if (sort_order == GTK_SORT_DESCENDING)
        		result = -1 * result;
        }

	return result;
}


static int
path_column_sort_func (GtkTreeModel *model,
		       GtkTreeIter  *a,
		       GtkTreeIter  *b,
		       gpointer      user_data)
{
	FileData *fdata1;
	FileData *fdata2;
	char     *path1;
	char     *path2;
	int       result;

	gtk_tree_model_get (model, a, COLUMN_FILE_DATA, &fdata1, COLUMN_PATH, &path1, -1);
	gtk_tree_model_get (model, b, COLUMN_FILE_DATA, &fdata2, COLUMN_PATH, &path2, -1);

	result = strcmp (path1, path2);
	if (result == 0)
		result = strcmp (fdata1->sort_key, fdata2->sort_key);

	g_free (path1);
	g_free (path2);

	return result;
}


static void
set_active (FrWindow   *window,
	    const char *action_name,
	    gboolean    is_active)
{
	GtkAction *action;

	action = gtk_action_group_get_action (window->priv->actions, action_name);
	gtk_toggle_action_set_active (GTK_TOGGLE_ACTION (action), is_active);
}


static gboolean
fr_window_show_cb (GtkWidget *widget,
		   FrWindow  *window)
{
	fr_window_update_current_location (window);

	set_active (window, "ViewToolbar", g_settings_get_boolean (window->priv->settings_ui, PREF_UI_VIEW_TOOLBAR));
	set_active (window, "ViewStatusbar", g_settings_get_boolean (window->priv->settings_ui, PREF_UI_VIEW_STATUSBAR));

	window->priv->view_folders = g_settings_get_boolean (window->priv->settings_ui, PREF_UI_VIEW_FOLDERS);
	set_active (window, "ViewFolders", window->priv->view_folders);

	gtk_widget_hide (window->priv->filter_bar);

	return TRUE;
}


/* preferences changes notification callbacks */


static void
pref_history_len_changed (GSettings  *settings,
			  const char *key,
			  gpointer    user_data)
{
	FrWindow  *window = user_data;
	int        limit;
	GtkAction *action;

	limit = g_settings_get_int (settings, PREF_UI_HISTORY_LEN);

	action = gtk_action_group_get_action (window->priv->actions, "OpenRecent");
	gtk_recent_chooser_set_limit (GTK_RECENT_CHOOSER (action), limit);

	action = gtk_action_group_get_action (window->priv->actions, "OpenRecent_Toolbar");
	gtk_recent_chooser_set_limit (GTK_RECENT_CHOOSER (action), limit);
}


static void
pref_view_toolbar_changed (GSettings  *settings,
		  	   const char *key,
		  	   gpointer    user_data)
{
	FrWindow *window = user_data;

	fr_window_set_toolbar_visibility (window, g_settings_get_boolean (settings, key));
}


static void
pref_view_statusbar_changed (GSettings  *settings,
		  	     const char *key,
		  	     gpointer    user_data)
{
	FrWindow *window = user_data;

	fr_window_set_statusbar_visibility (window, g_settings_get_boolean (settings, key));
}


static void
pref_view_folders_changed (GSettings  *settings,
		  	   const char *key,
		  	   gpointer    user_data)
{
	FrWindow *window = user_data;

	fr_window_set_folders_visibility (window, g_settings_get_boolean (settings, key));
}


static void
pref_show_field_changed (GSettings  *settings,
		  	 const char *key,
		  	 gpointer    user_data)
{
	FrWindow *window = user_data;

	fr_window_update_columns_visibility (window);
}


static void
pref_list_mode_changed (GSettings  *settings,
			const char *key,
			gpointer    user_data)
{
	FrWindow *window = user_data;

	fr_window_set_list_mode (window, g_settings_get_enum (settings, key));
}


static void
pref_click_policy_changed (GSettings  *settings,
		  	   const char *key,
		  	   gpointer    user_data)
{
	FrWindow   *window = user_data;
	GdkWindow  *win = gtk_tree_view_get_bin_window (GTK_TREE_VIEW (window->priv->list_view));
	GdkDisplay *display;

	window->priv->single_click = is_single_click_policy (window);

	gdk_window_set_cursor (win, NULL);
	display = gtk_widget_get_display (GTK_WIDGET (window->priv->list_view));
	if (display != NULL)
		gdk_display_flush (display);
}


static void
theme_changed_cb (GtkIconTheme *theme,
		  FrWindow     *window)
{
	if (window->priv->populating_file_list)
		return;

	gth_icon_cache_clear (window->priv->list_icon_cache);
	gth_icon_cache_clear (window->priv->tree_icon_cache);

	fr_window_update_file_list (window, TRUE);
	fr_window_update_dir_tree (window);
}


static gboolean
fr_archive_stoppable_cb (FrArchive *archive,
			 gboolean   stoppable,
			 FrWindow  *window)
{
	window->priv->stoppable = stoppable;
	if (window->priv->progress_dialog != NULL)
		gtk_dialog_set_response_sensitive (GTK_DIALOG (window->priv->progress_dialog),
						   GTK_RESPONSE_OK,
						   stoppable);
	return TRUE;
}


static void
menu_item_select_cb (GtkMenuItem *proxy,
		     FrWindow    *window)
{
	GtkAction *action;
	char      *message;

	action = gtk_activatable_get_related_action (GTK_ACTIVATABLE (proxy));
	g_return_if_fail (action != NULL);

	g_object_get (G_OBJECT (action), "tooltip", &message, NULL);
	if (message) {
		gtk_statusbar_push (GTK_STATUSBAR (window->priv->statusbar),
				    window->priv->help_message_cid, message);
		g_free (message);
	}
}


static void
menu_item_deselect_cb (GtkMenuItem *proxy,
		       FrWindow    *window)
{
	gtk_statusbar_pop (GTK_STATUSBAR (window->priv->statusbar),
			   window->priv->help_message_cid);
}


static void
disconnect_proxy_cb (GtkUIManager *manager,
		     GtkAction    *action,
		     GtkWidget    *proxy,
		     FrWindow     *window)
{
	if (GTK_IS_MENU_ITEM (proxy)) {
		g_signal_handlers_disconnect_by_func
			(proxy, G_CALLBACK (menu_item_select_cb), window);
		g_signal_handlers_disconnect_by_func
			(proxy, G_CALLBACK (menu_item_deselect_cb), window);
	}
}


static void
connect_proxy_cb (GtkUIManager *manager,
		  GtkAction    *action,
		  GtkWidget    *proxy,
		  FrWindow     *window)
{
	if (GTK_IS_MENU_ITEM (proxy)) {
		g_signal_connect (proxy, "select",
				  G_CALLBACK (menu_item_select_cb), window);
		g_signal_connect (proxy, "deselect",
				  G_CALLBACK (menu_item_deselect_cb), window);
	}
}


static void
view_as_radio_action (GtkAction      *action,
		      GtkRadioAction *current,
		      gpointer        data)
{
	FrWindow *window = data;
	fr_window_set_list_mode (window, gtk_radio_action_get_current_value (current));
}


static void
recent_chooser_item_activated_cb (GtkRecentChooser *chooser,
				  FrWindow         *window)
{
	char  *uri;
	GFile *file;

	uri = gtk_recent_chooser_get_current_uri (chooser);
	if (uri == NULL)
		return;

	file = g_file_new_for_uri (uri);
	fr_window_archive_open (window, file, GTK_WINDOW (window));

	g_object_unref (file);
	g_free (uri);
}


static void
fr_window_init_recent_chooser (FrWindow         *window,
			       GtkRecentChooser *chooser)
{
	GtkRecentFilter *filter;
	int              i;

	g_return_if_fail (chooser != NULL);

	filter = gtk_recent_filter_new ();
	gtk_recent_filter_set_name (filter, _("All archives"));
	for (i = 0; open_type[i] != -1; i++)
		gtk_recent_filter_add_mime_type (filter, mime_type_desc[open_type[i]].mime_type);
	gtk_recent_chooser_add_filter (chooser, filter);

	gtk_recent_chooser_set_local_only (chooser, FALSE);
	gtk_recent_chooser_set_limit (chooser, g_settings_get_int (window->priv->settings_ui, PREF_UI_HISTORY_LEN));
	gtk_recent_chooser_set_show_not_found (chooser, TRUE);
	gtk_recent_chooser_set_sort_type (chooser, GTK_RECENT_SORT_MRU);

	g_signal_connect (G_OBJECT (chooser),
			  "item_activated",
			  G_CALLBACK (recent_chooser_item_activated_cb),
			  window);
}


static void
fr_window_activate_filter (FrWindow *window)
{
	GtkTreeView       *tree_view = GTK_TREE_VIEW (window->priv->list_view);
	GtkTreeViewColumn *column;

	gtk_widget_show (window->priv->filter_bar);
	window->priv->list_mode = FR_WINDOW_LIST_MODE_FLAT;

	gtk_list_store_clear (window->priv->list_store);

	column = gtk_tree_view_get_column (tree_view, 4);
	gtk_tree_view_column_set_visible (column, TRUE);

	fr_window_update_file_list (window, TRUE);
	fr_window_update_dir_tree (window);
	fr_window_update_current_location (window);
}


static void
filter_entry_activate_cb (GtkEntry *entry,
			  FrWindow *window)
{
	fr_window_activate_filter (window);
}


static void
fr_window_attach (FrWindow      *window,
		  GtkWidget     *child,
		  FrWindowArea   area)
{
	int position;

	g_return_if_fail (window != NULL);
	g_return_if_fail (FR_IS_WINDOW (window));
	g_return_if_fail (child != NULL);
	g_return_if_fail (GTK_IS_WIDGET (child));

	switch (area) {
	case FR_WINDOW_AREA_MENUBAR:
		position = 0;
		break;
	case FR_WINDOW_AREA_TOOLBAR:
		position = 1;
		break;
	case FR_WINDOW_AREA_LOCATIONBAR:
		position = 2;
		break;
	case FR_WINDOW_AREA_FILTERBAR:
		position = 3;
		break;
	case FR_WINDOW_AREA_CONTENTS:
		position = 4;
		if (window->priv->contents != NULL)
			gtk_widget_destroy (window->priv->contents);
		window->priv->contents = child;
		gtk_widget_set_vexpand (child, TRUE);
		break;
	case FR_WINDOW_AREA_STATUSBAR:
		position = 5;
		break;
	default:
		g_critical ("%s: area not recognized!", G_STRFUNC);
		return;
		break;
	}

	gtk_widget_set_hexpand (child, TRUE);
	gtk_grid_attach (GTK_GRID (window->priv->layout),
			 child,
			 0, position,
			 1, 1);
}


static void
set_action_important (GtkUIManager *ui,
		      const char   *action_name)
{
	GtkAction *action;

	action = gtk_ui_manager_get_action (ui, action_name);
	g_object_set (action, "is_important", TRUE, NULL);
	g_object_unref (action);
}


static void
fr_window_construct (FrWindow *window)
{
	GtkWidget          *menubar;
	GtkWidget          *toolbar;
	GtkWidget          *list_scrolled_window;
	GtkWidget          *location_box;
	GtkStatusbar       *statusbar;
	GtkWidget          *statusbar_box;
	GtkWidget          *filter_box;
	GtkWidget          *tree_scrolled_window;
	GtkTreeSelection   *selection;
	GtkActionGroup     *actions;
	GtkAction          *action;
	GtkAction          *other_actions_action;
	GtkUIManager       *ui;
	GtkSizeGroup       *toolbar_size_group;
	GError             *error = NULL;
	const char * const *schemas;

	/* Create the settings objects */

	window->priv->settings_listing = g_settings_new (FILE_ROLLER_SCHEMA_LISTING);
	window->priv->settings_ui = g_settings_new (FILE_ROLLER_SCHEMA_UI);
	window->priv->settings_general = g_settings_new (FILE_ROLLER_SCHEMA_GENERAL);
	window->priv->settings_dialogs = g_settings_new (FILE_ROLLER_SCHEMA_DIALOGS);

	/* Only use the nautilus schema if it's installed */
	for (schemas = g_settings_list_schemas ();
	     *schemas != NULL;
	     schemas++)
	{
		if (g_strcmp0 (*schemas, NAUTILUS_SCHEMA) == 0) {
			window->priv->settings_nautilus = g_settings_new (NAUTILUS_SCHEMA);
			break;
		}
	}

	/* Create the application. */

	window->priv->layout = gtk_grid_new ();
	gtk_container_add (GTK_CONTAINER (window), window->priv->layout);
	gtk_widget_show (window->priv->layout);

	gtk_window_set_title (GTK_WINDOW (window), _("Archive Manager"));
	gtk_window_set_has_resize_grip (GTK_WINDOW (window), TRUE);

	g_signal_connect (G_OBJECT (window),
			  "delete_event",
			  G_CALLBACK (fr_window_delete_event_cb),
			  window);

	g_signal_connect (G_OBJECT (window),
			  "show",
			  G_CALLBACK (fr_window_show_cb),
			  window);

	window->priv->theme_changed_handler_id =
		g_signal_connect (gtk_icon_theme_get_default (),
				  "changed",
				  G_CALLBACK (theme_changed_cb),
				  window);

	gtk_window_set_default_size (GTK_WINDOW (window),
				     g_settings_get_int (window->priv->settings_ui, PREF_UI_WINDOW_WIDTH),
				     g_settings_get_int (window->priv->settings_ui, PREF_UI_WINDOW_HEIGHT));

	gtk_drag_dest_set (GTK_WIDGET (window),
			   GTK_DEST_DEFAULT_ALL,
			   target_table, G_N_ELEMENTS (target_table),
			   GDK_ACTION_COPY);

	g_signal_connect (G_OBJECT (window),
			  "drag_data_received",
			  G_CALLBACK (fr_window_drag_data_received),
			  window);
	g_signal_connect (G_OBJECT (window),
			  "drag_motion",
			  G_CALLBACK (fr_window_drag_motion),
			  window);

	g_signal_connect (G_OBJECT (window),
			  "key_press_event",
			  G_CALLBACK (key_press_cb),
			  window);

	/* Initialize Data. */

	window->priv->list_mode = window->priv->last_list_mode = g_settings_get_enum (window->priv->settings_listing, PREF_LISTING_LIST_MODE);
	g_settings_set_boolean (window->priv->settings_listing, PREF_LISTING_SHOW_PATH, (window->priv->list_mode == FR_WINDOW_LIST_MODE_FLAT));

	window->priv->history = NULL;
	window->priv->history_current = NULL;

	window->priv->action = FR_ACTION_NONE;

	window->priv->open_default_dir = g_object_ref (_g_file_get_home ());
	window->priv->add_default_dir = NULL;
	window->priv->extract_default_dir = g_object_ref (_g_file_get_home ());

	window->priv->give_focus_to_the_list = FALSE;

	window->priv->activity_ref = 0;
	window->priv->activity_timeout_handle = 0;

	window->priv->update_timeout_handle = 0;

	window->priv->archive_present = FALSE;
	window->priv->archive_new = FALSE;
	window->priv->reload_archive = FALSE;
	window->priv->archive_file = NULL;

	window->priv->drag_destination_folder = NULL;
	window->priv->drag_base_dir = NULL;
	window->priv->drag_error = NULL;
	window->priv->drag_file_list = NULL;

	window->priv->batch_mode = FALSE;
	window->priv->batch_action_list = NULL;
	window->priv->batch_action = NULL;
	window->priv->extract_interact_use_default_dir = FALSE;

	window->priv->password = NULL;
	window->priv->compression = g_settings_get_enum (window->priv->settings_general, PREF_GENERAL_COMPRESSION_LEVEL);
	window->priv->encrypt_header = g_settings_get_boolean (window->priv->settings_general, PREF_GENERAL_ENCRYPT_HEADER);
	window->priv->volume_size = 0;

	window->priv->stoppable = TRUE;

	window->priv->batch_adding_one_file = FALSE;

	window->priv->path_clicked = NULL;

	window->priv->current_view_length = 0;

	window->priv->current_batch_action.type = FR_BATCH_ACTION_NONE;
	window->priv->current_batch_action.data = NULL;
	window->priv->current_batch_action.free_func = NULL;

	window->priv->pd_last_archive = NULL;
	window->priv->pd_last_message = NULL;
	window->priv->pd_last_fraction = 0.0;

	/* Create the widgets. */

	/* * File list. */

	window->priv->list_store = fr_list_model_new (NUMBER_OF_COLUMNS,
						      G_TYPE_POINTER,
						      GDK_TYPE_PIXBUF,
						      G_TYPE_STRING,
						      GDK_TYPE_PIXBUF,
						      G_TYPE_STRING,
						      G_TYPE_STRING,
						      G_TYPE_STRING,
						      G_TYPE_STRING);
	g_object_set_data (G_OBJECT (window->priv->list_store), "FrWindow", window);
	window->priv->list_view = gtk_tree_view_new_with_model (GTK_TREE_MODEL (window->priv->list_store));

	gtk_tree_view_set_rules_hint (GTK_TREE_VIEW (window->priv->list_view), TRUE);
	add_file_list_columns (window, GTK_TREE_VIEW (window->priv->list_view));
	gtk_tree_view_set_enable_search (GTK_TREE_VIEW (window->priv->list_view),
					 TRUE);
	gtk_tree_view_set_search_column (GTK_TREE_VIEW (window->priv->list_view),
					 COLUMN_NAME);

	gtk_tree_sortable_set_sort_func (GTK_TREE_SORTABLE (window->priv->list_store),
					 FR_WINDOW_SORT_BY_NAME, name_column_sort_func,
					 NULL, NULL);
	gtk_tree_sortable_set_sort_func (GTK_TREE_SORTABLE (window->priv->list_store),
					 FR_WINDOW_SORT_BY_SIZE, size_column_sort_func,
					 NULL, NULL);
	gtk_tree_sortable_set_sort_func (GTK_TREE_SORTABLE (window->priv->list_store),
					 FR_WINDOW_SORT_BY_TYPE, type_column_sort_func,
					 NULL, NULL);
	gtk_tree_sortable_set_sort_func (GTK_TREE_SORTABLE (window->priv->list_store),
					 FR_WINDOW_SORT_BY_TIME, time_column_sort_func,
					 NULL, NULL);
	gtk_tree_sortable_set_sort_func (GTK_TREE_SORTABLE (window->priv->list_store),
					 FR_WINDOW_SORT_BY_PATH, path_column_sort_func,
					 NULL, NULL);

	selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (window->priv->list_view));
	gtk_tree_selection_set_mode (selection, GTK_SELECTION_MULTIPLE);

	g_signal_connect (selection,
			  "changed",
			  G_CALLBACK (selection_changed_cb),
			  window);
	g_signal_connect (G_OBJECT (window->priv->list_view),
			  "row_activated",
			  G_CALLBACK (row_activated_cb),
			  window);
	g_signal_connect (G_OBJECT (window->priv->list_view),
			  "button_press_event",
			  G_CALLBACK (file_button_press_cb),
			  window);
	g_signal_connect (G_OBJECT (window->priv->list_view),
			  "button_release_event",
			  G_CALLBACK (file_button_release_cb),
			  window);
	g_signal_connect (G_OBJECT (window->priv->list_view),
			  "motion_notify_event",
			  G_CALLBACK (file_motion_notify_callback),
			  window);
	g_signal_connect (G_OBJECT (window->priv->list_view),
			  "leave_notify_event",
			  G_CALLBACK (file_leave_notify_callback),
			  window);
	g_signal_connect (G_OBJECT (window->priv->list_view),
			  "drag_begin",
			  G_CALLBACK (file_list_drag_begin),
			  window);
	g_signal_connect (G_OBJECT (window->priv->list_view),
			  "drag_end",
			  G_CALLBACK (file_list_drag_end),
			  window);
	egg_tree_multi_drag_add_drag_support (GTK_TREE_VIEW (window->priv->list_view));

	list_scrolled_window = gtk_scrolled_window_new (NULL, NULL);
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (list_scrolled_window),
					GTK_POLICY_AUTOMATIC,
					GTK_POLICY_AUTOMATIC);
	gtk_container_add (GTK_CONTAINER (list_scrolled_window), window->priv->list_view);

	/* filter bar */

	window->priv->filter_bar = filter_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
	g_object_set (window->priv->filter_bar,
		      "halign", GTK_ALIGN_CENTER,
		      "margin-left", 6,
		      "border-width", 3,
		      NULL);
	fr_window_attach (FR_WINDOW (window), window->priv->filter_bar, FR_WINDOW_AREA_FILTERBAR);

	/* * filter entry */

	window->priv->filter_entry = GTK_WIDGET (gtk_entry_new ());
	gtk_entry_set_icon_from_icon_name (GTK_ENTRY (window->priv->filter_entry),
					   GTK_ENTRY_ICON_SECONDARY,
					   "edit-find-symbolic");
	gtk_entry_set_icon_activatable (GTK_ENTRY (window->priv->filter_entry),
					GTK_ENTRY_ICON_SECONDARY,
					FALSE);
	gtk_entry_set_width_chars (GTK_ENTRY (window->priv->filter_entry), 40);
	gtk_box_pack_start (GTK_BOX (filter_box),
			    window->priv->filter_entry, FALSE, FALSE, 6);

	g_signal_connect (G_OBJECT (window->priv->filter_entry),
			  "activate",
			  G_CALLBACK (filter_entry_activate_cb),
			  window);

	gtk_widget_show_all (filter_box);

	/* tree view */

	window->priv->tree_store = gtk_tree_store_new (TREE_NUMBER_OF_COLUMNS,
						       G_TYPE_STRING,
						       GDK_TYPE_PIXBUF,
						       G_TYPE_STRING,
						       PANGO_TYPE_WEIGHT);
	window->priv->tree_view = gtk_tree_view_new_with_model (GTK_TREE_MODEL (window->priv->tree_store));
	gtk_tree_view_set_headers_visible (GTK_TREE_VIEW (window->priv->tree_view), FALSE);
	add_dir_tree_columns (window, GTK_TREE_VIEW (window->priv->tree_view));

	g_signal_connect (G_OBJECT (window->priv->tree_view),
			  "button_press_event",
			  G_CALLBACK (dir_tree_button_press_cb),
			  window);

	selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (window->priv->tree_view));
	g_signal_connect (selection,
			  "changed",
			  G_CALLBACK (dir_tree_selection_changed_cb),
			  window);

	g_signal_connect (G_OBJECT (window->priv->tree_view),
			  "drag_begin",
			  G_CALLBACK (file_list_drag_begin),
			  window);
	g_signal_connect (G_OBJECT (window->priv->tree_view),
			  "drag_end",
			  G_CALLBACK (file_list_drag_end),
			  window);
	g_signal_connect (G_OBJECT (window->priv->tree_view),
			  "drag_data_get",
			  G_CALLBACK (fr_window_folder_tree_drag_data_get),
			  window);
	gtk_drag_source_set (window->priv->tree_view,
			     GDK_BUTTON1_MASK,
			     folder_tree_targets, G_N_ELEMENTS (folder_tree_targets),
			     GDK_ACTION_COPY);

	tree_scrolled_window = gtk_scrolled_window_new (NULL, NULL);
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (tree_scrolled_window),
					GTK_POLICY_AUTOMATIC,
					GTK_POLICY_AUTOMATIC);
	gtk_container_add (GTK_CONTAINER (tree_scrolled_window), window->priv->tree_view);

	/* side pane */

	window->priv->sidepane = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
	gtk_box_pack_start (GTK_BOX (window->priv->sidepane), tree_scrolled_window, TRUE, TRUE, 0);

	/* main content */

	window->priv->paned = gtk_paned_new (GTK_ORIENTATION_HORIZONTAL);
	gtk_paned_pack1 (GTK_PANED (window->priv->paned), window->priv->sidepane, FALSE, TRUE);
	gtk_paned_pack2 (GTK_PANED (window->priv->paned), list_scrolled_window, TRUE, TRUE);
	gtk_paned_set_position (GTK_PANED (window->priv->paned), g_settings_get_int (window->priv->settings_ui, PREF_UI_SIDEBAR_WIDTH));

	fr_window_attach (FR_WINDOW (window), window->priv->paned, FR_WINDOW_AREA_CONTENTS);
	gtk_widget_show_all (window->priv->paned);

	/* Build the menu and the toolbar. */

	ui = gtk_ui_manager_new ();

	window->priv->actions = actions = gtk_action_group_new ("Actions");

	/* open recent menu item action  */

	action = g_object_new (GTK_TYPE_RECENT_ACTION,
			       "name", "OpenRecent",
			       /* Translators: this is the label for the "open recent file" sub-menu. */
			       "label", _("Open _Recent"),
			       "tooltip", _("Open a recently used archive"),
			       "stock-id", GTK_STOCK_OPEN,
			       NULL);
	fr_window_init_recent_chooser (window, GTK_RECENT_CHOOSER (action));
	gtk_action_group_add_action (actions, action);
	g_object_unref (action);

	/* open recent toolbar item action  */

	action = g_object_new (GTK_TYPE_RECENT_ACTION,
			       "name", "OpenRecent_Toolbar",
			       "label", _("Open"),
			       "tooltip", _("Open a recently used archive"),
			       "stock-id", GTK_STOCK_OPEN,
			       "is-important", TRUE,
			       NULL);
	fr_window_init_recent_chooser (window, GTK_RECENT_CHOOSER (action));
	g_signal_connect (action,
			  "activate",
			  G_CALLBACK (activate_action_open),
			  window);
	gtk_action_group_add_action (actions, action);
	g_object_unref (action);

	/* menu actions */

	other_actions_action = action = g_object_new (GTH_TYPE_TOGGLE_MENU_ACTION,
			       "name", "OtherActions",
			       "label", _("_Other Actions"),
			       "tooltip", _("Other actions"),
			       "menu-halign", GTK_ALIGN_CENTER,
			       "show-arrow", TRUE,
			       NULL);
	gtk_action_group_add_action (actions, action);

	/* other actions */

	gtk_action_group_set_translation_domain (actions, NULL);
	gtk_action_group_add_actions (actions,
				      action_entries,
				      n_action_entries,
				      window);
	gtk_action_group_add_toggle_actions (actions,
					     action_toggle_entries,
					     n_action_toggle_entries,
					     window);
	gtk_action_group_add_radio_actions (actions,
					    view_as_entries,
					    n_view_as_entries,
					    window->priv->list_mode,
					    G_CALLBACK (view_as_radio_action),
					    window);

	g_signal_connect (ui, "connect_proxy",
			  G_CALLBACK (connect_proxy_cb), window);
	g_signal_connect (ui, "disconnect_proxy",
			  G_CALLBACK (disconnect_proxy_cb), window);

	gtk_ui_manager_insert_action_group (ui, actions, 0);
	gtk_window_add_accel_group (GTK_WINDOW (window),
				    gtk_ui_manager_get_accel_group (ui));

	/* Add a hidden short cut Ctrl-Q for power users */
	gtk_accel_group_connect (gtk_ui_manager_get_accel_group (ui),
				 GDK_KEY_q, GDK_CONTROL_MASK, 0,
				 g_cclosure_new_swap (G_CALLBACK (fr_window_close), window, NULL));

	if (! gtk_ui_manager_add_ui_from_resource (ui, "/org/gnome/FileRoller/ui/menus-toolbars.ui", &error)) {
		g_message ("building menus failed: %s", error->message);
		g_error_free (error);
	}

	g_object_set (other_actions_action, "menu", gtk_ui_manager_get_widget (ui, "/OtherActionsMenu"), NULL);
	g_object_unref (other_actions_action);

	menubar = gtk_ui_manager_get_widget (ui, "/MenuBar");
	fr_window_attach (FR_WINDOW (window), menubar, FR_WINDOW_AREA_MENUBAR);
	gtk_widget_show (menubar);

	window->priv->toolbar = toolbar = gtk_ui_manager_get_widget (ui, "/ToolBar");
	gtk_toolbar_set_show_arrow (GTK_TOOLBAR (toolbar), TRUE);
	gtk_style_context_add_class (gtk_widget_get_style_context (toolbar), GTK_STYLE_CLASS_PRIMARY_TOOLBAR);
	set_action_important (ui, "/ToolBar/Extract_Toolbar");
	set_action_important (ui, "/ToolBar/Add_Toolbar");

	/* location bar */

	window->priv->location_bar = gtk_ui_manager_get_widget (ui, "/LocationBar");
	gtk_toolbar_set_show_arrow (GTK_TOOLBAR (window->priv->location_bar), FALSE);
	gtk_toolbar_set_style (GTK_TOOLBAR (window->priv->location_bar), GTK_TOOLBAR_BOTH_HORIZ);
	gtk_style_context_add_class (gtk_widget_get_style_context (window->priv->location_bar), GTK_STYLE_CLASS_TOOLBAR);
	set_action_important (ui, "/LocationBar/GoBack");

	/* current location */

	location_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
	/* Translators: after the colon there is a folder name. */
	window->priv->location_label = gtk_label_new_with_mnemonic (_("_Location:"));
	gtk_box_pack_start (GTK_BOX (location_box),
			    window->priv->location_label, FALSE, FALSE, 5);

	window->priv->location_entry = gtk_entry_new ();
	gtk_entry_set_icon_from_stock (GTK_ENTRY (window->priv->location_entry),
				       GTK_ENTRY_ICON_PRIMARY,
				       GTK_STOCK_DIRECTORY);

	gtk_box_pack_start (GTK_BOX (location_box),
			    window->priv->location_entry, TRUE, TRUE, 5);

	g_signal_connect (G_OBJECT (window->priv->location_entry),
			  "key_press_event",
			  G_CALLBACK (location_entry_key_press_event_cb),
			  window);

	{
		GtkToolItem *tool_item;

		tool_item = gtk_separator_tool_item_new ();
		gtk_widget_show_all (GTK_WIDGET (tool_item));
		gtk_toolbar_insert (GTK_TOOLBAR (window->priv->location_bar), tool_item, -1);

		tool_item = gtk_tool_item_new ();
		gtk_tool_item_set_expand (tool_item, TRUE);
		gtk_container_add (GTK_CONTAINER (tool_item), location_box);
		gtk_widget_show_all (GTK_WIDGET (tool_item));
		gtk_toolbar_insert (GTK_TOOLBAR (window->priv->location_bar), tool_item, -1);
	}

	fr_window_attach (FR_WINDOW (window), window->priv->location_bar, FR_WINDOW_AREA_LOCATIONBAR);
	if (window->priv->list_mode == FR_WINDOW_LIST_MODE_FLAT)
		gtk_widget_hide (window->priv->location_bar);
	else
		gtk_widget_show (window->priv->location_bar);

	/**/

	fr_window_attach (FR_WINDOW (window), window->priv->toolbar, FR_WINDOW_AREA_TOOLBAR);
	if (g_settings_get_boolean (window->priv->settings_ui, PREF_UI_VIEW_TOOLBAR))
		gtk_widget_show (toolbar);
	else
		gtk_widget_hide (toolbar);

	window->priv->file_popup_menu = gtk_ui_manager_get_widget (ui, "/FilePopupMenu");
	window->priv->folder_popup_menu = gtk_ui_manager_get_widget (ui, "/FolderPopupMenu");
	window->priv->sidebar_folder_popup_menu = gtk_ui_manager_get_widget (ui, "/SidebarFolderPopupMenu");

	/* Create the statusbar. */

	window->priv->statusbar = gtk_statusbar_new ();
	window->priv->help_message_cid = gtk_statusbar_get_context_id (GTK_STATUSBAR (window->priv->statusbar), "help_message");
	window->priv->list_info_cid = gtk_statusbar_get_context_id (GTK_STATUSBAR (window->priv->statusbar), "list_info");
	window->priv->progress_cid = gtk_statusbar_get_context_id (GTK_STATUSBAR (window->priv->statusbar), "progress");

	statusbar = GTK_STATUSBAR (window->priv->statusbar);
	statusbar_box = gtk_statusbar_get_message_area (statusbar);
	gtk_box_set_homogeneous (GTK_BOX (statusbar_box), FALSE);
	gtk_box_set_spacing (GTK_BOX (statusbar_box), 4);
	gtk_box_set_child_packing (GTK_BOX (statusbar_box), gtk_statusbar_get_message_area (statusbar), TRUE, TRUE, 0, GTK_PACK_START );

	window->priv->progress_bar = gtk_progress_bar_new ();
	gtk_progress_bar_set_pulse_step (GTK_PROGRESS_BAR (window->priv->progress_bar), ACTIVITY_PULSE_STEP);
	gtk_widget_set_size_request (window->priv->progress_bar, -1, PROGRESS_BAR_HEIGHT);
	{
		GtkWidget *vbox;

		vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
		gtk_box_pack_start (GTK_BOX (statusbar_box), vbox, FALSE, FALSE, 0);
		gtk_box_pack_start (GTK_BOX (vbox), window->priv->progress_bar, TRUE, TRUE, 1);
		gtk_widget_show (vbox);
	}
	gtk_widget_show (statusbar_box);

	fr_window_attach (FR_WINDOW (window), window->priv->statusbar, FR_WINDOW_AREA_STATUSBAR);
	if (g_settings_get_boolean (window->priv->settings_ui, PREF_UI_VIEW_STATUSBAR))
		gtk_widget_show (window->priv->statusbar);
	else
		gtk_widget_hide (window->priv->statusbar);

	/**/

	toolbar_size_group = gtk_size_group_new (GTK_SIZE_GROUP_VERTICAL);
	gtk_size_group_add_widget (toolbar_size_group, window->priv->location_bar);
	gtk_size_group_add_widget (toolbar_size_group, window->priv->filter_bar);

	/**/

	fr_window_update_title (window);
	fr_window_update_sensitivity (window);
	fr_window_update_current_location (window);
	fr_window_update_columns_visibility (window);

	/* Add notification callbacks. */

	g_signal_connect (window->priv->settings_ui,
			  "changed::" PREF_UI_HISTORY_LEN,
			  G_CALLBACK (pref_history_len_changed),
			  window);
	g_signal_connect (window->priv->settings_ui,
			  "changed::" PREF_UI_VIEW_TOOLBAR,
			  G_CALLBACK (pref_view_toolbar_changed),
			  window);
	g_signal_connect (window->priv->settings_ui,
			  "changed::" PREF_UI_VIEW_STATUSBAR,
			  G_CALLBACK (pref_view_statusbar_changed),
			  window);
	g_signal_connect (window->priv->settings_ui,
			  "changed::" PREF_UI_VIEW_FOLDERS,
			  G_CALLBACK (pref_view_folders_changed),
			  window);
	g_signal_connect (window->priv->settings_listing,
			  "changed::" PREF_LISTING_SHOW_TYPE,
			  G_CALLBACK (pref_show_field_changed),
			  window);
	g_signal_connect (window->priv->settings_listing,
			  "changed::" PREF_LISTING_SHOW_SIZE,
			  G_CALLBACK (pref_show_field_changed),
			  window);
	g_signal_connect (window->priv->settings_listing,
			  "changed::" PREF_LISTING_SHOW_TIME,
			  G_CALLBACK (pref_show_field_changed),
			  window);
	g_signal_connect (window->priv->settings_listing,
			  "changed::" PREF_LISTING_SHOW_PATH,
			  G_CALLBACK (pref_show_field_changed),
			  window);
	g_signal_connect (window->priv->settings_listing,
			  "changed::" PREF_LISTING_LIST_MODE,
			  G_CALLBACK (pref_list_mode_changed),
			  window);

	if (window->priv->settings_nautilus)
		g_signal_connect (window->priv->settings_nautilus,
				  "changed::" NAUTILUS_CLICK_POLICY,
				  G_CALLBACK (pref_click_policy_changed),
				  window);

	/* Give focus to the list. */

	gtk_widget_grab_focus (window->priv->list_view);
}


GtkWidget *
fr_window_new (void)
{
	GtkWidget *window;

	window = g_object_new (FR_TYPE_WINDOW, "application", g_application_get_default (), NULL);
	fr_window_construct ((FrWindow*) window);

	return window;
}


static void
_fr_window_set_archive (FrWindow  *window,
			FrArchive *archive)
{
	if (window->archive != NULL) {
		g_signal_handlers_disconnect_by_data (window->archive, window);
		g_object_unref (window->archive);
	}

	window->archive = _g_object_ref (archive);

	if (window->archive == NULL)
		return;

	g_signal_connect (G_OBJECT (window->archive),
			  "progress",
			  G_CALLBACK (fr_archive_progress_cb),
			  window);
	g_signal_connect (G_OBJECT (window->archive),
			  "message",
			  G_CALLBACK (fr_archive_message_cb),
			  window);
	g_signal_connect (G_OBJECT (window->archive),
			  "start",
			  G_CALLBACK (fr_archive_start_cb),
			  window);
	g_signal_connect (G_OBJECT (window->archive),
			  "stoppable",
			  G_CALLBACK (fr_archive_stoppable_cb),
			  window);
	g_signal_connect (G_OBJECT (window->archive),
			  "working-archive",
			  G_CALLBACK (fr_window_working_archive_cb),
			  window);
}


static void
_fr_window_set_archive_file (FrWindow *window,
			     GFile    *file)
{
	_g_object_unref (window->priv->archive_file);
	window->priv->archive_file = _g_object_ref (file);
}


gboolean
fr_window_archive_new (FrWindow   *window,
		       GFile      *file,
		       const char *mime_type)
{
	FrArchive *archive;

	g_return_val_if_fail (window != NULL, FALSE);

	archive = fr_archive_create (file, mime_type);
	if (archive != NULL) {
		fr_window_archive_close (window);
		_fr_window_set_archive (window, archive);
		_fr_window_set_archive_file (window, file);
		window->priv->archive_present = TRUE;
		window->priv->archive_new = TRUE;

		g_object_unref (archive);
	}

	return archive != NULL;
}


static void
archive_list_ready_cb (GObject      *source_object,
		       GAsyncResult *result,
		       gpointer      user_data)
{
	FrWindow *window = user_data;
	GError   *error = NULL;

	fr_archive_operation_finish (FR_ARCHIVE (source_object), result, &error);
	_archive_operation_completed (window, FR_ACTION_LISTING_CONTENT, error);

	_g_error_free (error);
}


static void
fr_window_archive_list (FrWindow *window)
{
	if (! fr_archive_is_capable_of (window->archive, FR_ARCHIVE_CAN_READ)) {
		fr_window_archive_close (window);
		fr_window_archive_open (window, window->priv->archive_file, NULL);
		return;
	}

	_archive_operation_started (window, FR_ACTION_LISTING_CONTENT);
	fr_archive_list (window->archive,
			 window->priv->password,
			 window->priv->cancellable,
			 archive_list_ready_cb,
			 window);
}


static void
archive_open_ready_cb (GObject      *source_object,
		       GAsyncResult *result,
		       gpointer      user_data)
{
	FrWindow  *window = user_data;
	FrArchive *archive;
	GError    *error = NULL;

	archive = fr_archive_open_finish (G_FILE (source_object), result, &error);
	_fr_window_set_archive (window, archive);

	g_signal_emit (window,
		       fr_window_signals[ARCHIVE_LOADED],
		       0,
		       error == NULL);

	_archive_operation_completed (window, FR_ACTION_LOADING_ARCHIVE, error);
	_g_error_free (error);
	_g_object_unref (archive);
}


FrWindow *
fr_window_archive_open (FrWindow   *current_window,
			GFile      *file,
			GtkWindow  *parent)
{
	FrWindow *window = current_window;

	g_return_val_if_fail (file != NULL, FALSE);

	if (current_window->priv->archive_present)
		window = (FrWindow *) fr_window_new ();

	g_return_val_if_fail (window != NULL, FALSE);

	fr_window_archive_close (window);
	_fr_window_set_archive_file (window, file);
	window->priv->give_focus_to_the_list = TRUE;
	window->priv->load_error_parent_window = parent;

	_archive_operation_started (window, FR_ACTION_LOADING_ARCHIVE);

	/* this is used to reload the archive after asking a password */
	fr_window_set_current_batch_action (window,
					    FR_BATCH_ACTION_LOAD,
					    g_object_ref (file),
					    (GFreeFunc) g_object_unref);

	fr_archive_open (file,
			 window->priv->cancellable,
			 archive_open_ready_cb,
			 window);

	return window;
}


void
fr_window_archive_close (FrWindow *window)
{
	g_return_if_fail (window != NULL);

	if (! window->priv->archive_new && ! window->priv->archive_present)
		return;

	fr_window_free_open_files (window);
	fr_clipboard_data_unref (window->priv->copy_data);
	window->priv->copy_data = NULL;

	fr_window_set_password (window, NULL);
	fr_window_set_volume_size (window, 0);
	fr_window_history_clear (window);

	_fr_window_set_archive (window, NULL);
	window->priv->archive_new = FALSE;
	window->priv->archive_present = FALSE;

	fr_window_update_title (window);
	fr_window_update_sensitivity (window);
	fr_window_update_file_list (window, FALSE);
	fr_window_update_dir_tree (window);
	fr_window_update_current_location (window);
	fr_window_update_statusbar_list_info (window);
}


GFile *
fr_window_get_archive_file (FrWindow *window)
{
	g_return_val_if_fail (window != NULL, NULL);

	return window->priv->archive_file;
}


GFile *
fr_window_get_archive_file_for_paste (FrWindow *window)
{
	g_return_val_if_fail (window != NULL, NULL);

	if (window->priv->clipboard_data != NULL)
		return window->priv->clipboard_data->file;
	else
		return NULL;
}


gboolean
fr_window_archive_is_present (FrWindow *window)
{
	g_return_val_if_fail (window != NULL, FALSE);

	return window->priv->archive_present;
}


void
fr_window_archive_reload (FrWindow *window)
{
	g_return_if_fail (window != NULL);

	if (window->priv->activity_ref > 0)
		return;
	if (window->priv->archive_new)
		return;
	if (window->archive == NULL)
		return;

	fr_window_archive_list (window);
}


/**/


#ifdef ENABLE_NOTIFICATION


static void
notify_action_open_archive_cb (NotifyNotification *notification,
			       char               *action,
			       gpointer            user_data)
{
	GFile     *saved_file = user_data;
	GtkWidget *new_window;

	new_window = fr_window_new ();
	gtk_widget_show (new_window);
	fr_window_archive_open (FR_WINDOW (new_window),
				saved_file,
				GTK_WINDOW (new_window));
}


static void
_fr_window_notify_creation_complete (FrWindow *window)
{
	char               *basename;
	char               *message;
	NotifyNotification *notification;
	gboolean            notification_supports_actions;
	GList              *caps;

	basename = _g_file_get_display_basename (window->priv->saving_file);
	/* Translators: %s is a filename */
	message = g_strdup_printf (_("\"%s\" created successfully"), basename);
	notification = notify_notification_new (window->priv->batch_title, message, "file-roller");
	notify_notification_set_hint_string (notification, "desktop-entry", "file-roller");

	notification_supports_actions = FALSE;
	caps = notify_get_server_caps ();
	if (caps != NULL) {
		notification_supports_actions = g_list_find_custom (caps, "actions", (GCompareFunc) strcmp) != NULL;
		_g_string_list_free (caps);
	}

	if (notification_supports_actions) {
		notify_notification_add_action (notification,
						"document-open-symbolic",
						_("Open"),
						notify_action_open_archive_cb,
						g_object_ref (window->priv->saving_file),
						g_object_unref);
		/*notify_notification_set_hint (notification,
					      "action-icons",
					      g_variant_new_boolean (TRUE));*/
	}

	notify_notification_show (notification, NULL);
	g_free (message);
	g_free (basename);
}


#else


static void
_fr_window_notify_creation_complete (FrWindow *window)
{
	gtk_window_present (GTK_WINDOW (window->priv->progress_dialog));
}


#endif


static void
archive_add_files_ready_cb (GObject      *source_object,
			    GAsyncResult *result,
			    gpointer      user_data)
{
	FrWindow *window = user_data;
	gboolean  notify;
	GError   *error = NULL;

	notify = window->priv->notify;

	fr_archive_operation_finish (FR_ARCHIVE (source_object), result, &error);
	_archive_operation_completed (window, FR_ACTION_ADDING_FILES, error);

	if ((error == NULL) && notify) {
		window->priv->quit_with_progress_dialog = TRUE;
		open_progress_dialog_with_open_archive (window);

		if (! gtk_window_has_toplevel_focus (GTK_WINDOW (window->priv->progress_dialog)))
			_fr_window_notify_creation_complete (window);
	}

	_g_error_free (error);
}


void
fr_window_archive_add_files (FrWindow   *window,
			     GList      *file_list, /* GFile list */
			     GFile      *base_dir,
			     gboolean    update)
{
	_archive_operation_started (window, FR_ACTION_ADDING_FILES);

	fr_archive_add_files (window->archive,
			      file_list,
			      base_dir,
			      fr_window_get_current_location (window),
			      update,
			      FALSE,
			      window->priv->password,
			      window->priv->encrypt_header,
			      window->priv->compression,
			      window->priv->volume_size,
			      window->priv->cancellable,
			      archive_add_files_ready_cb,
			      window);
}


void
fr_window_archive_add_with_filter (FrWindow      *window,
				   GList         *file_list, /* GFile list */
				   GFile         *base_dir,
				   const char    *include_files,
				   const char    *exclude_files,
				   const char    *exclude_folders,
				   const char    *dest_dir,
				   gboolean       update,
				   gboolean       follow_links)
{
	_archive_operation_started (window, FR_ACTION_ADDING_FILES);

	fr_archive_add_files_with_filter (window->archive,
					  file_list,
					  base_dir,
					  include_files,
					  exclude_files,
					  exclude_folders,
					  (dest_dir == NULL)? fr_window_get_current_location (window): dest_dir,
					  update,
					  follow_links,
					  window->priv->password,
					  window->priv->encrypt_header,
					  window->priv->compression,
					  window->priv->volume_size,
					  window->priv->cancellable,
					  archive_add_files_ready_cb,
					  window);
}


void
fr_window_archive_add_dropped_items (FrWindow *window,
				     GList    *file_list)
{
	_archive_operation_started (window, FR_ACTION_ADDING_FILES);

	fr_archive_add_dropped_items (window->archive,
				      file_list,
				      fr_window_get_current_location (window),
				      window->priv->password,
				      window->priv->encrypt_header,
				      window->priv->compression,
				      window->priv->volume_size,
				      window->priv->cancellable,
				      archive_add_files_ready_cb,
				      window);
}


static void
archive_remove_ready_cb (GObject      *source_object,
			 GAsyncResult *result,
			 gpointer      user_data)
{
	FrWindow *window = user_data;
	GError   *error = NULL;

	fr_archive_operation_finish (FR_ARCHIVE (source_object), result, &error);
	_archive_operation_completed (window, FR_ACTION_DELETING_FILES, error);

	_g_error_free (error);
}


void
fr_window_archive_remove (FrWindow *window,
			  GList    *file_list)
{
	_archive_operation_started (window, FR_ACTION_DELETING_FILES);
	fr_window_clipboard_remove_file_list (window, file_list);
	fr_archive_remove (window->archive,
			   file_list,
			   window->priv->compression,
			   window->priv->cancellable,
			   archive_remove_ready_cb,
			   window);
}


/* -- fr_window_archive_extract -- */


static ExtractData*
extract_data_new (FrWindow    *window,
		  GList       *file_list,
		  GFile       *destination,
		  const char  *base_dir,
		  gboolean     skip_older,
		  FrOverwrite  overwrite,
		  gboolean     junk_paths,
		  gboolean     extract_here,
		  gboolean     ask_to_open_destination)
{
	ExtractData *edata;

	edata = g_new0 (ExtractData, 1);
	edata->window = window;
	edata->file_list = _g_string_list_dup (file_list);
	edata->destination = _g_object_ref (destination);
	edata->skip_older = skip_older;
	edata->overwrite = overwrite;
	edata->junk_paths = junk_paths;
	if (base_dir != NULL)
		edata->base_dir = g_strdup (base_dir);
	edata->ask_to_open_destination = ask_to_open_destination;

	return edata;
}


static ExtractData *
extract_to_data_new (FrWindow *window,
		     GFile    *extract_to_dir)
{
	return extract_data_new (window,
				 NULL,
				 extract_to_dir,
				 NULL,
				 FALSE,
				 TRUE,
				 FALSE,
				 FALSE,
				 FALSE);
}


static void
extract_data_free (ExtractData *edata)
{
	g_return_if_fail (edata != NULL);

	_g_string_list_free (edata->file_list);
	_g_object_unref (edata->destination);
	g_free (edata->base_dir);

	g_free (edata);
}


typedef struct {
	FrWindow    *window;
	ExtractData *edata;
	GList       *current_file;
	gboolean     extract_all;
} OverwriteData;


#define _FR_RESPONSE_OVERWRITE_YES_ALL 100
#define _FR_RESPONSE_OVERWRITE_YES     101
#define _FR_RESPONSE_OVERWRITE_NO      102


static void
archive_extraction_ready_cb (GObject      *source_object,
			     GAsyncResult *result,
			     gpointer      user_data)
{
	ExtractData *edata = user_data;
	FrWindow    *window = edata->window;
	gboolean     ask_to_open_destination;
	gboolean     batch_mode;
	GError      *error = NULL;

	ask_to_open_destination = edata->ask_to_open_destination;
	batch_mode = window->priv->batch_mode;

	_g_clear_object (&window->priv->last_extraction_destination);
	window->priv->last_extraction_destination = _g_object_ref (fr_archive_get_last_extraction_destination (window->archive));

	fr_archive_operation_finish (FR_ARCHIVE (source_object), result, &error);
	_archive_operation_completed (window, FR_ACTION_EXTRACTING_FILES, error);

	if ((error == NULL) && ask_to_open_destination) {
		window->priv->quit_with_progress_dialog = window->priv->batch_mode;
		open_progress_dialog_with_open_destination (window);
	}
	else if ((error == NULL) && ! batch_mode && ! gtk_window_has_toplevel_focus (GTK_WINDOW (window->priv->progress_dialog)))
		gtk_window_present (GTK_WINDOW (window));

	_g_error_free (error);
}


static void
_fr_window_archive_extract_from_edata (FrWindow    *window,
				       ExtractData *edata)
{
	GList *scan;
	gsize  total_size;

	total_size = 0;
	for (scan = edata->file_list; scan; scan = scan->next) {
		char     *filename = scan->data;
		FileData *file_data;

		file_data = g_hash_table_lookup (window->archive->files_hash, filename);
		if (file_data == NULL)
			continue;

		total_size += file_data->size;
	}
	fr_archive_progress_set_total_bytes (window->archive, total_size);

	_archive_operation_started (window, FR_ACTION_EXTRACTING_FILES);

	fr_archive_extract (window->archive,
			    edata->file_list,
			    edata->destination,
			    edata->base_dir,
			    edata->skip_older,
			    edata->overwrite == FR_OVERWRITE_YES,
			    edata->junk_paths,
			    window->priv->password,
			    window->priv->cancellable,
			    archive_extraction_ready_cb,
			    edata);
}


static void _fr_window_ask_overwrite_dialog (OverwriteData *odata);


static void
overwrite_dialog_response_cb (GtkDialog *dialog,
			      int        response_id,
			      gpointer   user_data)
{
	OverwriteData *odata = user_data;
	gboolean       do_not_extract = FALSE;

	switch (response_id) {
	case _FR_RESPONSE_OVERWRITE_YES_ALL:
		odata->edata->overwrite = FR_OVERWRITE_YES;
		break;

	case _FR_RESPONSE_OVERWRITE_YES:
		odata->current_file = odata->current_file->next;
		break;

	case _FR_RESPONSE_OVERWRITE_NO:
		{
			/* remove the file from the list to extract */
			GList *next = odata->current_file->next;

			odata->edata->file_list = g_list_remove_link (odata->edata->file_list, odata->current_file);
			_g_string_list_free (odata->current_file);
			odata->current_file = next;
			odata->extract_all = FALSE;
		}
		break;

	case GTK_RESPONSE_DELETE_EVENT:
	case GTK_RESPONSE_CANCEL:
		do_not_extract = TRUE;
		break;

	default:
		break;
	}

	gtk_widget_destroy (GTK_WIDGET (dialog));

	if (do_not_extract) {
		fr_window_stop_batch (odata->window);
		g_free (odata);
		return;
	}

	_fr_window_ask_overwrite_dialog (odata);
}


static void
query_info_ready_for_overwrite_dialog_cb (GObject      *source_object,
					  GAsyncResult *result,
					  gpointer      user_data)
{
	OverwriteData *odata = user_data;
	GFile         *destination = G_FILE (source_object);
	GFileInfo     *info;
	GFileType      file_type;

	info = g_file_query_info_finish (destination, result, NULL);
	if (info == NULL) {
		odata->current_file = odata->current_file->next;
		_fr_window_ask_overwrite_dialog (odata);
		g_object_unref (destination);
		return;
	}

	file_type = g_file_info_get_file_type (info);
	if ((file_type != G_FILE_TYPE_UNKNOWN) && (file_type != G_FILE_TYPE_DIRECTORY)) {
		char      *msg;
		GFile     *parent;
		char      *parent_name;
		char      *details;
		GtkWidget *d;

		msg = g_strdup_printf (_("Replace file \"%s\"?"), g_file_info_get_display_name (info));
		parent = g_file_get_parent (destination);
		parent_name = g_file_get_parse_name (parent);
		details = g_strdup_printf (_("Another file with the same name already exists in \"%s\"."), parent_name);
		d = _gtk_message_dialog_new (GTK_WINDOW (odata->window),
					     GTK_DIALOG_MODAL,
					     GTK_STOCK_DIALOG_QUESTION,
					     msg,
					     details,
					     GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
					     _("Replace _All"), _FR_RESPONSE_OVERWRITE_YES_ALL,
					     _("_Skip"), _FR_RESPONSE_OVERWRITE_NO,
					     _("_Replace"), _FR_RESPONSE_OVERWRITE_YES,
					     NULL);
		gtk_dialog_set_default_response (GTK_DIALOG (d), _FR_RESPONSE_OVERWRITE_YES);
		g_signal_connect (d,
				  "response",
				  G_CALLBACK (overwrite_dialog_response_cb),
				  odata);
		gtk_widget_show (d);

		g_free (parent_name);
		g_object_unref (parent);
		g_object_unref (info);
		g_object_unref (destination);

		return;
	}

	g_object_unref (info);
	g_object_unref (destination);

	odata->current_file = odata->current_file->next;
	_fr_window_ask_overwrite_dialog (odata);
}


static void
_fr_window_ask_overwrite_dialog (OverwriteData *odata)
{
	gboolean perform_extraction = TRUE;

	if ((odata->edata->overwrite == FR_OVERWRITE_ASK) && (odata->current_file != NULL)) {
		const char *base_name;
		GFile      *destination;

		base_name = _g_path_get_relative_basename_safe ((char *) odata->current_file->data, odata->edata->base_dir, odata->edata->junk_paths);
		if (base_name != NULL) {
			destination = g_file_get_child (odata->edata->destination, base_name);
			g_file_query_info_async (destination,
						 G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME,
						 G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
						 G_PRIORITY_DEFAULT,
						 odata->window->priv->cancellable,
						 query_info_ready_for_overwrite_dialog_cb,
						 odata);


			return;
		}
		else
			perform_extraction = FALSE;
	}

	if (odata->edata->file_list == NULL)
		perform_extraction = FALSE;

	if (perform_extraction) {
		/* speed optimization: passing NULL when extracting all the
		 * files is faster if the command supports the
		 * propCanExtractAll property. */
		if (odata->extract_all) {
			_g_string_list_free (odata->edata->file_list);
			odata->edata->file_list = NULL;
		}
		odata->edata->overwrite = FR_OVERWRITE_YES;
		_fr_window_archive_extract_from_edata (odata->window, odata->edata);
	}
	else {
		GtkWidget *d;

		d = _gtk_message_dialog_new (GTK_WINDOW (odata->window),
					     0,
					     GTK_STOCK_DIALOG_WARNING,
					     _("Extraction not performed"),
					     NULL,
					     GTK_STOCK_OK, GTK_RESPONSE_OK,
					     NULL);
		gtk_dialog_set_default_response (GTK_DIALOG (d), GTK_RESPONSE_OK);
		fr_window_show_error_dialog (odata->window, d, GTK_WINDOW (odata->window), _("Extraction not performed"));

		fr_window_stop_batch (odata->window);
	}

	g_free (odata);
}


static gboolean
archive_is_encrypted (FrWindow *window,
		      GList    *file_list)
{
	gboolean encrypted = FALSE;

	if (file_list == NULL) {
		int i;

		for (i = 0; ! encrypted && i < window->archive->files->len; i++) {
			FileData *fdata = g_ptr_array_index (window->archive->files, i);

			if (fdata->encrypted)
				encrypted = TRUE;
		}
	}
	else {
		GList *scan;

		for (scan = file_list; ! encrypted && scan; scan = scan->next) {
			char     *filename = scan->data;
			FileData *fdata;

			fdata = g_hash_table_lookup (window->archive->files_hash, filename);
			g_return_val_if_fail (fdata != NULL, FALSE);

			if (fdata->encrypted)
				encrypted = TRUE;
		}
	}

	return encrypted;
}


void
fr_window_archive_extract (FrWindow    *window,
			   GList       *file_list,
			   GFile       *destination,
			   const char  *base_dir,
			   gboolean     skip_older,
			   FrOverwrite  overwrite,
			   gboolean     junk_paths,
			   gboolean     ask_to_open_destination)
{
	ExtractData *edata;
	gboolean     do_not_extract = FALSE;
	GError      *error = NULL;

	edata = extract_data_new (window,
				  file_list,
				  destination,
				  base_dir,
				  skip_older,
				  overwrite,
				  junk_paths,
				  FALSE,
				  ask_to_open_destination);

	fr_window_set_current_batch_action (window,
					    FR_BATCH_ACTION_EXTRACT,
					    edata,
					    (GFreeFunc) extract_data_free);

	if (archive_is_encrypted (window, edata->file_list) && (window->priv->password == NULL)) {
		dlg_ask_password (window);
		return;
	}

	if (! _g_file_query_is_dir (edata->destination)) {

		/* There is nothing to ask if the destination doesn't exist. */
		if (edata->overwrite == FR_OVERWRITE_ASK)
			edata->overwrite = FR_OVERWRITE_YES;

		if (! ForceDirectoryCreation) {
			GtkWidget *d;
			int        r;
			char      *folder_name;
			char      *msg;

			folder_name = _g_file_get_display_basename (edata->destination);
			msg = g_strdup_printf (_("Destination folder \"%s\" does not exist.\n\nDo you want to create it?"), folder_name);
			g_free (folder_name);

			d = _gtk_message_dialog_new (GTK_WINDOW (window),
						     GTK_DIALOG_MODAL,
						     GTK_STOCK_DIALOG_QUESTION,
						     msg,
						     NULL,
						     GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
						     _("Create _Folder"), GTK_RESPONSE_YES,
						     NULL);

			gtk_dialog_set_default_response (GTK_DIALOG (d), GTK_RESPONSE_YES);
			r = gtk_dialog_run (GTK_DIALOG (d));
			gtk_widget_destroy (GTK_WIDGET (d));

			g_free (msg);

			if (r != GTK_RESPONSE_YES)
				do_not_extract = TRUE;
		}

		if (! do_not_extract && ! _g_file_make_directory_tree (edata->destination, 0755, &error)) {
			GtkWidget *d;
			char      *details;

			details = g_strdup_printf (_("Could not create the destination folder: %s."), error->message);
			d = _gtk_error_dialog_new (GTK_WINDOW (window),
						   0,
						   NULL,
						   _("Extraction not performed"),
						   "%s",
						   details);
			g_clear_error (&error);
			fr_window_show_error_dialog (window, d, GTK_WINDOW (window), details);
			fr_window_stop_batch (window);

			g_free (details);

			return;
		}
	}

	if (do_not_extract) {
		GtkWidget *d;

		d = _gtk_message_dialog_new (GTK_WINDOW (window),
					     0,
					     GTK_STOCK_DIALOG_WARNING,
					     _("Extraction not performed"),
					     NULL,
					     GTK_STOCK_OK, GTK_RESPONSE_OK,
					     NULL);
		gtk_dialog_set_default_response (GTK_DIALOG (d), GTK_RESPONSE_OK);
		fr_window_show_error_dialog (window, d, GTK_WINDOW (window), _("Extraction not performed"));
		fr_window_stop_batch (window);

		return;
	}

	if (edata->overwrite == FR_OVERWRITE_ASK) {
		OverwriteData *odata;

		odata = g_new0 (OverwriteData, 1);
		odata->window = window;
		odata->edata = edata;
		odata->extract_all = (edata->file_list == NULL) || (g_list_length (edata->file_list) == window->archive->files->len);
		if (edata->file_list == NULL)
			edata->file_list = fr_window_get_file_list (window);
		odata->current_file = odata->edata->file_list;

		_fr_window_ask_overwrite_dialog (odata);
	}
	else
		_fr_window_archive_extract_from_edata (window, edata);
}


/* -- fr_window_archive_extract_here -- */


void
fr_window_archive_extract_here (FrWindow   *window,
				gboolean    skip_older,
				gboolean    overwrite,
				gboolean    junk_paths,
				gboolean    ask_to_open_destination)
{
	ExtractData *edata;

	edata = extract_data_new (window,
				  NULL,
				  NULL,
				  NULL,
				  skip_older,
				  overwrite,
				  junk_paths,
				  TRUE,
				  ask_to_open_destination);
	fr_window_set_current_batch_action (window,
					    FR_BATCH_ACTION_EXTRACT,
					    edata,
					    (GFreeFunc) extract_data_free);

	if (archive_is_encrypted (window, NULL) && (window->priv->password == NULL)) {
		dlg_ask_password (window);
		return;
	}

	_archive_operation_started (window, FR_ACTION_EXTRACTING_FILES);

	fr_archive_extract_here (window->archive,
				 edata->skip_older,
				 edata->overwrite,
				 edata->junk_paths,
				 window->priv->password,
				 window->priv->cancellable,
				 archive_extraction_ready_cb,
				 edata);
}


/* -- fr_window_archive_test -- */


static void
archive_test_ready_cb (GObject      *source_object,
		       GAsyncResult *result,
		       gpointer      user_data)
{
	FrWindow *window = user_data;
	GError   *error = NULL;

	fr_archive_operation_finish (FR_ARCHIVE (source_object), result, &error);
	_archive_operation_completed (window, FR_ACTION_TESTING_ARCHIVE, error);

	_g_error_free (error);
}


void
fr_window_archive_test (FrWindow *window)
{
	_archive_operation_started (window, FR_ACTION_TESTING_ARCHIVE);
	fr_window_set_current_batch_action (window,
					    FR_BATCH_ACTION_TEST,
					    NULL,
					    NULL);
	fr_archive_test (window->archive,
			 window->priv->password,
			 window->priv->cancellable,
			 archive_test_ready_cb,
			 window);
}


void
fr_window_set_password (FrWindow   *window,
			const char *password)
{
	g_return_if_fail (window != NULL);

	if (window->priv->password == password)
		return;

	if (window->priv->password != NULL) {
		g_free (window->priv->password);
		window->priv->password = NULL;
	}

	if ((password != NULL) && (password[0] != '\0'))
		window->priv->password = g_strdup (password);
}


void
fr_window_set_password_for_second_archive (FrWindow   *window,
					   const char *password)
{
	g_return_if_fail (window != NULL);

	if (window->priv->second_password != NULL) {
		g_free (window->priv->second_password);
		window->priv->second_password = NULL;
	}

	if ((password != NULL) && (password[0] != '\0'))
		window->priv->second_password = g_strdup (password);
}


const char *
fr_window_get_password (FrWindow *window)
{
	g_return_val_if_fail (window != NULL, NULL);

	return window->priv->password;
}


const char *
fr_window_get_password_for_second_archive (FrWindow *window)
{
	g_return_val_if_fail (window != NULL, NULL);

	return window->priv->second_password;
}


void
fr_window_set_encrypt_header (FrWindow *window,
			      gboolean  encrypt_header)
{
	g_return_if_fail (window != NULL);

	window->priv->encrypt_header = encrypt_header;
}


gboolean
fr_window_get_encrypt_header (FrWindow *window)
{
	return window->priv->encrypt_header;
}


void
fr_window_set_compression (FrWindow      *window,
			   FrCompression  compression)
{
	g_return_if_fail (window != NULL);

	window->priv->compression = compression;
}


FrCompression
fr_window_get_compression (FrWindow *window)
{
	return window->priv->compression;
}


void
fr_window_set_volume_size (FrWindow *window,
			   guint     volume_size)
{
	g_return_if_fail (window != NULL);

	window->priv->volume_size = volume_size;
}


guint
fr_window_get_volume_size (FrWindow *window)
{
	return window->priv->volume_size;
}


void
fr_window_go_to_location (FrWindow   *window,
			  const char *path,
			  gboolean    force_update)
{
	char *dir;

	g_return_if_fail (window != NULL);
	g_return_if_fail (path != NULL);

	if (force_update) {
		g_free (window->priv->last_location);
		window->priv->last_location = NULL;
	}

	if (path[strlen (path) - 1] != '/')
		dir = g_strconcat (path, "/", NULL);
	else
		dir = g_strdup (path);

	if ((window->priv->last_location == NULL) || (strcmp (window->priv->last_location, dir) != 0)) {
		g_free (window->priv->last_location);
		window->priv->last_location = dir;

		fr_window_history_add (window, dir);
		fr_window_update_file_list (window, TRUE);
		fr_window_update_current_location (window);
	}
	else
		g_free (dir);
}


const char *
fr_window_get_current_location (FrWindow *window)
{
	if (window->priv->history_current == NULL) {
		fr_window_history_add (window, "/");
		return window->priv->history_current->data;
	}
	else
		return (const char*) window->priv->history_current->data;
}


void
fr_window_go_up_one_level (FrWindow *window)
{
	char *parent_dir;

	g_return_if_fail (window != NULL);

	parent_dir = get_parent_dir (fr_window_get_current_location (window));
	fr_window_go_to_location (window, parent_dir, FALSE);
	g_free (parent_dir);
}


void
fr_window_go_back (FrWindow *window)
{
	g_return_if_fail (window != NULL);

	if (window->priv->history == NULL)
		return;
	if (window->priv->history_current == NULL)
		return;
	if (window->priv->history_current->next == NULL)
		return;
	window->priv->history_current = window->priv->history_current->next;

	fr_window_go_to_location (window, window->priv->history_current->data, FALSE);
}


void
fr_window_go_forward (FrWindow *window)
{
	g_return_if_fail (window != NULL);

	if (window->priv->history == NULL)
		return;
	if (window->priv->history_current == NULL)
		return;
	if (window->priv->history_current->prev == NULL)
		return;
	window->priv->history_current = window->priv->history_current->prev;

	fr_window_go_to_location (window, window->priv->history_current->data, FALSE);
}


void
fr_window_set_list_mode (FrWindow         *window,
			 FrWindowListMode  list_mode)
{
	g_return_if_fail (window != NULL);

	if (list_mode == window->priv->list_mode)
		return;

	window->priv->list_mode = window->priv->last_list_mode = list_mode;
	if (window->priv->list_mode == FR_WINDOW_LIST_MODE_FLAT) {
		fr_window_history_clear (window);
		fr_window_history_add (window, "/");
	}

	g_settings_set_enum (window->priv->settings_listing, PREF_LISTING_LIST_MODE, window->priv->last_list_mode);
	g_settings_set_boolean (window->priv->settings_listing, PREF_LISTING_SHOW_PATH, (window->priv->list_mode == FR_WINDOW_LIST_MODE_FLAT));

	fr_window_update_file_list (window, TRUE);
	fr_window_update_dir_tree (window);
	fr_window_update_current_location (window);
}


GtkTreeModel *
fr_window_get_list_store (FrWindow *window)
{
	return GTK_TREE_MODEL (window->priv->list_store);
}


void
fr_window_find (FrWindow *window,
		gboolean  active)
{
	if (active) {
		window->priv->filter_mode = TRUE;
		gtk_widget_show (window->priv->filter_bar);
		gtk_widget_hide (window->priv->location_bar);
		gtk_widget_grab_focus (window->priv->filter_entry);
	}
	else {
		window->priv->filter_mode = FALSE;
		window->priv->list_mode = window->priv->last_list_mode;

		gtk_entry_set_text (GTK_ENTRY (window->priv->filter_entry), "");
		gtk_widget_hide (window->priv->filter_bar);

		gtk_list_store_clear (window->priv->list_store);

		fr_window_update_columns_visibility (window);
		fr_window_update_file_list (window, TRUE);
		fr_window_update_dir_tree (window);
		fr_window_update_current_location (window);
		fr_window_go_to_location (window, gtk_entry_get_text (GTK_ENTRY (window->priv->location_entry)), FALSE);
	}
}


void
fr_window_select_all (FrWindow *window)
{
	gtk_tree_selection_select_all (gtk_tree_view_get_selection (GTK_TREE_VIEW (window->priv->list_view)));
}


void
fr_window_unselect_all (FrWindow *window)
{
	gtk_tree_selection_unselect_all (gtk_tree_view_get_selection (GTK_TREE_VIEW (window->priv->list_view)));
}


void
fr_window_stop (FrWindow *window)
{
	if (window->priv->stoppable && (window->priv->activity_ref > 0))
		g_cancellable_cancel (window->priv->cancellable);
}


void
fr_window_action_new_archive (FrWindow *window)
{
	GtkWidget *dialog;

	if (fr_window_present_dialog_if_created (window, "new_archive"))
		return;

	dialog = fr_new_archive_dialog_new (_("New Archive"),
					    GTK_WINDOW (window),
					    FR_NEW_ARCHIVE_ACTION_NEW_MANY_FILES,
					    fr_window_get_open_default_dir (window),
					    NULL,
					    NULL);
	if ((fr_window_archive_is_present (window) && ! fr_window_is_batch_mode (window) ? NULL : GTK_WINDOW (window)))
		gtk_window_set_modal (GTK_WINDOW (dialog), TRUE);
	g_signal_connect (G_OBJECT (dialog),
			  "response",
			  G_CALLBACK (new_archive_dialog_response_cb),
			  window);
	fr_window_set_dialog (window, "new_archive", dialog);
	gtk_window_present (GTK_WINDOW (dialog));
}


/* -- fr_window_action_save_as -- */


typedef struct {
	FrWindow  *window;
	FrArchive *new_archive;
	GFile     *file;
	char      *mime_type;
	char      *password;
	gboolean   encrypt_header;
	guint      volume_size;
	GFile     *temp_extraction_dir;
} ConvertData;


static ConvertData *
convert_data_new (GFile      *file,
		  const char *mime_type,
		  const char *password,
		  gboolean    encrypt_header,
	  	  guint       volume_size)
{
	ConvertData *cdata;

	cdata = g_new0 (ConvertData, 1);
	cdata->file = _g_object_ref (file);
	if (mime_type != NULL)
		cdata->mime_type = g_strdup (mime_type);
	if (password != NULL)
		cdata->password = g_strdup (password);
	cdata->encrypt_header = encrypt_header;
	cdata->volume_size = volume_size;
	cdata->temp_extraction_dir = _g_file_get_temp_work_dir (NULL);

	return cdata;
}


static void
convert_data_free (ConvertData *cdata)
{
	if (cdata == NULL)
		return;
	_g_file_remove_directory (cdata->temp_extraction_dir, NULL, NULL);
	_g_object_unref (cdata->temp_extraction_dir);
	_g_object_unref (cdata->file);
	_g_object_unref (cdata->new_archive);
	g_free (cdata->mime_type);
	g_free (cdata->password);
	g_free (cdata);
}


static void
archive_add_ready_for_conversion_cb (GObject      *source_object,
				     GAsyncResult *result,
				     gpointer      user_data)
{
	ConvertData *cdata = user_data;
	FrWindow    *window = cdata->window;
	GError      *error = NULL;

	fr_archive_operation_finish (FR_ARCHIVE (source_object), result, &error);

	_fr_window_stop_activity_mode (window);
	close_progress_dialog (window, FALSE);

	if (error != NULL) {
		_handle_archive_operation_error (window,
						 cdata->new_archive,
						 FR_ACTION_ADDING_FILES,
						 error,
						 NULL,
						 NULL);
		fr_window_stop_batch (window);
		g_error_free (error);
		return;
	}

	open_progress_dialog_with_open_archive (window);
	fr_window_exec_next_batch_action (window);
}


static void
_convertion_completed_with_error (FrWindow *window,
				  FrAction  action,
				  GError   *error)
{
	gboolean opens_dialog;

	g_return_if_fail (error != NULL);

#ifdef DEBUG
	debug (DEBUG_INFO, "%s [DONE] (FR::Window)\n", action_names[action]);
#endif

	_fr_window_stop_activity_mode (window);
	close_progress_dialog (window, FALSE);

	_handle_archive_operation_error (window, window->archive, action, error, NULL, &opens_dialog);
	if (opens_dialog)
		return;

	_g_clear_object (&window->priv->saving_file);
	fr_window_stop_batch (window);
}


static void
archive_extraction_ready_for_convertion_cb (GObject      *source_object,
					    GAsyncResult *result,
					    gpointer      user_data)
{
	ConvertData *cdata = user_data;
	FrWindow    *window = cdata->window;
	GList       *list;
	GError      *error = NULL;

	if (! fr_archive_operation_finish (FR_ARCHIVE (source_object), result, &error)) {
		_convertion_completed_with_error (window, FR_ACTION_EXTRACTING_FILES, error);
		return;
	}

	list = g_list_prepend (NULL, cdata->temp_extraction_dir);
	fr_archive_add_files (cdata->new_archive,
			      list,
			      cdata->temp_extraction_dir,
			      NULL,
			      FALSE,
			      FALSE,
			      cdata->password,
			      cdata->encrypt_header,
			      window->priv->compression,
			      cdata->volume_size,
			      window->priv->cancellable,
			      archive_add_ready_for_conversion_cb,
			      cdata);

	g_list_free (list);
}


static void
fr_window_archive_save_as (FrWindow   *window,
			   GFile      *file,
			   const char *mime_type,
			   const char *password,
			   gboolean    encrypt_header,
			   guint       volume_size)
{
	FrArchive   *new_archive;
	ConvertData *cdata;

	g_return_if_fail (window != NULL);
	g_return_if_fail (file != NULL);
	g_return_if_fail (window->archive != NULL);

	/* create the new archive */

	new_archive = fr_archive_create (file, mime_type);
	if (new_archive == NULL) {
		GtkWidget *d;
		char      *utf8_name;
		char      *message;

		utf8_name = _g_file_get_display_basename (file);
		message = g_strdup_printf (_("Could not save the archive \"%s\""), utf8_name);
		g_free (utf8_name);

		d = _gtk_error_dialog_new (GTK_WINDOW (window),
					   GTK_DIALOG_DESTROY_WITH_PARENT,
					   NULL,
					   message,
					   "%s",
					   _("Archive type not supported."));
		gtk_dialog_run (GTK_DIALOG (d));
		gtk_widget_destroy (d);

		g_free (message);

		return;
	}

	cdata = convert_data_new (file, mime_type, password, encrypt_header, volume_size);
	cdata->window = window;
	cdata->new_archive = new_archive;

	_archive_operation_started (window, FR_ACTION_CREATING_ARCHIVE);
	fr_window_set_current_batch_action (window,
					    FR_BATCH_ACTION_SAVE_AS,
					    cdata,
					    (GFreeFunc) convert_data_free);

	g_signal_connect (cdata->new_archive,
			  "progress",
			  G_CALLBACK (fr_archive_progress_cb),
			  window);
	g_signal_connect (cdata->new_archive,
			  "message",
			  G_CALLBACK (fr_archive_message_cb),
			  window);
	g_signal_connect (cdata->new_archive,
			  "start",
			  G_CALLBACK (fr_archive_start_cb),
			  window);
	g_signal_connect (cdata->new_archive,
			  "stoppable",
			  G_CALLBACK (fr_archive_stoppable_cb),
			  window);
	g_signal_connect (cdata->new_archive,
			  "working-archive",
			  G_CALLBACK (fr_window_working_archive_cb),
			  window);

	_g_object_unref (window->priv->saving_file);
	window->priv->saving_file = g_object_ref (cdata->file);

	fr_archive_action_started (window->archive, FR_ACTION_EXTRACTING_FILES);
	fr_archive_extract (window->archive,
			    NULL,
			    cdata->temp_extraction_dir,
			    NULL,
			    FALSE,
			    TRUE,
			    FALSE,
			    window->priv->password,
			    window->priv->cancellable,
			    archive_extraction_ready_for_convertion_cb,
			    cdata);
}


static void
save_as_archive_dialog_response_cb (GtkDialog *dialog,
				    int        response,
				    gpointer   user_data)
{
	FrWindow   *window = user_data;
	GFile      *file;
	const char *mime_type;
	const char *password;
	gboolean    encrypt_header;
	int         volume_size;
	GSettings  *settings;

	if ((response == GTK_RESPONSE_CANCEL) || (response == GTK_RESPONSE_DELETE_EVENT)) {
		gtk_widget_destroy (GTK_WIDGET (dialog));
		_archive_operation_cancelled (window, FR_ACTION_CREATING_ARCHIVE);
		return;
	}

	if (response != GTK_RESPONSE_OK)
		return;

	file = fr_new_archive_dialog_get_file (FR_NEW_ARCHIVE_DIALOG (dialog), &mime_type);
	if (file == NULL)
		return;

	password = fr_new_archive_dialog_get_password (FR_NEW_ARCHIVE_DIALOG (dialog));
	encrypt_header = fr_new_archive_dialog_get_encrypt_header (FR_NEW_ARCHIVE_DIALOG (dialog));
	volume_size = fr_new_archive_dialog_get_volume_size (FR_NEW_ARCHIVE_DIALOG (dialog));

	settings = g_settings_new (FILE_ROLLER_SCHEMA_NEW);
	g_settings_set_int (settings, PREF_NEW_VOLUME_SIZE, volume_size);
	g_object_unref (settings);

	fr_window_archive_save_as (window, file, mime_type, password, encrypt_header, volume_size);

	gtk_widget_destroy (GTK_WIDGET (dialog));
	g_object_unref (file);
}


void
fr_window_action_save_as (FrWindow *window)
{
	char      *archive_name;
	GtkWidget *dialog;

	archive_name = NULL;
	if (window->priv->archive_file != NULL) {
		GFileInfo *info;

		info = g_file_query_info (window->priv->archive_file,
					  G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME,
					  0, NULL, NULL);

		if (info != NULL) {
			archive_name = g_strdup (g_file_info_get_display_name (info));
			g_object_unref (info);
		}
	}

	dialog = fr_new_archive_dialog_new (_("Save"),
					    GTK_WINDOW (window),
					    FR_NEW_ARCHIVE_ACTION_SAVE_AS,
					    fr_window_get_open_default_dir (window),
					    archive_name,
					    window->priv->archive_file);
	gtk_window_set_modal (GTK_WINDOW (dialog), TRUE);
	g_signal_connect (G_OBJECT (dialog),
			  "response",
			  G_CALLBACK (save_as_archive_dialog_response_cb),
			  window);
	gtk_window_present (GTK_WINDOW (dialog));

	g_free (archive_name);
}


/* -- fr_window_archive_encrypt -- */


typedef struct {
	FrWindow  *window;
	char      *password;
	gboolean   encrypt_header;
	GFile     *temp_extraction_dir;
	GFile     *temp_new_file;
	FrArchive *new_archive;
} EncryptData;


static EncryptData *
encrypt_data_new (FrWindow   *window,
		  const char *password,
		  gboolean    encrypt_header)
{
	EncryptData *edata;

	edata = g_new0 (EncryptData, 1);
	edata->window = window;
	if (password != NULL)
		edata->password = g_strdup (password);
	edata->encrypt_header = encrypt_header;
	edata->temp_extraction_dir = _g_file_get_temp_work_dir (NULL);

	return edata;
}


static void
encrypt_data_free (EncryptData *edata)
{
	if (edata == NULL)
		return;

	if (edata->temp_new_file != NULL) {
		GFile *parent = g_file_get_parent (edata->temp_new_file);
		if (parent != NULL)
			_g_file_remove_directory (parent, NULL, NULL);
		_g_object_unref (parent);
	}
	_g_object_unref (edata->temp_new_file);
	_g_object_unref (edata->new_archive);
	_g_file_remove_directory (edata->temp_extraction_dir, NULL, NULL);
	_g_object_unref (edata->temp_extraction_dir);
	g_free (edata->password);
	g_free (edata);
}


static void
_encrypt_operation_completed_with_error (FrWindow *window,
					 FrAction  action,
					 GError   *error)
{
	gboolean opens_dialog;

	g_return_if_fail (error != NULL);

#ifdef DEBUG
	debug (DEBUG_INFO, "%s [DONE] (FR::Window)\n", action_names[action]);
#endif

	_fr_window_stop_activity_mode (window);
	_handle_archive_operation_error (window, window->archive, action, error, NULL, &opens_dialog);
	if (opens_dialog)
		return;

	close_progress_dialog (window, FALSE);
	fr_window_stop_batch (window);
}


static void
ecryption_copy_ready_cb (GObject      *source_object,
			 GAsyncResult *result,
			 gpointer      user_data)
{
	EncryptData *edata = user_data;
	FrWindow    *window = edata->window;
	GError      *error = NULL;

	_fr_window_stop_activity_mode (window);
	close_progress_dialog (window, FALSE);

	if (! g_file_copy_finish (G_FILE (source_object), result, &error)) {
		_handle_archive_operation_error (window,
						 edata->new_archive,
						 FR_ACTION_CREATING_NEW_ARCHIVE,
						 error,
						 NULL,
						 NULL);
		fr_window_stop_batch (window);

		g_error_free (error);
		return;
	}

	fr_window_set_password (window, edata->password);
	fr_window_set_encrypt_header (window, edata->encrypt_header);
	window->priv->reload_archive = TRUE;
	fr_window_exec_next_batch_action (window);
}


static void
encryption_copy_progress_cb (goffset  current_num_bytes,
			     goffset  total_num_bytes,
			     gpointer user_data)
{
	EncryptData *edata = user_data;

	fr_archive_progress (edata->new_archive, (double) current_num_bytes / total_num_bytes);
}


static void
archive_add_ready_for_encryption_cb (GObject      *source_object,
				     GAsyncResult *result,
				     gpointer      user_data)
{
	EncryptData *edata = user_data;
	FrWindow    *window = edata->window;
	GError      *error = NULL;

	if (! fr_archive_operation_finish (FR_ARCHIVE (source_object), result, &error)) {
		_encrypt_operation_completed_with_error (window, FR_ACTION_ENCRYPTING_ARCHIVE, error);
		return;
	}

	fr_archive_action_started (window->archive, FR_ACTION_SAVING_REMOTE_ARCHIVE);
	g_file_copy_async (edata->temp_new_file,
			   fr_archive_get_file (window->archive),
			   G_FILE_COPY_OVERWRITE,
			   G_PRIORITY_DEFAULT,
			   window->priv->cancellable,
			   encryption_copy_progress_cb,
			   edata,
			   ecryption_copy_ready_cb,
			   edata);
}


static void
archive_extraction_ready_for_encryption_cb (GObject      *source_object,
					    GAsyncResult *result,
					    gpointer      user_data)
{
	EncryptData *edata = user_data;
	FrWindow    *window = edata->window;
	GList       *list;
	GError      *error = NULL;

	if (! fr_archive_operation_finish (FR_ARCHIVE (source_object), result, &error)) {
		_encrypt_operation_completed_with_error (window, FR_ACTION_ENCRYPTING_ARCHIVE, error);
		return;
	}

	fr_archive_action_started (window->archive, FR_ACTION_ENCRYPTING_ARCHIVE);

	list = g_list_prepend (NULL, edata->temp_extraction_dir);
	fr_archive_add_files (edata->new_archive,
			      list,
			      edata->temp_extraction_dir,
			      NULL,
			      FALSE,
			      FALSE,
			      edata->password,
			      edata->encrypt_header,
			      window->priv->compression,
			      0,
			      window->priv->cancellable,
			      archive_add_ready_for_encryption_cb,
			      edata);

	g_list_free (list);
}


void
fr_window_archive_encrypt (FrWindow   *window,
			   const char *password,
			   gboolean    encrypt_header)
{
	EncryptData *edata;
	GFile       *temp_destination_parent;
	GFile       *temp_destination;
	char        *basename;
	GFile       *temp_new_file;
	FrArchive   *new_archive;

	/* create the new archive */

	if (g_file_is_native (fr_archive_get_file (window->archive)))
		temp_destination_parent = g_file_get_parent (fr_archive_get_file (window->archive));
	else
		temp_destination_parent = NULL;
	temp_destination = _g_file_get_temp_work_dir (temp_destination_parent);
	basename = g_file_get_basename (fr_archive_get_file (window->archive));
	temp_new_file = g_file_get_child (temp_destination, basename);

	g_free (basename);
	_g_object_unref (temp_destination_parent);

	new_archive = fr_archive_create (temp_new_file, fr_archive_get_mime_type (window->archive));
	if (new_archive == NULL) {
		GtkWidget *d;
		char      *utf8_name;
		char      *message;

		utf8_name = _g_file_get_display_basename (temp_new_file);
		message = g_strdup_printf (_("Could not save the archive \"%s\""), utf8_name);
		g_free (utf8_name);

		d = _gtk_error_dialog_new (GTK_WINDOW (window),
					   GTK_DIALOG_DESTROY_WITH_PARENT,
					   NULL,
					   message,
					   "%s",
					   _("Archive type not supported."));
		gtk_dialog_run (GTK_DIALOG (d));
		gtk_widget_destroy (d);

		g_free (message);
		g_object_unref (temp_new_file);

		_g_file_remove_directory (temp_destination, NULL, NULL);
		g_object_unref (temp_destination);

		return;
	}

	g_object_unref (temp_destination);

	edata = encrypt_data_new (window, password, encrypt_header);
	edata->temp_new_file = temp_new_file;
	edata->new_archive = new_archive;

	g_signal_connect (edata->new_archive,
			  "progress",
			  G_CALLBACK (fr_archive_progress_cb),
			  window);
	g_signal_connect (edata->new_archive,
			  "message",
			  G_CALLBACK (fr_archive_message_cb),
			  window);
	g_signal_connect (edata->new_archive,
			  "start",
			  G_CALLBACK (fr_archive_start_cb),
			  window);
	g_signal_connect (edata->new_archive,
			  "stoppable",
			  G_CALLBACK (fr_archive_stoppable_cb),
			  window);
	g_signal_connect (edata->new_archive,
			  "working-archive",
			  G_CALLBACK (fr_window_working_archive_cb),
			  window);

	_archive_operation_started (window, FR_ACTION_ENCRYPTING_ARCHIVE);
	fr_window_set_current_batch_action (window,
					    FR_BATCH_ACTION_ENCRYPT,
					    edata,
					    (GFreeFunc) encrypt_data_free);

	fr_archive_action_started (window->archive, FR_ACTION_EXTRACTING_FILES);
	fr_archive_extract (window->archive,
			    NULL,
			    edata->temp_extraction_dir,
			    NULL,
			    FALSE,
			    TRUE,
			    FALSE,
			    window->priv->password,
			    window->priv->cancellable,
			    archive_extraction_ready_for_encryption_cb,
			    edata);
}


/* -- fr_window_view_last_output  -- */


static gboolean
last_output_window__unrealize_cb (GtkWidget  *widget,
				  gpointer    data)
{
	pref_util_save_window_geometry (GTK_WINDOW (widget), LAST_OUTPUT_SCHEMA_NAME);
	return FALSE;
}


void
fr_window_view_last_output (FrWindow   *window,
			    const char *title)
{
	GtkWidget     *dialog;
	GtkWidget     *vbox;
	GtkWidget     *text_view;
	GtkWidget     *scrolled;
	GtkTextBuffer *text_buffer;
	GtkTextIter    iter;
	GList         *scan;

	if (title == NULL)
		title = _("Last Output");

	dialog = gtk_dialog_new_with_buttons (title,
					      GTK_WINDOW (window),
					      GTK_DIALOG_DESTROY_WITH_PARENT,
					      GTK_STOCK_CLOSE, GTK_RESPONSE_CLOSE,
					      NULL);
	gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_CLOSE);
	gtk_window_set_resizable (GTK_WINDOW (dialog), TRUE);
	gtk_widget_set_size_request (dialog, 500, 300);

	/* Add text */

	scrolled = gtk_scrolled_window_new (NULL, NULL);
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled),
					GTK_POLICY_AUTOMATIC,
					GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (scrolled),
					     GTK_SHADOW_ETCHED_IN);

	text_buffer = gtk_text_buffer_new (NULL);
	gtk_text_buffer_create_tag (text_buffer, "monospace",
				    "family", "monospace", NULL);

	text_view = gtk_text_view_new_with_buffer (text_buffer);
	g_object_unref (text_buffer);
	gtk_text_view_set_editable (GTK_TEXT_VIEW (text_view), FALSE);
	gtk_text_view_set_cursor_visible (GTK_TEXT_VIEW (text_view), FALSE);

	/**/

	vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
	gtk_container_set_border_width (GTK_CONTAINER (vbox), 5);

	gtk_container_add (GTK_CONTAINER (scrolled), text_view);
	gtk_box_pack_start (GTK_BOX (vbox), scrolled,
			    TRUE, TRUE, 0);

	gtk_widget_show_all (vbox);
	gtk_box_pack_start (GTK_BOX (gtk_dialog_get_content_area (GTK_DIALOG (dialog))),
			    vbox,
			    TRUE, TRUE, 0);

	/* signals */

	g_signal_connect (G_OBJECT (dialog),
			  "response",
			  G_CALLBACK (gtk_widget_destroy),
			  NULL);

	g_signal_connect (G_OBJECT (dialog),
			  "unrealize",
			  G_CALLBACK (last_output_window__unrealize_cb),
			  NULL);

	/**/

	gtk_text_buffer_get_iter_at_offset (text_buffer, &iter, 0);
	if (FR_IS_COMMAND (window->archive))
		scan = fr_command_get_last_output (FR_COMMAND (window->archive));
	else
		scan = NULL;
	for (; scan; scan = scan->next) {
		char        *line = scan->data;
		char        *utf8_line;
		gsize        bytes_written;

		utf8_line = g_locale_to_utf8 (line, -1, NULL, &bytes_written, NULL);
		gtk_text_buffer_insert_with_tags_by_name (text_buffer,
							  &iter,
							  utf8_line,
							  bytes_written,
							  "monospace", NULL);
		g_free (utf8_line);
		gtk_text_buffer_insert (text_buffer, &iter, "\n", 1);
	}

	/**/

	pref_util_restore_window_geometry (GTK_WINDOW (dialog), LAST_OUTPUT_SCHEMA_NAME);
}


/* -- fr_window_rename_selection -- */


typedef struct {
	char     *path_to_rename;
	char     *old_name;
	char     *new_name;
	char     *current_dir;
	gboolean  is_dir;
	gboolean  dir_in_archive;
	char     *original_path;
} RenameData;


static RenameData*
rename_data_new (const char *path_to_rename,
		 const char *old_name,
		 const char *new_name,
		 const char *current_dir,
		 gboolean    is_dir,
		 gboolean    dir_in_archive,
		 const char *original_path)
{
	RenameData *rdata;

	rdata = g_new0 (RenameData, 1);
	rdata->path_to_rename = g_strdup (path_to_rename);
	if (old_name != NULL)
		rdata->old_name = g_strdup (old_name);
	if (new_name != NULL)
		rdata->new_name = g_strdup (new_name);
	if (current_dir != NULL)
		rdata->current_dir = g_strdup (current_dir);
	rdata->is_dir = is_dir;
	rdata->dir_in_archive = dir_in_archive;
	if (original_path != NULL)
		rdata->original_path = g_strdup (original_path);

	return rdata;
}


static void
rename_data_free (RenameData *rdata)
{
	g_return_if_fail (rdata != NULL);

	g_free (rdata->path_to_rename);
	g_free (rdata->old_name);
	g_free (rdata->new_name);
	g_free (rdata->current_dir);
	g_free (rdata->original_path);
	g_free (rdata);
}


static void
archive_rename_ready_cb (GObject      *source_object,
			 GAsyncResult *result,
			 gpointer      user_data)
{
	FrWindow *window = user_data;
	GError   *error = NULL;

	fr_archive_operation_finish (FR_ARCHIVE (source_object), result, &error);
	_archive_operation_completed (window, FR_ACTION_RENAMING_FILES, error);

	_g_error_free (error);
}


static void
rename_selection (FrWindow   *window,
		  const char *path_to_rename,
		  const char *old_name,
		  const char *new_name,
		  const char *current_dir,
		  gboolean    is_dir,
		  gboolean    dir_in_archive,
		  const char *original_path)
{
	RenameData *rdata;
	GList      *file_list;

	rdata = rename_data_new (path_to_rename,
				 old_name,
				 new_name,
				 current_dir,
				 is_dir,
				 dir_in_archive,
				 original_path);
	fr_window_set_current_batch_action (window,
					    FR_BATCH_ACTION_RENAME,
					    rdata,
					    (GFreeFunc) rename_data_free);

	_archive_operation_started (window, FR_ACTION_RENAMING_FILES);

	g_object_set (window->archive,
		      "compression", window->priv->compression,
		      "encrypt-header", window->priv->encrypt_header,
		      "password", window->priv->password,
		      "volume-size", window->priv->volume_size,
		      NULL);

	if (is_dir)
		file_list = get_dir_list_from_path (window, rdata->path_to_rename);
	else
		file_list = g_list_append (NULL, g_strdup (rdata->path_to_rename));
	fr_window_clipboard_remove_file_list (window, file_list);
	fr_archive_rename (window->archive,
			   file_list,
			   old_name,
			   new_name,
			   current_dir,
			   is_dir,
			   dir_in_archive,
			   original_path,
			   window->priv->cancellable,
			   archive_rename_ready_cb,
			   window);

	_g_string_list_free (file_list);
}


static gboolean
valid_name (const char  *new_name,
	    const char  *old_name,
	    char       **reason)
{
	char     *utf8_new_name;
	gboolean  retval = TRUE;

	new_name = _g_str_eat_spaces (new_name);
	utf8_new_name = g_filename_display_name (new_name);

	if (*new_name == '\0') {
		/* Translators: the name references to a filename.  This message can appear when renaming a file. */
		*reason = g_strdup (_("New name is void, please type a name."));
		retval = FALSE;
	}
	else if (strcmp (new_name, old_name) == 0) {
		/* Translators: the name references to a filename.  This message can appear when renaming a file. */
		*reason = g_strdup (_("New name is the same as old one, please type other name."));
		retval = FALSE;
	}
	else if (_g_strchrs (new_name, BAD_CHARS)) {
		/* Translators: the %s references to a filename.  This message can appear when renaming a file. */
		*reason = g_strdup_printf (_("Name \"%s\" is not valid because it contains at least one of the following characters: %s, please type other name."), utf8_new_name, BAD_CHARS);
		retval = FALSE;
	}

	g_free (utf8_new_name);

	return retval;
}


static gboolean
name_is_present (FrWindow    *window,
		 const char  *current_dir,
		 const char  *new_name,
		 char       **reason)
{
	gboolean  retval = FALSE;
	int       i;
	char     *new_filename;
	int       new_filename_l;

	*reason = NULL;

	new_filename = g_build_filename (current_dir, new_name, NULL);
	new_filename_l = strlen (new_filename);

	for (i = 0; i < window->archive->files->len; i++) {
		FileData   *fdata = g_ptr_array_index (window->archive->files, i);
		const char *filename = fdata->full_path;

		if ((strncmp (filename, new_filename, new_filename_l) == 0)
		    && ((filename[new_filename_l] == '\0')
			|| (filename[new_filename_l] == G_DIR_SEPARATOR))) {
			char *utf8_name = g_filename_display_name (new_name);

			if (filename[new_filename_l] == G_DIR_SEPARATOR)
				*reason = g_strdup_printf (_("A folder named \"%s\" already exists.\n\n%s"), utf8_name, _("Please use a different name."));
			else
				*reason = g_strdup_printf (_("A file named \"%s\" already exists.\n\n%s"), utf8_name, _("Please use a different name."));

			retval = TRUE;
			break;
		}
	}

	g_free (new_filename);

	return retval;
}


void
fr_window_rename_selection (FrWindow *window,
			    gboolean  from_sidebar)
{
	char     *path_to_rename;
	char     *parent_dir;
	char     *old_name;
	gboolean  renaming_dir = FALSE;
	gboolean  dir_in_archive = FALSE;
	char     *original_path = NULL;
	char     *utf8_old_name;
	char     *utf8_new_name;

	if (from_sidebar) {
		path_to_rename = fr_window_get_selected_folder_in_tree_view (window);
		if (path_to_rename == NULL)
			return;
		parent_dir = _g_path_remove_level (path_to_rename);
		old_name = g_strdup (_g_path_get_basename (path_to_rename));
		renaming_dir = TRUE;
	}
	else {
		FileData *selected_item;

		selected_item = fr_window_get_selected_item_from_file_list (window);
		if (selected_item == NULL)
			return;

		renaming_dir = file_data_is_dir (selected_item);
		dir_in_archive = selected_item->dir && ! selected_item->list_dir;
		original_path = g_strdup (selected_item->original_path);

		if (renaming_dir && ! dir_in_archive) {
			parent_dir = g_strdup (fr_window_get_current_location (window));
			old_name = g_strdup (selected_item->list_name);
			path_to_rename = g_build_filename (parent_dir, old_name, NULL);
		}
		else {
			if (renaming_dir) {
				path_to_rename = _g_path_remove_ending_separator (selected_item->full_path);
				parent_dir = _g_path_remove_level (path_to_rename);
			}
			else {
				path_to_rename = g_strdup (selected_item->original_path);
				parent_dir = _g_path_remove_level (selected_item->full_path);
			}
			old_name = g_strdup (selected_item->name);
		}

		file_data_free (selected_item);
	}

 retry__rename_selection:
	utf8_old_name = g_locale_to_utf8 (old_name, -1 ,0 ,0 ,0);
	utf8_new_name = _gtk_request_dialog_run (GTK_WINDOW (window),
						 (GTK_DIALOG_DESTROY_WITH_PARENT
						  | GTK_DIALOG_MODAL),
						 _("Rename"),
						 (renaming_dir ? _("_New folder name:") : _("_New file name:")),
						 utf8_old_name,
						 1024,
						 GTK_STOCK_CANCEL,
						 _("_Rename"));
	g_free (utf8_old_name);

	if (utf8_new_name != NULL) {
		char *new_name;
		char *reason = NULL;

		new_name = g_filename_from_utf8 (utf8_new_name, -1, 0, 0, 0);
		g_free (utf8_new_name);

		if (! valid_name (new_name, old_name, &reason)) {
			char      *utf8_name = g_filename_display_name (new_name);
			GtkWidget *dlg;

			dlg = _gtk_error_dialog_new (GTK_WINDOW (window),
						     GTK_DIALOG_DESTROY_WITH_PARENT,
						     NULL,
						     (renaming_dir ? _("Could not rename the folder") : _("Could not rename the file")),
						     "%s",
						     reason);
			gtk_dialog_run (GTK_DIALOG (dlg));
			gtk_widget_destroy (dlg);

			g_free (reason);
			g_free (utf8_name);
			g_free (new_name);

			goto retry__rename_selection;
		}

		if (name_is_present (window, parent_dir, new_name, &reason)) {
			GtkWidget *dlg;

			dlg = _gtk_message_dialog_new (GTK_WINDOW (window),
						       GTK_DIALOG_MODAL,
						       GTK_STOCK_DIALOG_QUESTION,
						       (renaming_dir ? _("Could not rename the folder") : _("Could not rename the file")),
						       reason,
						       GTK_STOCK_CLOSE, GTK_RESPONSE_OK,
						       NULL);
			gtk_dialog_run (GTK_DIALOG (dlg));
			gtk_widget_destroy (dlg);
			g_free (reason);
			g_free (new_name);
			goto retry__rename_selection;
		}

		rename_selection (window,
				  path_to_rename,
				  old_name,
				  new_name,
				  parent_dir,
				  renaming_dir,
				  dir_in_archive,
				  original_path);

		g_free (new_name);
	}

	g_free (old_name);
	g_free (parent_dir);
	g_free (path_to_rename);
	g_free (original_path);
}


/* -- fr_window_paste_selection -- */


static void
fr_clipboard_get (GtkClipboard     *clipboard,
		  GtkSelectionData *selection_data,
		  guint             info,
		  gpointer          user_data_or_owner)
{
	FrWindow *window = user_data_or_owner;
	char     *data;

	if (gtk_selection_data_get_target (selection_data) != FR_SPECIAL_URI_LIST)
		return;

	data = get_selection_data_from_clipboard_data (window, window->priv->copy_data);
	if (data != NULL) {
		gtk_selection_data_set (selection_data,
					gtk_selection_data_get_target (selection_data),
					8,
					(guchar *) data,
					strlen (data));
		g_free (data);
	}
}


static void
fr_clipboard_clear (GtkClipboard *clipboard,
		    gpointer      user_data_or_owner)
{
	FrWindow *window = user_data_or_owner;

	if (window->priv->copy_data != NULL) {
		fr_clipboard_data_unref (window->priv->copy_data);
		window->priv->copy_data = NULL;
	}
}


GList *
fr_window_get_selection (FrWindow   *window,
		  	 gboolean    from_sidebar,
		  	 char      **return_base_dir)
{
	GList *files;
	char  *base_dir;

	if (from_sidebar) {
		char *selected_folder;
		char *parent_folder;

		files = fr_window_get_folder_tree_selection (window, TRUE, NULL);
		selected_folder = fr_window_get_selected_folder_in_tree_view (window);
		parent_folder = _g_path_remove_level (selected_folder);
		if (parent_folder == NULL)
			base_dir = g_strdup ("/");
		else if (parent_folder[strlen (parent_folder) - 1] == '/')
			base_dir = g_strdup (parent_folder);
		else
			base_dir = g_strconcat (parent_folder, "/", NULL);
		g_free (selected_folder);
		g_free (parent_folder);
	}
	else {
		files = fr_window_get_file_list_selection (window, TRUE, NULL);
		base_dir = g_strdup (fr_window_get_current_location (window));
	}

	if (return_base_dir)
		*return_base_dir = base_dir;
	else
		g_free (base_dir);

	return files;
}


static void
fr_window_copy_or_cut_selection (FrWindow      *window,
				 FrClipboardOp  op,
			  	 gboolean       from_sidebar)
{
	GList        *files;
	char         *base_dir;
	GtkClipboard *clipboard;

	files = fr_window_get_selection (window, from_sidebar, &base_dir);

	if (window->priv->copy_data != NULL)
		fr_clipboard_data_unref (window->priv->copy_data);
	window->priv->copy_data = fr_clipboard_data_new ();
	window->priv->copy_data->files = files;
	window->priv->copy_data->op = op;
	window->priv->copy_data->base_dir = base_dir;

	clipboard = gtk_clipboard_get (FR_CLIPBOARD);
	gtk_clipboard_set_with_owner (clipboard,
				      clipboard_targets,
				      G_N_ELEMENTS (clipboard_targets),
				      fr_clipboard_get,
				      fr_clipboard_clear,
				      G_OBJECT (window));

	fr_window_update_sensitivity (window);
}


void
fr_window_copy_selection (FrWindow *window,
			  gboolean  from_sidebar)
{
	fr_window_copy_or_cut_selection (window, FR_CLIPBOARD_OP_COPY, from_sidebar);
}


void
fr_window_cut_selection (FrWindow *window,
			 gboolean  from_sidebar)
{
	fr_window_copy_or_cut_selection (window, FR_CLIPBOARD_OP_CUT, from_sidebar);
}


/* -- fr_window_paste_from_clipboard_data -- */


static void
_paste_from_archive_operation_completed (FrWindow *window,
					 FrAction  action,
					 GError   *error)
{
	FrArchive *archive;

#ifdef DEBUG
	debug (DEBUG_INFO, "%s [DONE] (FR::Window)\n", action_names[action]);
#endif

	_fr_window_stop_activity_mode (window);
	close_progress_dialog (window, FALSE);

	if ((error != NULL) && (error->code == FR_ERROR_ASK_PASSWORD)) {
		dlg_ask_password_for_second_archive (window);
		return;
	}

	if (action == FR_ACTION_ADDING_FILES)
		archive = window->archive;
	else
		archive = window->priv->copy_from_archive;
	_handle_archive_operation_error (window, archive, action, error, NULL, NULL);

	if (error != NULL) {
		if (window->priv->second_password != NULL) {
			g_free (window->priv->second_password);
			window->priv->second_password = NULL;
		}

		fr_clipboard_data_unref (window->priv->clipboard_data);
		window->priv->clipboard_data = NULL;
	}
}


static void
paste_from_archive_completed_successfully (FrWindow *window)
{
	_paste_from_archive_operation_completed (window, FR_ACTION_PASTING_FILES, NULL);

	fr_clipboard_data_unref (window->priv->clipboard_data);
	window->priv->clipboard_data = NULL;

	if (window->priv->second_password != NULL) {
		g_free (window->priv->second_password);
		window->priv->second_password = NULL;
	}

	window->priv->archive_new = FALSE;
	fr_window_archive_reload (window);
}


static void
paste_from_archive_remove_ready_cb (GObject      *source_object,
				    GAsyncResult *result,
				    gpointer      user_data)
{
	FrWindow *window = user_data;
	GError   *error = NULL;

	if (! fr_archive_operation_finish (FR_ARCHIVE (source_object), result, &error)) {
		_paste_from_archive_operation_completed (window, FR_ACTION_PASTING_FILES, error);
		g_error_free (error);
		return;
	}

	paste_from_archive_completed_successfully (window);
}


static void
paste_from_archive_paste_clipboard_ready_cb (GObject      *source_object,
					     GAsyncResult *result,
					     gpointer      user_data)
{
	FrWindow *window = user_data;
	GError   *error = NULL;

	if (! fr_archive_operation_finish (FR_ARCHIVE (source_object), result, &error)) {
		_paste_from_archive_operation_completed (window, FR_ACTION_PASTING_FILES, error);
		g_error_free (error);
		return;
	}

	if (window->priv->clipboard_data->op == FR_CLIPBOARD_OP_CUT) {
		fr_archive_action_started (window->priv->copy_from_archive, FR_ACTION_DELETING_FILES);
		fr_archive_remove (window->priv->copy_from_archive,
				   window->priv->clipboard_data->files,
				   window->priv->compression,
				   window->priv->cancellable,
				   paste_from_archive_remove_ready_cb,
				   window);
	}
	else
		paste_from_archive_completed_successfully (window);
}


static void
paste_from_archive_extract_ready_cb (GObject      *source_object,
				     GAsyncResult *result,
				     gpointer      user_data)
{
	FrWindow *window = user_data;
	GError   *error = NULL;

	if (! fr_archive_operation_finish (FR_ARCHIVE (source_object), result, &error)) {
		_paste_from_archive_operation_completed (window, FR_ACTION_PASTING_FILES, error);
		g_error_free (error);
		return;
	}

	fr_archive_paste_clipboard (window->archive,
				    window->priv->clipboard_data->file,
				    window->priv->password,
				    window->priv->encrypt_header,
				    window->priv->compression,
				    window->priv->volume_size,
				    window->priv->clipboard_data->op,
				    window->priv->clipboard_data->base_dir,
				    window->priv->clipboard_data->files,
				    window->priv->clipboard_data->tmp_dir,
				    window->priv->clipboard_data->current_dir,
				    window->priv->cancellable,
				    paste_from_archive_paste_clipboard_ready_cb,
				    window);
}


static void
paste_from_archive_list_ready_cb (GObject      *source_object,
				  GAsyncResult *result,
				  gpointer      user_data)
{
	FrWindow *window = user_data;
	GError   *error = NULL;

	if (! fr_archive_operation_finish (FR_ARCHIVE (source_object), result, &error)) {
		_paste_from_archive_operation_completed (window, FR_ACTION_PASTING_FILES, error);
		g_error_free (error);
		return;
	}

	fr_archive_action_started (window->priv->copy_from_archive, FR_ACTION_EXTRACTING_FILES);
	fr_archive_extract (window->priv->copy_from_archive,
			    window->priv->clipboard_data->files,
			    window->priv->clipboard_data->tmp_dir,
			    NULL,
			    FALSE,
			    TRUE,
			    FALSE,
			    window->priv->clipboard_data->password,
			    window->priv->cancellable,
			    paste_from_archive_extract_ready_cb,
			    window);
}


static void
paste_from_archive_open_cb (GObject      *source_object,
			    GAsyncResult *result,
			    gpointer      user_data)
{
	FrWindow *window = user_data;
	GError   *error;

	_g_object_unref (window->priv->copy_from_archive);
	window->priv->copy_from_archive = fr_archive_open_finish (G_FILE (source_object), result, &error);
	if (window->priv->copy_from_archive == NULL) {
		_paste_from_archive_operation_completed (window, FR_ACTION_PASTING_FILES, error);
		g_error_free (error);
		return;
	}

	g_signal_connect (G_OBJECT (window->priv->copy_from_archive),
			  "progress",
			  G_CALLBACK (fr_archive_progress_cb),
			  window);
	g_signal_connect (G_OBJECT (window->priv->copy_from_archive),
			  "message",
			  G_CALLBACK (fr_archive_message_cb),
			  window);
	g_signal_connect (G_OBJECT (window->priv->copy_from_archive),
			  "start",
			  G_CALLBACK (fr_archive_start_cb),
			  window);
	g_signal_connect (G_OBJECT (window->priv->copy_from_archive),
			  "stoppable",
			  G_CALLBACK (fr_archive_stoppable_cb),
			  window);
	g_signal_connect (G_OBJECT (window->priv->copy_from_archive),
			  "working-archive",
			  G_CALLBACK (fr_window_working_archive_cb),
			  window);

	fr_archive_action_started (window->priv->copy_from_archive, FR_ACTION_LISTING_CONTENT);
	fr_archive_list (window->priv->copy_from_archive,
			 window->priv->clipboard_data->password,
			 window->priv->cancellable,
			 paste_from_archive_list_ready_cb,
			 window);
}


static void
_window_started_loading_file (FrWindow *window,
			      GFile    *file)
{
	char *description;

	description = get_action_description (window, FR_ACTION_LOADING_ARCHIVE, file);
	fr_archive_message_cb (NULL, description, window);

	g_free (description);
}


static void
fr_window_paste_from_clipboard_data (FrWindow        *window,
				     FrClipboardData *data)
{
	const char *current_dir_relative;
	GHashTable *created_dirs;
	GList      *scan;
	char       *from_archive;
	char       *to_archive;

	if (window->priv->second_password != NULL)
		fr_clipboard_data_set_password (data, window->priv->second_password);

	if (window->priv->clipboard_data != data) {
		fr_clipboard_data_unref (window->priv->clipboard_data);
		window->priv->clipboard_data = data;
	}

	fr_window_set_current_batch_action (window,
					    FR_BATCH_ACTION_PASTE,
					    fr_clipboard_data_ref (data),
					    (GFreeFunc) fr_clipboard_data_unref);

	current_dir_relative = data->current_dir + 1;

	data->tmp_dir = _g_file_get_temp_work_dir (NULL);
	created_dirs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
	for (scan = data->files; scan; scan = scan->next) {
		const char *old_name = (char*) scan->data;
		char       *new_name;
		char       *dir;

		new_name = g_build_filename (current_dir_relative, old_name + strlen (data->base_dir) - 1, NULL);
		dir = _g_path_remove_level (new_name);
		if ((dir != NULL) && (g_hash_table_lookup (created_dirs, dir) == NULL)) {
			GFile *directory;

			directory = _g_file_append_path (data->tmp_dir, dir, NULL);
			debug (DEBUG_INFO, "mktree %s\n", g_file_get_uri (directory));
			_g_file_make_directory_tree (directory, 0700, NULL);

			g_hash_table_replace (created_dirs, g_strdup (dir), GINT_TO_POINTER (1));
		}

		g_free (dir);
		g_free (new_name);
	}
	g_hash_table_destroy (created_dirs);

	/**/

	g_free (window->priv->custom_action_message);
	from_archive = _g_file_get_display_basename (data->file);
	to_archive = _g_file_get_display_basename (window->priv->archive_file);
	if (data->op == FR_CLIPBOARD_OP_CUT)
		/* Translators: %s are archive filenames */
		window->priv->custom_action_message = g_strdup_printf (_("Moving the files from \"%s\" to \"%s\""), from_archive, to_archive);
	else
		/* Translators: %s are archive filenames */
		window->priv->custom_action_message = g_strdup_printf (_("Copying the files from \"%s\" to \"%s\""), from_archive, to_archive);
	_archive_operation_started (window, FR_ACTION_PASTING_FILES);

	_window_started_loading_file (window, data->file);
	fr_archive_open (data->file,
			 window->priv->cancellable,
			 paste_from_archive_open_cb,
			 window);

	g_free (to_archive);
	g_free (from_archive);
}


static void
fr_window_paste_selection_to (FrWindow   *window,
			      const char *current_dir)
{
	GtkClipboard     *clipboard;
	GtkSelectionData *selection_data;
	FrClipboardData  *paste_data;

	clipboard = gtk_clipboard_get (FR_CLIPBOARD);
	selection_data = gtk_clipboard_wait_for_contents (clipboard, FR_SPECIAL_URI_LIST);
	if (selection_data == NULL)
		return;

	paste_data = get_clipboard_data_from_selection_data (window, (char*) gtk_selection_data_get_data (selection_data));
	paste_data->current_dir = g_strdup (current_dir);
	fr_window_paste_from_clipboard_data (window, paste_data);

	gtk_selection_data_free (selection_data);
}


void
fr_window_paste_selection (FrWindow *window,
			   gboolean  from_sidebar)
{
	char *utf8_path, *utf8_old_path, *destination;
	char *current_dir;

	if (window->priv->list_mode == FR_WINDOW_LIST_MODE_FLAT)
		return;

	/**/

	utf8_old_path = g_filename_to_utf8 (fr_window_get_current_location (window), -1, NULL, NULL, NULL);
	utf8_path = _gtk_request_dialog_run (GTK_WINDOW (window),
					       (GTK_DIALOG_DESTROY_WITH_PARENT
						| GTK_DIALOG_MODAL),
					       _("Paste Selection"),
					       _("_Destination folder:"),
					       utf8_old_path,
					       1024,
					       GTK_STOCK_CANCEL,
					       GTK_STOCK_PASTE);
	g_free (utf8_old_path);
	if (utf8_path == NULL)
		return;

	destination = g_filename_from_utf8 (utf8_path, -1, NULL, NULL, NULL);
	g_free (utf8_path);

	if (destination[0] != '/')
		current_dir = g_build_filename (fr_window_get_current_location (window), destination, NULL);
	else
		current_dir = g_strdup (destination);
	g_free (destination);

	fr_window_paste_selection_to (window, current_dir);

	g_free (current_dir);
}


/* -- fr_window_open_files -- */


void
fr_window_open_files_with_command (FrWindow *window,
				   GList    *file_list,
				   char     *command)
{
	GAppInfo *app;
	GError   *error = NULL;

	app = g_app_info_create_from_commandline (command, NULL, G_APP_INFO_CREATE_NONE, &error);
	if (error != NULL) {
		_gtk_error_dialog_run (GTK_WINDOW (window),
				       _("Could not perform the operation"),
				       "%s",
				       error->message);
		g_clear_error (&error);
		return;
	}

	fr_window_open_files_with_application (window, file_list, app);
}


void
fr_window_open_files_with_application (FrWindow *window,
				       GList    *file_list,
				       GAppInfo *app)
{
	GList               *uris;
	GList               *scan;
	GdkAppLaunchContext *context;
	GError              *error = NULL;

	if (window->priv->activity_ref > 0)
		return;

	uris = NULL;
	for (scan = file_list; scan; scan = scan->next)
		uris = g_list_prepend (uris, g_file_get_uri (G_FILE (scan->data)));

	context = gdk_display_get_app_launch_context (gtk_widget_get_display (GTK_WIDGET (window)));
	gdk_app_launch_context_set_screen (context, gtk_widget_get_screen (GTK_WIDGET (window)));
	gdk_app_launch_context_set_timestamp (context, 0);

	if (! g_app_info_launch_uris (app, uris, G_APP_LAUNCH_CONTEXT (context), &error)) {
		_gtk_error_dialog_run (GTK_WINDOW (window),
				       _("Could not perform the operation"),
				       "%s",
				       error->message);
		g_clear_error (&error);
	}

	g_object_unref (context);
	_g_string_list_free (uris);
}


typedef struct {
	int          ref_count;
	FrWindow    *window;
	GList       *file_list;
	gboolean     ask_application;
	CommandData *cdata;
} OpenFilesData;


static OpenFilesData*
open_files_data_new (FrWindow *window,
		     GList    *file_list,
		     gboolean  ask_application)

{
	OpenFilesData *odata;
	GList         *scan;

	odata = g_new0 (OpenFilesData, 1);
	odata->ref_count = 1;
	odata->window = window;
	odata->file_list = _g_string_list_dup (file_list);
	odata->ask_application = ask_application;
	odata->cdata = g_new0 (CommandData, 1);
	odata->cdata->temp_dir = _g_file_get_temp_work_dir (NULL);
	odata->cdata->file_list = NULL;
	for (scan = file_list; scan; scan = scan->next) {
		char  *filename = scan->data;
		GFile *file;

		file = _g_file_append_path (odata->cdata->temp_dir, filename, NULL);
		odata->cdata->file_list = g_list_prepend (odata->cdata->file_list, file);
	}

	/* Add to CommandList so the cdata is released on exit. */
	CommandList = g_list_prepend (CommandList, odata->cdata);

	return odata;
}


static void
open_files_data_ref (OpenFilesData *odata)
{
	g_return_if_fail (odata != NULL);
	odata->ref_count++;
}


static void
open_files_data_unref (OpenFilesData *odata)
{
	g_return_if_fail (odata != NULL);

	if (--odata->ref_count > 0)
		return;

	_g_string_list_free (odata->file_list);
	g_free (odata);
}


/* -- -- */


void
fr_window_update_dialog_closed (FrWindow *window)
{
	window->priv->update_dialog = NULL;
}


static void
update_files_ready_cb (GObject      *source_object,
		       GAsyncResult *result,
		       gpointer      user_data)
{
	FrWindow *window = user_data;
	GError   *error = NULL;

	fr_archive_operation_finish (FR_ARCHIVE (source_object), result, &error);
	_archive_operation_completed (window, FR_ACTION_UPDATING_FILES, error);

	_g_error_free (error);
}


gboolean
fr_window_update_files (FrWindow *window,
			GList    *open_file_list)
{
	GList *file_list;
	GList *dir_list;
	GList *scan;

	if (window->priv->activity_ref > 0)
		return FALSE;

	if (window->archive->read_only)
		return FALSE;

	/* the size will be computed by the archive object */
	window->archive->files_to_add_size = 0;

	file_list = NULL;
	dir_list = NULL;
	for (scan = open_file_list; scan; scan = scan->next) {
		OpenFile *open_file = scan->data;

		file_list = g_list_prepend (file_list, g_object_ref (open_file->extracted_file));
		dir_list = g_list_prepend (dir_list, g_object_ref (open_file->temp_dir));
	}

	_archive_operation_started (window, FR_ACTION_UPDATING_FILES);

	fr_archive_update_open_files (window->archive,
				      file_list,
				      dir_list,
				      window->priv->password,
				      window->priv->encrypt_header,
				      window->priv->compression,
				      window->priv->volume_size,
				      window->priv->cancellable,
				      update_files_ready_cb,
				      window);

	_g_object_list_unref (dir_list);
	_g_object_list_unref (file_list);

	return TRUE;
}


static void
open_file_modified_cb (GFileMonitor     *monitor,
		       GFile            *monitor_file,
		       GFile            *other_file,
		       GFileMonitorEvent event_type,
		       gpointer          user_data)
{
	FrWindow *window = user_data;
	OpenFile *file;
	GList    *scan;

	if ((event_type != G_FILE_MONITOR_EVENT_CHANGED)
	    && (event_type != G_FILE_MONITOR_EVENT_CREATED))
	{
		return;
	}

	file = NULL;
	for (scan = window->priv->open_files; scan; scan = scan->next) {
		OpenFile *test = scan->data;
		if (_g_file_cmp_uris (test->extracted_file, monitor_file) == 0) {
			file = test;
			break;
		}
	}

	g_return_if_fail (file != NULL);

	if (window->priv->update_dialog == NULL)
		window->priv->update_dialog = dlg_update (window);
	dlg_update_add_file (window->priv->update_dialog, file);
}


static void
fr_window_monitor_open_file (FrWindow *window,
			     OpenFile *file)
{
	window->priv->open_files = g_list_prepend (window->priv->open_files, file);
	file->monitor = g_file_monitor_file (file->extracted_file, 0, NULL, NULL);
	g_signal_connect (file->monitor,
			  "changed",
			  G_CALLBACK (open_file_modified_cb),
			  window);
}


static void
monitor_extracted_files (OpenFilesData *odata)
{
	FrWindow *window = odata->window;
	GList    *scan1, *scan2;

	for (scan1 = odata->file_list, scan2 = odata->cdata->file_list;
	     scan1 && scan2;
	     scan1 = scan1->next, scan2 = scan2->next)
	{
		char     *original_path = (char *) scan1->data;
		GFile    *extracted_file = G_FILE (scan2->data);
		OpenFile *ofile;

		ofile = open_file_new (original_path, extracted_file, odata->cdata->temp_dir);
		if (ofile != NULL)
			fr_window_monitor_open_file (window, ofile);
	}
}


static gboolean
fr_window_open_extracted_files (OpenFilesData *odata)
{
	GList               *file_list = odata->cdata->file_list;
	GFile               *first_file;
	const char          *first_mime_type;
	GAppInfo            *app;
	GList               *files_to_open = NULL;
	GdkAppLaunchContext *context;
	gboolean             result;
	GError              *error = NULL;

	g_return_val_if_fail (file_list != NULL, FALSE);

	first_file = G_FILE (file_list->data);
	if (first_file == NULL)
		return FALSE;

	if (! odata->window->archive->read_only)
		monitor_extracted_files (odata);

	if (odata->ask_application) {
		dlg_open_with (odata->window, file_list);
		return FALSE;
	}

	first_mime_type = _g_file_get_mime_type (first_file, FALSE);
	app = g_app_info_get_default_for_type (first_mime_type, FALSE);

	if (app == NULL) {
		dlg_open_with (odata->window, file_list);
		return FALSE;
	}

	files_to_open = g_list_append (files_to_open, g_file_get_uri (first_file));

	if (g_app_info_supports_files (app)) {
		GList *scan;

		for (scan = file_list->next; scan; scan = scan->next) {
			GFile      *file = G_FILE (scan->data);
			const char *mime_type;

			mime_type = _g_file_get_mime_type (file, FALSE);
			if (mime_type == NULL)
				continue;

			if (strcmp (mime_type, first_mime_type) == 0) {
				files_to_open = g_list_append (files_to_open, g_file_get_uri (file));
			}
			else {
				GAppInfo *app2;

				app2 = g_app_info_get_default_for_type (mime_type, FALSE);
				if (g_app_info_equal (app, app2))
					files_to_open = g_list_append (files_to_open, g_file_get_uri (file));
				g_object_unref (app2);
			}
		}
	}

	context = gdk_display_get_app_launch_context (gtk_widget_get_display (GTK_WIDGET (odata->window)));
	gdk_app_launch_context_set_screen (context, gtk_widget_get_screen (GTK_WIDGET (odata->window)));
	gdk_app_launch_context_set_timestamp (context, 0);
	result = g_app_info_launch_uris (app, files_to_open, G_APP_LAUNCH_CONTEXT (context), &error);
	if (! result) {
		_gtk_error_dialog_run (GTK_WINDOW (odata->window),
				       _("Could not perform the operation"),
				       "%s",
				       error->message);
		g_clear_error (&error);
	}

	g_object_unref (context);
	g_object_unref (app);
	_g_string_list_free (files_to_open);

	return result;
}


static void
open_files_extract_ready_cb (GObject      *source_object,
			     GAsyncResult *result,
			     gpointer      user_data)
{
	OpenFilesData *odata = user_data;
	GError        *error = NULL;

	open_files_data_ref (odata);
	fr_archive_operation_finish (FR_ARCHIVE (source_object), result, &error);
	_archive_operation_completed (odata->window, FR_ACTION_EXTRACTING_FILES, error);

	if (error == NULL)
		fr_window_open_extracted_files (odata);

	open_files_data_unref (odata);
	_g_error_free (error);
}


void
fr_window_open_files (FrWindow *window,
		      GList    *file_list,
		      gboolean  ask_application)
{
	OpenFilesData *odata;

	if (window->priv->activity_ref > 0)
		return;

	odata = open_files_data_new (window, file_list, ask_application);
	fr_window_set_current_batch_action (window,
					    FR_BATCH_ACTION_OPEN_FILES,
					    odata,
					    (GFreeFunc) open_files_data_unref);

	_archive_operation_started (odata->window, FR_ACTION_EXTRACTING_FILES);

	fr_archive_extract (window->archive,
			    odata->file_list,
			    odata->cdata->temp_dir,
			    NULL,
			    FALSE,
			    TRUE,
			    FALSE,
			    window->priv->password,
			    window->priv->cancellable,
			    open_files_extract_ready_cb,
			    odata);
}


/**/


static GFile *
_get_default_dir (GFile *directory)
{
	if (! _g_file_is_temp_dir (directory))
		return g_object_ref (directory);
	else
		return NULL;
}


void
fr_window_set_open_default_dir (FrWindow *window,
				GFile    *default_dir)
{
	g_return_if_fail (window != NULL);
	g_return_if_fail (default_dir != NULL);

	_g_object_unref (window->priv->open_default_dir);
	window->priv->open_default_dir = _get_default_dir (default_dir);
}


GFile *
fr_window_get_open_default_dir (FrWindow *window)
{
	if (window->priv->open_default_dir == NULL)
		return _g_file_get_home ();
	else
		return  window->priv->open_default_dir;
}


void
fr_window_set_add_default_dir (FrWindow *window,
			       GFile    *default_dir)
{
	g_return_if_fail (window != NULL);
	g_return_if_fail (default_dir != NULL);

	_g_object_unref (window->priv->add_default_dir);
	window->priv->add_default_dir = _get_default_dir (default_dir);
}


GFile *
fr_window_get_add_default_dir (FrWindow *window)
{
	return  window->priv->add_default_dir;
}


void
fr_window_set_extract_default_dir (FrWindow *window,
				   GFile    *default_dir,
				   gboolean  freeze)
{
	g_return_if_fail (window != NULL);
	g_return_if_fail (default_dir != NULL);

	/* do not change this dir while it's used by the non-interactive
	 * extraction operation. */
	if (window->priv->extract_interact_use_default_dir)
		return;

	window->priv->extract_interact_use_default_dir = freeze;

	_g_object_unref (window->priv->extract_default_dir);
	window->priv->extract_default_dir = _get_default_dir (default_dir);
}


GFile *
fr_window_get_extract_default_dir (FrWindow *window)
{
	if (window->priv->extract_default_dir == NULL)
		return _g_file_get_home ();
	else
		return  window->priv->extract_default_dir;
}


void
fr_window_set_default_dir (FrWindow *window,
			   GFile    *default_dir,
			   gboolean  freeze)
{
	g_return_if_fail (window != NULL);
	g_return_if_fail (default_dir != NULL);

	window->priv->freeze_default_dir = freeze;

	fr_window_set_open_default_dir (window, default_dir);
	fr_window_set_add_default_dir (window, default_dir);
	fr_window_set_extract_default_dir (window, default_dir, FALSE);
}


void
fr_window_set_toolbar_visibility (FrWindow *window,
				  gboolean  visible)
{
	g_return_if_fail (window != NULL);

	if (visible)
		gtk_widget_show (window->priv->toolbar);
	else
		gtk_widget_hide (window->priv->toolbar);

	set_active (window, "ViewToolbar", visible);
}


void
fr_window_set_statusbar_visibility  (FrWindow *window,
				     gboolean  visible)
{
	g_return_if_fail (window != NULL);

	if (visible)
		gtk_widget_show (window->priv->statusbar);
	else
		gtk_widget_hide (window->priv->statusbar);

	set_active (window, "ViewStatusbar", visible);
}


void
fr_window_set_folders_visibility (FrWindow   *window,
				  gboolean    value)
{
	g_return_if_fail (window != NULL);

	window->priv->view_folders = value;
	fr_window_update_dir_tree (window);

	set_active (window, "ViewFolders", window->priv->view_folders);
}


void
fr_window_use_progress_dialog (FrWindow *window,
			       gboolean  value)
{
	window->priv->use_progress_dialog = value;
}


/* -- batch mode procedures -- */


static void fr_window_exec_current_batch_action (FrWindow *window);


static void
fr_window_exec_batch_action (FrWindow      *window,
			     FrBatchAction *action)
{
	ExtractData   *edata;
	RenameData    *rdata;
	OpenFilesData *odata;
	ConvertData   *cdata;
	EncryptData   *enc_data;

	switch (action->type) {
	case FR_BATCH_ACTION_LOAD:
		debug (DEBUG_INFO, "[BATCH] LOAD\n");

		if (! g_file_query_exists (G_FILE (action->data), NULL)) {
			GError *error = NULL;

			if (! fr_window_archive_new (window, G_FILE (action->data), NULL))
				error = g_error_new_literal (FR_ERROR, FR_ERROR_GENERIC, _("Archive type not supported."));
			_archive_operation_completed (window, FR_ACTION_CREATING_NEW_ARCHIVE, error);

			_g_error_free (error);
		}
		else
			fr_window_archive_open (window, G_FILE (action->data), GTK_WINDOW (window));
		break;

	case FR_BATCH_ACTION_ADD:
		debug (DEBUG_INFO, "[BATCH] ADD\n");

		fr_window_archive_add_dropped_items (window, (GList *) action->data);
		break;

	case FR_BATCH_ACTION_OPEN:
		debug (DEBUG_INFO, "[BATCH] OPEN\n");

		fr_window_push_message (window, _("Add files to an archive"));
		dlg_batch_add_files (window, (GList *) action->data);
		break;

	case FR_BATCH_ACTION_EXTRACT:
		debug (DEBUG_INFO, "[BATCH] EXTRACT\n");

		edata = action->data;
		fr_window_archive_extract (window,
					   edata->file_list,
					   edata->destination,
					   edata->base_dir,
					   edata->skip_older,
					   edata->overwrite,
					   edata->junk_paths,
					   ! window->priv->batch_mode || window->priv->notify);
		break;

	case FR_BATCH_ACTION_EXTRACT_HERE:
		debug (DEBUG_INFO, "[BATCH] EXTRACT HERE\n");

		edata = action->data;
		fr_window_archive_extract_here (window,
						FALSE,
						TRUE,
						FALSE,
						! window->priv->batch_mode || window->priv->notify);
		break;

	case FR_BATCH_ACTION_EXTRACT_INTERACT:
		debug (DEBUG_INFO, "[BATCH] EXTRACT_INTERACT\n");

		if (window->priv->extract_interact_use_default_dir
		    && (window->priv->extract_default_dir != NULL))
		{
			fr_window_archive_extract (window,
						   NULL,
						   window->priv->extract_default_dir,
						   NULL,
						   FALSE,
						   FR_OVERWRITE_ASK,
						   FALSE,
						   TRUE);
		}
		else {
			fr_window_push_message (window, _("Extract archive"));
			dlg_extract (NULL, window);
		}
		break;

	case FR_BATCH_ACTION_RENAME:
		debug (DEBUG_INFO, "[BATCH] RENAME\n");

		rdata = action->data;
		rename_selection (window,
				  rdata->path_to_rename,
				  rdata->old_name,
				  rdata->new_name,
				  rdata->current_dir,
				  rdata->is_dir,
				  rdata->dir_in_archive,
				  rdata->original_path);
		break;

	case FR_BATCH_ACTION_PASTE:
		debug (DEBUG_INFO, "[BATCH] PASTE\n");

		fr_window_paste_from_clipboard_data (window, (FrClipboardData*) action->data);
		break;

	case FR_BATCH_ACTION_OPEN_FILES:
		debug (DEBUG_INFO, "[BATCH] OPEN FILES\n");

		odata = action->data;
		fr_window_open_files (window, odata->file_list, odata->ask_application);
		break;

	case FR_BATCH_ACTION_SAVE_AS:
		debug (DEBUG_INFO, "[BATCH] SAVE_AS\n");

		cdata = action->data;
		fr_window_archive_save_as (window,
					   cdata->file,
					   cdata->mime_type,
					   cdata->password,
					   cdata->encrypt_header,
					   cdata->volume_size);
		break;

	case FR_BATCH_ACTION_TEST:
		debug (DEBUG_INFO, "[BATCH] TEST\n");

		fr_window_archive_test (window);
		break;

	case FR_BATCH_ACTION_ENCRYPT:
		debug (DEBUG_INFO, "[BATCH] ENCRYPT\n");

		enc_data = action->data;
		fr_window_archive_encrypt (window,
					   enc_data->password,
					   enc_data->encrypt_header);
		break;


	case FR_BATCH_ACTION_CLOSE:
		debug (DEBUG_INFO, "[BATCH] CLOSE\n");

		fr_window_archive_close (window);
		fr_window_exec_next_batch_action (window);
		break;

	case FR_BATCH_ACTION_QUIT:
		debug (DEBUG_INFO, "[BATCH] QUIT\n");

		g_signal_emit (window,
			       fr_window_signals[READY],
			       0,
			       NULL);

		if ((window->priv->progress_dialog != NULL) && (gtk_widget_get_parent (window->priv->progress_dialog) != GTK_WIDGET (window))) {
			gtk_widget_destroy (window->priv->progress_dialog);
			window->priv->progress_dialog = NULL;
		}
		gtk_widget_destroy (GTK_WIDGET (window));
		break;

	default:
		break;
	}
}


void
fr_window_reset_current_batch_action (FrWindow *window)
{
	FrBatchAction *action = &window->priv->current_batch_action;

	if ((action->data != NULL) && (action->free_func != NULL))
		(*action->free_func) (action->data);
	action->type = FR_BATCH_ACTION_NONE;
	action->data = NULL;
	action->free_func = NULL;
}


void
fr_window_set_current_batch_action (FrWindow          *window,
				    FrBatchActionType  action_type,
				    void              *data,
				    GFreeFunc          free_func)
{
	FrBatchAction *action;

	fr_window_reset_current_batch_action (window);

	action = &window->priv->current_batch_action;
	action->type = action_type;
	action->data = data;
	action->free_func = free_func;
}


void
fr_window_restart_current_batch_action (FrWindow *window)
{
	fr_window_exec_batch_action (window, &window->priv->current_batch_action);
}


void
fr_window_append_batch_action (FrWindow          *window,
			       FrBatchActionType  action,
			       void              *data,
			       GFreeFunc          free_func)
{
	FrBatchAction *a_desc;

	g_return_if_fail (window != NULL);

	a_desc = g_new0 (FrBatchAction, 1);
	a_desc->type = action;
	a_desc->data = data;
	a_desc->free_func = free_func;

	window->priv->batch_action_list = g_list_append (window->priv->batch_action_list, a_desc);
}


static void
fr_window_exec_current_batch_action (FrWindow *window)
{
	FrBatchAction *action;

	if (window->priv->batch_action == NULL) {
		fr_window_free_batch_data (window);
		if (window->priv->reload_archive) {
			window->priv->reload_archive = FALSE;
			fr_window_archive_reload (window);
		}
		return;
	}

	action = (FrBatchAction *) window->priv->batch_action->data;
	fr_window_exec_batch_action (window, action);
}


static void
fr_window_exec_next_batch_action (FrWindow *window)
{
	if (window->priv->batch_action != NULL)
		window->priv->batch_action = g_list_next (window->priv->batch_action);
	else
		window->priv->batch_action = window->priv->batch_action_list;
	fr_window_exec_current_batch_action (window);
}


void
fr_window_start_batch (FrWindow *window)
{
	g_return_if_fail (window != NULL);

	if (window->priv->batch_mode)
		return;

	if (window->priv->batch_action_list == NULL)
		return;

	if (window->priv->progress_dialog != NULL)
		gtk_window_set_title (GTK_WINDOW (window->priv->progress_dialog),
				      window->priv->batch_title);

	window->priv->batch_mode = TRUE;
	window->priv->batch_action = window->priv->batch_action_list;
	gtk_widget_hide (GTK_WIDGET (window));

	fr_window_exec_current_batch_action (window);
}


void
fr_window_stop_batch (FrWindow *window)
{
	if (! window->priv->batch_mode) {
		fr_window_free_batch_data (window);
		window->priv->reload_archive = FALSE;
		return;
	}

	window->priv->extract_interact_use_default_dir = FALSE;

	if (! window->priv->showing_error_dialog) {
		g_signal_emit (window,
			       fr_window_signals[READY],
			       0,
			       NULL);
		gtk_widget_destroy (GTK_WIDGET (window));
	}
}


void
fr_window_stop_batch_with_error (FrWindow     *window,
				 FrAction      action,
				 FrErrorType   error_type,
				 const char   *error_message)
{
	GError *error;

	error = g_error_new_literal (FR_ERROR, error_type, error_message);
	_archive_operation_completed (window, action , error);

	g_error_free (error);
}


void
fr_window_resume_batch (FrWindow *window)
{
	fr_window_exec_current_batch_action (window);
}


gboolean
fr_window_is_batch_mode (FrWindow *window)
{
	return window->priv->batch_mode;
}


void
fr_window_new_batch (FrWindow   *window,
		     const char *title)
{
	fr_window_free_batch_data (window);
	g_free (window->priv->batch_title);
	window->priv->batch_title = g_strdup (title);
}


const char *
fr_window_get_batch_title (FrWindow *window)
{
	return window->priv->batch_title;
}


void
fr_window_set_batch__extract_here (FrWindow *window,
				   GFile    *archive)
{
	g_return_if_fail (window != NULL);
	g_return_if_fail (archive != NULL);

	fr_window_append_batch_action (window,
				       FR_BATCH_ACTION_LOAD,
				       g_object_ref (archive),
				       (GFreeFunc) g_object_unref);
	fr_window_append_batch_action (window,
				       FR_BATCH_ACTION_EXTRACT_HERE,
				       extract_to_data_new (window, NULL),
				       (GFreeFunc) extract_data_free);
	fr_window_append_batch_action (window,
				       FR_BATCH_ACTION_CLOSE,
				       NULL,
				       NULL);
}


void
fr_window_set_batch__extract (FrWindow  *window,
			      GFile     *archive,
			      GFile     *destination)
{
	g_return_if_fail (window != NULL);
	g_return_if_fail (archive != NULL);

	fr_window_append_batch_action (window,
				       FR_BATCH_ACTION_LOAD,
				       g_object_ref (archive),
				       (GFreeFunc) g_object_unref);
	if (destination != NULL)
		fr_window_append_batch_action (window,
					       FR_BATCH_ACTION_EXTRACT,
					       extract_to_data_new (window, destination),
					       (GFreeFunc) extract_data_free);
	else
		fr_window_append_batch_action (window,
					       FR_BATCH_ACTION_EXTRACT_INTERACT,
					       NULL,
					       NULL);
	fr_window_append_batch_action (window,
				       FR_BATCH_ACTION_CLOSE,
				       NULL,
				       NULL);
}


void
fr_window_set_batch__add (FrWindow *window,
			  GFile    *archive,
			  GList    *file_list)
{
	window->priv->batch_adding_one_file = (file_list->next == NULL) && (_g_file_query_is_file (file_list->data));

	if (archive != NULL)
		fr_window_append_batch_action (window,
					       FR_BATCH_ACTION_LOAD,
					       g_object_ref (archive),
					       (GFreeFunc) g_object_unref);
	else
		fr_window_append_batch_action (window,
					       FR_BATCH_ACTION_OPEN,
					       _g_object_list_ref (file_list),
					       (GFreeFunc) _g_object_list_unref);
	fr_window_append_batch_action (window,
				       FR_BATCH_ACTION_ADD,
				       _g_object_list_ref (file_list),
				       (GFreeFunc) _g_object_list_unref);
	fr_window_append_batch_action (window,
				       FR_BATCH_ACTION_CLOSE,
				       NULL,
				       NULL);
}
