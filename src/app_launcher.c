/*
 * Copyright (C) 2021 Collabora Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "app_info.h"
#include "app_launcher.h"
#include "systemd_manager.h"
#include "utils.h"

typedef struct _AppLauncher {
    applaunchdAppLaunchSkeleton parent;

/* TODO: try to move event and bus properties down to systemd_manager */
    sd_event *event;
    sd_bus *bus;

    SystemdManager *systemd_manager;

    GList *apps_list;
} AppLauncher;

extern GMainLoop *main_loop;

extern GSource *g_sd_event_create_source(sd_event *event, sd_bus *bus);

static void app_launcher_iface_init(applaunchdAppLaunchIface *iface);

G_DEFINE_TYPE_WITH_CODE(AppLauncher, app_launcher,
                        APPLAUNCHD_TYPE_APP_LAUNCH_SKELETON,
                        G_IMPLEMENT_INTERFACE(APPLAUNCHD_TYPE_APP_LAUNCH,
                                              app_launcher_iface_init));

static void app_launcher_started_cb(AppLauncher *self,
                                    const gchar *app_id,
                                    gpointer caller);

/*
 * Internal functions
 */

/*
 * This function is executed during the object initialization. It goes through
 * all available applications on the system and creates a static list
 * containing all the relevant info (ID, name, unit, icon...) for further
 * processing.
 */
static void app_launcher_update_applications_list(AppLauncher *self)
{
    g_auto(GStrv) dirlist = NULL;

    char *xdg_data_dirs = getenv("XDG_DATA_DIRS");
    if (xdg_data_dirs)
        dirlist = g_strsplit(getenv("XDG_DATA_DIRS"), ":", -1);

    GList *units = NULL;
    if (!systemd_manager_enumerate_app_units(self->systemd_manager, self, &units)) {
        return;
    }

    GList *iterator;
    for (iterator = units; iterator != NULL; iterator = iterator->next) {
        g_autofree const gchar *app_id = NULL;
        g_autofree const gchar *icon_path = NULL;
        AppInfo *app_info = NULL;

        if (!iterator->data)
            continue;

        // Parse service and app id out of unit filename
        gchar *service = g_strrstr(iterator->data, "/");
        if (!service)
            service = iterator->data;
        else
            service += 1;

        g_autofree char *tmp = g_strdup(service);
        char *end = tmp + strlen(tmp);
        while (end > tmp && *end != '.') {
            --end;
        }
        if (end > tmp) {
            *end = '\0';
        } else {
            g_free(tmp);
            continue;
        }
        while (end > tmp && *end != '@') {
            --end;
        }
        if (end > tmp) {
            app_id = g_strdup(end + 1);
        }
        // Potentially handle non-template agl-app-foo.service units here

        // Try getting display name from unit Description property
        char *name = NULL;
        if (!systemd_manager_get_app_description(self->systemd_manager,
                                                 self,
                                                 service,
                                                 &name) ||
            name == NULL) {

            // Fall back to the application ID
            g_warning("Could not retrieve Description of '%s'", service);
            name = app_id;
        }

        /*
         * GAppInfo retrieves the icon data but doesn't provide a way to retrieve
         * the corresponding file name, so we have to look it up by ourselves.
         */
        if (app_id && dirlist)
            icon_path = applaunchd_utils_get_icon(dirlist, app_id);

        app_info = app_info_new(app_id,
                                name,
                                icon_path ? icon_path : "",
                                service);

        g_debug("Adding application '%s' with display name '%s'", app_id, name);

        self->apps_list = g_list_append(self->apps_list, app_info);
    }
    g_list_free_full(units, g_free);
}

/*
 * Construct the application list to be sent over D-Bus. It has format "av", meaning
 * the list itself is an array, each item being a variant consisting of 3 strings:
 *   - app-id
 *   - app name
 *   - icon path
 */
