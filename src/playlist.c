/*
 * Play List
 * Load and manage the playlist.
 *
 * playlist.c
 * This file is part of <RhythmCat>
 *
 * Copyright (C) 2010 - SuperCat, license: GPL v3
 *
 * <RhythmCat> is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * <RhythmCat> is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with <RhythmCat>; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, 
 * Boston, MA  02110-1301  USA
 */

#include "playlist.h"
#include "core.h"
#include "gui.h"
#include "gui_treeview.h"
#include "settings.h"
#include "tag.h"
#include "msg.h"
#include "lyric.h"
#include "main.h"
#include "debug.h"
#include "player.h"

typedef struct RCPlistImportData
{
    gchar *uri;
    gint list2_index;
    gboolean refresh_flag;
    gboolean auto_clean;
    GtkTreeRowReference *reference;
    GtkListStore *store;
}RCPlistImportData;

/* Variables */
static gchar play_list_setting_file[] = "playlist.dat";
static gchar *default_list_name = "[Default]";
static RCPlistData rc_plist;
static GThread *plist_import_threads[2];
static GAsyncQueue *plist_import_job_queue;
static const gint plist_import_thread_num = 2;
static gboolean plist_import_job_flag = TRUE;
static GCancellable *plist_import_thread_cancel = NULL;

/*
 * Process import jobs.
 */

static gpointer rc_plist_import_job_func(gpointer data)
{
    RCMusicMetaData *mmd = NULL;
    RCPlistImportData *import_data;
    plist_import_thread_cancel = g_cancellable_new();
    while(plist_import_job_flag)
    {
        import_data = g_async_queue_pop(plist_import_job_queue);
        if(import_data==NULL) continue;
        if(import_data->uri!=NULL)
        {
            mmd = rc_tag_read_metadata(import_data->uri);
            g_free(import_data->uri);
            if(mmd==NULL || !mmd->audio_flag)
            {
                rc_debug_print("Plist: The file is not a music file!\n");
                if(mmd!=NULL) rc_tag_free(mmd);
                if(import_data->auto_clean)
                    rc_msg_push(MSG_TYPE_PL_REMOVE, import_data->reference);
                else
                {
                    rc_msg_push(MSG_TYPE_EMPTY, NULL);
                    if(import_data->reference!=NULL)
                        gtk_tree_row_reference_free(import_data->reference);
                }
                g_free(import_data);
                continue;
            }
            mmd->list2_index = import_data->list2_index;
            mmd->reference = import_data->reference;
            mmd->store = import_data->store;
            if(!import_data->refresh_flag)
                rc_msg_push(MSG_TYPE_PL_INSERT, mmd);
            else
                rc_msg_push(MSG_TYPE_PL_REFRESH, mmd);
        }
        g_free(import_data);
    }
    rc_debug_print("Plist: Job thread cancelled!\n");
    return NULL;
}

/*
 * Initial playlist store list.
 */

gboolean rc_plist_init()
{
    static gboolean init = FALSE;
    GError *error = NULL;
    gint i;
    if(init) return FALSE;
    init = TRUE;
    rc_debug_print("Plist: Loading playlists...\n");
    default_list_name = _("Default Playlist");
    bzero(&rc_plist, sizeof(RCPlistData));
    rc_plist.list_store = gtk_list_store_new(PLIST1_LAST, G_TYPE_STRING,
        G_TYPE_STRING, G_TYPE_POINTER);
    rc_gui_list_tree_reset_list_store();
    rc_plist_load_playlist_setting();
    if(rc_plist_get_list1_length()<1)
        rc_plist_build_default_list();
    rc_gui_select_list1(0);
    plist_import_job_queue = g_async_queue_new();
    for(i=0;i<plist_import_thread_num;i++)
    {
        plist_import_threads[i] = g_thread_create(
            (GThreadFunc)rc_plist_import_job_func, NULL, FALSE, &error);
        if(error!=NULL)
        {
            rc_debug_perror("Plist-ERROR: %s\n", error->message);
            g_error_free(error);
        }
    }
    plist_import_job_flag = TRUE;
    rc_gui_set_player_mode();
    rc_debug_print("Plist: Playlists are successfully loaded!\n");
    return TRUE;
}

/*
 * Uninitial playlist data.
 */

void rc_plist_uninit_playlist()
{
    gint list_count = 0;
    plist_import_job_flag = FALSE;
    if(plist_import_thread_cancel!=NULL)
    {
        g_cancellable_cancel(plist_import_thread_cancel);
        g_object_unref(plist_import_thread_cancel);
    }
    for(list_count=rc_plist_get_list1_length()-1;list_count>=0;list_count--)
    {
        rc_plist_remove_list(list_count);
    }
}

/*
 * Insert music data to playlist.
 */

gboolean rc_plist_insert_music(const gchar *uri, gint list1_index,
    gint list2_index)
{
    GtkListStore *store;
    RCPlistImportData *import_data;
    store = rc_plist_get_list_store(list1_index);
    if(store==NULL) return FALSE;
    import_data = g_malloc0(sizeof(RCPlistImportData));
    import_data->uri = g_strdup(uri);
    import_data->store = rc_plist_get_list_store(list1_index);
    import_data->list2_index = list2_index;
    import_data->refresh_flag = FALSE;
    g_async_queue_push(plist_import_job_queue, import_data);
    return TRUE;
}

/*
 * Insert music to list2 by metadata.
 */

void rc_plist_list2_insert_item(const gchar *uri, const gchar *title,
    const gchar *artist, const gchar *album, gint64 length, gint trackno,
    GtkListStore *store, gint list2_index)
{
    GtkTreeIter iter;
    gint64 seclength;
    gint time_min, time_sec;
    gchar *realname = NULL;
    gchar *fpathname = NULL;
    gchar new_title[512];
    gchar new_length[64];
    if(!GTK_IS_LIST_STORE(store)) return;
    if(title[0]!='\0')
        g_utf8_strncpy(new_title, title, 127);
    else
    {
        fpathname = g_filename_from_uri(uri, NULL, NULL);
        if(fpathname!=NULL)
        {
            realname = rc_tag_get_name_from_fpath(fpathname);
            g_free(fpathname);
        }
        if(realname!=NULL)
        {
            g_utf8_strncpy(new_title, realname, 127);
            g_free(realname);
        }
        else
            g_utf8_strncpy(new_title, _("Unknown Title"), 127);
    }
    seclength = length / GST_SECOND;
    time_min = seclength / 60;
    time_sec = seclength % 60;
    g_snprintf(new_length, 63, "%02d:%02d", time_min, time_sec);
    if(list2_index>=0)
        gtk_list_store_insert(store, &iter, list2_index);
    else
        gtk_list_store_append(store, &iter);
    gtk_list_store_set(store, &iter, PLIST2_URI, uri, PLIST2_STATE, NULL,
        PLIST2_TITLE, new_title, PLIST2_ARTIST, artist, PLIST2_ALBUM, album,
        PLIST2_LENGTH, new_length, PLIST2_TRACKNO, trackno, -1);
}

