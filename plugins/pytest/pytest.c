/*
 * Lyric Show Plugin
 * Show lyric in the player, or in a single window.
 *
 * lyricshow.c
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

#include <pygobject.h>
#include <pygtk/pygtk.h>
#include <Python.h>
#include <pythonrun.h>
#include <signal.h>
#include <glib.h>
#include <glib/gprintf.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <gdk/gdk.h>
#include <gtk/gtk.h>
#include "plugin.h"
#include "lyric.h"
#include "core.h"
#include "gui.h"
#include "gui_dialog.h"
#include "debug.h"
#include "player_object.h"
#include "settings.h"

static RCPluginModuleData plugin_module_data =
{
    RC_PLUGIN_MAGIC_NUMBER, /* magic_number */
    "PyTest", /* group_name */
    NULL, /* path */
    FALSE, /* resident */
    0 /* id */
};

static GKeyFile *keyfile = NULL;
static GModule *self_module = NULL;
static const gchar *py_module = "pytest.py";

static void rc_plugin_module_pygtk_init()
{
    init_pygtk();
}

static gint rc_plugin_module_python_init()
{
    gint ret;
    char *argv[] = {"rhythmcat", "rhythmcat", NULL };
    struct sigaction old_sigint;
    PyObject *pygtk, *mdict, *require;
    PyObject *gtk, *pygtk_version, *pygtk_required_version;
    PyObject *gettext;
    if(Py_IsInitialized())
    {
        rc_debug_module_pmsg(plugin_module_data.group_name,
            "Initialized already!");
        return 0;
    }
    ret = sigaction(SIGINT, NULL, &old_sigint);
    if(ret!=0)
    {
        rc_debug_module_perror(plugin_module_data.group_name,
            "Cannot save SIGINT handler!");
        return 2;
    }
    Py_Initialize();
    ret = sigaction(SIGINT, &old_sigint, NULL);
    if(ret!=0)
    {
        rc_debug_module_perror(plugin_module_data.group_name,
            "Cannot restore SIGINT handler!");
        return 2;
    }
    PySys_SetArgv(1, argv);
    pygtk = PyImport_ImportModule("pygtk");
    if(pygtk==NULL)
    {
        rc_debug_module_perror(plugin_module_data.group_name,
            "Cannot not import PyGtk!");
        PyErr_Print();
        return 3;
    }
    mdict = PyModule_GetDict(pygtk);
    require = PyDict_GetItemString(mdict, "require");
    PyObject_CallObject(require, Py_BuildValue("(S)",
        PyString_FromString("2.0")));
    if(PyErr_Occurred())
    {
        rc_debug_module_perror(plugin_module_data.group_name,
            "Require PyGtk 2.0 failed!");
        PyErr_Print();
        return 3;
    }
    if(pygobject_init(2, 16, 0)==NULL)
    {
        rc_debug_module_perror(plugin_module_data.group_name,
           "Could not initialize PyGObject!");
        PyErr_Print();
        return 4;
    }
    pyg_disable_warning_redirections();
    rc_plugin_module_pygtk_init();
    if(PyErr_Occurred())
    {
        rc_debug_module_perror(plugin_module_data.group_name,
            "Cannot not initialize PyGtk!");
        PyErr_Print();
        return 5;
    }
    pyg_enable_threads();
    gtk = PyImport_ImportModule("gtk");
    if(gtk==NULL)
    {
        rc_debug_module_perror(plugin_module_data.group_name,
            "Cannot not import GTK!");
        PyErr_Print();
        return 5;
    }
    mdict = PyModule_GetDict(gtk);
    pygtk_version = PyDict_GetItemString(mdict, "pygtk_version");
    pygtk_required_version = Py_BuildValue("(iii)", 2, 8, 0);
    if(PyObject_Compare(pygtk_version, pygtk_required_version)==-1)
    {
        rc_debug_module_perror(plugin_module_data.group_name,
            "PyGTK %s required, but %s found.",
            PyString_AsString(PyObject_Repr(pygtk_required_version)),
            PyString_AsString(PyObject_Repr(pygtk_version)));
        Py_DECREF(pygtk_required_version);
        return 6;
    }
    gettext = PyImport_ImportModule("gettext");
    if(gettext==NULL)
    {
        rc_debug_module_perror(plugin_module_data.group_name,
            "Cannot not import gettext");
        PyErr_Print();
        return 7;
    }
    return 0;
}

