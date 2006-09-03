/*                                                     -*- linux-c -*-
    Copyright (C) 2004 Tom Szilagyi

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

    $Id$
*/

#include <config.h>

#include <dirent.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <unistd.h>
#include <fnmatch.h>
#include <regex.h>
#include <gtk/gtk.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <glib.h>
#else
#include <pthread.h>
#endif /* _WIN32*/

#include "common.h"
#include "i18n.h"
#include "options.h"
#include "music_browser.h"
#include "gui_main.h"
#include "meta_decoder.h"
#include "build_store.h"
#include "cddb_lookup.h"

#define BUILD_THREAD_BUSY  0
#define BUILD_THREAD_FREE  1

#define BUILD_STORE   0
#define BUILD_ARTIST  1

#define ARTIST_SORT_NAME      0
#define ARTIST_SORT_NAME_LOW  1
#define ARTIST_SORT_DIR       2
#define ARTIST_SORT_DIR_LOW   3

#define RECORD_SORT_YEAR      0
#define RECORD_SORT_NAME      1
#define RECORD_SORT_NAME_LOW  2
#define RECORD_SORT_DIR       3
#define RECORD_SORT_DIR_LOW   4

#define CAP_ALL_WORDS   0
#define CAP_FIRST_WORD  1


extern options_t options;
extern GtkTreeStore * music_store;

extern GtkWidget * browser_window;
extern GtkWidget * gui_stock_label_button(gchar * blabel, const gchar * bstock);

extern GdkPixbuf * icon_artist;
extern GdkPixbuf * icon_record;
extern GdkPixbuf * icon_track;

#ifdef HAVE_MPEG
extern char * valid_extensions_mpeg[];
#endif /* HAVE_MPEG */

#ifdef HAVE_MOD
extern char * valid_extensions_mod[];
#endif /* HAVE_MOD */

AQUALUNG_THREAD_DECLARE(build_thread_id)


int build_thread_state = BUILD_THREAD_FREE;
int build_type;

volatile int build_cancelled = 0;
volatile int write_data_locked = 0;

GtkTreeIter store_iter;
GtkTreeIter artist_iter;
int artist_iter_is_set = 0;

GtkWidget * build_prog_window = NULL;
GtkWidget * prog_cancel_button;
GtkWidget * prog_file_entry;
GtkWidget * prog_action_label;

GtkWidget * gen_check_excl;
GtkWidget * gen_entry_excl;
GtkWidget * gen_check_incl;
GtkWidget * gen_entry_incl;

GtkWidget * gen_check_cap;
GtkWidget * gen_combo_cap;
GtkWidget * gen_check_cap_pre;
GtkWidget * gen_entry_cap_pre;
GtkWidget * gen_check_cap_low;

GtkWidget * meta_check_enable;
GtkWidget * meta_check_wspace;
GtkWidget * meta_check_title;
GtkWidget * meta_check_artist;
GtkWidget * meta_check_record;
GtkWidget * meta_check_rva;
GtkWidget * meta_check_comment;

#ifdef HAVE_CDDB
GtkWidget * cddb_check_enable;
GtkWidget * cddb_check_title;
GtkWidget * cddb_check_artist;
GtkWidget * cddb_check_record;
#endif /* HAVE_CDDB */

GtkWidget * fs_radio_preset;
GtkWidget * fs_radio_regexp;
GtkWidget * fs_check_rm_number;
GtkWidget * fs_check_rm_ext;
GtkWidget * fs_check_underscore;
GtkWidget * fs_entry_regexp1;
GtkWidget * fs_entry_regexp2;
GtkWidget * fs_label_regexp;
GtkWidget * fs_label_repl;
GtkWidget * fs_label_input;
GtkWidget * fs_entry_input;
GtkWidget * fs_button_test;
GtkWidget * fs_entry_output;
GtkWidget * fs_label_error;

int artist_sort_by = ARTIST_SORT_NAME_LOW;
int record_sort_by = RECORD_SORT_YEAR;
int reset_existing_data = 0;
int add_year_to_comment = 0;

int pri_meta_first = 1;

int excl_enabled = 1;
char ** excl_patternv = NULL;
int incl_enabled = 0;
char ** incl_patternv = NULL;

int cap_enabled = 0;
int cap_pre_enabled = 1;
int cap_low_enabled = 0;
int cap_mode = CAP_ALL_WORDS;
char ** cap_pre_stringv = NULL;

int rm_spaces_enabled = 1;

int meta_enable = 1;
int meta_wspace = 1;
int meta_title = 1;
int meta_artist = 1;
int meta_record = 1;
int meta_rva = 1;
int meta_comment = 1;

int cddb_enable = 1;
int cddb_title = 1;
int cddb_artist = 1;
int cddb_record = 1;

int fs_preset = 1;
int fs_rm_number = 1;
int fs_rm_ext = 1;
int fs_underscore = 1;

char fs_regexp[MAXLEN];
char fs_replacement[MAXLEN];
regex_t fs_compiled;

char root[MAXLEN];


void transform_filename(char * dest, char * src);


map_t *
map_new(char * str) {

	map_t * map;

	if ((map = (map_t *)malloc(sizeof(map_t))) == NULL) {
		fprintf(stderr, "build_store.c: map_new(): malloc error\n");
		return NULL;
	}

	strncpy(map->str, str, MAXLEN-1);
	map->count = 1;
	map->next = NULL;

	return map;
}

void
map_put(map_t ** map, char * str) {

	map_t * pmap;
	map_t * _pmap;

	if (*map == NULL) {
		*map = map_new(str);
	} else {

		for (_pmap = pmap = *map; pmap; _pmap = pmap, pmap = pmap->next) {

			char * key1 = g_utf8_casefold(str, -1);
			char * key2 = g_utf8_casefold(pmap->str, -1);

			if (!g_utf8_collate(key1, key2)) {
				pmap->count++;
				g_free(key1);
				g_free(key2);
				return;
			}

			g_free(key1);
			g_free(key2);
		}

		_pmap->next = map_new(str);
	}
}

char *
map_get_max(map_t * map) {

	map_t * pmap;
	int max = 0;
	char * str = NULL;

	for (pmap = map; pmap; pmap = pmap->next) {
		if (max < pmap->count) {
			str = pmap->str;
			max = pmap->count;
		}
	}

	return str;
}

void
map_free(map_t * map) {

	map_t * pmap;

	for (pmap = map; pmap; map = pmap) {
		pmap = map->next;
		free(map);
	}
}


char *
filter_string(const char * str) {

	int len;
	char * tmp;
	int i;
	int j;


	len = strlen(str);

	if (len == 0) {
		return NULL;
	}

	if ((tmp = (char *)malloc((len + 1) * sizeof(char))) == NULL) {
		fprintf(stderr, "build_store.c: filter_string(): malloc error\n");
		return NULL;
	}

	for (i = 0, j = 0; i < len; i++) {
		if (str[i] != ' ' &&
		    str[i] != '.' &&
		    str[i] != ',' &&
		    str[i] != '?' &&
		    str[i] != '!' &&
		    str[i] != '\'' &&
		    str[i] != '"' &&
		    str[i] != '-' &&
		    str[i] != '_' &&
		    str[i] != '/' &&
		    str[i] != '(' &&
		    str[i] != ')' &&
		    str[i] != '[' &&
		    str[i] != ']' &&
		    str[i] != '{' &&
		    str[i] != '}') {
			tmp[j++] = str[i];
		}
	}

	tmp[j] = '\0';

	return tmp;
}

int
collate(const char * str1, const char * str2) {

	char * tmp1 = filter_string(str1);
	char * tmp2 = filter_string(str2);

	char * key1 = g_utf8_casefold(tmp1, -1);;
	char * key2 = g_utf8_casefold(tmp2, -1);;

	int i = g_utf8_collate(key1, key2);

	g_free(key1);
	g_free(key2);
	free(tmp1);
	free(tmp2);

	return i;
}


/* return:
     0: if iter was newly created
     1: if iter has already existed
*/

int
store_get_iter_for_artist_and_record(GtkTreeIter * store_iter,
				     GtkTreeIter * artist_iter,
				     GtkTreeIter * record_iter,
				     build_record_t * record) {
	int i;
	int j;

	i = 0;
	while (gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(music_store),
					     artist_iter, store_iter, i++)) {

		char * artist_name;

		gtk_tree_model_get(GTK_TREE_MODEL(music_store), artist_iter,
				   0, &artist_name, -1);

		if (collate(record->artist, artist_name)) {
			continue;
		}

		j = 0;
		while (gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(music_store),
						     record_iter, artist_iter, j++)) {
			char * record_name;

			gtk_tree_model_get(GTK_TREE_MODEL(music_store), record_iter,
					   0, &record_name, -1);

			if (!collate(record->record, record_name)) {
				return 1;
			}
		}

		/* create record */
		gtk_tree_store_append(music_store, record_iter, artist_iter);
                if (options.enable_ms_tree_icons) {
                        gtk_tree_store_set(music_store, record_iter,
                                           0, record->record, 1, record->record_sort_name,
                                           2, "", 3, record->record_comment, 9, icon_record, -1);
                } else {
                        gtk_tree_store_set(music_store, record_iter,
                                           0, record->record, 1, record->record_sort_name,
                                           2, "", 3, record->record_comment, -1);
                }
		return 0;
	}

	/* create both artist and record */
	gtk_tree_store_append(music_store, artist_iter, store_iter);
        if (options.enable_ms_tree_icons) {
                gtk_tree_store_set(music_store, artist_iter,
                                   0, record->artist, 1, record->artist_sort_name,
                                   2, "", 3, "", 9, icon_artist, -1);
        } else {
                gtk_tree_store_set(music_store, artist_iter,
                                   0, record->artist, 1, record->artist_sort_name,
                                   2, "", 3, "", -1);
        }

	gtk_tree_store_append(music_store, record_iter, artist_iter);
        if (options.enable_ms_tree_icons) {
                gtk_tree_store_set(music_store, record_iter,
                                   0, record->record, 1, record->record_sort_name,
                                   2, "", 3, record->record_comment, 9, icon_record, -1);
        } else {
                gtk_tree_store_set(music_store, record_iter,
                                   0, record->record, 1, record->record_sort_name,
                                   2, "", 3, record->record_comment, -1);
        }
	return 0;
}

int
artist_get_iter_for_record(GtkTreeIter * artist_iter,
			   GtkTreeIter * record_iter,
			   build_record_t * record) {
	int i = 0;

	while (gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(music_store),
					     record_iter, artist_iter, i++)) {
		char * record_name;

		gtk_tree_model_get(GTK_TREE_MODEL(music_store), record_iter,
				   0, &record_name, -1);

		if (!collate(record->record, record_name)) {
			return 1;
		}
	}

	/* create record */
	gtk_tree_store_append(music_store, record_iter, artist_iter);
        if (options.enable_ms_tree_icons) {
                gtk_tree_store_set(music_store, record_iter,
                                   0, record->record, 1, record->record_sort_name,
                                   2, "", 3, record->record_comment, 9, icon_record, -1);
        } else {
                gtk_tree_store_set(music_store, record_iter,
                                   0, record->record, 1, record->record_sort_name,
                                   2, "", 3, record->record_comment, -1);
        }
	return 0;
}