/*
 * Refresh list2 by metadata.
 */

void rc_plist_list2_refresh_item(const gchar *uri, const gchar *title,
    const gchar *artist, const gchar *album, gint64 length, gint trackno,
    GtkTreeRowReference *reference)
{
    GtkTreeIter iter;
    GtkListStore *pl_store;
    GtkTreePath *path;
    gint64 seclength;
    gint time_min, time_sec;
    gchar *realname = NULL;
    gchar *fpathname = NULL;
    gchar new_title[512];
    gchar new_length[64];
    if(!gtk_tree_row_reference_valid(reference))
    {
        gtk_tree_row_reference_free(reference);
        return;
    }
    pl_store = GTK_LIST_STORE(gtk_tree_row_reference_get_model(reference));
    if(!GTK_IS_LIST_STORE(pl_store))
    {
        gtk_tree_row_reference_free(reference);
        return;
    }
    path = gtk_tree_row_reference_get_path(reference);
    gtk_tree_row_reference_free(reference);
    gtk_tree_model_get_iter(GTK_TREE_MODEL(pl_store), &iter, path);
    gtk_tree_path_free(path);
    if(title[0]!='\0')
        g_utf8_strncpy(new_title, title, 127);
    else
    {
        fpathname = g_filename_from_uri(uri, NULL, NULL);
        if(fpathname!=NULL)
        {
            realname = rc_tag_get_name_from_fpath(fpathname);
            g_free(fpathname);
        }
        if(realname!=NULL)
        {
            g_utf8_strncpy(new_title, realname, 127);
            g_free(realname);
        }
        else
            g_utf8_strncpy(new_title, _("Unknown Title"), 127);
    }
    seclength = length / GST_SECOND;
    time_min = seclength / 60;
    time_sec = seclength % 60;
    g_snprintf(new_length, 63, "%02d:%02d", time_min, time_sec);
    gtk_list_store_set(pl_store, &iter, PLIST2_URI, uri, PLIST2_STATE, NULL,
        PLIST2_TITLE, new_title, PLIST2_ARTIST, artist, PLIST2_ALBUM, album,
        PLIST2_LENGTH, new_length, PLIST2_TRACKNO, trackno, -1);
}

/*
 * Remove invalid item in list2.
 */

void rc_plist_list2_remove_item(GtkTreeRowReference *reference)
{
    GtkTreeIter iter;
    GtkListStore *pl_store;
    GtkTreePath *path;
    if(!gtk_tree_row_reference_valid(reference))
    {
        gtk_tree_row_reference_free(reference);
        return;
    }
    pl_store = GTK_LIST_STORE(gtk_tree_row_reference_get_model(reference));
    if(!GTK_IS_LIST_STORE(pl_store))
    {
        gtk_tree_row_reference_free(reference);
        return;
    }
    path = gtk_tree_row_reference_get_path(reference);
    gtk_tree_row_reference_free(reference);
    gtk_tree_model_get_iter(GTK_TREE_MODEL(pl_store), &iter, path);
    gtk_tree_path_free(path);
    gtk_list_store_remove(pl_store, &iter);
}

/*
 * Play music by given index.
 */

