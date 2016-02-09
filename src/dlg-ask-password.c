/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/*
 *  File-Roller
 *
 *  Copyright (C) 2001 The Free Software Foundation, Inc.
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
#include <string.h>
#include <gtk/gtk.h>
#include "dlg-ask-password.h"
#include "file-utils.h"
#include "fr-window.h"
#include "glib-utils.h"
#include "gtk-utils.h"


#define GET_WIDGET(x) (_gtk_builder_get_widget (data->builder, (x)))


typedef enum {
	FR_PASSWORD_TYPE_MAIN_ARCHIVE,
	FR_PASSWORD_TYPE_SECOND_ARCHIVE
} FrPasswordType;


typedef struct {
	GtkBuilder     *builder;
	FrWindow       *window;
	FrPasswordType  pwd_type;
	GtkWidget      *dialog;
	GtkWidget      *password_entry;
} DialogData;


/* called when the main dialog is closed. */
static void
destroy_cb (GtkWidget  *widget,
	    DialogData *data)
{
	g_object_unref (data->builder);
	g_free (data);
}


static void
ask_password__response_cb (GtkWidget  *dialog,
			   int         response_id,
			   DialogData *data)
{
	char *password;

	switch (response_id) {
	case GTK_RESPONSE_OK:
		password = _gtk_entry_get_locale_text (GTK_ENTRY (data->password_entry));
		if (data->pwd_type == FR_PASSWORD_TYPE_MAIN_ARCHIVE)
			fr_window_set_password (data->window, password);
		else if (data->pwd_type == FR_PASSWORD_TYPE_SECOND_ARCHIVE)
			fr_window_set_password_for_second_archive (data->window, password);
		g_free (password);

		if (fr_window_is_batch_mode (data->window))
			fr_window_resume_batch (data->window);
		else
			fr_window_restart_current_batch_action (data->window);
		break;

	default:
		if (fr_window_is_batch_mode (data->window))
			gtk_widget_destroy (GTK_WIDGET (data->window));
		else
			fr_window_reset_current_batch_action (data->window);
		break;
	}

	gtk_widget_destroy (data->dialog);
}


static void
dlg_ask_password__common (FrWindow       *window,
			  FrPasswordType  pwd_type)
{
	DialogData *data;
	GFile      *file;
	const char *old_password;
	char       *filename;
	char       *message;

	data = g_new0 (DialogData, 1);
	data->builder = _gtk_builder_new_from_resource ("ask-password.ui");
	if (data->builder == NULL) {
		g_free (data);
		return;
	}
	data->window = window;
	data->pwd_type = pwd_type;

	/* Get the widgets. */

	data->dialog = GET_WIDGET ("password_dialog");
	data->password_entry = GET_WIDGET ("password_entry");

	/* Set widgets data. */

	if (data->pwd_type == FR_PASSWORD_TYPE_MAIN_ARCHIVE) {
		file = fr_window_get_archive_file (window);
		old_password = fr_window_get_password (window);
	}
	else if (data->pwd_type == FR_PASSWORD_TYPE_SECOND_ARCHIVE) {
		file = fr_window_get_archive_file_for_paste (window);
		old_password = fr_window_get_password_for_second_archive (window);
	} else
		g_assert_not_reached ();

	filename = _g_file_get_display_basename (file);
	/* Translators: %s is a filename */
	message = g_strdup_printf (_("Password required for \"%s\""), filename);
	gtk_label_set_label (GTK_LABEL (GET_WIDGET ("title_label")), message);

	_gtk_entry_use_as_password_entry (GTK_ENTRY (data->password_entry));
	if (old_password != NULL) {
		GtkWidget *info_bar;
		GtkWidget *label;

		info_bar = gtk_info_bar_new ();
		label = gtk_label_new (_("Wrong password."));
		gtk_container_add (GTK_CONTAINER (gtk_info_bar_get_content_area (GTK_INFO_BAR (info_bar))), label);
		gtk_info_bar_set_message_type (GTK_INFO_BAR (info_bar), GTK_MESSAGE_ERROR);
		gtk_box_pack_start (GTK_BOX (GET_WIDGET ("error_box")), info_bar, TRUE, TRUE, 0);
		gtk_widget_show_all (GET_WIDGET ("error_box"));

		_gtk_entry_set_locale_text (GTK_ENTRY (data->password_entry),
					    fr_window_get_password (window));
	}

	/* Set the signals handlers. */

	g_signal_connect (G_OBJECT (data->dialog),
			  "destroy",
			  G_CALLBACK (destroy_cb),
			  data);
	g_signal_connect (G_OBJECT (data->dialog),
			  "response",
			  G_CALLBACK (ask_password__response_cb),
			  data);

	/* Run dialog. */

	gtk_widget_grab_focus (data->password_entry);
	if (gtk_widget_get_realized (GTK_WIDGET (window))) {
		gtk_window_set_transient_for (GTK_WINDOW (data->dialog),
					      GTK_WINDOW (window));
		gtk_window_set_modal (GTK_WINDOW (data->dialog), TRUE);
	}
	else
		gtk_window_set_title (GTK_WINDOW (data->dialog),
				      fr_window_get_batch_title (window));
	gtk_widget_show (data->dialog);

	g_free (message);
	g_free (filename);
}


void
dlg_ask_password (FrWindow *window)
{
	dlg_ask_password__common (window, FR_PASSWORD_TYPE_MAIN_ARCHIVE);
}


void
dlg_ask_password_for_second_archive (FrWindow *window)
{
	dlg_ask_password__common (window, FR_PASSWORD_TYPE_SECOND_ARCHIVE);
}