void
meta_check_enable_toggled(GtkWidget * widget, gpointer * data) {

	gboolean state = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(meta_check_enable));

	gtk_widget_set_sensitive(meta_check_wspace, state);
	gtk_widget_set_sensitive(meta_check_title, state);
	gtk_widget_set_sensitive(meta_check_artist, state);
	gtk_widget_set_sensitive(meta_check_record, state);
	gtk_widget_set_sensitive(meta_check_rva, state);
	gtk_widget_set_sensitive(meta_check_comment, state);
}


#ifdef HAVE_CDDB
void
cddb_check_enable_toggled(GtkWidget * widget, gpointer * data) {

	gboolean state = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(cddb_check_enable));

	gtk_widget_set_sensitive(cddb_check_title, state);
	gtk_widget_set_sensitive(cddb_check_artist, state);
	gtk_widget_set_sensitive(cddb_check_record, state);
}
#endif /* HAVE_CDDB */


void
fs_radio_preset_toggled(GtkWidget * widget, gpointer * data) {

	gboolean state = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(fs_radio_preset));

	gtk_widget_set_sensitive(fs_check_rm_number, state);
	gtk_widget_set_sensitive(fs_check_rm_ext, state);
	gtk_widget_set_sensitive(fs_check_underscore, state);

	gtk_widget_set_sensitive(fs_entry_regexp1, !state);
	gtk_widget_set_sensitive(fs_entry_regexp2, !state);
	gtk_widget_set_sensitive(fs_label_regexp, !state);
	gtk_widget_set_sensitive(fs_label_repl, !state);
	gtk_widget_set_sensitive(fs_label_error, !state);
}


void
gen_check_excl_toggled(GtkWidget * widget, gpointer * data) {

	gboolean state = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(gen_check_excl));
	gtk_widget_set_sensitive(gen_entry_excl, state);
}


void
gen_check_incl_toggled(GtkWidget * widget, gpointer * data) {

	gboolean state = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(gen_check_incl));
	gtk_widget_set_sensitive(gen_entry_incl, state);
}


void
gen_check_cap_pre_toggled(GtkWidget * widget, gpointer * data) {

	gboolean state = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(gen_check_cap_pre));
	gtk_widget_set_sensitive(gen_entry_cap_pre, state);
}


void
gen_check_cap_toggled(GtkWidget * widget, gpointer * data) {

	gboolean state = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(gen_check_cap));
	gtk_widget_set_sensitive(gen_combo_cap, state);
	gtk_widget_set_sensitive(gen_check_cap_pre, state);
	gtk_widget_set_sensitive(gen_check_cap_low, state);

	if (state == FALSE) {
		gtk_widget_set_sensitive(gen_entry_cap_pre, FALSE);
	} else {
		gen_check_cap_pre_toggled(NULL, NULL);
	}
}


int
browse_button_clicked(GtkWidget * widget, gpointer * data) {

        GtkWidget * dialog;
	const gchar * selected_filename = gtk_entry_get_text(GTK_ENTRY(data));


        dialog = gtk_file_chooser_dialog_new(_("Please select the root directory."),
                                             GTK_WINDOW(browser_window),
					     GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
                                             GTK_STOCK_APPLY, GTK_RESPONSE_ACCEPT,
                                             GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
                                             NULL);

	if (options.show_hidden) {
		gtk_file_chooser_set_show_hidden(GTK_FILE_CHOOSER(dialog), options.show_hidden);
	} 

        deflicker();

        if (strlen(selected_filename)) {
      		char * locale = g_locale_from_utf8(selected_filename, -1, NULL, NULL, NULL);
                gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dialog), locale);
		g_free(locale);
	} else {
                gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dialog), options.currdir);
	}

        gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER_ON_PARENT);
        gtk_window_set_default_size(GTK_WINDOW(dialog), 580, 390);
        gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);


        if (aqualung_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {

		char * utf8;

                selected_filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
		utf8 = g_locale_to_utf8(selected_filename, -1, NULL, NULL, NULL);
		gtk_entry_set_text(GTK_ENTRY(data), utf8);

                strncpy(options.currdir, selected_filename, MAXLEN-1);
		g_free(utf8);
        }


        gtk_widget_destroy(dialog);

	return 0;
}


int
test_button_clicked(GtkWidget * widget, gpointer * data) {

	char * input = (char *)gtk_entry_get_text(GTK_ENTRY(data));
	char output[MAXLEN];
	int err;

	fs_preset = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(fs_radio_preset));
	fs_rm_number = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(fs_check_rm_number));
	fs_rm_ext = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(fs_check_rm_ext));
	fs_underscore = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(fs_check_underscore));

	strncpy(fs_regexp, gtk_entry_get_text(GTK_ENTRY(fs_entry_regexp1)), MAXLEN-1);
	strncpy(fs_replacement, gtk_entry_get_text(GTK_ENTRY(fs_entry_regexp2)), MAXLEN-1);

	gtk_widget_hide(fs_label_error);
	gtk_entry_set_text(GTK_ENTRY(fs_entry_output), "");

	if (!fs_preset) {
		if ((err = regcomp(&fs_compiled, fs_regexp, REG_EXTENDED))) {

			char msg[MAXLEN];
			char * utf8;

			regerror(err, &fs_compiled, msg, MAXLEN);
			utf8 = g_locale_to_utf8(msg, -1, NULL, NULL, NULL);
			gtk_label_set_text(GTK_LABEL(fs_label_error), utf8);
			gtk_widget_show(fs_label_error);
			g_free(utf8);

			regfree(&fs_compiled);
			gtk_widget_grab_focus(fs_entry_regexp1);
			return 0;
		}

		if (!regexec(&fs_compiled, "", 0, NULL, 0)) {
			gtk_label_set_text(GTK_LABEL(fs_label_error),
					   _("Regexp matches empty string"));
			gtk_widget_show(fs_label_error);
			regfree(&fs_compiled);
			gtk_widget_grab_focus(fs_entry_regexp1);
			return 0;
		}
	}

	transform_filename(output, input);

	if (fs_preset) {
		regfree(&fs_compiled);
	}

	gtk_entry_set_text(GTK_ENTRY(fs_entry_output), output);

	return 0;
}

int
build_dialog(void) {

	GtkWidget * dialog;
	GtkWidget * notebook;
	GtkWidget * label;
	GtkWidget * table;
	GtkWidget * hbox;

	GtkWidget * root_entry;
	GtkWidget * browse_button;

	GtkWidget * gen_vbox;
	GtkWidget * gen_path_frame = NULL;

	GtkWidget * gen_artist_frame;
	GtkWidget * gen_artist_vbox;
	GtkWidget * gen_artist_combo;

	GtkWidget * gen_record_frame;
	GtkWidget * gen_record_vbox;
	GtkWidget * gen_record_combo;

	GtkWidget * gen_excl_frame;
	GtkWidget * gen_excl_frame_hbox;
	GtkWidget * gen_incl_frame;
	GtkWidget * gen_incl_frame_hbox;

	GtkWidget * gen_cap_frame;
	GtkWidget * gen_cap_frame_vbox;

	GtkWidget * gen_check_rm_spaces;

	GtkWidget * gen_check_reset_data;
	GtkWidget * gen_check_add_year;

	GtkWidget * meta_vbox;
	GtkWidget * meta_frame;
	GtkWidget * meta_frame_vbox;

	GtkWidget * fs_vbox;
	GtkWidget * fs_frame_preset;
	GtkWidget * fs_frame_preset_vbox;
	GtkWidget * fs_frame_regexp;
	GtkWidget * fs_frame_regexp_vbox;
	GtkWidget * fs_frame_sandbox;

#ifdef HAVE_CDDB
	GtkWidget * gen_pri_frame;
	GtkWidget * gen_pri_combo;
	GtkWidget * gen_pri_vbox;
	GtkWidget * cddb_vbox;
	GtkWidget * cddb_frame;
	GtkWidget * cddb_frame_vbox;
#endif /* HAVE_CDDB */

	GdkColor red = { 0, 30000, 0, 0 };

	char * title = NULL;
        int i, ret;
	char filter[20480];
	char * pfilter = NULL;


	if (build_type == BUILD_STORE) {
		title = _("Build/Update store");
	} else {
		title = _("Build/Update artist");
	}

        dialog = gtk_dialog_new_with_buttons(title,
					     GTK_WINDOW(browser_window),
					     GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_NO_SEPARATOR,
					     GTK_STOCK_OK, GTK_RESPONSE_ACCEPT,
					     GTK_STOCK_CANCEL, GTK_RESPONSE_REJECT,
					     NULL);

	gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER);
        gtk_window_set_default_size(GTK_WINDOW(dialog), 400, -1);
        gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_REJECT);

	notebook = gtk_notebook_new();
	gtk_notebook_set_tab_pos(GTK_NOTEBOOK(notebook), GTK_POS_TOP);
	gtk_container_add(GTK_CONTAINER(GTK_DIALOG(dialog)->vbox), notebook);


	/* General */

        gen_vbox = gtk_vbox_new(FALSE, 0);
        gtk_container_set_border_width(GTK_CONTAINER(gen_vbox), 5);
	label = gtk_label_new(_("General"));
	gtk_notebook_append_page(GTK_NOTEBOOK(notebook), gen_vbox, label);

	if (build_type == BUILD_STORE) {
		gen_path_frame = gtk_frame_new(_("Root path"));
	} else {
		gen_path_frame = gtk_frame_new(_("Artist path"));
	}
        gtk_box_pack_start(GTK_BOX(gen_vbox), gen_path_frame, FALSE, FALSE, 5);

        hbox = gtk_hbox_new(FALSE, 0);
        gtk_container_set_border_width(GTK_CONTAINER(hbox), 5);
	gtk_container_add(GTK_CONTAINER(gen_path_frame), hbox);

        root_entry = gtk_entry_new ();
        gtk_entry_set_max_length(GTK_ENTRY(root_entry), MAXLEN-1);
        gtk_box_pack_start(GTK_BOX(hbox), root_entry, TRUE, TRUE, 5);

	browse_button = gui_stock_label_button(_("_Browse..."), GTK_STOCK_OPEN);
        gtk_box_pack_start(GTK_BOX(hbox), browse_button, FALSE, TRUE, 2);
        g_signal_connect(G_OBJECT(browse_button), "clicked", G_CALLBACK(browse_button_clicked),
			 (gpointer *)root_entry);