gboolean rc_plist_play_by_index(gint list_index, gint music_index)
{
    GtkListStore *list_store;
    GtkTreePath *path;
    GtkTreeIter iter;
    gint64 timeinfo;
    gint time_min, time_sec;
    gint trackno = -1;
    gchar *list_uri = NULL;
    gchar list_title[512];
    gchar album_name[512];
    gchar list_timelen[64];
    gchar *music_dir = NULL, *lyric_dir = NULL, *image_dir = NULL;
    gchar *lyric_filename = NULL;
    gchar *cover_filename = NULL;
    gchar *fpathname = NULL;
    gchar *realname = NULL;
    RCMusicMetaData *mmd_new = NULL;
    gboolean image_flag = FALSE;
    list_store = rc_plist_get_list_store(list_index);
    path = gtk_tree_path_new_from_indices(music_index, -1);
    if(!gtk_tree_model_get_iter(GTK_TREE_MODEL(list_store), &iter, path))
    {
        rc_debug_perror("Plist-ERROR: Cannot find iter!\n");
        if(path!=NULL) gtk_tree_path_free(path);
        return FALSE;
    }
    gtk_tree_model_get(GTK_TREE_MODEL(list_store), &iter, PLIST2_URI,
        &list_uri, PLIST2_TRACKNO, &trackno, -1);
    gtk_tree_path_free(path);
    if(list_uri==NULL)
    {
        return FALSE;
    }
    mmd_new = rc_tag_read_metadata(list_uri);
    g_free(list_uri);
    if(mmd_new==NULL || !mmd_new->audio_flag)
    {
        rc_debug_perror("Plist-ERROR: Cannot open the music!\n");
        if(rc_set_get_boolean("Playlist", "AutoClean", NULL))
            gtk_list_store_remove(GTK_LIST_STORE(list_store), &iter); 
        return FALSE;
    }
    timeinfo = mmd_new->length / GST_SECOND;
    time_min = timeinfo / 60;
    time_sec = timeinfo % 60;
    g_snprintf(list_timelen, 63, "%02d:%02d", time_min, time_sec);
    fpathname = g_filename_from_uri(mmd_new->uri, NULL, NULL);
    if(fpathname!=NULL)
    {
        realname = rc_tag_get_name_from_fpath(fpathname);
    }
    if(strlen(mmd_new->title)>0)
        g_utf8_strncpy(list_title, mmd_new->title, 127);
    else
    {
        if(realname!=NULL)
        {
            g_utf8_strncpy(list_title, realname, 127);
        }
        else
        {
            g_utf8_strncpy(list_title, _("Unknown title"), 127);
        }
    }
    g_utf8_strncpy(album_name, mmd_new->album, 127);
    rc_core_set_uri(mmd_new->uri);
    rc_tag_set_playing_metadata(mmd_new);
    gtk_list_store_set(list_store, &iter, PLIST2_STATE, GTK_STOCK_MEDIA_PLAY,
        PLIST2_TITLE, list_title, PLIST2_ARTIST, mmd_new->artist, PLIST2_ALBUM,
        mmd_new->album, PLIST2_LENGTH, list_timelen, -1);
    if(rc_plist.list1_reference!=NULL)
    {
        gtk_tree_row_reference_free(rc_plist.list1_reference);
        rc_plist.list1_reference = NULL;
    }
    if(rc_plist.list2_reference!=NULL)
    {
        gtk_tree_row_reference_free(rc_plist.list2_reference);
        rc_plist.list2_reference = NULL;
    }
    rc_debug_print("Plist: Play music file: %s\n", mmd_new->uri);
    rc_gui_music_info_set_data(list_title, mmd_new);
    if(mmd_new->image!=NULL)
    {
        image_flag = TRUE;
        rc_debug_print("Plist: Found cover image in tag!\n");
    }
    path = gtk_tree_path_new_from_indices(list_index, -1);
    if(gtk_tree_model_get_iter(GTK_TREE_MODEL(rc_plist.list_store), &iter,
        path))
    {
        gtk_list_store_set(rc_plist.list_store, &iter, PLIST1_STATE,
            GTK_STOCK_MEDIA_PLAY, -1);
    }
    rc_plist.list1_reference = gtk_tree_row_reference_new(GTK_TREE_MODEL(
        rc_plist.list_store), path);
    gtk_tree_path_free(path);
    path = gtk_tree_path_new_from_indices(music_index, -1);
    rc_plist.list2_reference = gtk_tree_row_reference_new(GTK_TREE_MODEL(
        list_store), path);
    gtk_tree_path_free(path);
    rc_tag_free(mmd_new);
    /* Search extra info for the music file in local filesystem. */
    rc_lrc_clean_data();
    if(fpathname==NULL) return TRUE;
    music_dir = g_path_get_dirname(fpathname);
    g_free(fpathname);
    lyric_dir = g_strdup_printf("%s%cLyrics", rc_get_set_dir(), G_DIR_SEPARATOR);
    image_dir = g_strdup_printf("%s%cAlbumImages", rc_get_set_dir(),
        G_DIR_SEPARATOR);
    lyric_filename = rc_tag_find_file(music_dir, realname, ".LRC");
    if(lyric_filename==NULL)
        lyric_filename = rc_tag_find_file(lyric_dir, realname, ".LRC");
    g_free(lyric_dir);
    if(lyric_filename!=NULL && rc_lrc_read_from_file(lyric_filename))
    {
        rc_debug_print("Plist: Found lyric file: %s, enable the lyric show.\n",
            lyric_filename);
        rc_player_object_signal_emit_simple("lyric-found");
    }
    else
    {
        rc_debug_print("Plist: Not found lyric file, disable the lyric "
            "show.\n");
        rc_player_object_signal_emit_simple("lyric-not-found");
    }
    if(lyric_filename!=NULL) g_free(lyric_filename);
    if(!image_flag)
    {
        cover_filename = rc_tag_find_file(music_dir, album_name,
            ".BMP|.JPG|.JPEG|.PNG");
        if(cover_filename==NULL)
            cover_filename = rc_tag_find_file(image_dir, album_name,
                ".BMP|.JPG|.JPEG|.PNG");
        g_free(image_dir);
        if(cover_filename!=NULL && rc_gui_set_cover_image_by_file(
            cover_filename))
        {
            rc_debug_print("Plist: Found cover image file: %s.\n",
                cover_filename);
        }
        else
            rc_gui_set_cover_image_by_file(NULL);
    }
    g_free(music_dir);
    g_free(realname);
    return TRUE;
}

/*
 * Get the list indices of playing music.
 */

gboolean rc_plist_play_get_index(gint *index1, gint *index2)
{
    GtkTreePath *path1, *path2;
    GtkTreeModel *model2;
    gint *indices;
    gint list1_index, list2_index;
    if(!gtk_tree_row_reference_valid(rc_plist.list1_reference))
        return FALSE;
    if(!gtk_tree_row_reference_valid(rc_plist.list2_reference))
        return FALSE;
    model2 = gtk_tree_row_reference_get_model(rc_plist.list2_reference);
    if(model2==NULL) return FALSE;
    path1 = gtk_tree_row_reference_get_path(rc_plist.list1_reference);
    path2 = gtk_tree_row_reference_get_path(rc_plist.list2_reference);
    indices = gtk_tree_path_get_indices(path1);
    list1_index = indices[0];
    indices = gtk_tree_path_get_indices(path2);
    list2_index = indices[0];
    gtk_tree_path_free(path1);
    gtk_tree_path_free(path2);
    if(index1!=NULL) *index1 = list1_index;
    if(index2!=NULL) *index2 = list2_index;
    return TRUE;
}

/*
 * Stop the music.
 */

void rc_plist_stop()
{
    GtkTreePath *path_old;
    GtkTreeIter iter_old;
    GtkTreeModel *model;
    if(gtk_tree_row_reference_valid(rc_plist.list1_reference))
    {
        path_old = gtk_tree_row_reference_get_path(rc_plist.list1_reference);
        if(path_old!=NULL)
        {
            if(gtk_tree_model_get_iter(GTK_TREE_MODEL(rc_plist.list_store),
                &iter_old, path_old))
            {
                gtk_list_store_set(rc_plist.list_store, &iter_old,
                    PLIST1_STATE, NULL, -1);
            }
            gtk_tree_path_free(path_old);
        }
    }
    if(gtk_tree_row_reference_valid(rc_plist.list2_reference))
    {
        model = gtk_tree_row_reference_get_model(rc_plist.list2_reference);
        if(model!=NULL)
        {
            path_old = gtk_tree_row_reference_get_path(
                rc_plist.list2_reference);
            if(path_old!=NULL)
            {
                if(gtk_tree_model_get_iter(model, &iter_old, path_old))
                {
                    gtk_list_store_set(GTK_LIST_STORE(model), &iter_old,
                        PLIST2_STATE, NULL, -1);
                }
                gtk_tree_path_free(path_old);
            }
        }
    }
}

/*
 * Play next music.
 */