static gint rc_plugin_module_python_run(const gchar *filename)
{
    PyObject *main_module, *main_dict, *run_module; 
    FILE *py_file;
    gchar *dir = NULL, *path = NULL;
    if(plugin_module_data.path!=NULL)
        dir = g_path_get_dirname(plugin_module_data.path);
    else
        dir = g_path_get_dirname(g_module_name(self_module));
    if(dir==NULL)
    {
        rc_debug_module_perror(plugin_module_data.group_name,
            "Cannot get plugin dir!");
        return 1;
    }
    path = g_build_filename(dir, filename, NULL);
    g_free(dir);
    if(path==NULL)
    {
        rc_debug_module_perror(plugin_module_data.group_name,
            "Cannot make file path!");
        return 1;
    }
    py_file = g_fopen(path, "r");
    g_free(path);
    if(py_file==NULL)
    {
        rc_debug_module_perror(plugin_module_data.group_name,
            "Cannot not open Python file!");
        return 2;
    }
    main_module = PyImport_AddModule("__main__");
    main_dict = PyModule_GetDict(main_module);
    run_module = PyRun_File(py_file, filename, Py_file_input, main_dict,
        main_dict);
    fclose(py_file);
    if(run_module==NULL)
    {
        rc_debug_module_perror(plugin_module_data.group_name,
            "Cannot run Python file!");
        PyErr_Print();
        return 3;
    }
    return 0;
}

G_MODULE_EXPORT const gchar *g_module_check_init(GModule *module)
{
    rc_debug_module_pmsg(plugin_module_data.group_name,
        "Plugin loaded successfully!");
    keyfile = rc_set_get_plugin_configure();
    self_module = module;
    return NULL;
}

G_MODULE_EXPORT void g_module_unload(GModule *module)
{
    rc_debug_module_pmsg(plugin_module_data.group_name,
        "Plugin exited!");
}

G_MODULE_EXPORT gint rc_plugin_module_init()
{
    gint ret;
    #ifdef USE_GTK3
        if(gtk_major_version<3)
        {
            rc_debug_perror("LRCShow-ERROR: This plugin need GTK+ 3.0 or "
                "newer version.\n");
            rc_gui_show_message_dialog(GTK_MESSAGE_ERROR,
                _("Cannot start Lyric Show"),
                _("This plugin need GTK+ 3.0 or newer version."));
            return 1;
        }
    #else
        if(gtk_major_version!=2 || gtk_minor_version<20)
        {
            rc_gui_show_message_dialog(GTK_MESSAGE_ERROR,
                _("Cannot start Lyric Show"),
                _(" This plugin need GTK+ 2.20 or newer GTK+ 2 version, "
                "somehow this plugin doesn't work on GTK+ 3.0."));
            rc_debug_perror("LRCShow-ERROR: This plugin need GTK+ 2.20 or "
                "newer version, somehow it doesn't work on GTK+ 3.0.\n");
            return 1;
        }
    #endif
    ret = rc_plugin_module_python_init();
    if(ret!=0)
    {
        Py_Finalize();
        return ret;
    }
    ret = rc_plugin_module_python_run(py_module);
    if(ret!=0)
    {
        Py_Finalize();
    }
    return ret;
}

G_MODULE_EXPORT void rc_plugin_module_exit()
{
    if(Py_IsInitialized()) Py_Finalize();
}

G_MODULE_EXPORT void rc_plugin_module_configure()
{

}

G_MODULE_EXPORT const RCPluginModuleData *rc_plugin_module_data()
{
    return &plugin_module_data;
}