#ifdef HAVE_CDDB
	gen_pri_frame = gtk_frame_new(_("Data source priority"));
        gtk_box_pack_start(GTK_BOX(gen_vbox), gen_pri_frame, FALSE, FALSE, 5);

        gen_pri_vbox = gtk_vbox_new(FALSE, 0);
        gtk_container_set_border_width(GTK_CONTAINER(gen_pri_vbox), 5);
	gtk_container_add(GTK_CONTAINER(gen_pri_frame), gen_pri_vbox);

	gen_pri_combo = gtk_combo_box_new_text();
        gtk_box_pack_start(GTK_BOX(gen_pri_vbox), gen_pri_combo, FALSE, FALSE, 0);

	gtk_combo_box_append_text(GTK_COMBO_BOX(gen_pri_combo), _("Metadata, CDDB, Filesystem"));
	gtk_combo_box_append_text(GTK_COMBO_BOX(gen_pri_combo), _("CDDB, Metadata, Filesystem"));

	if (pri_meta_first) {
		gtk_combo_box_set_active(GTK_COMBO_BOX(gen_pri_combo), 0);
	} else {
		gtk_combo_box_set_active(GTK_COMBO_BOX(gen_pri_combo), 1);
	}

#endif /* HAVE_CDDB */


	gen_artist_frame = gtk_frame_new(_("Sort artists by"));
        gtk_box_pack_start(GTK_BOX(gen_vbox), gen_artist_frame, FALSE, FALSE, 5);

        gen_artist_vbox = gtk_vbox_new(FALSE, 0);
        gtk_container_set_border_width(GTK_CONTAINER(gen_artist_vbox), 5);
	gtk_container_add(GTK_CONTAINER(gen_artist_frame), gen_artist_vbox);

	gen_artist_combo = gtk_combo_box_new_text();
        gtk_box_pack_start(GTK_BOX(gen_artist_vbox), gen_artist_combo, FALSE, FALSE, 0);

	gtk_combo_box_append_text(GTK_COMBO_BOX(gen_artist_combo), _("Artist name"));
	gtk_combo_box_append_text(GTK_COMBO_BOX(gen_artist_combo), _("Artist name (lowercase)"));
	gtk_combo_box_append_text(GTK_COMBO_BOX(gen_artist_combo), _("Directory name"));
	gtk_combo_box_append_text(GTK_COMBO_BOX(gen_artist_combo), _("Directory name (lowercase)"));

	switch (artist_sort_by) {
	case ARTIST_SORT_NAME:
		gtk_combo_box_set_active(GTK_COMBO_BOX(gen_artist_combo), 0);
		break;
	case ARTIST_SORT_NAME_LOW:
		gtk_combo_box_set_active(GTK_COMBO_BOX(gen_artist_combo), 1);
		break;
	case ARTIST_SORT_DIR:
		gtk_combo_box_set_active(GTK_COMBO_BOX(gen_artist_combo), 2);
		break;
	case ARTIST_SORT_DIR_LOW:
		gtk_combo_box_set_active(GTK_COMBO_BOX(gen_artist_combo), 3);
		break;
	}


	gen_record_frame = gtk_frame_new(_("Sort records by"));
        gtk_box_pack_start(GTK_BOX(gen_vbox), gen_record_frame, FALSE, FALSE, 5);

        gen_record_vbox = gtk_vbox_new(FALSE, 0);
        gtk_container_set_border_width(GTK_CONTAINER(gen_record_vbox), 5);
	gtk_container_add(GTK_CONTAINER(gen_record_frame), gen_record_vbox);

	gen_record_combo = gtk_combo_box_new_text();
        gtk_box_pack_start(GTK_BOX(gen_record_vbox), gen_record_combo, FALSE, FALSE, 0);

	gtk_combo_box_append_text(GTK_COMBO_BOX(gen_record_combo), _("Year"));
	gtk_combo_box_append_text(GTK_COMBO_BOX(gen_record_combo), _("Record name"));
	gtk_combo_box_append_text(GTK_COMBO_BOX(gen_record_combo), _("Record name (lowercase)"));
	gtk_combo_box_append_text(GTK_COMBO_BOX(gen_record_combo), _("Directory name"));
	gtk_combo_box_append_text(GTK_COMBO_BOX(gen_record_combo), _("Directory name (lowercase)"));

	switch (record_sort_by) {
	case RECORD_SORT_YEAR:
		gtk_combo_box_set_active(GTK_COMBO_BOX(gen_record_combo), 0);
		break;
	case RECORD_SORT_NAME:
		gtk_combo_box_set_active(GTK_COMBO_BOX(gen_record_combo), 1);
		break;
	case RECORD_SORT_NAME_LOW:
		gtk_combo_box_set_active(GTK_COMBO_BOX(gen_record_combo), 2);
		break;
	case RECORD_SORT_DIR:
		gtk_combo_box_set_active(GTK_COMBO_BOX(gen_record_combo), 3);
		break;
	case RECORD_SORT_DIR_LOW:
		gtk_combo_box_set_active(GTK_COMBO_BOX(gen_record_combo), 4);
		break;
	}


	gen_excl_frame = gtk_frame_new(_("Exclude files matching wildcard"));
        gtk_box_pack_start(GTK_BOX(gen_vbox), gen_excl_frame, FALSE, FALSE, 5);
        gen_excl_frame_hbox = gtk_hbox_new(FALSE, 0);
        gtk_container_set_border_width(GTK_CONTAINER(gen_excl_frame_hbox), 5);
	gtk_container_add(GTK_CONTAINER(gen_excl_frame), gen_excl_frame_hbox);

	gen_check_excl = gtk_check_button_new();
	gtk_widget_set_name(gen_check_excl, "check_on_notebook");
        gtk_box_pack_start(GTK_BOX(gen_excl_frame_hbox), gen_check_excl, FALSE, FALSE, 0);

	g_signal_connect(G_OBJECT(gen_check_excl), "toggled",
			 G_CALLBACK(gen_check_excl_toggled), NULL);

        gen_entry_excl = gtk_entry_new();
        gtk_entry_set_max_length(GTK_ENTRY(gen_entry_excl), MAXLEN-1);
	gtk_entry_set_text(GTK_ENTRY(gen_entry_excl), "*.jpg,*.jpeg,*.png,*.gif,*.pls,*.m3u,*.cue,*.xml,*.html,*.htm,*.txt,*.avi");
        gtk_box_pack_end(GTK_BOX(gen_excl_frame_hbox), gen_entry_excl, TRUE, TRUE, 0);

	if (excl_enabled) {
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(gen_check_excl), TRUE);
	} else {
		gtk_widget_set_sensitive(gen_entry_excl, FALSE);
	}

	gen_incl_frame = gtk_frame_new(_("Include only files matching wildcard"));
        gtk_box_pack_start(GTK_BOX(gen_vbox), gen_incl_frame, FALSE, FALSE, 5);
        gen_incl_frame_hbox = gtk_hbox_new(FALSE, 0);
        gtk_container_set_border_width(GTK_CONTAINER(gen_incl_frame_hbox), 5);
	gtk_container_add(GTK_CONTAINER(gen_incl_frame), gen_incl_frame_hbox);

	gen_check_incl = gtk_check_button_new();
	gtk_widget_set_name(gen_check_incl, "check_on_notebook");
        gtk_box_pack_start(GTK_BOX(gen_incl_frame_hbox), gen_check_incl, FALSE, FALSE, 0);

	g_signal_connect(G_OBJECT(gen_check_incl), "toggled",
			 G_CALLBACK(gen_check_incl_toggled), NULL);

        gen_entry_incl = gtk_entry_new();
        gtk_entry_set_max_length(GTK_ENTRY(gen_entry_incl), MAXLEN-1);

	filter[0] = '\0';

#ifdef HAVE_SNDFILE
	strcat(filter, "*.wav,");
#endif /* HAVE_SNDFILE */

#ifdef HAVE_FLAC
	strcat(filter, "*.flac,");
#endif /* HAVE_FLAC */

#ifdef HAVE_OGG_VORBIS
	strcat(filter, "*.ogg,");
#endif /* HAVE_OGG_VORBIS */

#ifdef HAVE_MPEG
	for (i = 0; valid_extensions_mpeg[i] != NULL; i++) {
		strcat(filter, "*.");
		strcat(filter, valid_extensions_mpeg[i]);
		strcat(filter, ",");
	}
#endif /* HAVE_MPEG */

#ifdef HAVE_SPEEX
	strcat(filter, "*.spx,");
#endif /* HAVE_SPEEX */

#ifdef HAVE_MPC
	strcat(filter, "*.mpc,");
#endif /* HAVE_MPC */

#ifdef HAVE_MAC
	strcat(filter, "*.ape,");
#endif /* HAVE_MAC */

#ifdef HAVE_MOD
	for (i = 0; valid_extensions_mod[i] != NULL; i++) {
		strcat(filter, "*.");
		strcat(filter, valid_extensions_mod[i]);
		strcat(filter, ",");
	}