gboolean rc_plist_play_prev()
{
    GtkTreePath *path1, *path2;
    GtkTreeModel *model2;
    gint *indices;
    gint list1_index, list2_index;
    if(!gtk_tree_row_reference_valid(rc_plist.list1_reference))
        return FALSE;
    if(!gtk_tree_row_reference_valid(rc_plist.list2_reference))
        return FALSE;
    model2 = gtk_tree_row_reference_get_model(rc_plist.list2_reference);
    if(model2==NULL) return FALSE;
    path1 = gtk_tree_row_reference_get_path(rc_plist.list1_reference);
    path2 = gtk_tree_row_reference_get_path(rc_plist.list2_reference);
    indices = gtk_tree_path_get_indices(path1);
    list1_index = indices[0];
    indices = gtk_tree_path_get_indices(path2);
    list2_index = indices[0];
    gtk_tree_path_free(path1);
    gtk_tree_path_free(path2);
    rc_core_stop();
    list2_index--;
    if(list2_index<0) list2_index = 0;
    if(rc_plist_play_by_index(list1_index, list2_index))
        return rc_core_play();
    else return FALSE;
}

/*
 * Play next music.
 */

gboolean rc_plist_play_next(gboolean next_list)
{
    GtkTreePath *path1, *path2;
    GtkTreeModel *model2;
    gint *indices;
    gint list1_index, list2_index;
    gint list1_length, list2_length;
    gint repeat_mode = 0, random_mode = 0;
    gint i;
    repeat_mode = rc_set_get_integer("Player", "RepeatMode", NULL);
    random_mode = rc_set_get_integer("Player", "RandomMode", NULL);
    if(random_mode==0 || !next_list)
    {
        if(!gtk_tree_row_reference_valid(rc_plist.list1_reference))
            return FALSE;
        if(!gtk_tree_row_reference_valid(rc_plist.list2_reference))
            return FALSE;
        model2 = gtk_tree_row_reference_get_model(rc_plist.list2_reference);
        if(model2==NULL) return FALSE;
        path1 = gtk_tree_row_reference_get_path(rc_plist.list1_reference);
        path2 = gtk_tree_row_reference_get_path(rc_plist.list2_reference);
        indices = gtk_tree_path_get_indices(path1);
        list1_index = indices[0];
        indices = gtk_tree_path_get_indices(path2);
        list2_index = indices[0];
        gtk_tree_path_free(path1);
        gtk_tree_path_free(path2);
        list2_index++;
        rc_core_stop();
        if(repeat_mode==1 && next_list)
        {
            if(rc_plist_play_by_index(list1_index, list2_index-1))
                return rc_core_play();
            else return FALSE;
        }
        if(list2_index<rc_plist_get_list2_length(list1_index))
        {
            if(rc_plist_play_by_index(list1_index, list2_index))
                return rc_core_play();
            else return FALSE;
        }
        if(!next_list)
        {
            if(rc_plist_play_by_index(list1_index, list2_index-1))
                return rc_core_play();
            else return FALSE;
        }
        if(repeat_mode==2)
        {
            if(rc_plist_play_by_index(list1_index, 0))
                return rc_core_play();
            else return FALSE;
        }
        list1_length = rc_plist_get_list1_length();
        list1_index++;
        for(i=list1_index;i<list1_length;i++)
        {
            list2_length = rc_plist_get_list2_length(i);
            if(list2_length>0)
            {
                if(rc_plist_play_by_index(list1_index, 0))
                    return rc_core_play();
                else return FALSE;
            }
        }
        if(repeat_mode==3)
        {
            if(rc_plist_play_by_index(0, 0))
                return rc_core_play();
            else return FALSE;
        }
        else return FALSE;
    }
    else
    {
        if(random_mode==1)
        {
            if(!gtk_tree_row_reference_valid(rc_plist.list1_reference))
                return FALSE;
            if(!gtk_tree_row_reference_valid(rc_plist.list2_reference))
                return FALSE;
            model2 = gtk_tree_row_reference_get_model(rc_plist.list2_reference);
            if(model2==NULL) return FALSE;
            path1 = gtk_tree_row_reference_get_path(rc_plist.list1_reference);
            path2 = gtk_tree_row_reference_get_path(rc_plist.list2_reference);
            indices = gtk_tree_path_get_indices(path1);
            list1_index = indices[0];
            indices = gtk_tree_path_get_indices(path2);
            list2_index = indices[0];
            gtk_tree_path_free(path1);
            gtk_tree_path_free(path2);
            list2_length = rc_plist_get_list2_length(list1_index);
            list2_index = rand() % list2_length;
            if(rc_plist_play_by_index(list1_index, list2_index))
                return rc_core_play();
            else return FALSE;
        }
        else if(random_mode==2)
        {
            list1_length = rc_plist_get_list1_length();
            list2_length = 0;
            indices = g_malloc(list1_length*sizeof(gint));
            for(i=0;i<list1_length;i++)
            {
                indices[i] = rc_plist_get_list2_length(i);
                list2_length+=indices[i];
            }
            list2_index = rand() % list2_length;
            for(i=0;i<list1_length;i++)
            {
                if(list2_index>=indices[i])
                {
                    list2_index-=indices[i];
                }
                else
                {
                    g_free(indices);
                    rc_plist_play_by_index(i, list2_index);
                    return rc_core_play();
                }
            }
            g_free(indices);
        }
    }
    return FALSE;
}

/*
 * Set the play mode of the player.
 */

void rc_plist_set_play_mode(gint repeat, gint random)
{
    if(repeat>=0 && repeat<=3)
    {
        rc_set_set_integer("Player", "RepeatMode", repeat);
    }
    if(random>=0 && random<=2)
    {
        rc_set_set_integer("Player", "RandomMode", random);
    }
    rc_gui_set_player_mode();
}

/*
 * Get the play mode of the player.
 */

void rc_plist_get_play_mode(gint *repeat, gint *random)
{
    if(repeat!=NULL)
        *repeat = rc_set_get_integer("Player", "RepeatMode", NULL);
    if(random!=NULL)
        *random = rc_set_get_integer("Player", "RandomMode", NULL);
}

/*
 * Insert a new list into the list store.
 */

gboolean rc_plist_insert_list(const gchar *listname, gint index)
{
    if(rc_plist.list_store==NULL) return FALSE;
    GtkListStore *pl_store = NULL;
    GtkTreeIter iter;
    gchar new_name[512];
    g_utf8_strncpy(new_name, listname, 127);
    pl_store = gtk_list_store_new(PLIST2_LAST, G_TYPE_STRING, G_TYPE_STRING,
        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
        G_TYPE_INT);
    if(index>=0)
        gtk_list_store_insert(rc_plist.list_store, &iter, index);
    else
        gtk_list_store_append(rc_plist.list_store, &iter);
    gtk_list_store_set(rc_plist.list_store, &iter, PLIST1_STATE, NULL,
        PLIST1_NAME, new_name, PLIST1_STORE, pl_store, -1);
    return TRUE;
}

/* 
 * Get the name of the list. (Free it after usage!)
 */