static GVariant *app_launcher_get_list_variant(AppLauncher *self, gboolean graphical)
{
    GVariantBuilder builder;
    guint len = g_list_length(self->apps_list);

    /* Init array variant for storing the applications list */
    g_variant_builder_init (&builder, G_VARIANT_TYPE_ARRAY);

    for (guint i = 0; i < len; i++) {
        GVariantBuilder app_builder;
        AppInfo *app_info = g_list_nth_data(self->apps_list, i);

        g_variant_builder_init (&app_builder, G_VARIANT_TYPE("(sss)"));

        /* Create application entry */
        g_variant_builder_add(&app_builder, "s", app_info_get_app_id(app_info));
        g_variant_builder_add(&app_builder, "s", app_info_get_name(app_info));
        g_variant_builder_add(&app_builder, "s", app_info_get_icon_path(app_info));

        /* Add entry to apps list */
        g_variant_builder_add(&builder, "v", g_variant_builder_end(&app_builder));
    }

    return g_variant_builder_end(&builder);
}

/*
 * Starts the requested application using either the D-Bus activation manager
 * or the process manager.
 */
static gboolean app_launcher_start_app(AppLauncher *self, AppInfo *app_info)
{
    g_return_val_if_fail(APPLAUNCHD_IS_APP_LAUNCHER(self), FALSE);
    g_return_val_if_fail(APPLAUNCHD_IS_APP_INFO(app_info), FALSE);

    AppStatus app_status = app_info_get_status(app_info);
    const gchar *app_id = app_info_get_app_id(app_info);

    switch (app_status) {
    case APP_STATUS_STARTING:
        g_debug("Application '%s' is already starting", app_id);
        return TRUE;
    case APP_STATUS_RUNNING:
        g_debug("Application '%s' is already running", app_id);
        /*
        * The application may be running in the background, activate it
        * and notify subscribers it should be activated/brought to the
        * foreground
        */
        app_launcher_started_cb(self, app_id, NULL);
        return TRUE;
    case APP_STATUS_INACTIVE:
        systemd_manager_start_app(self->systemd_manager, app_info);
        return TRUE;
    default:
        g_critical("Unknown status %d for application '%s'", app_status, app_id);
        break;
    }

    return FALSE;
}

/*
 * Internal callbacks
 */

/*
 * Handler for the "start" D-Bus method.
 */
static gboolean app_launcher_handle_start(applaunchdAppLaunch *object,
                                          GDBusMethodInvocation *invocation,
                                          const gchar *app_id)
{
    AppInfo *app;
    AppLauncher *self = APPLAUNCHD_APP_LAUNCHER(object);
    g_return_val_if_fail(APPLAUNCHD_IS_APP_LAUNCHER(self), FALSE);

    /* Search the apps list for the given app-id */
    app = app_launcher_get_app_info(self, app_id);
    if (!app) {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
                                              G_DBUS_ERROR_INVALID_ARGS,
                                              "Unknown application '%s'",
                                              app_id);
        return FALSE;
    }

    app_launcher_start_app(self, app);
    applaunchd_app_launch_complete_start(object, invocation);

    return TRUE;
}

/*
 * Handler for the "listApplications" D-Bus method.
 */
static gboolean app_launcher_handle_list_applications(applaunchdAppLaunch *object,
                                                      GDBusMethodInvocation *invocation,
                                                      gboolean graphical)
{
    GVariant *result = NULL;
    AppLauncher *self = APPLAUNCHD_APP_LAUNCHER(object);
    g_return_val_if_fail(APPLAUNCHD_IS_APP_LAUNCHER(self), FALSE);

    /* Retrieve the applications list in the right format for sending over D-Bus */
    result = app_launcher_get_list_variant(self, graphical);
    applaunchd_app_launch_complete_list_applications(object, invocation, result);

    return TRUE;
}

/*
 * Callback for the "started" signal emitted by both the
 * process manager and D-Bus activation manager. Forwards
 * the signal to other applications through D-Bus.
 */
static void app_launcher_started_cb(AppLauncher *self,
                                    const gchar *app_id,
                                    gpointer caller)
{
    applaunchdAppLaunch *iface = APPLAUNCHD_APP_LAUNCH(self);
    g_return_if_fail(APPLAUNCHD_IS_APP_LAUNCH(iface));

    g_debug("Application '%s' started", app_id);
    /*
     * Emit the "started" D-Bus signal so subscribers get notified
     * the application with ID "app_id" started and should be
     * activated
     */
    applaunchd_app_launch_emit_started(iface, app_id);
}