#endif /* HAVE_MOD */

	if ((pfilter = strrchr(filter, ',')) != NULL) {
		*pfilter = '\0';
	}

	gtk_entry_set_text(GTK_ENTRY(gen_entry_incl), filter);
        gtk_box_pack_end(GTK_BOX(gen_incl_frame_hbox), gen_entry_incl, TRUE, TRUE, 0);

	if (incl_enabled) {
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(gen_check_incl), TRUE);
	} else {
		gtk_widget_set_sensitive(gen_entry_incl, FALSE);
	}


	gen_cap_frame = gtk_frame_new(_("Capitalization"));
        gtk_box_pack_start(GTK_BOX(gen_vbox), gen_cap_frame, FALSE, FALSE, 5);

        gen_cap_frame_vbox = gtk_vbox_new(FALSE, 0);
        gtk_container_set_border_width(GTK_CONTAINER(gen_cap_frame_vbox), 5);
	gtk_container_add(GTK_CONTAINER(gen_cap_frame), gen_cap_frame_vbox);

	table = gtk_table_new(2, 2, FALSE);
        gtk_box_pack_start(GTK_BOX(gen_cap_frame_vbox), table, FALSE, FALSE, 0);

	gen_check_cap = gtk_check_button_new_with_label(_("Capitalize: "));
	gtk_widget_set_name(gen_check_cap, "check_on_notebook");
	gtk_table_attach(GTK_TABLE(table), gen_check_cap, 0, 1, 0, 1,
			 GTK_FILL, GTK_FILL, 0, 0);

	g_signal_connect(G_OBJECT(gen_check_cap), "toggled",
			 G_CALLBACK(gen_check_cap_toggled), NULL);

	gen_combo_cap = gtk_combo_box_new_text();
	gtk_table_attach(GTK_TABLE(table), gen_combo_cap, 1, 2, 0, 1,
			 GTK_EXPAND | GTK_FILL, GTK_FILL, 5, 0);

	gtk_combo_box_append_text(GTK_COMBO_BOX(gen_combo_cap), _("All words"));
	gtk_combo_box_append_text(GTK_COMBO_BOX(gen_combo_cap), _("First word only"));

	gen_check_cap_pre = gtk_check_button_new_with_label(_("Force case: "));
	gtk_widget_set_name(gen_check_cap_pre, "check_on_notebook");
	gtk_table_attach(GTK_TABLE(table), gen_check_cap_pre, 0, 1, 1, 2,
			 GTK_FILL, GTK_FILL, 0, 5);

	g_signal_connect(G_OBJECT(gen_check_cap_pre), "toggled",
			 G_CALLBACK(gen_check_cap_pre_toggled), NULL);

        gen_entry_cap_pre = gtk_entry_new();
        gtk_entry_set_max_length(GTK_ENTRY(gen_entry_cap_pre), MAXLEN-1);
	gtk_entry_set_text(GTK_ENTRY(gen_entry_cap_pre), "CD,a),b),c),d),I,II,III,IV,V,VI,VII,VIII,IX,X");
	gtk_table_attach(GTK_TABLE(table), gen_entry_cap_pre, 1, 2, 1, 2,
			 GTK_EXPAND | GTK_FILL, GTK_FILL, 5, 5);

	gen_check_cap_low = gtk_check_button_new_with_label(_("Force other letters to lowercase"));
	gtk_widget_set_name(gen_check_cap_low, "check_on_notebook");
        gtk_box_pack_start(GTK_BOX(gen_cap_frame_vbox), gen_check_cap_low, TRUE, TRUE, 0);


	switch (cap_mode) {
	case CAP_ALL_WORDS:
		gtk_combo_box_set_active(GTK_COMBO_BOX(gen_combo_cap), 0);
		break;
	case CAP_FIRST_WORD:
		gtk_combo_box_set_active(GTK_COMBO_BOX(gen_combo_cap), 1);
		break;
	}

	if (cap_pre_enabled) {
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(gen_check_cap_pre), TRUE);
	} else {
		gtk_widget_set_sensitive(gen_combo_cap, FALSE);
	}

	if (cap_low_enabled) {
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(gen_check_cap_low), TRUE);
	}

	if (cap_enabled) {
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(gen_check_cap), TRUE);
	} else {
		gtk_widget_set_sensitive(gen_combo_cap, FALSE);
		gtk_widget_set_sensitive(gen_check_cap_pre, FALSE);
		gtk_widget_set_sensitive(gen_entry_cap_pre, FALSE);
		gtk_widget_set_sensitive(gen_check_cap_low, FALSE);
	}


	gen_check_rm_spaces =
	     gtk_check_button_new_with_label(_("Trim leading, tailing and duplicate spaces"));
	gtk_widget_set_name(gen_check_rm_spaces, "check_on_notebook");
        gtk_box_pack_start(GTK_BOX(gen_vbox), gen_check_rm_spaces, FALSE, FALSE, 0);

	if (rm_spaces_enabled) {
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(gen_check_rm_spaces), TRUE);
	}

	gen_check_add_year =
		gtk_check_button_new_with_label(_("Add year to the comments of new records"));
	gtk_widget_set_name(gen_check_add_year, "check_on_notebook");
        gtk_box_pack_start(GTK_BOX(gen_vbox), gen_check_add_year, FALSE, FALSE, 0);

	if (add_year_to_comment) {
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(gen_check_add_year), TRUE);
	}


	gen_check_reset_data =
		gtk_check_button_new_with_label(_("Reread data for existing tracks"));
	gtk_widget_set_name(gen_check_reset_data, "check_on_notebook");
        gtk_box_pack_start(GTK_BOX(gen_vbox), gen_check_reset_data, FALSE, FALSE, 0);

	if (reset_existing_data) {
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(gen_check_reset_data), TRUE);
	}


	/* Metadata */

        meta_vbox = gtk_vbox_new(FALSE, 0);
        gtk_container_set_border_width(GTK_CONTAINER(meta_vbox), 5);
	label = gtk_label_new(_("Metadata"));
	gtk_notebook_append_page(GTK_NOTEBOOK(notebook), meta_vbox, label);

	meta_check_enable =
		gtk_check_button_new_with_label(_("Import file metadata"));
	gtk_widget_set_name(meta_check_enable, "check_on_notebook");
        gtk_box_pack_start(GTK_BOX(meta_vbox), meta_check_enable, FALSE, FALSE, 5);

	if (meta_enable) {
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(meta_check_enable), TRUE);
	}

	g_signal_connect(G_OBJECT(meta_check_enable), "toggled",
			 G_CALLBACK(meta_check_enable_toggled), NULL);

	meta_check_wspace =
		gtk_check_button_new_with_label(_("Exclude whitespace-only metadata"));
	gtk_widget_set_name(meta_check_wspace, "check_on_notebook");
        gtk_box_pack_start(GTK_BOX(meta_vbox), meta_check_wspace, FALSE, FALSE, 0);

	if (meta_wspace) {
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(meta_check_wspace), TRUE);
	}


	meta_frame = gtk_frame_new(_("Import"));
        gtk_box_pack_start(GTK_BOX(meta_vbox), meta_frame, FALSE, FALSE, 5);

        meta_frame_vbox = gtk_vbox_new(FALSE, 0);
        gtk_container_set_border_width(GTK_CONTAINER(meta_frame_vbox), 5);
        gtk_container_add(GTK_CONTAINER(meta_frame), meta_frame_vbox);

	meta_check_artist =
		gtk_check_button_new_with_label(_("Artist name"));
	gtk_widget_set_name(meta_check_artist, "check_on_notebook");

	if (build_type == BUILD_STORE) {
		gtk_box_pack_start(GTK_BOX(meta_frame_vbox), meta_check_artist, FALSE, FALSE, 0);
	}

	if (meta_artist) {
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(meta_check_artist), TRUE);
	}

	meta_check_record =
		gtk_check_button_new_with_label(_("Record name"));
	gtk_widget_set_name(meta_check_record, "check_on_notebook");
        gtk_box_pack_start(GTK_BOX(meta_frame_vbox), meta_check_record, FALSE, FALSE, 0);

	if (meta_record) {
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(meta_check_record), TRUE);
	}

	meta_check_title =
		gtk_check_button_new_with_label(_("Track name"));
	gtk_widget_set_name(meta_check_title, "check_on_notebook");
        gtk_box_pack_start(GTK_BOX(meta_frame_vbox), meta_check_title, FALSE, FALSE, 0);

	if (meta_title) {
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(meta_check_title), TRUE);
	}

	meta_check_comment =
		gtk_check_button_new_with_label(_("Comment"));
	gtk_widget_set_name(meta_check_comment, "check_on_notebook");
        gtk_box_pack_start(GTK_BOX(meta_frame_vbox), meta_check_comment, FALSE, FALSE, 0);

	if (meta_comment) {
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(meta_check_comment), TRUE);
	}

	meta_check_rva =
		gtk_check_button_new_with_label(_("Replaygain tag as manual RVA"));
	gtk_widget_set_name(meta_check_rva, "check_on_notebook");
        gtk_box_pack_start(GTK_BOX(meta_frame_vbox), meta_check_rva, FALSE, FALSE, 0);

	if (meta_rva) {
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(meta_check_rva), TRUE);
	}

	if (!meta_enable) {
		gtk_widget_set_sensitive(meta_check_title, FALSE);
		gtk_widget_set_sensitive(meta_check_artist, FALSE);
		gtk_widget_set_sensitive(meta_check_record, FALSE);
		gtk_widget_set_sensitive(meta_check_rva, FALSE);
		gtk_widget_set_sensitive(meta_check_comment, FALSE);
		gtk_widget_set_sensitive(meta_check_wspace, FALSE);
	}


	/* CDDB */

#ifdef HAVE_CDDB
        cddb_vbox = gtk_vbox_new(FALSE, 0);
        gtk_container_set_border_width(GTK_CONTAINER(cddb_vbox), 5);
	label = gtk_label_new(_("CDDB"));
	gtk_notebook_append_page(GTK_NOTEBOOK(notebook), cddb_vbox, label);

	cddb_check_enable =
		gtk_check_button_new_with_label(_("Perform CDDB lookup for records"));
	gtk_widget_set_name(cddb_check_enable, "check_on_notebook");
        gtk_box_pack_start(GTK_BOX(cddb_vbox), cddb_check_enable, FALSE, FALSE, 5);

	if (cddb_enable) {
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cddb_check_enable), TRUE);
	}

	g_signal_connect(G_OBJECT(cddb_check_enable), "toggled",
			 G_CALLBACK(cddb_check_enable_toggled), NULL);


	cddb_frame = gtk_frame_new(_("Import"));
        gtk_box_pack_start(GTK_BOX(cddb_vbox), cddb_frame, FALSE, FALSE, 0);

        cddb_frame_vbox = gtk_vbox_new(FALSE, 0);
        gtk_container_set_border_width(GTK_CONTAINER(cddb_frame_vbox), 5);
        gtk_container_add(GTK_CONTAINER(cddb_frame), cddb_frame_vbox);

	cddb_check_artist =
		gtk_check_button_new_with_label(_("Artist name"));
	gtk_widget_set_name(cddb_check_artist, "check_on_notebook");

	if (build_type == BUILD_STORE) {
		gtk_box_pack_start(GTK_BOX(cddb_frame_vbox), cddb_check_artist, FALSE, FALSE, 0);
	}

	if (cddb_artist) {
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cddb_check_artist), TRUE);
	}

	cddb_check_record =
		gtk_check_button_new_with_label(_("Record name"));
	gtk_widget_set_name(cddb_check_record, "check_on_notebook");
        gtk_box_pack_start(GTK_BOX(cddb_frame_vbox), cddb_check_record, FALSE, FALSE, 0);

	if (cddb_record) {
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cddb_check_record), TRUE);
	}

	cddb_check_title =
		gtk_check_button_new_with_label(_("Track name"));
	gtk_widget_set_name(cddb_check_title, "check_on_notebook");
        gtk_box_pack_start(GTK_BOX(cddb_frame_vbox), cddb_check_title, FALSE, FALSE, 0);

	if (cddb_title) {
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cddb_check_title), TRUE);
	}

	if (!cddb_enable) {
		gtk_widget_set_sensitive(cddb_check_title, FALSE);
		gtk_widget_set_sensitive(cddb_check_artist, FALSE);
		gtk_widget_set_sensitive(cddb_check_record, FALSE);
	}