gchar *rc_plist_get_list1_name(gint index)
{
    GtkTreeIter iter;
    GtkTreePath *path;
    gchar *name;
    if(index<0) return NULL;
    path = gtk_tree_path_new_from_indices(index, -1);
    if(!gtk_tree_model_get_iter(GTK_TREE_MODEL(rc_plist.list_store), &iter,
        path))
        return NULL;
    gtk_tree_path_free(path);
    gtk_tree_model_get(GTK_TREE_MODEL(rc_plist.list_store), &iter, PLIST1_NAME,
        &name, -1);
    return name;
}

/*
 * Rename an exist list.
 */

void rc_plist_set_list1_name(gint index, const gchar *name)
{
    GtkTreeIter iter;
    GtkTreePath *path;
    gchar *old_name;
    gchar new_name[512];
    if(index<0) return;
    path = gtk_tree_path_new_from_indices(index, -1);
    if(!gtk_tree_model_get_iter(GTK_TREE_MODEL(rc_plist.list_store), &iter,
        path))
        return;
    gtk_tree_path_free(path);
    gtk_tree_model_get(GTK_TREE_MODEL(rc_plist.list_store), &iter, PLIST1_NAME,
        &old_name, -1);
    if(old_name==NULL) return;
    g_utf8_strncpy(new_name, name, 127);
    if(g_strcmp0(old_name, new_name)==0)
    {
        rc_debug_print("The list name is the same, there's no need to "
            "rename.\n");
        g_free(old_name);
        return;
    }
    g_free(old_name);
    gtk_list_store_set(rc_plist.list_store, &iter, PLIST1_NAME, new_name, -1);
}

/*
 * Get the length of list1.
 */

gint rc_plist_get_list1_length()
{
    return gtk_tree_model_iter_n_children(GTK_TREE_MODEL(rc_plist.list_store),
        NULL);
}

/*
 * Get the length of list2.
 */

gint rc_plist_get_list2_length(gint index)
{
    GtkListStore *pl_store;
    pl_store = rc_plist_get_list_store(index);
    if(pl_store==NULL) return 0;
    return gtk_tree_model_iter_n_children(GTK_TREE_MODEL(pl_store), NULL);
}

/*
 * Remove a list by index.
 */

gboolean rc_plist_remove_list(gint index)
{
    GtkListStore *pl_store;
    GtkTreeIter iter;
    GtkTreePath *path;
    if(index<0) return FALSE;
    path = gtk_tree_path_new_from_indices(index, -1);
    if(!gtk_tree_model_get_iter(GTK_TREE_MODEL(rc_plist.list_store), &iter,
        path))
        return FALSE;
    gtk_tree_path_free(path);
    gtk_tree_model_get(GTK_TREE_MODEL(rc_plist.list_store), &iter,
        PLIST1_STORE, &pl_store, -1);
    if(pl_store==NULL) return FALSE;
    gtk_list_store_clear(pl_store);
    g_object_unref(pl_store);
    return gtk_list_store_remove(rc_plist.list_store, &iter);
}

/*
 * Load playlists from file.
 */

gboolean rc_plist_load_playlist_setting()
{
    const gchar *rc_set_dir = rc_get_set_dir();
    GtkListStore *pl_store = NULL;
    gsize s_length;
    gchar bytechr = 0;
    gchar *file_data = NULL;
    gchar *line = NULL, *buf = NULL;
    gint linenum = 0;
    gint listnum = -1;
    gint listflag = FALSE;
    gint existlist = FALSE;
    gint linecount = 0;
    gulong file_pointer = 0L;
    gulong line_length = 0L;
    gulong file_length = 0L;
    gint fname_length = 0;
    gint64 timeinfo;
    gint time_min, time_sec;
    gint trackno;
    gchar time_str[64];
    GtkTreeIter iter;
    fname_length = strlen(rc_set_dir) + strlen(play_list_setting_file) + 16;
    gchar *plist_set_file_full_path = g_malloc0(fname_length);
    g_sprintf(plist_set_file_full_path,"%s/%s",rc_set_dir,
        play_list_setting_file);
    if(!g_file_get_contents(plist_set_file_full_path,&file_data,&s_length,
        NULL))
    {
        g_free(plist_set_file_full_path);
        return FALSE;
    }
    g_free(plist_set_file_full_path);
    file_length = s_length;
    if(file_length<1)
    {
        if(file_data!=NULL) g_free(file_data);
        return FALSE;
    }
    for(file_pointer=0;file_pointer<=file_length-1;file_pointer++)
    {
        bytechr = file_data[file_pointer];
        if(bytechr!='\n') line_length++;
        else if(line_length>0)
        {
            line = g_malloc0(line_length+16);
            for(linecount=0;linecount<=line_length-1;linecount++)
                line[linecount]=file_data[file_pointer-line_length+linecount];
            buf = g_malloc0(line_length+16);
            sscanf(line,"UR=%[^\n]", buf);
            if(line_length>=4)
            {
                if(line[0]=='U' && line[1]=='R' && line[2]=='=')  /* uri */
                {
                    gtk_list_store_append(pl_store, &iter);
                    gtk_list_store_set(pl_store, &iter, PLIST2_URI, buf,
                        PLIST2_TRACKNO, -1, -1);
                    buf[0]='\0';
                }
            }
            sscanf(line,"TI=%[^\n]", buf); /* title */
            if(line_length>=4 && pl_store!=NULL)
            {
                if(line[0]=='T' && line[1]=='I' && line[2]=='=')
                {
                    gtk_list_store_set(pl_store, &iter, PLIST2_TITLE, buf, -1);
                    buf[0]='\0';
                }
            }
            sscanf(line,"AR=%[^\n]", buf);  /* artist */
            if(line_length>=4 && pl_store!=NULL)
            {
                if(line[0]=='A' && line[1]=='R' && line[2]=='=')
                {
                    gtk_list_store_set(pl_store, &iter, PLIST2_ARTIST, buf,
                        -1);
                    buf[0]='\0';
                }
            }
            sscanf(line,"AL=%[^\n]",buf);  /* album */
            if(line_length>=4 && pl_store!=NULL)
            {
                if(line[0]=='A' && line[1]=='L' && line[2]=='=')
                {
                    gtk_list_store_set(pl_store, &iter, PLIST2_ALBUM, buf, -1);
                    buf[0]='\0';
                }
            }
            sscanf(line,"TL=%[^\n]",buf);  /* time length */
            if(line_length>=4 && pl_store!=NULL)
            {
                if(line[0]=='T' && line[1]=='L' && line[2]=='=')
                {
                    sscanf(buf,"%lld",(long long *)&timeinfo);
                    timeinfo = timeinfo / 100;
                    time_min = timeinfo / 60;
                    time_sec = timeinfo % 60;
                    g_snprintf(time_str, 63, "%02d:%02d", time_min, time_sec);
                    gtk_list_store_set(pl_store, &iter, PLIST2_LENGTH,
                        time_str, -1);
                    buf[0]='\0';
                }
            }
            sscanf(line,"TN=%[^\n]",buf);  /* track number */
            if(line_length>=4 && pl_store!=NULL)
            {
                if(line[0]=='T' && line[1]=='N' && line[2]=='=')
                {
                    sscanf(buf,"%d",&trackno);
                    gtk_list_store_set(pl_store, &iter, PLIST2_TRACKNO,
                        trackno, -1);
                    buf[0]='\0';
                }
            }
            sscanf(line,"LI=%[^\n]",buf); /* list (name) */
            if(line_length>=4)
            {
                if(line[0]=='L' && line[1]=='I' && line[2]=='=')
                {
                    listnum++;
                    listflag = TRUE;
                    existlist = TRUE;
                    rc_plist_insert_list(buf, listnum);
                    pl_store = rc_plist_get_list_store(listnum);
                    buf[0]='\0';
                }
            }
            g_free(buf);
            linenum++;
            g_free(line);
            line_length = 0L;
        }
    }
    g_free(file_data);
    if(existlist)
    {
        rc_gui_select_list1(0);
        return TRUE;
    }
    else return FALSE;
}