/*
 * Callback for the "terminated" signal emitted by both the
 * process manager and D-Bus activation manager. Forwards
 * the signal to other applications through D-Bus.
 */
static void app_launcher_terminated_cb(AppLauncher *self,
                                       const gchar *app_id,
                                       gpointer caller)
{
    applaunchdAppLaunch *iface = APPLAUNCHD_APP_LAUNCH(self);
    g_return_if_fail(APPLAUNCHD_IS_APP_LAUNCH(iface));

    g_debug("Application '%s' terminated", app_id);
    /*
     * Emit the "terminated" D-Bus signal so subscribers get
     * notified the application with ID "app_id" terminated
     */
    applaunchd_app_launch_emit_terminated(iface, app_id);
}

/*
 * Initialization & cleanup functions
 */

static void app_launcher_dispose(GObject *object)
{
    AppLauncher *self = APPLAUNCHD_APP_LAUNCHER(object);

    if (self->apps_list)
        g_list_free_full(g_steal_pointer(&self->apps_list), g_object_unref);

    g_clear_object(&self->systemd_manager);

    G_OBJECT_CLASS(app_launcher_parent_class)->dispose(object);
}

static void app_launcher_finalize(GObject *object)
{
    G_OBJECT_CLASS(app_launcher_parent_class)->finalize(object);
}

static void app_launcher_class_init(AppLauncherClass *klass)
{
    GObjectClass *object_class = (GObjectClass *)klass;

    object_class->dispose = app_launcher_dispose;
}

static void app_launcher_iface_init(applaunchdAppLaunchIface *iface)
{
    iface->handle_start = app_launcher_handle_start;
    iface->handle_list_applications = app_launcher_handle_list_applications;
}

static void app_launcher_init (AppLauncher *self)
{
    sd_bus_open_system(&self->bus);
    sd_event_default(&self->event);
    sd_bus_attach_event(self->bus, self->event, SD_EVENT_PRIORITY_NORMAL);
    g_source_attach(g_sd_event_create_source(self->event, self->bus), g_main_loop_get_context(main_loop));

    /*
     * Create the systemd manager and connect to its signals
     * so we get notified on app startup/termination
     */
    self->systemd_manager = g_object_new(APPLAUNCHD_TYPE_SYSTEMD_MANAGER,
                                         NULL);
    g_signal_connect_swapped(self->systemd_manager, "started",
                             G_CALLBACK(app_launcher_started_cb), self);
    g_signal_connect_swapped(self->systemd_manager, "terminated",
                             G_CALLBACK(app_launcher_terminated_cb), self);

    /* Initialize the applications list */
    app_launcher_update_applications_list(self);
}

/*
 * Public functions
 */

AppLauncher *app_launcher_get_default(void)
{
    static AppLauncher *launcher;

    /*
    * AppLauncher is a singleton, only create the object if it doesn't
    * exist already.
    */
    if (launcher == NULL) {
        g_debug("Initializing app launcher service...");
        launcher = g_object_new(APPLAUNCHD_TYPE_APP_LAUNCHER, NULL);
        g_object_add_weak_pointer(G_OBJECT(launcher), (gpointer *)&launcher);
    }

    return launcher;
}

/*
 * Search the applications list for an app which matches the provided app-id
 * and return the corresponding AppInfo object.
 */
AppInfo *app_launcher_get_app_info(AppLauncher *self, const gchar *app_id)
{
    g_return_val_if_fail(APPLAUNCHD_IS_APP_LAUNCHER(self), NULL);

    guint len = g_list_length(self->apps_list);

    for (guint i = 0; i < len; i++) {
        AppInfo *app_info = g_list_nth_data(self->apps_list, i);

        if (g_strcmp0(app_info_get_app_id(app_info), app_id) == 0)
            return app_info;
    }

    g_warning("Unable to find application with ID '%s'", app_id);

    return NULL;
}

sd_bus *app_launcher_get_bus(AppLauncher *self)
{
    return self->bus;
}

sd_event *app_launcher_get_event(AppLauncher *self)
{
    return self->event;
}
