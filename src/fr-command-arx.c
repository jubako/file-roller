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
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <glib.h>
#include "fr-file-data.h"
#include "file-utils.h"
#include "glib-utils.h"
#include "fr-command.h"
#include "fr-command-arx.h"


struct _FrCommandArx
{
	FrCommand  parent_instance;
	gboolean   is_empty;
};


G_DEFINE_TYPE (FrCommandArx, fr_command_arx, fr_command_get_type ())


/* -- list -- */
static void
list__process_line (char     *line,
                    gpointer  data)
{
	FrFileData *fdata;
	FrCommand   *comm = FR_COMMAND (data);
	char       **fields;
	const char  *name_field;
	char        *name;
	int          ofs = 0;

	g_return_if_fail (line != NULL);

	/* arx listing stable_output(1) is :
	 * <type> <timeststamp> (<size>) <path>(-><target>)
	 * where <type> is `f`, `d` or `l` for file, directly and symlink.
	 * <size> is present only for file (<type> == `f`)
	 * -><target> is present only for link (<type> == `l`)
	 */

	fdata = fr_file_data_new ();

	/* Handle char and block device files */
	if (line[0] == 'f') {
		fields = _g_str_split_line (line, 4);
		fdata->modified = g_ascii_strtoull (fields[1], NULL, 10);
		fdata->size = g_ascii_strtoull (fields[2], NULL, 10);
		g_strfreev (fields);
		ofs = 1;
	} else {
		fields = _g_str_split_line (line, 3);
		fdata->modified = g_ascii_strtoull (fields[1], NULL, 10);
		g_strfreev (fields);
	}

	fdata->dir = line[0] == 'd';
	name_field = _g_str_get_last_field (line, 3+ofs);

	fields = g_strsplit (name_field, "->", 2);

	name = g_strcompress (fields[0]);
	if (*(fields[0]) == '/') {
		fdata->full_path = g_strdup (name);
		fdata->original_path = fdata->full_path;
	}
	else {
		fdata->full_path = g_strconcat ("/", name, NULL);
		fdata->original_path = fdata->full_path + 1;
	}

	if (fdata->dir && (name[strlen (name) - 1] != '/')) {
		char *old_full_path = fdata->full_path;
		fdata->full_path = g_strconcat (old_full_path, "/", NULL);
		g_free (old_full_path);
		fdata->original_path = g_strdup (name);
		fdata->free_original_path = TRUE;
	}
	g_free (name);

	if (fields[1] != NULL)
		fdata->link = g_strcompress (fields[1]);
	g_strfreev (fields);

	if (fdata->dir)
		fdata->name = _g_path_get_dir_name (fdata->full_path);
	else
		fdata->name = g_strdup (_g_path_get_basename (fdata->full_path));
	fdata->path = _g_path_remove_level (fdata->full_path);

	if (*fdata->name == 0)
		fr_file_data_free (fdata);
	else
		fr_archive_add_file (FR_ARCHIVE (comm), fdata);
}


static gboolean
fr_command_arx_list (FrCommand *comm)
{
	fr_process_set_out_line_func (comm->process, list__process_line, comm);

	fr_process_begin_command (comm->process, "arx");
	fr_process_add_arg (comm->process, "list");
	fr_process_add_arg (comm->process, "--stable-output");
	fr_process_add_arg (comm->process, "1");
	fr_process_add_arg (comm->process, comm->filename);
	fr_process_end_command (comm->process);

	FrArchive *archive = FR_ARCHIVE (comm);
	archive->read_only = TRUE;

	return TRUE;
}


/** Extract **/

static void
extract_process_line (char     *line,
		       gpointer  data)
{
	FrCommand *comm = FR_COMMAND (data);
	FrArchive *archive = FR_ARCHIVE (comm);

	if (line == NULL)
		return;

	if (fr_archive_progress_get_total_files (archive) > 1) {
		fr_archive_progress (archive, fr_archive_progress_inc_completed_files (archive, 1));
	}
}

static void
fr_command_arx_extract (FrCommand  *comm,
		        const char  *from_file,
			GList      *file_list,
			const char *dest_dir,
			gboolean    overwrite,
			gboolean    skip_older,
			gboolean    junk_paths)
{
	GList   *scan;

	fr_process_set_out_line_func (FR_COMMAND (comm)->process, extract_process_line, comm);

	fr_process_begin_command (comm->process, "arx");
	fr_process_add_arg (comm->process, "extract");
	if (dest_dir != NULL) {
		fr_process_add_arg (comm->process, "-C");
		fr_process_add_arg (comm->process, dest_dir);
	}
	fr_process_add_arg (comm->process, "--file");
	fr_process_add_arg (comm->process, comm->filename);

	fr_process_add_arg (comm->process, "--progress");

	if (from_file == NULL) {
		for (scan = file_list; scan; scan = scan->next) {
			fr_process_add_arg (comm->process, scan->data);
		}
	} else {
		fr_process_add_arg (comm->process, "-L");
		fr_process_add_arg (comm->process, from_file);
	}

	fr_process_end_command (comm->process);
}