/*
 * Save the playlist settings.
 */

gboolean rc_plist_save_playlist_setting()
{
    const gchar *rc_set_dir = rc_get_set_dir();
    FILE *fp;
    GtkTreeIter iter_head, iter;
    GtkListStore *pl_store = NULL;
    gchar *list_name = NULL;
    gulong list_length = rc_plist_get_list1_length();
    gint fname_length = 0;
    gint time_min, time_sec;
    long long time_length = 0;
    gint list1_index, list2_index;
    gchar *list_uri, *list_title, *list_artist, *list_album, *list_time;
    gint list_trackno = -1;
    if(list_length<1) return FALSE;
    fname_length = strlen(rc_set_dir) + strlen(play_list_setting_file) + 16;
    gchar *plist_set_file_full_path = g_malloc0(fname_length);
    g_sprintf(plist_set_file_full_path,"%s/%s",rc_set_dir,
        play_list_setting_file);
    fp = fopen(plist_set_file_full_path,"wb");
    g_free(plist_set_file_full_path);
    if(fp==NULL) return FALSE;
    fprintf(fp,"/* PLEASE DO NOT EDIT THIS FILE!!! */\n");
    if(gtk_tree_model_get_iter_first(GTK_TREE_MODEL(rc_plist.list_store), 
        &iter_head))
    {
        do
        {
            gtk_tree_model_get(GTK_TREE_MODEL(rc_plist.list_store), &iter_head,
                PLIST1_NAME, &list_name, PLIST1_STORE, &pl_store, -1);
            fprintf(fp,"LI=%s\n", list_name);
            if(list_name!=NULL) g_free(list_name);
            if(pl_store==NULL) continue;
            if(gtk_tree_model_get_iter_first(GTK_TREE_MODEL(pl_store),
                &iter))
            {
                do
                {
                    gtk_tree_model_get(GTK_TREE_MODEL(pl_store), &iter,
                        PLIST2_URI, &list_uri, PLIST2_TITLE, &list_title,
                        PLIST2_ARTIST, &list_artist, PLIST2_ALBUM, &list_album,
                        PLIST2_LENGTH, &list_time, PLIST2_TRACKNO,
                        &list_trackno, -1);
                    sscanf(list_time, "%d:%d", &time_min, &time_sec);
                    time_length = (time_min * 60 + time_sec) * 100;
                    fprintf(fp, "UR=%s\n", list_uri);
                    fprintf(fp, "TI=%s\n", list_title);
                    if(list_artist!=NULL)
                        fprintf(fp, "AR=%s\n", list_artist);
                    else
                        fprintf(fp, "AR=%s\n", "");
                    if(list_album!=NULL)
                        fprintf(fp, "AL=%s\n", list_album);
                    else
                        fprintf(fp, "AL=%s\n", "");
                    fprintf(fp, "TL=%lld\n", time_length);
                    fprintf(fp, "TN=%d\n", list_trackno);
                    if(list_uri!=NULL) g_free(list_uri);
                    if(list_title!=NULL) g_free(list_title);
                    if(list_artist!=NULL) g_free(list_artist);
                    if(list_album!=NULL) g_free(list_album);
                    if(list_time!=NULL) g_free(list_time);
                }
                while(gtk_tree_model_iter_next(GTK_TREE_MODEL(pl_store),
                    &iter));
            }
        }
        while(gtk_tree_model_iter_next(GTK_TREE_MODEL(rc_plist.list_store),
            &iter_head));
    }
    fclose(fp);
    if(rc_plist_play_get_index(&list1_index, &list2_index))
    {
        rc_set_set_integer("Playlist", "LastList", list1_index);
        rc_set_set_integer("Playlist", "LastPosition", list2_index);
    }
    return TRUE;
}

/*
 * Build a default playlist if the data file do not exist.
 */

void rc_plist_build_default_list()
{
    rc_plist_insert_list(default_list_name, 0);
}

/*
 * Move item(s) in the playlist to another playlist.
 */