#endif /* HAVE_CDDB */

	/* Filenames */

        fs_vbox = gtk_vbox_new(FALSE, 0);
        gtk_container_set_border_width(GTK_CONTAINER(fs_vbox), 5);
	label = gtk_label_new(_("Filesystem"));
	gtk_notebook_append_page(GTK_NOTEBOOK(notebook), fs_vbox, label);

	fs_frame_preset = gtk_frame_new(NULL);
        gtk_box_pack_start(GTK_BOX(fs_vbox), fs_frame_preset, FALSE, FALSE, 5);
        fs_frame_preset_vbox = gtk_vbox_new(FALSE, 0);
        gtk_container_set_border_width(GTK_CONTAINER(fs_frame_preset_vbox), 5);
	gtk_container_add(GTK_CONTAINER(fs_frame_preset), fs_frame_preset_vbox);

	fs_radio_preset = gtk_radio_button_new_with_label(NULL, _("Predefined transformations"));
	gtk_widget_set_name(fs_radio_preset, "check_on_notebook");
        gtk_frame_set_label_widget(GTK_FRAME(fs_frame_preset), fs_radio_preset);

	if (fs_preset) {
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(fs_radio_preset), TRUE);
	}

	g_signal_connect(G_OBJECT(fs_radio_preset), "toggled",
			 G_CALLBACK(fs_radio_preset_toggled), NULL);

	fs_check_rm_number =
		gtk_check_button_new_with_label(_("Remove leading number"));
	gtk_widget_set_name(fs_check_rm_number, "check_on_notebook");
        gtk_box_pack_start(GTK_BOX(fs_frame_preset_vbox), fs_check_rm_number, FALSE, FALSE, 0);

	if (fs_rm_number) {
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(fs_check_rm_number), TRUE);
	}

	fs_check_rm_ext =
		gtk_check_button_new_with_label(_("Remove file extension"));
	gtk_widget_set_name(fs_check_rm_ext, "check_on_notebook");
        gtk_box_pack_start(GTK_BOX(fs_frame_preset_vbox), fs_check_rm_ext, FALSE, FALSE, 0);

	if (fs_rm_ext) {
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(fs_check_rm_ext), TRUE);
	}

	fs_check_underscore =
		gtk_check_button_new_with_label(_("Convert underscore to space"));
	gtk_widget_set_name(fs_check_underscore, "check_on_notebook");
        gtk_box_pack_start(GTK_BOX(fs_frame_preset_vbox), fs_check_underscore, FALSE, FALSE, 0);

	if (fs_underscore) {
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(fs_check_underscore), TRUE);
	}


	fs_frame_regexp = gtk_frame_new(NULL);
        gtk_box_pack_start(GTK_BOX(fs_vbox), fs_frame_regexp, FALSE, FALSE, 5);
        fs_frame_regexp_vbox = gtk_vbox_new(FALSE, 0);
        gtk_container_set_border_width(GTK_CONTAINER(fs_frame_regexp_vbox), 5);
	gtk_container_add(GTK_CONTAINER(fs_frame_regexp), fs_frame_regexp_vbox);

	fs_radio_regexp = gtk_radio_button_new_with_label_from_widget(
			      GTK_RADIO_BUTTON(fs_radio_preset), _("Regular expression"));
	gtk_widget_set_name(fs_radio_regexp, "check_on_notebook");
	gtk_frame_set_label_widget(GTK_FRAME(fs_frame_regexp), fs_radio_regexp);


	table = gtk_table_new(2, 2, FALSE);
        gtk_box_pack_start(GTK_BOX(fs_frame_regexp_vbox), table, FALSE, FALSE, 5);

        hbox = gtk_hbox_new(FALSE, 0);
	fs_label_regexp = gtk_label_new(_("Regexp:"));
	gtk_widget_set_sensitive(fs_label_regexp, FALSE);
        gtk_box_pack_start(GTK_BOX(hbox), fs_label_regexp, FALSE, FALSE, 5);
	gtk_table_attach(GTK_TABLE(table), hbox, 0, 1, 0, 1,
			 GTK_FILL, GTK_FILL, 0, 5);

	fs_entry_regexp1 = gtk_entry_new();
	gtk_widget_set_sensitive(fs_entry_regexp1, FALSE);
	gtk_table_attach(GTK_TABLE(table), fs_entry_regexp1, 1, 2, 0, 1,
			 GTK_FILL | GTK_EXPAND, GTK_FILL, 5, 5);

        hbox = gtk_hbox_new(FALSE, 0);
	fs_label_repl = gtk_label_new(_("Replace:"));
	gtk_widget_set_sensitive(fs_label_repl, FALSE);
        gtk_box_pack_start(GTK_BOX(hbox), fs_label_repl, FALSE, FALSE, 5);
	gtk_table_attach(GTK_TABLE(table), hbox, 0, 1, 1, 2,
			 GTK_FILL, GTK_FILL, 0, 5);

	fs_entry_regexp2 = gtk_entry_new();
	gtk_widget_set_sensitive(fs_entry_regexp2, FALSE);
	gtk_table_attach(GTK_TABLE(table), fs_entry_regexp2, 1, 2, 1, 2,
			 GTK_FILL | GTK_EXPAND, GTK_FILL, 5, 5);

        hbox = gtk_hbox_new(FALSE, 0);
	fs_label_error = gtk_label_new("");
	gtk_widget_modify_fg(fs_label_error, GTK_STATE_NORMAL, &red);
        gtk_box_pack_start(GTK_BOX(hbox), fs_label_error, FALSE, FALSE, 5);
        gtk_box_pack_start(GTK_BOX(fs_frame_regexp_vbox), hbox, FALSE, FALSE, 0);


	fs_frame_sandbox = gtk_frame_new(_("Sandbox"));
        gtk_box_pack_start(GTK_BOX(fs_vbox), fs_frame_sandbox, FALSE, FALSE, 5);

	table = gtk_table_new(2, 2, FALSE);
        gtk_container_set_border_width(GTK_CONTAINER(table), 5);
	gtk_container_add(GTK_CONTAINER(fs_frame_sandbox), table);

        hbox = gtk_hbox_new(FALSE, 0);
	fs_label_input = gtk_label_new(_("Filename:"));
        gtk_box_pack_start(GTK_BOX(hbox), fs_label_input, FALSE, FALSE, 5);
	gtk_table_attach(GTK_TABLE(table), hbox, 0, 1, 0, 1,
			 GTK_FILL, GTK_FILL, 0, 5);

	fs_entry_input = gtk_entry_new();
	gtk_table_attach(GTK_TABLE(table), fs_entry_input, 1, 2, 0, 1,
			 GTK_FILL | GTK_EXPAND, GTK_FILL, 5, 5);

	fs_button_test = gtk_button_new_with_label(_("Test"));
	gtk_table_attach(GTK_TABLE(table), fs_button_test, 0, 1, 1, 2,
			 GTK_FILL, GTK_FILL, 0, 5);
        g_signal_connect(G_OBJECT(fs_button_test), "clicked", G_CALLBACK(test_button_clicked),
			 (gpointer *)fs_entry_input);

	fs_entry_output = gtk_entry_new();
	gtk_table_attach(GTK_TABLE(table), fs_entry_output, 1, 2, 1, 2,
			 GTK_FILL | GTK_EXPAND, GTK_FILL, 5, 5);


	/* run dialog */

	gtk_widget_show_all(dialog);
	gtk_widget_hide(fs_label_error);
	gtk_widget_grab_focus(root_entry);

 display:

	root[0] = '\0';

        if (aqualung_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {

		char * proot = g_locale_from_utf8(gtk_entry_get_text(GTK_ENTRY(root_entry)), -1, NULL, NULL, NULL);

		if (proot[0] == '~') {
			snprintf(root, MAXLEN-1, "%s%s", options.home, proot + 1);
		} else if (proot[0] == '/') {
			strncpy(root, proot, MAXLEN-1);
		} else if (proot[0] != '\0') {
			snprintf(root, MAXLEN-1, "%s/%s", options.cwd, proot);
		}

		g_free(proot);


		switch (gtk_combo_box_get_active(GTK_COMBO_BOX(gen_artist_combo))) {
		case 0:
			artist_sort_by = ARTIST_SORT_NAME;
			break;
		case 1:
			artist_sort_by = ARTIST_SORT_NAME_LOW;
			break;
		case 2:
			artist_sort_by = ARTIST_SORT_DIR;
			break;
		case 3:
			artist_sort_by = ARTIST_SORT_DIR_LOW;
			break;
		}

		switch (gtk_combo_box_get_active(GTK_COMBO_BOX(gen_record_combo))) {
		case 0:
			record_sort_by = RECORD_SORT_YEAR;
			break;
		case 1:
			record_sort_by = RECORD_SORT_NAME;
			break;
		case 2:
			record_sort_by = RECORD_SORT_NAME_LOW;
			break;
		case 3:
			record_sort_by = RECORD_SORT_DIR;
			break;
		case 4:
			record_sort_by = RECORD_SORT_DIR_LOW;
			break;
		}


		excl_enabled = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(gen_check_excl));

		if (excl_enabled) {
			excl_patternv =
			    g_strsplit(gtk_entry_get_text(GTK_ENTRY(gen_entry_excl)), ",", 0);
		}

		incl_enabled = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(gen_check_incl));

		if (incl_enabled) {
			incl_patternv =
			    g_strsplit(gtk_entry_get_text(GTK_ENTRY(gen_entry_incl)), ",", 0);
		}

		cap_enabled = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(gen_check_cap));

		switch (gtk_combo_box_get_active(GTK_COMBO_BOX(gen_combo_cap))) {
		case 0:
			cap_mode = CAP_ALL_WORDS;
			break;
		case 1:
			cap_mode = CAP_FIRST_WORD;
			break;
		}

		cap_pre_enabled = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(gen_check_cap_pre));
		if (cap_pre_enabled) {
			cap_pre_stringv =
			   g_strsplit(gtk_entry_get_text(GTK_ENTRY(gen_entry_cap_pre)), ",", 0);
		}

		cap_low_enabled = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(gen_check_cap_low));

		rm_spaces_enabled = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(gen_check_rm_spaces));
		add_year_to_comment = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(gen_check_add_year));
		reset_existing_data = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(gen_check_reset_data));

		meta_enable = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(meta_check_enable));
		meta_wspace = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(meta_check_wspace));
		meta_title = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(meta_check_title));
		meta_artist = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(meta_check_artist));
		meta_record = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(meta_check_record));
		meta_rva = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(meta_check_rva));
		meta_comment = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(meta_check_comment));

#ifdef HAVE_CDDB
		switch (gtk_combo_box_get_active(GTK_COMBO_BOX(gen_pri_combo))) {
		case 0:
			pri_meta_first = 1;
			break;
		case 1:
			pri_meta_first = 0;
			break;
		}
		cddb_enable = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(cddb_check_enable));
		cddb_title = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(cddb_check_title));
		cddb_artist = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(cddb_check_artist));
		cddb_record = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(cddb_check_record));