static void
fr_command_arx_add (FrCommand  *comm,
		    const char *from_file,
		    GList      *file_list,
		    const char *base_dir,
		    gboolean    update,
		    gboolean    follow_links)
{
	FrCommandArx *c_arx = FR_COMMAND_ARX (comm);
	GList        *scan;

	if (!comm->creating_archive) {
		fr_process_cancel(comm->process);
		return;
	}

	fr_process_begin_command (comm->process, "arx");
	fr_process_add_arg (comm->process, "create");

	fr_process_add_arg (comm->process, "--file");
	fr_process_add_arg (comm->process, comm->filename);

	if (base_dir != NULL) {
		fr_process_add_arg (comm->process, "-C");
		fr_process_add_arg (comm->process, base_dir);
	}

	if (from_file != NULL) {
		fr_process_add_arg (comm->process, "-L");
		fr_process_add_arg (comm->process, from_file);
	}

	fr_process_add_arg (comm->process, "--");

	if (from_file == NULL)
		for (scan = file_list; scan; scan = scan->next)
			fr_process_add_arg (comm->process, scan->data);

	fr_process_end_command (comm->process);
}



const char *arx_mime_type[] = { "application/x-arx", NULL };


static const char **
fr_command_arx_get_mime_types (FrArchive *archive)
{
	return arx_mime_type;
}


static FrArchiveCaps
fr_command_arx_get_capabilities (FrArchive  *archive,
			         const char *mime_type,
				 gboolean    check_command)
{
	FrArchiveCaps capabilities;

	capabilities = FR_ARCHIVE_CAN_STORE_MANY_FILES;
	if (_g_program_is_available ("arx", check_command))
		capabilities |= FR_ARCHIVE_CAN_READ;

	/*if (archive->private.creating_archive)
		capabilities |= FR_ARCHIVE_CAN_WRITE;*/

	return capabilities;
}


static const char *
fr_command_arx_get_packages (FrArchive  *archive,
			     const char *mime_type)
{
	return FR_PACKAGES ("arx");
}


static void
fr_command_arx_finalize (GObject *object)
{
	g_return_if_fail (object != NULL);
	g_return_if_fail (FR_IS_COMMAND_ARX (object));

	/* Chain up */
	if (G_OBJECT_CLASS (fr_command_arx_parent_class)->finalize)
		G_OBJECT_CLASS (fr_command_arx_parent_class)->finalize (object);
}


static void
fr_command_arx_class_init (FrCommandArxClass *klass)
{
	GObjectClass   *gobject_class;
	FrArchiveClass *archive_class;
	FrCommandClass *command_class;

	fr_command_arx_parent_class = g_type_class_peek_parent (klass);

	gobject_class = G_OBJECT_CLASS (klass);
	gobject_class->finalize = fr_command_arx_finalize;

	archive_class = FR_ARCHIVE_CLASS (klass);
	archive_class->get_mime_types   = fr_command_arx_get_mime_types;
	archive_class->get_capabilities = fr_command_arx_get_capabilities;
	archive_class->get_packages     = fr_command_arx_get_packages;

	command_class = FR_COMMAND_CLASS (klass);
	command_class->list             = fr_command_arx_list;
	command_class->extract          = fr_command_arx_extract;
	command_class->add              = fr_command_arx_add;
}


static void
fr_command_arx_init (FrCommandArx *self)
{
	FrArchive *base = FR_ARCHIVE (self);

	base->propAddCanUpdate             = FALSE;
	base->propAddCanReplace            = FALSE;
	base->propAddCanStoreFolders       = TRUE;
	base->propAddCanStoreLinks         = TRUE;
	base->propExtractCanAvoidOverwrite = FALSE;
	base->propExtractCanSkipOlder      = FALSE;
	base->propExtractCanJunkPaths      = FALSE;
	base->propPassword                 = FALSE;
	base->propTest                     = FALSE;
	base->propCanExtractAll            = TRUE;
	base->propCanDeleteNonEmptyFolders = FALSE;
	base->propCanDeleteAllFiles        = FALSE;
	base->propCanExtractNonEmptyFolders= FALSE;
	base->propListFromFile             = TRUE;
	base->read_only = TRUE;
}