void rc_plist_plist_move2(gint list_index, GtkTreePath **from_paths,
    gint f_length, gint to_list_index)
{
    if(to_list_index<0 || to_list_index>=rc_plist_get_list1_length()) return;
    GtkListStore *from_list_store = NULL, *to_list_store = NULL;
    GtkTreeIter from_iter, to_iter, tmp_iter;
    GtkTreePath *path;
    gchar *list_uri, *list_state, *list_title, *list_artist, *list_album;
    gchar *list_time;
    gint *indices1;
    gint list1_index, list2_index;
    gint i = 0;
    from_list_store = rc_plist_get_list_store(list_index);
    to_list_store = rc_plist_get_list_store(to_list_index);;
    for(i=0;i<f_length;i++)
    {
        indices1 = gtk_tree_path_get_indices(from_paths[i]);
        gtk_tree_model_get_iter(GTK_TREE_MODEL(from_list_store),
            &from_iter, from_paths[i]);
        gtk_tree_model_get(GTK_TREE_MODEL(from_list_store), &from_iter,
            PLIST2_URI, &list_uri, PLIST2_STATE, &list_state, PLIST2_TITLE,
            &list_title, PLIST2_ARTIST, &list_artist, PLIST2_ALBUM,
            &list_album, PLIST2_LENGTH, &list_time, -1);
        gtk_list_store_append(to_list_store, &to_iter);
        gtk_list_store_set(to_list_store, &to_iter, PLIST2_URI, list_uri, 
            PLIST2_STATE, list_state, PLIST2_TITLE, list_title, PLIST2_ARTIST,
            list_artist, PLIST2_ALBUM, list_album, PLIST2_LENGTH, list_time,
            -1);
        if(!rc_plist_play_get_index(&list1_index, &list2_index))
        {
            list1_index = -1;
            list2_index = -1;
        }
        if(indices1!=NULL && list_index==list1_index &&
            indices1[0]==list2_index)
        {
            if(gtk_tree_row_reference_valid(rc_plist.list1_reference))
            {
                path = gtk_tree_row_reference_get_path(
                    rc_plist.list1_reference);
                if(path!=NULL)
                {
                    if(gtk_tree_model_get_iter(GTK_TREE_MODEL(
                        rc_plist.list_store), &tmp_iter, path))
                    {
                        gtk_list_store_set(rc_plist.list_store, &tmp_iter,
                            PLIST1_STATE, NULL, -1);
                    }
                    gtk_tree_path_free(path);
                }
            }
            if(rc_plist.list1_reference!=NULL)
            {
                gtk_tree_row_reference_free(rc_plist.list1_reference);
                rc_plist.list1_reference = NULL;
            }
            if(rc_plist.list2_reference!=NULL)
            {
                gtk_tree_row_reference_free(rc_plist.list2_reference);
                rc_plist.list2_reference = NULL;
            }
            path = gtk_tree_path_new_from_indices(to_list_index, -1);
            rc_plist.list1_reference = gtk_tree_row_reference_new(
                GTK_TREE_MODEL(rc_plist.list_store), path);
            if(gtk_tree_model_get_iter(GTK_TREE_MODEL(
                rc_plist.list_store), &tmp_iter, path))
            {
                gtk_list_store_set(rc_plist.list_store, &tmp_iter,
                    PLIST1_STATE, GTK_STOCK_MEDIA_PLAY, -1);
            }
            gtk_tree_path_free(path);
            path = gtk_tree_model_get_path(GTK_TREE_MODEL(to_list_store),
                &to_iter);
            rc_plist.list2_reference = gtk_tree_row_reference_new(
                GTK_TREE_MODEL(to_list_store), path);
            gtk_tree_path_free(path);
        }
        if(list_uri!=NULL) g_free(list_uri);
        if(list_state!=NULL) g_free(list_state);
        if(list_title!=NULL) g_free(list_title);
        if(list_artist!=NULL) g_free(list_artist);
        if(list_album!=NULL) g_free(list_album);
        if(list_time!=NULL) g_free(list_time);
    }
    for(i=f_length-1;i>=0;i--)
    {
        gtk_tree_model_get_iter(GTK_TREE_MODEL(from_list_store),
            &from_iter, from_paths[i]);
        gtk_list_store_remove(from_list_store, &from_iter);
    }
}

/*
 * Save the playlist.
 */

void rc_plist_save_playlist(const gchar *s_filename, gint index)
{
    if(index<0 || index>=rc_plist_get_list1_length()) return;
    if(s_filename==NULL || *s_filename=='\0') return;
    GtkListStore *pl_store;
    GtkTreeIter iter;
    gchar *filename;
    gchar *uri;
    gchar *list_title, *list_artist, *list_time;
    gint time_min, time_sec;
    glong time_length;
    FILE *fp;
    pl_store = rc_plist_get_list_store(index);
    if(!gtk_tree_model_get_iter_first(GTK_TREE_MODEL(pl_store), &iter))
        return;
    if(g_str_has_suffix(s_filename, ".M3U") || 
        g_str_has_suffix(s_filename, ".m3u"))
        filename = g_strdup(s_filename);
    else
        filename = g_strdup_printf("%s.M3U", s_filename);
    fp = fopen(filename, "wb");
    g_free(filename);
    if(fp==NULL) return;
    fprintf(fp, "#EXTM3U\n");
    do
    {
        gtk_tree_model_get(GTK_TREE_MODEL(pl_store), &iter,
            PLIST2_URI, &uri, PLIST2_TITLE, &list_title, PLIST2_ARTIST,
            &list_artist, PLIST2_LENGTH, &list_time, -1);
        if(uri!=NULL)
        {
            sscanf(list_time, "%d:%d", &time_min, &time_sec);
            time_length = time_min * 60 + time_sec;
            if(list_artist==NULL)
                fprintf(fp, "#EXTINF:%ld,%s\n%s\n", time_length,
                    list_title, uri);
            else
                fprintf(fp, "#EXTINF:%ld,%s - %s\n%s\n", time_length,
                    list_artist, list_title, uri);
        }
        if(uri!=NULL) g_free(uri);
        if(list_title!=NULL) g_free(list_title);
        if(list_artist!=NULL) g_free(list_artist);
        if(list_time!=NULL) g_free(list_time);
    }
    while(gtk_tree_model_iter_next(GTK_TREE_MODEL(pl_store), &iter));
    fclose(fp);
}

/*
 * Load the playlist.
 */

void rc_plist_load_playlist(const gchar *s_filename, gint index)
{
    if(index<0 || index>=rc_plist_get_list1_length()) return;
    if(s_filename==NULL || *s_filename=='\0') return;
    gchar *contents = NULL;
    gchar *file_list = NULL;
    gchar *file_data = NULL;
    gchar **file_array = NULL;
    gchar *line = NULL;
    gchar *uri = NULL;
    gchar *temp_name = NULL;
    gchar *path = NULL;
    gsize s_length = 0;
    guint length = 0;
    guint i = 0;
    guint linenum = 0;
    if(!g_file_get_contents(s_filename, &contents, &s_length, NULL))
        return;
    path = g_path_get_dirname(s_filename);
    file_list = g_malloc0(s_length * sizeof(gchar));
    for(i=0;i<s_length;i++)
    {
        if(contents[i]!='\r')
        {
            file_list[length] = contents[i];
            length++;
        }
    }
    g_free(contents);
    file_data = file_list;
    file_array = g_strsplit(file_data, "\n", 0);
    i = 0;
    while(file_array[linenum]!=NULL)
    {
        line = file_array[linenum];
        if(!g_str_has_prefix(line, "#") && *line!='\n' && *line!='\0')
        {
            if(!g_path_is_absolute(line) && strncmp(line, "file://", 7)!=0
                && strncmp(line, "http://", 7)!=0)
            {
                temp_name = g_strdup_printf("%s%c%s", path, G_DIR_SEPARATOR,
                    line);
                line = temp_name;
            }
            if(strncmp(line, "file://", 7)!=0 ||
                strncmp(line, "http://", 7)!=0)
                uri = g_filename_to_uri(line, NULL, NULL);
            else
                uri = g_strdup(line);
            if(uri!=NULL)
            {
                rc_plist_insert_music(uri, index, -1);
                i++;
                g_free(uri);
            }
        }
        linenum++;
    }
    g_strfreev(file_array);
    if(i>0)
        rc_gui_status_task_set(1, i);
    g_free(path);
    g_free(file_list);
}