#endif /* HAVE_CDDB */

		fs_preset = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(fs_radio_preset));
		fs_rm_number = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(fs_check_rm_number));
		fs_rm_ext = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(fs_check_rm_ext));
		fs_underscore = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(fs_check_underscore));

		strncpy(fs_regexp, gtk_entry_get_text(GTK_ENTRY(fs_entry_regexp1)), MAXLEN-1);
		strncpy(fs_replacement, gtk_entry_get_text(GTK_ENTRY(fs_entry_regexp2)), MAXLEN-1);

		gtk_widget_hide(fs_label_error);

		if (root[0] == '\0') {
			gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), 0);
			gtk_widget_grab_focus(root_entry);
			goto display;
		}

		if (!fs_preset) {

			int err;

			if ((err = regcomp(&fs_compiled, fs_regexp, REG_EXTENDED))) {

				char msg[MAXLEN];
				char * utf8;

				regerror(err, &fs_compiled, msg, MAXLEN);
				utf8 = g_locale_to_utf8(msg, -1, NULL, NULL, NULL);
				gtk_label_set_text(GTK_LABEL(fs_label_error), utf8);
				gtk_widget_show(fs_label_error);
				g_free(utf8);

				regfree(&fs_compiled);

				gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), 3);
				gtk_widget_grab_focus(fs_entry_regexp1);

				goto display;
			}

			if (!regexec(&fs_compiled, "", 0, NULL, 0)) {
				gtk_label_set_text(GTK_LABEL(fs_label_error),
						   _("Regexp matches empty string"));
				gtk_widget_show(fs_label_error);
				regfree(&fs_compiled);

				gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), 3);
				gtk_widget_grab_focus(fs_entry_regexp1);

				goto display;
			}
		}

		ret = 1;

        } else {
                ret = 0;
        }

        gtk_widget_destroy(dialog);

        return ret;
}


void
prog_window_close(GtkWidget * widget, gpointer data) {

	build_cancelled = 1;

	if (build_prog_window) {
		gtk_widget_destroy(build_prog_window);
		build_prog_window = NULL;
	}
}

void
cancel_build(GtkWidget * widget, gpointer data) {

	build_cancelled = 1;

	if (build_prog_window) {
		gtk_widget_destroy(build_prog_window);
		build_prog_window = NULL;
	}
}

gboolean
finish_build(gpointer data) {

	if (build_prog_window) {
		gtk_widget_destroy(build_prog_window);
		build_prog_window = NULL;
	}

	g_strfreev(cap_pre_stringv);
	cap_pre_stringv = NULL;

	g_strfreev(excl_patternv);
	excl_patternv = NULL;

	g_strfreev(incl_patternv);
	incl_patternv = NULL;

	if (!fs_preset) {
		regfree(&fs_compiled);
	}

	build_thread_state = BUILD_THREAD_FREE;

	return FALSE;
}


void
progress_window(void) {

	GtkWidget * table;
	GtkWidget * label;
	GtkWidget * vbox;
	GtkWidget * hbox;
	GtkWidget * hbuttonbox;
	GtkWidget * hseparator;

	char * title = NULL;

	if (build_type == BUILD_STORE) {
		title = _("Building store from filesystem");
	} else {
		title = _("Building artist from filesystem");
	}

	build_prog_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title(GTK_WINDOW(build_prog_window), title);
        gtk_window_set_position(GTK_WINDOW(build_prog_window), GTK_WIN_POS_CENTER);
        gtk_window_resize(GTK_WINDOW(build_prog_window), 430, 110);
        g_signal_connect(G_OBJECT(build_prog_window), "delete_event",
                         G_CALLBACK(prog_window_close), NULL);
        gtk_container_set_border_width(GTK_CONTAINER(build_prog_window), 5);

        vbox = gtk_vbox_new(FALSE, 0);
        gtk_container_add(GTK_CONTAINER(build_prog_window), vbox);

	table = gtk_table_new(2, 2, FALSE);
        gtk_box_pack_start(GTK_BOX(vbox), table, FALSE, FALSE, 0);

        hbox = gtk_hbox_new(FALSE, 0);
	label = gtk_label_new(_("Directory:"));
        gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, TRUE, 0);
	gtk_table_attach(GTK_TABLE(table), hbox, 0, 1, 0, 1,
			 GTK_FILL, GTK_FILL, 5, 5);

        prog_file_entry = gtk_entry_new();
        gtk_editable_set_editable(GTK_EDITABLE(prog_file_entry), FALSE);
	gtk_table_attach(GTK_TABLE(table), prog_file_entry, 1, 2, 0, 1,
			 GTK_FILL | GTK_EXPAND, GTK_FILL, 5, 5);

        hbox = gtk_hbox_new(FALSE, 0);
	label = gtk_label_new(_("Action:"));
        gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, TRUE, 0);
	gtk_table_attach(GTK_TABLE(table), hbox, 0, 1, 1, 2,
			 GTK_FILL, GTK_FILL, 5, 5);

        hbox = gtk_hbox_new(FALSE, 0);
	prog_action_label = gtk_label_new("");
        gtk_box_pack_start(GTK_BOX(hbox), prog_action_label, FALSE, TRUE, 0);
	gtk_table_attach(GTK_TABLE(table), hbox, 1, 2, 1, 2,
			 GTK_FILL, GTK_FILL, 5, 5);

        hseparator = gtk_hseparator_new();
        gtk_box_pack_start(GTK_BOX(vbox), hseparator, FALSE, TRUE, 5);

	hbuttonbox = gtk_hbutton_box_new();
	gtk_box_pack_end(GTK_BOX(vbox), hbuttonbox, FALSE, TRUE, 0);
	gtk_button_box_set_layout(GTK_BUTTON_BOX(hbuttonbox), GTK_BUTTONBOX_END);

        prog_cancel_button = gui_stock_label_button (_("Abort"), GTK_STOCK_CANCEL); 
        g_signal_connect(prog_cancel_button, "clicked", G_CALLBACK(cancel_build), NULL);
  	gtk_container_add(GTK_CONTAINER(hbuttonbox), prog_cancel_button);   

        gtk_widget_grab_focus(prog_cancel_button);

        gtk_widget_show_all(build_prog_window);
}

gboolean
set_prog_file_entry(gpointer data) {

	if (build_prog_window) {

		char * utf8 = g_filename_display_name((char *)data);
		gtk_entry_set_text(GTK_ENTRY(prog_file_entry), utf8);
		gtk_widget_grab_focus(prog_cancel_button);
		g_free(utf8);
	}

	return FALSE;
}


gboolean
set_prog_action_label(gpointer data) {

	if (build_prog_window) {
		gtk_label_set_text(GTK_LABEL(prog_action_label), (char *)data);
	}

	return FALSE;
}


static int
filter(const struct dirent * de) {

	return de->d_name[0] != '.';
}


int
is_dir(char * name) {

	struct stat st_file;

	if (stat(name, &st_file) == -1) {
		return 0;
	}

	return S_ISDIR(st_file.st_mode);
}


int
is_reg(char * name) {

	struct stat st_file;

	if (stat(name, &st_file) == -1) {
		return 0;
	}

	return S_ISREG(st_file.st_mode);
}


int
is_wspace(char * str) {

	int i;

	for (i = 0; str[i]; i++) {
		if (str[i] != ' ' || str[i] != '\t') {
			return 0;
		}
	}

	return 1;
}

int
is_valid_year(long y) {

	/* Please update when we reach the 22nd century. */
	return y > 1900 && y < 2100;
}

int
num_invalid_tracks(build_record_t * record) {

	int invalid = 0;
	build_track_t * ptrack;

	for (ptrack = record->tracks; ptrack; ptrack = ptrack->next) {
		if (!ptrack->valid) {
			invalid++;
		}
	}

	return invalid;
}

void
transform_filename(char * dest, char * src) {

	if (fs_preset) {

		int i;
		char tmp[MAXLEN];
		char * ptmp = tmp;
		
		strncpy(tmp, src, MAXLEN-1);

		if (fs_rm_number) {
			while (*ptmp && (isdigit(*ptmp) || *ptmp == ' ' ||
					 *ptmp == '_' || *ptmp == '-')) {
				ptmp++;
			}
		}

		if (fs_rm_ext) {
			char * c = NULL;
			if ((c = strrchr(tmp, '.')) != NULL) {
				*c = '\0';
			}
		}

		if (fs_underscore) {
			for (i = 0; ptmp[i]; i++) {
				if (ptmp[i] == '_') {
					ptmp[i] = ' ';
				}
			}
		}

		strncpy(dest, ptmp, MAXLEN-1);

	} else {

		int offs = 0;
		regmatch_t regmatch[10];

		dest[0] = '\0';

		while (!regexec(&fs_compiled, src + offs, 10, regmatch, 0)) {
		
			int i = 0;
			int b = strlen(fs_replacement) - 1;

			strncat(dest, src + offs, regmatch[0].rm_so);

			for (i = 0; i < b; i++) {

				if (fs_replacement[i] == '\\' && isdigit(fs_replacement[i+1])) {

					int j = fs_replacement[i+1] - '0';
				
					if (j == 0 || j > fs_compiled.re_nsub) {
						i++;
						continue;
					}
				
					strncat(dest, src + offs + regmatch[j].rm_so,
						regmatch[j].rm_eo - regmatch[j].rm_so);
					i++;
				} else {
					strncat(dest, fs_replacement + i, 1);
				}
			}
			
			strncat(dest, fs_replacement + i, 1);
			offs += regmatch[0].rm_eo;
		}

		if (!*dest) {
			strncpy(dest, src + offs, MAXLEN-1);
		} else {
			strcat(dest, src + offs);
		}
	}
}


void
cap_pre(char * str) {

	int i = 0;
	int len_str = 0;
	gchar * sub = NULL;
	gchar * haystack = NULL;

	if (cap_pre_stringv[0] == NULL) {
		return;
	}

	len_str = strlen(str);
	haystack = g_utf8_strdown(str, -1);

	for (i = 0; cap_pre_stringv[i]; i++) {
		int len_cap = 0;
		int off = 0;
		gchar * needle = NULL;
		gchar * p = NULL;

		if (*(cap_pre_stringv[i]) == '\0') {
			continue;
		}

		len_cap = strlen(cap_pre_stringv[i]);
		needle = g_utf8_strdown(cap_pre_stringv[i], -1);

		while ((sub = strstr(haystack + off, needle)) != NULL) {
			int len_sub = strlen(sub);

			if (((p = g_utf8_find_prev_char(haystack, sub)) == NULL ||
			     !g_unichar_isalpha(g_utf8_get_char(p))) &&
			    ((p = g_utf8_find_next_char(sub + len_cap - 1, NULL)) == NULL ||
			     !g_unichar_isalpha(g_utf8_get_char(p)))) {
				strncpy(str + len_str - len_sub, cap_pre_stringv[i], len_cap);
			}

			off = len_str - len_sub + len_cap;
		}

		g_free(needle);
	}

	g_free(haystack);
}


