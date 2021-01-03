/*
 * Copyright © 2020 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Dylan McCall <dylan@dylanmccall.ca>
 */

#include "config.h"

#include <string.h>
#include <gio/gio.h>

#include "idle-monitor.h"
#include "request.h"
#include "session.h"
#include "permissions.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-utils.h"

#define PERMISSION_TABLE "idle-monitor"
#define PERMISSION_ID "idle-monitor"

typedef struct _IdleMonitor IdleMonitor;
typedef struct _IdleMonitorClass IdleMonitorClass;

struct _IdleMonitor
{
  XdpIdleMonitorSkeleton parent_instance;
};

struct _IdleMonitorClass
{
  XdpIdleMonitorSkeletonClass parent_class;
};

static XdpImplIdleMonitor *impl;
static IdleMonitor *idle_monitor;

GType idle_monitor_get_type (void) G_GNUC_CONST;
static void idle_monitor_iface_init (XdpIdleMonitorIface *iface);

G_DEFINE_TYPE_WITH_CODE (IdleMonitor, idle_monitor, XDP_TYPE_IDLE_MONITOR_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_TYPE_IDLE_MONITOR, idle_monitor_iface_init));


static gboolean
get_idle_monitor_allowed (const char *app_id)
{
  Permission permission;

  permission = get_permission_sync (app_id, PERMISSION_TABLE, PERMISSION_ID);

  if (permission == PERMISSION_NO)
    return FALSE;

  if (permission == PERMISSION_UNSET)
    {
      g_debug ("No idle-mnoitor permissions stored for %s: allowing", app_id);

      set_permission_sync (app_id, PERMISSION_TABLE, PERMISSION_ID, PERMISSION_YES);
    }

  return TRUE;
}

/* TODO: Implement add_idle_watch */
/* static XdpOptionKey add_idle_watch_options[] = { */
/*   { "interval", G_VARIANT_TYPE_INT64, NULL } */
/* }; */

/* TODO: Implement add_user_active_watch */

/* TODO: Implement remove_watch */

static void
get_idletime_done (GObject *source,
                   GAsyncResult *result,
                   gpointer data)
{
  Request *request = data;
  guint response = 0;
  g_autoptr(GError) error = NULL;

  REQUEST_AUTOLOCK (request);

  if (!xdp_impl_idle_monitor_call_get_idletime_finish (impl, &response, result, &error))
    response = 2;

  if (request->exported)
    {
      GVariantBuilder new_results;

      g_variant_builder_init (&new_results, G_VARIANT_TYPE_VARDICT);

      xdp_request_emit_response (XDP_REQUEST (request),
                                 response,
                                 g_variant_builder_end (&new_results));
    }
}

static void
handle_get_idletime_in_thread_func (GTask *task,
                               gpointer source_object,
                               gpointer task_data,
                               GCancellable *cancellable)
{
  Request *request = (Request *)task_data;
  const char *app_id;

  REQUEST_AUTOLOCK (request);

  app_id = xdp_app_info_get_id (request->app_info);

  if (!get_idle_monitor_allowed (app_id))
    return;

  g_debug ("Calling idle_monitor backend for %s", app_id);
  xdp_impl_idle_monitor_call_get_idletime (impl,
                                           request->id,
                                           app_id,
                                           NULL,
                                           get_idletime_done,
                                           g_object_ref (request));
  }

static gboolean
handle_get_idletime (XdpIdleMonitor *object,
                GDBusMethodInvocation *invocation,
                const char *arg_window,
                guint32 arg_flags,
                GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation);
  g_autoptr(GTask) task = NULL;
  g_autoptr(GError) error = NULL;

  task = g_task_new (object, NULL, NULL, NULL);
  g_task_set_task_data (task, g_object_ref (request), g_object_unref);
  g_task_run_in_thread (task, handle_get_idletime_in_thread_func);

  xdp_idle_monitor_complete_get_idletime (object, invocation, request->id);

  return TRUE;
}

static void
idle_monitor_iface_init (XdpIdleMonitorIface *iface)
{
  iface->handle_get_idletime = handle_get_idletime;
}

static void
idle_monitor_init (IdleMonitor *idle_monitor)
{
  xdp_idle_monitor_set_version (XDP_IDLE_MONITOR (idle_monitor), 3);
}

static void
idle_monitor_class_init (IdleMonitorClass *klass)
{
}

static void
watch_fired_cb (XdpImplIdleMonitor *impl,
                const char *session_id,
                GVariant *state,
                gpointer data)
{
  GDBusConnection *connection = g_dbus_proxy_get_connection (G_DBUS_PROXY (impl));

  guint32 watch_id = 0;
  gboolean is_listening = TRUE;

  g_variant_lookup (state, "session-state", "u", &watch_id);
  g_debug ("Received watch-fired %s: watch-id: %d",
           session_id, watch_id);

  /* FIXME: Only fire if the client has created this watch */

  if (is_listening)
    g_dbus_connection_emit_signal (connection,
                                   NULL, // FIXME: Only emit for the watching client
                                   "/org/freedesktop/portal/desktop",
                                   "org.freedesktop.portal.IdleMonitor",
                                   "WatchFired",
                                   g_variant_new ("(u)", watch_id),
                                   NULL);
}

GDBusInterfaceSkeleton *
idle_monitor_create (GDBusConnection *connection,
                const char *dbus_name)
{
  g_autoptr(GError) error = NULL;

  impl = xdp_impl_idle_monitor_proxy_new_sync (connection,
                                          G_DBUS_PROXY_FLAGS_NONE,
                                          dbus_name,
                                          "/org/freedesktop/portal/desktop",
                                          NULL, &error);
  if (impl == NULL)
    {
      g_warning ("Failed to create idle_monitor proxy: %s", error->message);
      return NULL;
    }

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (impl), G_MAXINT);

  idle_monitor = g_object_new (idle_monitor_get_type (), NULL);

  g_signal_connect (impl, "watch-fired", G_CALLBACK (watch_fired_cb), idle_monitor);

  return G_DBUS_INTERFACE_SKELETON (idle_monitor);
}