/*
 * Get the list store the playlist.
 */

GtkListStore *rc_plist_get_list_store(gint index)
{
    GtkListStore *pl_store;
    GtkTreeIter iter;
    GtkTreePath *path;
    if(index<0) return NULL;
    path = gtk_tree_path_new_from_indices(index, -1);
    if(!gtk_tree_model_get_iter(GTK_TREE_MODEL(rc_plist.list_store), &iter,
        path))
        return NULL;
    gtk_tree_path_free(path);
    gtk_tree_model_get(GTK_TREE_MODEL(rc_plist.list_store), &iter,
        PLIST1_STORE, &pl_store, -1);
    if(pl_store==NULL) return NULL;
    return pl_store;
}

/*
 * Get the list head.
 */

GtkListStore *rc_plist_get_list_head()
{
    return rc_plist.list_store;
}

/*
 * Refresh the items in list2.
 */

gboolean rc_plist_list2_refresh(gint list1_index)
{
    RCPlistImportData *refresh_data;
    GtkListStore *store;
    GtkTreeModel *model;
    GtkTreeIter iter;
    GtkTreeRowReference *reference;
    GtkTreePath *path;
    gint i = 0;
    gboolean auto_clean;
    store = rc_plist_get_list_store(list1_index);
    if(store==NULL) return FALSE;
    model = GTK_TREE_MODEL(store);
    auto_clean = rc_set_get_boolean("Playlist", "AutoClean", NULL);
    if(!gtk_tree_model_get_iter_first(model, &iter)) return FALSE;
    do
    {
        path = gtk_tree_model_get_path(model, &iter);
        if(path==NULL) continue;
        reference = gtk_tree_row_reference_new(model, path);
        gtk_tree_path_free(path);
        if(reference==NULL) continue;
        refresh_data = g_malloc0(sizeof(RCPlistImportData));
        gtk_tree_model_get(model, &iter, PLIST2_URI, &refresh_data->uri, -1);
        refresh_data->reference = reference;
        refresh_data->refresh_flag = TRUE;
        refresh_data->auto_clean = auto_clean;
        g_async_queue_push(plist_import_job_queue, refresh_data);
        i++;
    }
    while(gtk_tree_model_iter_next(model, &iter));
    rc_gui_status_task_set(2, i);
    return TRUE;
}

/*
 * Get the remain job number in the job queue.
 */

gint rc_plist_import_job_get_length()
{
    return g_async_queue_length(plist_import_job_queue) +
        plist_import_thread_num;
}

/*
 * Cancel all jobs in the job queue.
 */

void rc_plist_import_job_cancel()
{
    RCPlistImportData *import_data;
    while(g_async_queue_length(plist_import_job_queue) +
        plist_import_thread_num>0)
    {
        import_data = g_async_queue_try_pop(plist_import_job_queue);
        if(import_data!=NULL)
        {
            if(import_data->uri!=NULL) g_free(import_data->uri);
            if(import_data->reference!=NULL)
                gtk_tree_row_reference_free(import_data->reference);
            g_free(import_data);
        }
    }
}

/*
 * Load music from the argument list of application. 
 */

void rc_plist_load_argument(char *argv[])
{
    if(argv==NULL) return;
    gint i = 0;
    gint list_index = -1;
    gint music_index = 0;
    gchar *uri = NULL;
    GtkTreeIter iter;
    gchar *list_name;
    GFile *gfile;
    if(!gtk_tree_model_get_iter_first(GTK_TREE_MODEL(rc_plist.list_store), 
        &iter)) return;
    i = 0;
    do
    {
        gtk_tree_model_get(GTK_TREE_MODEL(rc_plist.list_store), &iter,
            PLIST1_NAME, &list_name, -1);
        if(strncmp(list_name, default_list_name, 512)==0)
        {
            list_index = i;
        }
        g_free(list_name);
        i++;
    }
    while(gtk_tree_model_iter_next(GTK_TREE_MODEL(rc_plist.list_store),
        &iter));
    if(list_index<0)
    {
        list_index = 0;
        rc_plist_insert_list(default_list_name, 0);
    }
    for(i=0;argv[i]!=NULL;i++)
    {
        gfile = g_file_new_for_commandline_arg(argv[i]);
        uri = g_file_get_uri(gfile);
        g_object_unref(gfile);
        if(uri!=NULL)
        {
            rc_plist_insert_music(uri, list_index, music_index);
            music_index++;
            g_free(uri);
        }
    }
    rc_gui_select_list1(list_index);
}

/*
 * Load music from remote.
 */

gboolean rc_plist_load_uri_from_remote(const gchar *uri)
{
    if(uri==NULL) return FALSE;
    gboolean flag = FALSE;
    gint i = 0;
    gint list_index = -1;
    GtkTreeIter iter;
    gchar *list_name;
    GFile *gfile;
    gchar *ruri = NULL;
    if(!gtk_tree_model_get_iter_first(GTK_TREE_MODEL(rc_plist.list_store),
        &iter)) return FALSE;
    i = 0;
    do
    {
        gtk_tree_model_get(GTK_TREE_MODEL(rc_plist.list_store), &iter,
            PLIST1_NAME, &list_name, -1);
        if(strncmp(list_name, default_list_name, 512)==0)
        {
            list_index = i;
        }
        g_free(list_name);
        i++;
    }
    while(gtk_tree_model_iter_next(GTK_TREE_MODEL(rc_plist.list_store),
        &iter));
    gfile = g_file_new_for_commandline_arg(uri);
    ruri = g_file_get_uri(gfile);
    g_object_unref(gfile);
    if(ruri!=NULL)
    {
        if(list_index<0)
        {
            list_index = 0;
            rc_plist_insert_list(default_list_name, 0);
        }
        flag = rc_plist_insert_music(ruri, list_index, -1);
        g_free(ruri);
    }
    return flag;
}