int
cap_after(gunichar ch) {

	static gunichar * chars = NULL;
	static int chars_set = 0;

	if (!chars_set) {
		chars = (gunichar *)malloc(6 * sizeof(gunichar));
		chars[0] = g_utf8_get_char(" ");
		chars[1] = g_utf8_get_char("\t");
		chars[2] = g_utf8_get_char("(");
		chars[3] = g_utf8_get_char("[");
		chars[4] = g_utf8_get_char("/");
		chars[5] = g_utf8_get_char("\"");
		chars_set = 1;
	}

	int i;

	for (i = 0; i < 6; i++) {
		if (chars[i] == ch) {
			return 1;
		}
	}

	return 0;
}


void
capitalize(char * str) {

	int n;
	gchar * p = str;
	gchar * result = NULL;

	gunichar prev = 0;

	gchar ** stringv = NULL;

	for (n = 0; *p; p = g_utf8_next_char(p)) {
		gunichar ch = g_utf8_get_char(p);
		gunichar new_ch;
		int len;

		if (prev == 0 || (cap_mode == CAP_ALL_WORDS && cap_after(prev))) {
			new_ch = g_unichar_totitle(ch);
		} else {
			if (cap_low_enabled) {
				new_ch = g_unichar_tolower(ch);
			} else {
				new_ch = ch;
			}
		}

		if ((stringv = (gchar **)realloc(stringv, (n + 2) * sizeof(gchar *))) == NULL) {
			fprintf(stderr, "build_store.c: capitalize(): realloc error\n");
			return;
		}

		len = g_unichar_to_utf8(new_ch, NULL);

		if ((*(stringv + n) = (gchar *)malloc((len + 1) * sizeof(gchar))) == NULL) {
			fprintf(stderr, "build_store.c: capitalize(): malloc error\n");
			return;
		}

		g_unichar_to_utf8(new_ch, *(stringv + n));
		*(*(stringv + n) + len) = '\0';

		prev = ch;
		++n;
	}

	if (stringv == NULL) {
		return;
	}

	*(stringv + n) = NULL;

	result = g_strjoinv(NULL, stringv);
	strncpy(str, result, MAXLEN-1);
	g_free(result);

	while (n >= 0) {
		free(*(stringv + n--));
	}

	free(stringv);

	if (cap_pre_enabled) {
		cap_pre(str);
	}
}


void
remove_spaces(char * str) {

	char tmp[MAXLEN];
	int i = 0;
	int j = 0;
	int len = strlen(str);

	for (i = 0; i < len; i++) {
		if (j == 0 && str[i] == ' ') {
			continue;
		}

		if (str[i] != ' ') {
			tmp[j++] = str[i];
			continue;
		} else {
			if (i + 1 < len && str[i + 1] != ' ') {
				tmp[j++] = ' ';
				continue;
			}
		}
	}

	tmp[j] = '\0';

	strncpy(str, tmp, MAXLEN-1);
}

void
process_meta(build_record_t * record) {

	build_track_t * ptrack;
	metadata * meta = meta_new();

	map_t * map_artist = NULL;
	map_t * map_record = NULL;
	map_t * map_year = NULL;

	char tmp[MAXLEN];

	for (ptrack = record->tracks; ptrack; ptrack = ptrack->next) {

		meta = meta_new();

		if (meta_read(meta, ptrack->filename)) {
			if (meta_artist &&
			    !record->artist_valid && meta_get_artist(meta, tmp)) {
				if (!meta_wspace || !is_wspace(tmp)) {
					map_put(&map_artist, tmp);
				}
			}
			if (meta_record &&
			    !record->record_valid && meta_get_record(meta, tmp)) {
				if (!meta_wspace || !is_wspace(tmp)) {
					map_put(&map_record, tmp);
				}
			}
			if (meta_title &&
			    !ptrack->valid && meta_get_title(meta, ptrack->name)) {
				if (!meta_wspace || !is_wspace(ptrack->name)) {
					ptrack->valid = 1;
				}
			}
			if (!record->year_valid && meta_get_year(meta, tmp)) {
				if (!meta_wspace || !is_wspace(tmp)) {
					long y = strtol(tmp, NULL, 10);
					if (is_valid_year(y)) {
						map_put(&map_year, tmp);
					}
				}
			}
			if (meta_rva) {
				meta_get_rva(meta, &ptrack->rva);
			}
			if (meta_comment && meta_get_comment(meta, tmp)) {
				if (!meta_wspace || !is_wspace(tmp)) {
					strncpy(ptrack->comment, tmp, MAXLEN-1);
				}
			}
		}

		meta_free(meta);
	}

	if (map_artist) {
		char * max = map_get_max(map_artist);

		if (max) {
			strncpy(record->artist, max, MAXLEN-1);
			record->artist_valid = 1;
		}
	}

	if (map_record) {
		char * max = map_get_max(map_record);

		if (max) {
			strncpy(record->record, max, MAXLEN-1);
			record->record_valid = 1;
		}
	}

	if (map_year) {
		char * max = map_get_max(map_year);

		if (max) {
			strncpy(record->year, max, MAXLEN-1);
			record->year_valid = 1;
		}
	}

	map_free(map_artist);
	map_free(map_record);
	map_free(map_year);
}


void
add_new_track(GtkTreeIter * record_iter, build_track_t * ptrack, int i) {

	GtkTreeIter iter;
	char sort_name[3];

	snprintf(sort_name, 3, "%02d", i + 1);

	gtk_tree_store_append(music_store, &iter, record_iter);

	if (ptrack->rva > 0.1f) { /* rva unmeasured */
                if (options.enable_ms_tree_icons) {
                        gtk_tree_store_set(music_store, &iter,
                                           0, ptrack->name,
                                           1, sort_name,
                                           2, ptrack->filename,
                                           3, ptrack->comment,
                                           4, ptrack->duration,
                                           5, 1.0f,
                                           6, 0.0f,
                                           7, -1.0f,
                                           9, icon_track,
                                           -1);
                } else {
                        gtk_tree_store_set(music_store, &iter,
                                           0, ptrack->name,
                                           1, sort_name,
                                           2, ptrack->filename,
                                           3, ptrack->comment,
                                           4, ptrack->duration,
                                           5, 1.0f,
                                           6, 0.0f,
                                           7, -1.0f,
                                           -1);
                }
	} else {
                if (options.enable_ms_tree_icons) {
                        gtk_tree_store_set(music_store, &iter,
                                           0, ptrack->name,
                                           1, sort_name,
                                           2, ptrack->filename,
                                           3, ptrack->comment,
                                           4, ptrack->duration,
                                           5, 1.0f,
                                           6, ptrack->rva,
                                           7, 1.0f,
                                           9, icon_track,
                                           -1);
                } else {
                        gtk_tree_store_set(music_store, &iter,
                                           0, ptrack->name,
                                           1, sort_name,
                                           2, ptrack->filename,
                                           3, ptrack->comment,
                                           4, ptrack->duration,
                                           5, 1.0f,
                                           6, ptrack->rva,
                                           7, 1.0f,
                                           -1);
                }
	}
}


gboolean
write_data_to_store(gpointer data) {

	build_record_t * record = (build_record_t *)data;
	build_track_t * ptrack = NULL;
	int result = 0;
	int i;

	GtkTreeIter record_iter;


	if (build_type == BUILD_STORE) {
		if (artist_iter_is_set) {
			result = artist_get_iter_for_record(&artist_iter, &record_iter, record);
		} else {
			result = store_get_iter_for_artist_and_record(&store_iter,
								      &artist_iter,
								      &record_iter,
								      record);
			artist_iter_is_set = 1;
		}
	} else {
		result = artist_get_iter_for_record(&artist_iter, &record_iter, record);
	}

	if (result == 0) {
		for (i = 0, ptrack = record->tracks; ptrack; i++, ptrack = ptrack->next) {
			add_new_track(&record_iter, ptrack, i);
		}
	}

	if (result == 1) {
		for (i = 0, ptrack = record->tracks; ptrack; i++, ptrack = ptrack->next) {

			GtkTreeIter iter;
			int has_track = 0;
			int j = 0;

			while (gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(music_store),
							     &iter, &record_iter, j++)) {
				char * file = NULL;

				gtk_tree_model_get(GTK_TREE_MODEL(music_store),
						   &iter, 2, &file, -1);

				if (!strcmp(file, ptrack->filename)) {
					has_track = 1;
					g_free(file);
					break;
				}

				g_free(file);
			}

			if (has_track) {
				if (reset_existing_data) {
					if (ptrack->rva > 0.1f) { /* rva unmeasured */
                                                if (options.enable_ms_tree_icons) {
                                                        gtk_tree_store_set(music_store, &iter,
                                                                           0, ptrack->name,
                                                                           4, ptrack->duration,
                                                                           9, icon_track,
                                                                           -1);
                                                } else {
                                                        gtk_tree_store_set(music_store, &iter,
                                                                           0, ptrack->name,
                                                                           4, ptrack->duration,
                                                                           -1);
                                                }
					} else {
                                                if (options.enable_ms_tree_icons) {
                                                        gtk_tree_store_set(music_store, &iter,
                                                                           0, ptrack->name,
                                                                           4, ptrack->duration,
                                                                           6, ptrack->rva,
                                                                           7, 1.0f,
                                                                           9, icon_track,
                                                                           -1);
                                                } else {
                                                        gtk_tree_store_set(music_store, &iter,
                                                                           0, ptrack->name,
                                                                           4, ptrack->duration,
                                                                           6, ptrack->rva,
                                                                           7, 1.0f,
                                                                           -1);
                                                }
					}
				}
			} else {
				add_new_track(&record_iter, ptrack, i);
			}
		}
	}

	music_store_mark_changed();

	write_data_locked = 0;

	return FALSE;
}


void
process_record(char * dir_record, char * artist_d_name, char * record_d_name) {

	struct timespec req_time;
	struct timespec rem_time;
	req_time.tv_sec = 0;
        req_time.tv_nsec = 10000000;

	int i;
	struct dirent ** ent_track;
	char basename[MAXLEN];
	char filename[MAXLEN];

	build_record_t record;

	int num_tracks = 0;

	build_track_t * ptrack = NULL;
	build_track_t * last_track = NULL;
	float duration;

	char * utf8;

	record.artist[0] = '\0';
	record.record[0] = '\0';
	record.year[0] = '\0';

	record.record_comment[0] = '\0';
	record.artist_sort_name[0] = '\0';
	record.record_sort_name[0] = '\0';

	record.artist_valid = artist_iter_is_set;
	record.record_valid = 0;
	record.year_valid = 0;

	record.tracks = NULL;


	g_idle_add(set_prog_action_label, (gpointer) _("Scanning files"));

	for (i = 0; i < scandir(dir_record, &ent_track, filter, alphasort); i++) {

		strncpy(basename, ent_track[i]->d_name, MAXLEN-1);
		snprintf(filename, MAXLEN-1, "%s/%s", dir_record, ent_track[i]->d_name);

		if (is_dir(filename)) {
			continue;
		}

		if (excl_enabled) {
			int i;
			int match = 0;
			for (i = 0; excl_patternv[i]; i++) {

				if (*(excl_patternv[i]) == '\0') {
					continue;
				}

				utf8 = g_locale_to_utf8(basename, -1, NULL, NULL, NULL);
				if (fnmatch(excl_patternv[i], utf8, FNM_CASEFOLD) == 0) {
					match = 1;
					g_free(utf8);
					break;
				}
				g_free(utf8);
			}

			if (match) {
				continue;
			}
		}

		if (incl_enabled) {
			int i;
			int match = 0;
			for (i = 0; incl_patternv[i]; i++) {

				if (*(incl_patternv[i]) == '\0') {
					continue;
				}

				utf8 = g_locale_to_utf8(basename, -1, NULL, NULL, NULL);
				if (fnmatch(incl_patternv[i], utf8, FNM_CASEFOLD) == 0) {
					match = 1;
					g_free(utf8);
					break;
				}
				g_free(utf8);
			}

			if (!match) {
				continue;
			}
		}

		if ((duration = get_file_duration(filename)) > 0.0f) {

			build_track_t * track;

			if ((track = (build_track_t *)malloc(sizeof(build_track_t))) == NULL) {
				fprintf(stderr, "build_store.c: process_record(): malloc error\n");
				return;
			} else {
				strncpy(track->filename, filename, MAXLEN-1);
				strncpy(track->d_name, ent_track[i]->d_name, MAXLEN-1);
				track->name[0] = '\0';
				track->comment[0] = '\0';
				track->duration = duration;
				track->rva = 1.0f;
				track->valid = 0;
				track->next = NULL;

				num_tracks++;
			}

			if (record.tracks == NULL) {
				record.tracks = last_track = track;
			} else {
				last_track->next = track;
				last_track = track;
			}
		}
	}

	if (record.tracks == NULL) {
		return;
	}

	/* metadata and cddb */

	if (pri_meta_first) {
		if (meta_enable) {
			g_idle_add(set_prog_action_label, (gpointer) _("Processing metadata"));
			process_meta(&record);
		}
	} else {
#ifdef HAVE_CDDB
		if (cddb_enable) {
			g_idle_add(set_prog_action_label, (gpointer) _("CDDB lookup"));
			cddb_get_batch(&record, cddb_title, cddb_artist, cddb_record);
		}
#endif /* HAVE_CDDB */
	}

	if (record.artist_valid && record.record_valid && num_invalid_tracks(&record) == 0 &&
	    (pri_meta_first || !meta_rva) &&
	    (record.year_valid || (record_sort_by != RECORD_SORT_YEAR && !add_year_to_comment))) {
		goto finish;
	}

	if (!pri_meta_first) {
		if (meta_enable) {
			g_idle_add(set_prog_action_label, (gpointer) _("Processing metadata"));
			process_meta(&record);
		}
	} else {
#ifdef HAVE_CDDB
		if (cddb_enable) {
			g_idle_add(set_prog_action_label, (gpointer) _("CDDB lookup"));
			cddb_get_batch(&record, cddb_title, cddb_artist, cddb_record);
		}
#endif /* HAVE_CDDB */
	}

	if (record.artist_valid && record.record_valid && num_invalid_tracks(&record) == 0) {
		goto finish;
	}

	/* filesystem */

	g_idle_add(set_prog_action_label, (gpointer) _("File name transformation"));

	if (!record.artist_valid) {
		utf8 = g_filename_display_name(artist_d_name);
		strncpy(record.artist, utf8, MAXLEN-1);
		record.artist_valid = 1;
		g_free(utf8);
	}

	if (!record.record_valid) {
		utf8 = g_filename_display_name(record_d_name);
		strncpy(record.record, utf8, MAXLEN-1);
		record.record_valid = 1;
		g_free(utf8);
	}

	for (i = 0, ptrack = record.tracks; ptrack; i++, ptrack = ptrack->next) {

		if (!ptrack->valid) {
			utf8 = g_filename_display_name(ptrack->d_name);
			transform_filename(ptrack->name, utf8);
			ptrack->valid = 1;
			g_free(utf8);
		}
	}


	/* add tracks to music store */

 finish:

	switch (artist_sort_by) {
	case ARTIST_SORT_NAME:
		strncpy(record.artist_sort_name, record.artist, MAXLEN-1);
		break;
	case ARTIST_SORT_NAME_LOW:
		utf8 = g_utf8_strdown(record.artist, -1);
		strncpy(record.artist_sort_name, utf8, MAXLEN-1);
		g_free(utf8);
		break;
	case ARTIST_SORT_DIR:
		utf8 = g_locale_to_utf8(artist_d_name, -1, NULL, NULL, NULL);
		strncpy(record.artist_sort_name, utf8, MAXLEN-1);
		g_free(utf8);
		break;
	case ARTIST_SORT_DIR_LOW:
		{
			char * tmp = g_locale_to_utf8(artist_d_name, -1, NULL, NULL, NULL);
			utf8 = g_utf8_strdown(tmp, -1);
			strncpy(record.artist_sort_name, utf8, MAXLEN-1);
			g_free(utf8);
			g_free(tmp);
		}
		break;
	}

	switch (record_sort_by) {
	case RECORD_SORT_YEAR:
		strncpy(record.record_sort_name, record.year, MAXLEN-1);
		break;
	case RECORD_SORT_NAME:
		strncpy(record.record_sort_name, record.record, MAXLEN-1);
		break;
	case RECORD_SORT_NAME_LOW:
		utf8 = g_utf8_strdown(record.record, -1);
		strncpy(record.record_sort_name, utf8, MAXLEN-1);
		g_free(utf8);
		break;
	case RECORD_SORT_DIR:
		utf8 = g_locale_to_utf8(record_d_name, -1, NULL, NULL, NULL);
		strncpy(record.record_sort_name, utf8, MAXLEN-1);
		g_free(utf8);
		break;
	case RECORD_SORT_DIR_LOW:
		{
			char * tmp = g_locale_to_utf8(record_d_name, -1, NULL, NULL, NULL);
			utf8 = g_utf8_strdown(tmp, -1);
			strncpy(record.record_sort_name, utf8, MAXLEN-1);
			g_free(utf8);
			g_free(tmp);
		}
		break;
	}

	if (rm_spaces_enabled) {
		for (ptrack = record.tracks; ptrack; ptrack = ptrack->next) {
			remove_spaces(ptrack->name);
		}

		remove_spaces(record.artist);
		remove_spaces(record.record);
	}

	if (cap_enabled) {

		for (ptrack = record.tracks; ptrack; ptrack = ptrack->next) {
			capitalize(ptrack->name);
		}

		capitalize(record.artist);
		capitalize(record.record);
	}

	if (add_year_to_comment) {
		strncpy(record.record_comment, record.year, MAXLEN-1);
	}


	/* wait for the gtk thread to update music_store */

	write_data_locked = 1;
	g_idle_add_full(G_PRIORITY_HIGH_IDLE,
			write_data_to_store,
			(gpointer)&record,
			NULL);


	while (write_data_locked) {
		nanosleep(&req_time, &rem_time);
	}


	/* free tracklist */

	for (ptrack = record.tracks; ptrack; record.tracks = ptrack) {
		ptrack = record.tracks->next;
		free(record.tracks);
	}
}


void *
build_artist_thread(void * arg) {

	int i;
	struct dirent ** ent_record;
	char dir_record[MAXLEN];
	char artist_d_name[MAXLEN];

	artist_iter_is_set = 1;

	i = strlen(root);
	while (root[--i] != '/');
	strncpy(artist_d_name, root + i + 1, MAXLEN-1);

	for (i = 0; i < scandir(root, &ent_record, filter, alphasort); i++) {

		snprintf(dir_record, MAXLEN-1, "%s/%s", root, ent_record[i]->d_name);
		
		if (!is_dir(dir_record)) {
			continue;
		}

		if (build_cancelled) {
			g_idle_add(finish_build, NULL);
			return NULL;
		}

		g_idle_add(set_prog_file_entry, (gpointer)dir_record);

		process_record(dir_record, artist_d_name, ent_record[i]->d_name);
	}

	g_idle_add(finish_build, NULL);

	return NULL;
}

void *
build_store_thread(void * arg) {

	int i, j;
	struct dirent ** ent_artist;
	struct dirent ** ent_record;
	char dir_artist[MAXLEN];
	char dir_record[MAXLEN];


	for (i = 0; i < scandir(root, &ent_artist, filter, alphasort); i++) {

		snprintf(dir_artist, MAXLEN-1, "%s/%s", root, ent_artist[i]->d_name);

		if (!is_dir(dir_artist)) {
			continue;
		}

		artist_iter_is_set = 0;

		for (j = 0; j < scandir(dir_artist, &ent_record, filter, alphasort); j++) {

			snprintf(dir_record, MAXLEN-1, "%s/%s", dir_artist, ent_record[j]->d_name);

			if (!is_dir(dir_record)) {
				continue;
			}

			if (build_cancelled) {
				g_idle_add(finish_build, NULL);
				return NULL;
			}

			g_idle_add(set_prog_file_entry, (gpointer)dir_record);

			process_record(dir_record, ent_artist[i]->d_name, ent_record[j]->d_name);
		}
	}

	g_idle_add(finish_build, NULL);

	return NULL;
}

void
build_artist(GtkTreeIter iter) {

	build_thread_state = BUILD_THREAD_BUSY;

	artist_iter = iter;
	build_cancelled = 0;
	build_type = BUILD_ARTIST;

	if (build_dialog()) {
		progress_window();
		AQUALUNG_THREAD_CREATE(build_thread_id, NULL, build_artist_thread, NULL)
	} else {
		build_thread_state = BUILD_THREAD_FREE;
	}
}

void
build_store(GtkTreeIter iter) {

	build_thread_state = BUILD_THREAD_BUSY;

	store_iter = iter;
	build_cancelled = 0;
	build_type = BUILD_STORE;

	if (build_dialog()) {
		progress_window();
		AQUALUNG_THREAD_CREATE(build_thread_id, NULL, build_store_thread, NULL)
	} else {
		build_thread_state = BUILD_THREAD_FREE;
	}
}


// vim: shiftwidth=8:tabstop=8:softtabstop=8 :  

