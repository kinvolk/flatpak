/*
 * Copyright © 2014 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 */

#include "config.h"

#include <string.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/personality.h>
#include <grp.h>
#include <unistd.h>
#include <gio/gunixfdlist.h>

#ifdef ENABLE_SECCOMP
#include <seccomp.h>
#endif

#ifdef ENABLE_XAUTH
#include <X11/Xauth.h>
#endif

#include <glib/gi18n.h>

#include <gio/gio.h>
#include "libglnx/libglnx.h"

#include "flatpak-run.h"
#include "flatpak-proxy.h"
#include "flatpak-utils.h"
#include "flatpak-dir.h"
#include "flatpak-systemd-dbus.h"
#include "document-portal/xdp-dbus.h"
#include "lib/flatpak-error.h"

#define DEFAULT_SHELL "/bin/sh"

typedef enum {
  FLATPAK_CONTEXT_SHARED_NETWORK   = 1 << 0,
  FLATPAK_CONTEXT_SHARED_IPC       = 1 << 1,
} FlatpakContextShares;

/* In numerical order of more privs */
typedef enum {
  FLATPAK_FILESYSTEM_MODE_READ_ONLY    = 1,
  FLATPAK_FILESYSTEM_MODE_READ_WRITE   = 2,
  FLATPAK_FILESYSTEM_MODE_CREATE       = 3,
} FlatpakFilesystemMode;


/* Same order as enum */
const char *flatpak_context_shares[] = {
  "network",
  "ipc",
  NULL
};

typedef enum {
  FLATPAK_CONTEXT_SOCKET_X11         = 1 << 0,
  FLATPAK_CONTEXT_SOCKET_WAYLAND     = 1 << 1,
  FLATPAK_CONTEXT_SOCKET_PULSEAUDIO  = 1 << 2,
  FLATPAK_CONTEXT_SOCKET_SESSION_BUS = 1 << 3,
  FLATPAK_CONTEXT_SOCKET_SYSTEM_BUS  = 1 << 4,
} FlatpakContextSockets;

/* Same order as enum */
const char *flatpak_context_sockets[] = {
  "x11",
  "wayland",
  "pulseaudio",
  "session-bus",
  "system-bus",
  NULL
};

const char *dont_mount_in_root[] = {
  ".", "..", "lib", "lib32", "lib64", "bin", "sbin", "usr", "boot", "root",
  "tmp", "etc", "app", "run", "proc", "sys", "dev", "var", NULL
};

/* We don't want to export paths pointing into these, because they are readonly
   (so we can't create mountpoints there) and don't match whats on the host anyway */
const char *dont_export_in[] = {
  "/lib", "/lib32", "/lib64", "/bin", "/sbin", "/usr", "/etc", "/app", "/dev", NULL
};

typedef enum {
  FLATPAK_CONTEXT_DEVICE_DRI         = 1 << 0,
  FLATPAK_CONTEXT_DEVICE_ALL         = 1 << 1,
  FLATPAK_CONTEXT_DEVICE_KVM         = 1 << 2,
} FlatpakContextDevices;

const char *flatpak_context_devices[] = {
  "dri",
  "all",
  "kvm",
  NULL
};

typedef enum {
  FLATPAK_CONTEXT_FEATURE_DEVEL        = 1 << 0,
  FLATPAK_CONTEXT_FEATURE_MULTIARCH    = 1 << 1,
} FlatpakContextFeatures;

const char *flatpak_context_features[] = {
  "devel",
  "multiarch",
  NULL
};

static gboolean
add_dbus_proxy_args (GPtrArray *argv_array,
                     GPtrArray *session_dbus_proxy_argv,
                     gboolean   enable_session_logging,
                     GPtrArray *system_dbus_proxy_argv,
                     gboolean   enable_system_logging,
                     GPtrArray *a11y_dbus_proxy_argv,
                     gboolean   enable_a11y_logging,
                     int        sync_fds[2],
                     const char *app_info_path,
                     GError   **error);

struct FlatpakContext
{
  FlatpakContextShares  shares;
  FlatpakContextShares  shares_valid;
  FlatpakContextSockets sockets;
  FlatpakContextSockets sockets_valid;
  FlatpakContextDevices devices;
  FlatpakContextDevices devices_valid;
  FlatpakContextFeatures  features;
  FlatpakContextFeatures  features_valid;
  GHashTable           *env_vars;
  GHashTable           *persistent;
  GHashTable           *filesystems;
  GHashTable           *session_bus_policy;
  GHashTable           *system_bus_policy;
  GHashTable           *generic_policy;
};

FlatpakContext *
flatpak_context_new (void)
{
  FlatpakContext *context;

  context = g_slice_new0 (FlatpakContext);
  context->env_vars = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  context->persistent = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  context->filesystems = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  context->session_bus_policy = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  context->system_bus_policy = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  context->generic_policy = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                   g_free, (GDestroyNotify)g_strfreev);

  return context;
}

void
flatpak_context_free (FlatpakContext *context)
{
  g_hash_table_destroy (context->env_vars);
  g_hash_table_destroy (context->persistent);
  g_hash_table_destroy (context->filesystems);
  g_hash_table_destroy (context->session_bus_policy);
  g_hash_table_destroy (context->system_bus_policy);
  g_hash_table_destroy (context->generic_policy);
  g_slice_free (FlatpakContext, context);
}

static guint32
flatpak_context_bitmask_from_string (const char *name, const char **names)
{
  guint32 i;

  for (i = 0; names[i] != NULL; i++)
    {
      if (strcmp (names[i], name) == 0)
        return 1 << i;
    }

  return 0;
}

static char **
flatpak_context_bitmask_to_string (guint32 enabled, guint32 valid, const char **names)
{
  guint32 i;
  GPtrArray *array;

  array = g_ptr_array_new ();

  for (i = 0; names[i] != NULL; i++)
    {
      guint32 bitmask = 1 << i;
      if (valid & bitmask)
        {
          if (enabled & bitmask)
            g_ptr_array_add (array, g_strdup (names[i]));
          else
            g_ptr_array_add (array, g_strdup_printf ("!%s", names[i]));
        }
    }

  g_ptr_array_add (array, NULL);
  return (char **) g_ptr_array_free (array, FALSE);
}

static void
flatpak_context_bitmask_to_args (guint32 enabled, guint32 valid, const char **names,
                                 const char *enable_arg, const char *disable_arg,
                                 GPtrArray *args)
{
  guint32 i;

  for (i = 0; names[i] != NULL; i++)
    {
      guint32 bitmask = 1 << i;
      if (valid & bitmask)
        {
          if (enabled & bitmask)
            g_ptr_array_add (args, g_strdup_printf ("%s=%s", enable_arg, names[i]));
          else
            g_ptr_array_add (args, g_strdup_printf ("%s=%s", disable_arg, names[i]));
        }
    }
}


static FlatpakContextShares
flatpak_context_share_from_string (const char *string, GError **error)
{
  FlatpakContextShares shares = flatpak_context_bitmask_from_string (string, flatpak_context_shares);

  if (shares == 0)
    {
      g_autofree char *values = g_strjoinv (", ", (char **)flatpak_context_shares);
      g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
                   _("Unknown share type %s, valid types are: %s"), string, values);
    }

  return shares;
}

static char **
flatpak_context_shared_to_string (FlatpakContextShares shares, FlatpakContextShares valid)
{
  return flatpak_context_bitmask_to_string (shares, valid, flatpak_context_shares);
}

static void
flatpak_context_shared_to_args (FlatpakContextShares shares,
                                FlatpakContextShares valid,
                                GPtrArray *args)
{
  return flatpak_context_bitmask_to_args (shares, valid, flatpak_context_shares, "--share", "--unshare", args);
}

static FlatpakPolicy
flatpak_policy_from_string (const char *string, GError **error)
{
  const char *policies[] = { "none", "see", "filtered", "talk", "own", NULL };
  int i;
  g_autofree char *values = NULL;

  for (i = 0; policies[i]; i++)
    {
      if (strcmp (string, policies[i]) == 0)
        return i;
    }

  values = g_strjoinv (", ", (char **)policies);
  g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
               _("Unknown policy type %s, valid types are: %s"), string, values);

  return -1;
}

static const char *
flatpak_policy_to_string (FlatpakPolicy policy)
{
  if (policy == FLATPAK_POLICY_SEE)
    return "see";
  if (policy == FLATPAK_POLICY_TALK)
    return "talk";
  if (policy == FLATPAK_POLICY_OWN)
    return "own";

  return "none";
}

static gboolean
flatpak_verify_dbus_name (const char *name, GError **error)
{
  const char *name_part;
  g_autofree char *tmp = NULL;

  if (g_str_has_suffix (name, ".*"))
    {
      tmp = g_strndup (name, strlen (name) - 2);
      name_part = tmp;
    }
  else
    {
      name_part = name;
    }

  if (g_dbus_is_name (name_part) && !g_dbus_is_unique_name (name_part))
    return TRUE;

  g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
               _("Invalid dbus name %s\n"), name);
  return FALSE;
}

static FlatpakContextSockets
flatpak_context_socket_from_string (const char *string, GError **error)
{
  FlatpakContextSockets sockets = flatpak_context_bitmask_from_string (string, flatpak_context_sockets);

  if (sockets == 0)
    {
      g_autofree char *values = g_strjoinv (", ", (char **)flatpak_context_sockets);
      g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
                   _("Unknown socket type %s, valid types are: %s"), string, values);
    }

  return sockets;
}

static char **
flatpak_context_sockets_to_string (FlatpakContextSockets sockets, FlatpakContextSockets valid)
{
  return flatpak_context_bitmask_to_string (sockets, valid, flatpak_context_sockets);
}

static void
flatpak_context_sockets_to_args (FlatpakContextSockets sockets,
                                 FlatpakContextSockets valid,
                                 GPtrArray *args)
{
  return flatpak_context_bitmask_to_args (sockets, valid, flatpak_context_sockets, "--socket", "--nosocket", args);
}

static FlatpakContextDevices
flatpak_context_device_from_string (const char *string, GError **error)
{
  FlatpakContextDevices devices = flatpak_context_bitmask_from_string (string, flatpak_context_devices);

  if (devices == 0)
    {
      g_autofree char *values = g_strjoinv (", ", (char **)flatpak_context_devices);
      g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
                   _("Unknown device type %s, valid types are: %s"), string, values);
    }
  return devices;
}

static char **
flatpak_context_devices_to_string (FlatpakContextDevices devices, FlatpakContextDevices valid)
{
  return flatpak_context_bitmask_to_string (devices, valid, flatpak_context_devices);
}

static void
flatpak_context_devices_to_args (FlatpakContextDevices devices,
                                 FlatpakContextDevices valid,
                                 GPtrArray *args)
{
  return flatpak_context_bitmask_to_args (devices, valid, flatpak_context_devices, "--device", "--nodevice", args);
}

static FlatpakContextFeatures
flatpak_context_feature_from_string (const char *string, GError **error)
{
  FlatpakContextFeatures feature = flatpak_context_bitmask_from_string (string, flatpak_context_features);

  if (feature == 0)
    {
      g_autofree char *values = g_strjoinv (", ", (char **)flatpak_context_features);
      g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
                   _("Unknown feature type %s, valid types are: %s"), string, values);
    }

  return feature;
}

static char **
flatpak_context_features_to_string (FlatpakContextFeatures features, FlatpakContextFeatures valid)
{
  return flatpak_context_bitmask_to_string (features, valid, flatpak_context_features);
}

static void
flatpak_context_features_to_args (FlatpakContextFeatures features,
                                FlatpakContextFeatures valid,
                                GPtrArray *args)
{
  return flatpak_context_bitmask_to_args (features, valid, flatpak_context_features, "--allow", "--disallow", args);
}

static void
flatpak_context_add_shares (FlatpakContext      *context,
                            FlatpakContextShares shares)
{
  context->shares_valid |= shares;
  context->shares |= shares;
}

static void
flatpak_context_remove_shares (FlatpakContext      *context,
                               FlatpakContextShares shares)
{
  context->shares_valid |= shares;
  context->shares &= ~shares;
}

static void
flatpak_context_add_sockets (FlatpakContext       *context,
                             FlatpakContextSockets sockets)
{
  context->sockets_valid |= sockets;
  context->sockets |= sockets;
}

static void
flatpak_context_remove_sockets (FlatpakContext       *context,
                                FlatpakContextSockets sockets)
{
  context->sockets_valid |= sockets;
  context->sockets &= ~sockets;
}

static void
flatpak_context_add_devices (FlatpakContext       *context,
                             FlatpakContextDevices devices)
{
  context->devices_valid |= devices;
  context->devices |= devices;
}

static void
flatpak_context_remove_devices (FlatpakContext       *context,
                                FlatpakContextDevices devices)
{
  context->devices_valid |= devices;
  context->devices &= ~devices;
}

static void
flatpak_context_add_features (FlatpakContext       *context,
                           FlatpakContextFeatures   features)
{
  context->features_valid |= features;
  context->features |= features;
}

static void
flatpak_context_remove_features (FlatpakContext       *context,
                               FlatpakContextFeatures  features)
{
  context->features_valid |= features;
  context->features &= ~features;
}

static void
flatpak_context_set_env_var (FlatpakContext *context,
                             const char     *name,
                             const char     *value)
{
  g_hash_table_insert (context->env_vars, g_strdup (name), g_strdup (value));
}

void
flatpak_context_set_session_bus_policy (FlatpakContext *context,
                                        const char     *name,
                                        FlatpakPolicy   policy)
{
  g_hash_table_insert (context->session_bus_policy, g_strdup (name), GINT_TO_POINTER (policy));
}

void
flatpak_context_set_system_bus_policy (FlatpakContext *context,
                                       const char     *name,
                                       FlatpakPolicy   policy)
{
  g_hash_table_insert (context->system_bus_policy, g_strdup (name), GINT_TO_POINTER (policy));
}

static void
flatpak_context_apply_generic_policy (FlatpakContext *context,
                                      const char     *key,
                                      const char     *value)
{
  GPtrArray *new = g_ptr_array_new ();
  const char **old_v;
  int i;

  g_assert (strchr (key, '.') != NULL);

  old_v = g_hash_table_lookup (context->generic_policy, key);
  for (i = 0; old_v != NULL && old_v[i] != NULL; i++)
    {
      const char *old = old_v[i];
      const char *cmp1 = old;
      const char *cmp2 = value;
      if (*cmp1 == '!')
        cmp1++;
      if (*cmp2 == '!')
        cmp2++;
      if (strcmp (cmp1, cmp2) != 0)
        g_ptr_array_add (new, g_strdup (old));
    }

  g_ptr_array_add (new, g_strdup (value));
  g_ptr_array_add (new, NULL);

  g_hash_table_insert (context->generic_policy, g_strdup (key),
                       g_ptr_array_free (new, FALSE));
}

static void
flatpak_context_set_persistent (FlatpakContext *context,
                                const char     *path)
{
  g_hash_table_insert (context->persistent, g_strdup (path), GINT_TO_POINTER (1));
}

static gboolean
get_xdg_dir_from_prefix (const char *prefix,
                         const char **where,
                         const char **dir)
{
  if (strcmp (prefix, "xdg-data") == 0)
    {
      if (where)
        *where = "data";
      if (dir)
        *dir = g_get_user_data_dir ();
      return TRUE;
    }
  if (strcmp (prefix, "xdg-cache") == 0)
    {
      if (where)
        *where = "cache";
      if (dir)
        *dir = g_get_user_cache_dir ();
      return TRUE;
    }
  if (strcmp (prefix, "xdg-config") == 0)
    {
      if (where)
        *where = "config";
      if (dir)
        *dir = g_get_user_config_dir ();
      return TRUE;
    }
  return FALSE;
}

/* This looks only in the xdg dirs (config, cache, data), not the user
   definable ones */
static char *
get_xdg_dir_from_string (const char *filesystem,
                         const char **suffix,
                         const char **where)
{
  char *slash;
  const char *rest;
  g_autofree char *prefix = NULL;
  const char *dir = NULL;
  gsize len;

  slash = strchr (filesystem, '/');

  if (slash)
    len = slash - filesystem;
  else
    len = strlen (filesystem);

  rest = filesystem + len;
  while (*rest == '/')
    rest++;

  if (suffix != NULL)
    *suffix = rest;

  prefix = g_strndup (filesystem, len);

  if (get_xdg_dir_from_prefix (prefix, where, &dir))
    return g_build_filename (dir, rest, NULL);

  return NULL;
}

static gboolean
get_xdg_user_dir_from_string (const char  *filesystem,
                              const char **config_key,
                              const char **suffix,
                              const char **dir)
{
  char *slash;
  const char *rest;
  g_autofree char *prefix = NULL;
  gsize len;

  slash = strchr (filesystem, '/');

  if (slash)
    len = slash - filesystem;
  else
    len = strlen (filesystem);

  rest = filesystem + len;
  while (*rest == '/')
    rest++;

  if (suffix)
    *suffix = rest;

  prefix = g_strndup (filesystem, len);

  if (strcmp (prefix, "xdg-desktop") == 0)
    {
      if (config_key)
        *config_key = "XDG_DESKTOP_DIR";
      if (dir)
        *dir = g_get_user_special_dir (G_USER_DIRECTORY_DESKTOP);
      return TRUE;
    }
  if (strcmp (prefix, "xdg-documents") == 0)
    {
      if (config_key)
        *config_key = "XDG_DOCUMENTS_DIR";
      if (dir)
        *dir = g_get_user_special_dir (G_USER_DIRECTORY_DOCUMENTS);
      return TRUE;
    }
  if (strcmp (prefix, "xdg-download") == 0)
    {
      if (config_key)
        *config_key = "XDG_DOWNLOAD_DIR";
      if (dir)
        *dir = g_get_user_special_dir (G_USER_DIRECTORY_DOWNLOAD);
      return TRUE;
    }
  if (strcmp (prefix, "xdg-music") == 0)
    {
      if (config_key)
        *config_key = "XDG_MUSIC_DIR";
      if (dir)
        *dir = g_get_user_special_dir (G_USER_DIRECTORY_MUSIC);
      return TRUE;
    }
  if (strcmp (prefix, "xdg-pictures") == 0)
    {
      if (config_key)
        *config_key = "XDG_PICTURES_DIR";
      if (dir)
        *dir = g_get_user_special_dir (G_USER_DIRECTORY_PICTURES);
      return TRUE;
    }
  if (strcmp (prefix, "xdg-public-share") == 0)
    {
      if (config_key)
        *config_key = "XDG_PUBLICSHARE_DIR";
      if (dir)
        *dir = g_get_user_special_dir (G_USER_DIRECTORY_PUBLIC_SHARE);
      return TRUE;
    }
  if (strcmp (prefix, "xdg-templates") == 0)
    {
      if (config_key)
        *config_key = "XDG_TEMPLATES_DIR";
      if (dir)
        *dir = g_get_user_special_dir (G_USER_DIRECTORY_TEMPLATES);
      return TRUE;
    }
  if (strcmp (prefix, "xdg-videos") == 0)
    {
      if (config_key)
        *config_key = "XDG_VIDEOS_DIR";
      if (dir)
        *dir = g_get_user_special_dir (G_USER_DIRECTORY_VIDEOS);
      return TRUE;
    }
  if (get_xdg_dir_from_prefix (prefix, NULL, dir))
    {
      if (config_key)
        *config_key = NULL;
      return TRUE;
    }
  /* Don't support xdg-run without suffix, because that doesn't work */
  if (strcmp (prefix, "xdg-run") == 0 &&
      *rest != 0)
    {
      if (config_key)
        *config_key = NULL;
      if (dir)
        *dir = g_get_user_runtime_dir ();
      return TRUE;
    }

  return FALSE;
}

static char *
parse_filesystem_flags (const char *filesystem, FlatpakFilesystemMode *mode)
{
  gsize len = strlen (filesystem);

  if (mode)
    *mode = FLATPAK_FILESYSTEM_MODE_READ_WRITE;

  if (g_str_has_suffix (filesystem, ":ro"))
    {
      len -= 3;
      if (mode)
        *mode = FLATPAK_FILESYSTEM_MODE_READ_ONLY;
    }
  else if (g_str_has_suffix (filesystem, ":rw"))
    {
      len -= 3;
      if (mode)
        *mode = FLATPAK_FILESYSTEM_MODE_READ_WRITE;
    }
  else if (g_str_has_suffix (filesystem, ":create"))
    {
      len -= 7;
      if (mode)
        *mode = FLATPAK_FILESYSTEM_MODE_CREATE;
    }

  return g_strndup (filesystem, len);
}

static gboolean
flatpak_context_verify_filesystem (const char *filesystem_and_mode,
                                   GError    **error)
{
  g_autofree char *filesystem = parse_filesystem_flags (filesystem_and_mode, NULL);

  if (strcmp (filesystem, "host") == 0)
    return TRUE;
  if (strcmp (filesystem, "home") == 0)
    return TRUE;
  if (get_xdg_user_dir_from_string (filesystem, NULL, NULL, NULL))
    return TRUE;
  if (g_str_has_prefix (filesystem, "~/"))
    return TRUE;
  if (g_str_has_prefix (filesystem, "/"))
    return TRUE;

  g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
               _("Unknown filesystem location %s, valid locations are: host, home, xdg-*[/...], ~/dir, /dir"), filesystem);
  return FALSE;
}

static void
flatpak_context_add_filesystem (FlatpakContext *context,
                                const char     *what)
{
  FlatpakFilesystemMode mode;
  char *fs = parse_filesystem_flags (what, &mode);

  g_hash_table_insert (context->filesystems, fs, GINT_TO_POINTER (mode));
}

static void
flatpak_context_remove_filesystem (FlatpakContext *context,
                                   const char     *what)
{
  g_hash_table_insert (context->filesystems,
                       parse_filesystem_flags (what, NULL),
                       NULL);
}

void
flatpak_context_merge (FlatpakContext *context,
                       FlatpakContext *other)
{
  GHashTableIter iter;
  gpointer key, value;

  context->shares &= ~other->shares_valid;
  context->shares |= other->shares;
  context->shares_valid |= other->shares_valid;
  context->sockets &= ~other->sockets_valid;
  context->sockets |= other->sockets;
  context->sockets_valid |= other->sockets_valid;
  context->devices &= ~other->devices_valid;
  context->devices |= other->devices;
  context->devices_valid |= other->devices_valid;
  context->features &= ~other->features_valid;
  context->features |= other->features;
  context->features_valid |= other->features_valid;

  g_hash_table_iter_init (&iter, other->env_vars);
  while (g_hash_table_iter_next (&iter, &key, &value))
    g_hash_table_insert (context->env_vars, g_strdup (key), g_strdup (value));

  g_hash_table_iter_init (&iter, other->persistent);
  while (g_hash_table_iter_next (&iter, &key, &value))
    g_hash_table_insert (context->persistent, g_strdup (key), value);

  g_hash_table_iter_init (&iter, other->filesystems);
  while (g_hash_table_iter_next (&iter, &key, &value))
    g_hash_table_insert (context->filesystems, g_strdup (key), value);

  g_hash_table_iter_init (&iter, other->session_bus_policy);
  while (g_hash_table_iter_next (&iter, &key, &value))
    g_hash_table_insert (context->session_bus_policy, g_strdup (key), value);

  g_hash_table_iter_init (&iter, other->system_bus_policy);
  while (g_hash_table_iter_next (&iter, &key, &value))
    g_hash_table_insert (context->system_bus_policy, g_strdup (key), value);

  g_hash_table_iter_init (&iter, other->system_bus_policy);
  while (g_hash_table_iter_next (&iter, &key, &value))
    g_hash_table_insert (context->system_bus_policy, g_strdup (key), value);

  g_hash_table_iter_init (&iter, other->generic_policy);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      const char **policy_values = (const char **)value;
      int i;

      for (i = 0; policy_values[i] != NULL; i++)
        flatpak_context_apply_generic_policy (context, (char *)key, policy_values[i]);
    }

}

static gboolean
option_share_cb (const gchar *option_name,
                 const gchar *value,
                 gpointer     data,
                 GError     **error)
{
  FlatpakContext *context = data;
  FlatpakContextShares share;

  share = flatpak_context_share_from_string (value, error);
  if (share == 0)
    return FALSE;

  flatpak_context_add_shares (context, share);

  return TRUE;
}

static gboolean
option_unshare_cb (const gchar *option_name,
                   const gchar *value,
                   gpointer     data,
                   GError     **error)
{
  FlatpakContext *context = data;
  FlatpakContextShares share;

  share = flatpak_context_share_from_string (value, error);
  if (share == 0)
    return FALSE;

  flatpak_context_remove_shares (context, share);

  return TRUE;
}

static gboolean
option_socket_cb (const gchar *option_name,
                  const gchar *value,
                  gpointer     data,
                  GError     **error)
{
  FlatpakContext *context = data;
  FlatpakContextSockets socket;

  socket = flatpak_context_socket_from_string (value, error);
  if (socket == 0)
    return FALSE;

  flatpak_context_add_sockets (context, socket);

  return TRUE;
}

static gboolean
option_nosocket_cb (const gchar *option_name,
                    const gchar *value,
                    gpointer     data,
                    GError     **error)
{
  FlatpakContext *context = data;
  FlatpakContextSockets socket;

  socket = flatpak_context_socket_from_string (value, error);
  if (socket == 0)
    return FALSE;

  flatpak_context_remove_sockets (context, socket);

  return TRUE;
}

static gboolean
option_device_cb (const gchar *option_name,
                  const gchar *value,
                  gpointer     data,
                  GError     **error)
{
  FlatpakContext *context = data;
  FlatpakContextDevices device;

  device = flatpak_context_device_from_string (value, error);
  if (device == 0)
    return FALSE;

  flatpak_context_add_devices (context, device);

  return TRUE;
}

static gboolean
option_nodevice_cb (const gchar *option_name,
                    const gchar *value,
                    gpointer     data,
                    GError     **error)
{
  FlatpakContext *context = data;
  FlatpakContextDevices device;

  device = flatpak_context_device_from_string (value, error);
  if (device == 0)
    return FALSE;

  flatpak_context_remove_devices (context, device);

  return TRUE;
}

static gboolean
option_allow_cb (const gchar *option_name,
                 const gchar *value,
                 gpointer     data,
                 GError     **error)
{
  FlatpakContext *context = data;
  FlatpakContextFeatures feature;

  feature = flatpak_context_feature_from_string (value, error);
  if (feature == 0)
    return FALSE;

  flatpak_context_add_features (context, feature);

  return TRUE;
}

static gboolean
option_disallow_cb (const gchar *option_name,
                    const gchar *value,
                    gpointer     data,
                    GError     **error)
{
  FlatpakContext *context = data;
  FlatpakContextFeatures feature;

  feature = flatpak_context_feature_from_string (value, error);
  if (feature == 0)
    return FALSE;

  flatpak_context_remove_features (context, feature);

  return TRUE;
}

static gboolean
option_filesystem_cb (const gchar *option_name,
                      const gchar *value,
                      gpointer     data,
                      GError     **error)
{
  FlatpakContext *context = data;

  if (!flatpak_context_verify_filesystem (value, error))
    return FALSE;

  flatpak_context_add_filesystem (context, value);
  return TRUE;
}

static gboolean
option_nofilesystem_cb (const gchar *option_name,
                        const gchar *value,
                        gpointer     data,
                        GError     **error)
{
  FlatpakContext *context = data;

  if (!flatpak_context_verify_filesystem (value, error))
    return FALSE;

  flatpak_context_remove_filesystem (context, value);
  return TRUE;
}

static gboolean
option_env_cb (const gchar *option_name,
               const gchar *value,
               gpointer     data,
               GError     **error)
{
  FlatpakContext *context = data;

  g_auto(GStrv) split = g_strsplit (value, "=", 2);

  if (split == NULL || split[0] == NULL || split[0][0] == 0 || split[1] == NULL)
    {
      g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
                   _("Invalid env format %s"), value);
      return FALSE;
    }

  flatpak_context_set_env_var (context, split[0], split[1]);
  return TRUE;
}

static gboolean
option_own_name_cb (const gchar *option_name,
                    const gchar *value,
                    gpointer     data,
                    GError     **error)
{
  FlatpakContext *context = data;

  if (!flatpak_verify_dbus_name (value, error))
    return FALSE;

  flatpak_context_set_session_bus_policy (context, value, FLATPAK_POLICY_OWN);
  return TRUE;
}

static gboolean
option_talk_name_cb (const gchar *option_name,
                     const gchar *value,
                     gpointer     data,
                     GError     **error)
{
  FlatpakContext *context = data;

  if (!flatpak_verify_dbus_name (value, error))
    return FALSE;

  flatpak_context_set_session_bus_policy (context, value, FLATPAK_POLICY_TALK);
  return TRUE;
}

static gboolean
option_system_own_name_cb (const gchar *option_name,
                           const gchar *value,
                           gpointer     data,
                           GError     **error)
{
  FlatpakContext *context = data;

  if (!flatpak_verify_dbus_name (value, error))
    return FALSE;

  flatpak_context_set_system_bus_policy (context, value, FLATPAK_POLICY_OWN);
  return TRUE;
}

static gboolean
option_system_talk_name_cb (const gchar *option_name,
                            const gchar *value,
                            gpointer     data,
                            GError     **error)
{
  FlatpakContext *context = data;

  if (!flatpak_verify_dbus_name (value, error))
    return FALSE;

  flatpak_context_set_system_bus_policy (context, value, FLATPAK_POLICY_TALK);
  return TRUE;
}

static gboolean
option_add_generic_policy_cb (const gchar *option_name,
                              const gchar *value,
                              gpointer     data,
                              GError     **error)
{
  FlatpakContext *context = data;
  char *t;
  g_autofree char *key = NULL;
  const char *policy_value;

  t = strchr (value, '=');
  if (t == NULL)
    return flatpak_fail (error, "--policy arguments must be in the form SUBSYSTEM.KEY=[!]VALUE");
  policy_value = t + 1;
  key = g_strndup (value, t - value);
  if (strchr (key, '.') == NULL)
    return flatpak_fail (error, "--policy arguments must be in the form SUBSYSTEM.KEY=[!]VALUE");

  if (policy_value[0] == '!')
    return flatpak_fail (error, "--policy values can't start with \"!\"");

  flatpak_context_apply_generic_policy (context, key, policy_value);

  return TRUE;
}

static gboolean
option_remove_generic_policy_cb (const gchar *option_name,
                                 const gchar *value,
                                 gpointer     data,
                                 GError     **error)
{
  FlatpakContext *context = data;
  char *t;
  g_autofree char *key = NULL;
  const char *policy_value;
  g_autofree char *extended_value = NULL;

  t = strchr (value, '=');
  if (t == NULL)
    return flatpak_fail (error, "--policy arguments must be in the form SUBSYSTEM.KEY=[!]VALUE");
  policy_value = t + 1;
  key = g_strndup (value, t - value);
  if (strchr (key, '.') == NULL)
    return flatpak_fail (error, "--policy arguments must be in the form SUBSYSTEM.KEY=[!]VALUE");

  if (policy_value[0] == '!')
    return flatpak_fail (error, "--policy values can't start with \"!\"");

  extended_value = g_strconcat ("!", policy_value, NULL);

  flatpak_context_apply_generic_policy (context, key, extended_value);

  return TRUE;
}

static gboolean
option_persist_cb (const gchar *option_name,
                   const gchar *value,
                   gpointer     data,
                   GError     **error)
{
  FlatpakContext *context = data;

  flatpak_context_set_persistent (context, value);
  return TRUE;
}

static gboolean option_no_desktop_deprecated;

static GOptionEntry context_options[] = {
  { "share", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_share_cb, N_("Share with host"), N_("SHARE") },
  { "unshare", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_unshare_cb, N_("Unshare with host"), N_("SHARE") },
  { "socket", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_socket_cb, N_("Expose socket to app"), N_("SOCKET") },
  { "nosocket", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_nosocket_cb, N_("Don't expose socket to app"), N_("SOCKET") },
  { "device", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_device_cb, N_("Expose device to app"), N_("DEVICE") },
  { "nodevice", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_nodevice_cb, N_("Don't expose device to app"), N_("DEVICE") },
  { "allow", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_allow_cb, N_("Allow feature"), N_("FEATURE") },
  { "disallow", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_disallow_cb, N_("Don't allow feature"), N_("FEATURE") },
  { "filesystem", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_filesystem_cb, N_("Expose filesystem to app (:ro for read-only)"), N_("FILESYSTEM[:ro]") },
  { "nofilesystem", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_nofilesystem_cb, N_("Don't expose filesystem to app"), N_("FILESYSTEM") },
  { "env", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_env_cb, N_("Set environment variable"), N_("VAR=VALUE") },
  { "own-name", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_own_name_cb, N_("Allow app to own name on the session bus"), N_("DBUS_NAME") },
  { "talk-name", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_talk_name_cb, N_("Allow app to talk to name on the session bus"), N_("DBUS_NAME") },
  { "system-own-name", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_system_own_name_cb, N_("Allow app to own name on the system bus"), N_("DBUS_NAME") },
  { "system-talk-name", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_system_talk_name_cb, N_("Allow app to talk to name on the system bus"), N_("DBUS_NAME") },
  { "add-policy", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_add_generic_policy_cb, N_("Add generic policy option"), N_("SUBSYSTEM.KEY=VALUE") },
  { "remove-policy", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_remove_generic_policy_cb, N_("Remove generic policy option"), N_("SUBSYSTEM.KEY=VALUE") },
  { "persist", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_persist_cb, N_("Persist home directory"), N_("FILENAME") },
  /* This is not needed/used anymore, so hidden, but we accept it for backwards compat */
  { "no-desktop", 0, G_OPTION_FLAG_IN_MAIN |  G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, &option_no_desktop_deprecated, N_("Don't require a running session (no cgroups creation)"), NULL },
  { NULL }
};

void
flatpak_context_complete (FlatpakContext *context, FlatpakCompletion *completion)
{
  flatpak_complete_options (completion, context_options);
}

GOptionGroup  *
flatpak_context_get_options (FlatpakContext *context)
{
  GOptionGroup *group;

  group = g_option_group_new ("environment",
                              "Runtime Environment",
                              "Runtime Environment",
                              context,
                              NULL);
  g_option_group_set_translation_domain (group, GETTEXT_PACKAGE);

  g_option_group_add_entries (group, context_options);

  return group;
}

static const char *
parse_negated (const char *option, gboolean *negated)
{
  if (option[0] == '!')
    {
      option++;
      *negated = TRUE;
    }
  else
    {
      *negated = FALSE;
    }
  return option;
}

/*
 * Merge the FLATPAK_METADATA_GROUP_CONTEXT,
 * FLATPAK_METADATA_GROUP_SESSION_BUS_POLICY,
 * FLATPAK_METADATA_GROUP_SYSTEM_BUS_POLICY and
 * FLATPAK_METADATA_GROUP_ENVIRONMENT groups, and all groups starting
 * with FLATPAK_METADATA_GROUP_PREFIX_POLICY, from metakey into context.
 *
 * This is a merge, not a replace!
 */
gboolean
flatpak_context_load_metadata (FlatpakContext *context,
                               GKeyFile       *metakey,
                               GError        **error)
{
  gboolean remove;
  g_auto(GStrv) groups = NULL;
  int i;

  if (g_key_file_has_key (metakey, FLATPAK_METADATA_GROUP_CONTEXT, FLATPAK_METADATA_KEY_SHARED, NULL))
    {
      g_auto(GStrv) shares = g_key_file_get_string_list (metakey, FLATPAK_METADATA_GROUP_CONTEXT,
                                                         FLATPAK_METADATA_KEY_SHARED, NULL, error);
      if (shares == NULL)
        return FALSE;

      for (i = 0; shares[i] != NULL; i++)
        {
          FlatpakContextShares share;

          share = flatpak_context_share_from_string (parse_negated (shares[i], &remove), error);
          if (share == 0)
            return FALSE;
          if (remove)
            flatpak_context_remove_shares (context, share);
          else
            flatpak_context_add_shares (context, share);
        }
    }

  if (g_key_file_has_key (metakey, FLATPAK_METADATA_GROUP_CONTEXT, FLATPAK_METADATA_KEY_SOCKETS, NULL))
    {
      g_auto(GStrv) sockets = g_key_file_get_string_list (metakey, FLATPAK_METADATA_GROUP_CONTEXT,
                                                          FLATPAK_METADATA_KEY_SOCKETS, NULL, error);
      if (sockets == NULL)
        return FALSE;

      for (i = 0; sockets[i] != NULL; i++)
        {
          FlatpakContextSockets socket = flatpak_context_socket_from_string (parse_negated (sockets[i], &remove), error);
          if (socket == 0)
            return FALSE;
          if (remove)
            flatpak_context_remove_sockets (context, socket);
          else
            flatpak_context_add_sockets (context, socket);
        }
    }

  if (g_key_file_has_key (metakey, FLATPAK_METADATA_GROUP_CONTEXT, FLATPAK_METADATA_KEY_DEVICES, NULL))
    {
      g_auto(GStrv) devices = g_key_file_get_string_list (metakey, FLATPAK_METADATA_GROUP_CONTEXT,
                                                          FLATPAK_METADATA_KEY_DEVICES, NULL, error);
      if (devices == NULL)
        return FALSE;


      for (i = 0; devices[i] != NULL; i++)
        {
          FlatpakContextDevices device = flatpak_context_device_from_string (parse_negated (devices[i], &remove), error);
          if (device == 0)
            return FALSE;
          if (remove)
            flatpak_context_remove_devices (context, device);
          else
            flatpak_context_add_devices (context, device);
        }
    }

  if (g_key_file_has_key (metakey, FLATPAK_METADATA_GROUP_CONTEXT, FLATPAK_METADATA_KEY_FEATURES, NULL))
    {
      g_auto(GStrv) features = g_key_file_get_string_list (metakey, FLATPAK_METADATA_GROUP_CONTEXT,
                                                         FLATPAK_METADATA_KEY_FEATURES, NULL, error);
      if (features == NULL)
        return FALSE;


      for (i = 0; features[i] != NULL; i++)
        {
          FlatpakContextFeatures feature = flatpak_context_feature_from_string (parse_negated (features[i], &remove), error);
          if (feature == 0)
            return FALSE;
          if (remove)
            flatpak_context_remove_features (context, feature);
          else
            flatpak_context_add_features (context, feature);
        }
    }

  if (g_key_file_has_key (metakey, FLATPAK_METADATA_GROUP_CONTEXT, FLATPAK_METADATA_KEY_FILESYSTEMS, NULL))
    {
      g_auto(GStrv) filesystems = g_key_file_get_string_list (metakey, FLATPAK_METADATA_GROUP_CONTEXT,
                                                              FLATPAK_METADATA_KEY_FILESYSTEMS, NULL, error);
      if (filesystems == NULL)
        return FALSE;

      for (i = 0; filesystems[i] != NULL; i++)
        {
          const char *fs = parse_negated (filesystems[i], &remove);
          if (!flatpak_context_verify_filesystem (fs, error))
            return FALSE;
          if (remove)
            flatpak_context_remove_filesystem (context, fs);
          else
            flatpak_context_add_filesystem (context, fs);
        }
    }

  if (g_key_file_has_key (metakey, FLATPAK_METADATA_GROUP_CONTEXT, FLATPAK_METADATA_KEY_PERSISTENT, NULL))
    {
      g_auto(GStrv) persistent = g_key_file_get_string_list (metakey, FLATPAK_METADATA_GROUP_CONTEXT,
                                                             FLATPAK_METADATA_KEY_PERSISTENT, NULL, error);
      if (persistent == NULL)
        return FALSE;

      for (i = 0; persistent[i] != NULL; i++)
        flatpak_context_set_persistent (context, persistent[i]);
    }

  if (g_key_file_has_group (metakey, FLATPAK_METADATA_GROUP_SESSION_BUS_POLICY))
    {
      g_auto(GStrv) keys = NULL;
      gsize i, keys_count;

      keys = g_key_file_get_keys (metakey, FLATPAK_METADATA_GROUP_SESSION_BUS_POLICY, &keys_count, NULL);
      for (i = 0; i < keys_count; i++)
        {
          const char *key = keys[i];
          g_autofree char *value = g_key_file_get_string (metakey, FLATPAK_METADATA_GROUP_SESSION_BUS_POLICY, key, NULL);
          FlatpakPolicy policy;

          if (!flatpak_verify_dbus_name (key, error))
            return FALSE;

          policy = flatpak_policy_from_string (value, error);
          if ((int) policy == -1)
            return FALSE;

          flatpak_context_set_session_bus_policy (context, key, policy);
        }
    }

  if (g_key_file_has_group (metakey, FLATPAK_METADATA_GROUP_SYSTEM_BUS_POLICY))
    {
      g_auto(GStrv) keys = NULL;
      gsize i, keys_count;

      keys = g_key_file_get_keys (metakey, FLATPAK_METADATA_GROUP_SYSTEM_BUS_POLICY, &keys_count, NULL);
      for (i = 0; i < keys_count; i++)
        {
          const char *key = keys[i];
          g_autofree char *value = g_key_file_get_string (metakey, FLATPAK_METADATA_GROUP_SYSTEM_BUS_POLICY, key, NULL);
          FlatpakPolicy policy;

          if (!flatpak_verify_dbus_name (key, error))
            return FALSE;

          policy = flatpak_policy_from_string (value, error);
          if ((int) policy == -1)
            return FALSE;

          flatpak_context_set_system_bus_policy (context, key, policy);
        }
    }

  if (g_key_file_has_group (metakey, FLATPAK_METADATA_GROUP_ENVIRONMENT))
    {
      g_auto(GStrv) keys = NULL;
      gsize i, keys_count;

      keys = g_key_file_get_keys (metakey, FLATPAK_METADATA_GROUP_ENVIRONMENT, &keys_count, NULL);
      for (i = 0; i < keys_count; i++)
        {
          const char *key = keys[i];
          g_autofree char *value = g_key_file_get_string (metakey, FLATPAK_METADATA_GROUP_ENVIRONMENT, key, NULL);

          flatpak_context_set_env_var (context, key, value);
        }
    }

  groups = g_key_file_get_groups (metakey, NULL);
  for (i = 0; groups[i] != NULL; i++)
    {
      const char *group = groups[i];
      const char *subsystem;
      int j;

      if (g_str_has_prefix (group, FLATPAK_METADATA_GROUP_PREFIX_POLICY))
        {
          g_auto(GStrv) keys = NULL;
          subsystem = group + strlen (FLATPAK_METADATA_GROUP_PREFIX_POLICY);
          keys = g_key_file_get_keys (metakey, group, NULL, NULL);
          for (j = 0; keys != NULL && keys[j] != NULL; j++)
            {
              const char *key = keys[j];
              g_autofree char *policy_key = g_strdup_printf ("%s.%s", subsystem, key);
              g_auto(GStrv) values = NULL;
              int k;

              values = g_key_file_get_string_list (metakey, group, key, NULL, NULL);
              for (k = 0; values != NULL && values[k] != NULL; k++)
                flatpak_context_apply_generic_policy (context, policy_key,
                                                      values[k]);
            }
        }
    }

  return TRUE;
}

/*
 * Save the FLATPAK_METADATA_GROUP_CONTEXT,
 * FLATPAK_METADATA_GROUP_SESSION_BUS_POLICY,
 * FLATPAK_METADATA_GROUP_SYSTEM_BUS_POLICY and
 * FLATPAK_METADATA_GROUP_ENVIRONMENT groups, and all groups starting
 * with FLATPAK_METADATA_GROUP_PREFIX_POLICY, into metakey
 */
void
flatpak_context_save_metadata (FlatpakContext *context,
                               gboolean        flatten,
                               GKeyFile       *metakey)
{
  g_auto(GStrv) shared = NULL;
  g_auto(GStrv) sockets = NULL;
  g_auto(GStrv) devices = NULL;
  g_auto(GStrv) features = NULL;
  GHashTableIter iter;
  gpointer key, value;
  FlatpakContextShares shares_mask = context->shares;
  FlatpakContextShares shares_valid = context->shares_valid;
  FlatpakContextSockets sockets_mask = context->sockets;
  FlatpakContextSockets sockets_valid = context->sockets_valid;
  FlatpakContextDevices devices_mask = context->devices;
  FlatpakContextDevices devices_valid = context->devices_valid;
  FlatpakContextFeatures features_mask = context->features;
  FlatpakContextFeatures features_valid = context->features;
  g_auto(GStrv) groups = NULL;
  int i;

  if (flatten)
    {
      /* A flattened format means we don't expect this to be merged on top of
         another context. In that case we never need to negate any flags.
         We calculate this by removing the zero parts of the mask from the valid set.
      */
      /* First we make sure only the valid parts of the mask are set, in case we
         got some leftover */
      shares_mask &= shares_valid;
      sockets_mask &= sockets_valid;
      devices_mask &= devices_valid;
      features_mask &= features_valid;

      /* Then just set the valid set to be the mask set */
      shares_valid = shares_mask;
      sockets_valid = sockets_mask;
      devices_valid = devices_mask;
      features_valid = features_mask;
    }

  shared = flatpak_context_shared_to_string (shares_mask, shares_valid);
  sockets = flatpak_context_sockets_to_string (sockets_mask, sockets_valid);
  devices = flatpak_context_devices_to_string (devices_mask, devices_valid);
  features = flatpak_context_features_to_string (features_mask, features_valid);

  if (shared[0] != NULL)
    {
      g_key_file_set_string_list (metakey,
                                  FLATPAK_METADATA_GROUP_CONTEXT,
                                  FLATPAK_METADATA_KEY_SHARED,
                                  (const char * const *) shared, g_strv_length (shared));
    }
  else
    {
      g_key_file_remove_key (metakey,
                             FLATPAK_METADATA_GROUP_CONTEXT,
                             FLATPAK_METADATA_KEY_SHARED,
                             NULL);
    }

  if (sockets[0] != NULL)
    {
      g_key_file_set_string_list (metakey,
                                  FLATPAK_METADATA_GROUP_CONTEXT,
                                  FLATPAK_METADATA_KEY_SOCKETS,
                                  (const char * const *) sockets, g_strv_length (sockets));
    }
  else
    {
      g_key_file_remove_key (metakey,
                             FLATPAK_METADATA_GROUP_CONTEXT,
                             FLATPAK_METADATA_KEY_SOCKETS,
                             NULL);
    }

  if (devices[0] != NULL)
    {
      g_key_file_set_string_list (metakey,
                                  FLATPAK_METADATA_GROUP_CONTEXT,
                                  FLATPAK_METADATA_KEY_DEVICES,
                                  (const char * const *) devices, g_strv_length (devices));
    }
  else
    {
      g_key_file_remove_key (metakey,
                             FLATPAK_METADATA_GROUP_CONTEXT,
                             FLATPAK_METADATA_KEY_DEVICES,
                             NULL);
    }

  if (features[0] != NULL)
    {
      g_key_file_set_string_list (metakey,
                                  FLATPAK_METADATA_GROUP_CONTEXT,
                                  FLATPAK_METADATA_KEY_FEATURES,
                                  (const char * const *) features, g_strv_length (features));
    }
  else
    {
      g_key_file_remove_key (metakey,
                             FLATPAK_METADATA_GROUP_CONTEXT,
                             FLATPAK_METADATA_KEY_FEATURES,
                             NULL);
    }

  if (g_hash_table_size (context->filesystems) > 0)
    {
      g_autoptr(GPtrArray) array = g_ptr_array_new_with_free_func (g_free);

      g_hash_table_iter_init (&iter, context->filesystems);
      while (g_hash_table_iter_next (&iter, &key, &value))
        {
          FlatpakFilesystemMode mode = GPOINTER_TO_INT (value);

          if (mode == FLATPAK_FILESYSTEM_MODE_READ_ONLY)
            g_ptr_array_add (array, g_strconcat (key, ":ro", NULL));
          else if (mode == FLATPAK_FILESYSTEM_MODE_CREATE)
            g_ptr_array_add (array, g_strconcat (key, ":create", NULL));
          else if (value != NULL)
            g_ptr_array_add (array, g_strdup (key));
        }

      g_key_file_set_string_list (metakey,
                                  FLATPAK_METADATA_GROUP_CONTEXT,
                                  FLATPAK_METADATA_KEY_FILESYSTEMS,
                                  (const char * const *) array->pdata, array->len);
    }
  else
    {
      g_key_file_remove_key (metakey,
                             FLATPAK_METADATA_GROUP_CONTEXT,
                             FLATPAK_METADATA_KEY_FILESYSTEMS,
                             NULL);
    }

  if (g_hash_table_size (context->persistent) > 0)
    {
      g_autofree char **keys = (char **) g_hash_table_get_keys_as_array (context->persistent, NULL);

      g_key_file_set_string_list (metakey,
                                  FLATPAK_METADATA_GROUP_CONTEXT,
                                  FLATPAK_METADATA_KEY_PERSISTENT,
                                  (const char * const *) keys, g_strv_length (keys));
    }
  else
    {
      g_key_file_remove_key (metakey,
                             FLATPAK_METADATA_GROUP_CONTEXT,
                             FLATPAK_METADATA_KEY_PERSISTENT,
                             NULL);
    }

  g_key_file_remove_group (metakey, FLATPAK_METADATA_GROUP_SESSION_BUS_POLICY, NULL);
  g_hash_table_iter_init (&iter, context->session_bus_policy);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      FlatpakPolicy policy = GPOINTER_TO_INT (value);
      if (policy > 0)
        g_key_file_set_string (metakey,
                               FLATPAK_METADATA_GROUP_SESSION_BUS_POLICY,
                               (char *) key, flatpak_policy_to_string (policy));
    }

  g_key_file_remove_group (metakey, FLATPAK_METADATA_GROUP_SYSTEM_BUS_POLICY, NULL);
  g_hash_table_iter_init (&iter, context->system_bus_policy);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      FlatpakPolicy policy = GPOINTER_TO_INT (value);
      if (policy > 0)
        g_key_file_set_string (metakey,
                               FLATPAK_METADATA_GROUP_SYSTEM_BUS_POLICY,
                               (char *) key, flatpak_policy_to_string (policy));
    }

  g_key_file_remove_group (metakey, FLATPAK_METADATA_GROUP_ENVIRONMENT, NULL);
  g_hash_table_iter_init (&iter, context->env_vars);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      g_key_file_set_string (metakey,
                             FLATPAK_METADATA_GROUP_ENVIRONMENT,
                             (char *) key, (char *) value);
    }


  groups = g_key_file_get_groups (metakey, NULL);
  for (i = 0; groups[i] != NULL; i++)
    {
      const char *group = groups[i];
      if (g_str_has_prefix (group, FLATPAK_METADATA_GROUP_PREFIX_POLICY))
        g_key_file_remove_group (metakey, group, NULL);
    }

  g_hash_table_iter_init (&iter, context->generic_policy);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      g_auto(GStrv) parts = g_strsplit ((const char *)key, ".", 2);
      g_autofree char *group = NULL;
      g_assert (parts[1] != NULL);
      const char **policy_values = (const char **)value;
      g_autoptr(GPtrArray) new = g_ptr_array_new ();

      for (i = 0; policy_values[i] != NULL; i++)
        {
          const char *policy_value = policy_values[i];

          if (!flatten || policy_value[0] != '!')
            g_ptr_array_add (new, (char *)policy_value);
        }

      if (new->len > 0)
        {
          group = g_strconcat (FLATPAK_METADATA_GROUP_PREFIX_POLICY,
                               parts[0], NULL);
          g_key_file_set_string_list (metakey, group, parts[1],
                                      (const char * const*)new->pdata,
                                      new->len);
        }
    }
}

void
flatpak_context_allow_host_fs (FlatpakContext *context)
{
  flatpak_context_add_filesystem (context, "host");
}

gboolean
flatpak_context_get_needs_session_bus_proxy (FlatpakContext *context)
{
  return g_hash_table_size (context->session_bus_policy) > 0;
}

gboolean
flatpak_context_get_needs_system_bus_proxy (FlatpakContext *context)
{
  return g_hash_table_size (context->system_bus_policy) > 0;
}

void
flatpak_context_to_args (FlatpakContext *context,
                         GPtrArray *args)
{
  GHashTableIter iter;
  gpointer key, value;

  flatpak_context_shared_to_args (context->shares, context->shares_valid, args);
  flatpak_context_sockets_to_args (context->sockets, context->sockets_valid, args);
  flatpak_context_devices_to_args (context->devices, context->devices_valid, args);
  flatpak_context_features_to_args (context->features, context->features_valid, args);

  g_hash_table_iter_init (&iter, context->env_vars);
  while (g_hash_table_iter_next (&iter, &key, &value))
    g_ptr_array_add (args, g_strdup_printf ("--env=%s=%s", (char *)key, (char *)value));

  g_hash_table_iter_init (&iter, context->persistent);
  while (g_hash_table_iter_next (&iter, &key, &value))
    g_ptr_array_add (args, g_strdup_printf ("--persist=%s", (char *)key));

  g_hash_table_iter_init (&iter, context->session_bus_policy);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      const char *name = key;
      FlatpakPolicy policy = GPOINTER_TO_INT (value);

      g_ptr_array_add (args, g_strdup_printf ("--%s-name=%s", flatpak_policy_to_string (policy), name));
    }

  g_hash_table_iter_init (&iter, context->system_bus_policy);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      const char *name = key;
      FlatpakPolicy policy = GPOINTER_TO_INT (value);

      g_ptr_array_add (args, g_strdup_printf ("--system-%s-name=%s", flatpak_policy_to_string (policy), name));
    }

  g_hash_table_iter_init (&iter, context->filesystems);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      FlatpakFilesystemMode mode = GPOINTER_TO_INT (value);

      if (mode == FLATPAK_FILESYSTEM_MODE_READ_ONLY)
        g_ptr_array_add (args, g_strdup_printf ("--filesystem=%s:ro", (char *)key));
      else if (mode == FLATPAK_FILESYSTEM_MODE_READ_WRITE)
        g_ptr_array_add (args, g_strdup_printf ("--filesystem=%s", (char *)key));
      else if (mode == FLATPAK_FILESYSTEM_MODE_CREATE)
        g_ptr_array_add (args, g_strdup_printf ("--filesystem=%s:create", (char *)key));
      else
        g_ptr_array_add (args, g_strdup_printf ("--nofilesystem=%s", (char *)key));
    }
}

static char *
extract_unix_path_from_dbus_address (const char *address)
{
  const char *path, *path_end;

  if (address == NULL)
    return NULL;

  if (!g_str_has_prefix (address, "unix:"))
    return NULL;

  path = strstr (address, "path=");
  if (path == NULL)
    return NULL;
  path += strlen ("path=");
  path_end = path;
  while (*path_end != 0 && *path_end != ',')
    path_end++;

  return g_strndup (path, path_end - path);
}

#ifdef ENABLE_XAUTH
static gboolean
auth_streq (char *str,
            char *au_str,
            int   au_len)
{
  return au_len == strlen (str) && memcmp (str, au_str, au_len) == 0;
}

static gboolean
xauth_entry_should_propagate (Xauth *xa,
                              char  *hostname,
                              char  *number)
{
  /* ensure entry isn't for remote access */
  if (xa->family != FamilyLocal && xa->family != FamilyWild)
    return FALSE;

  /* ensure entry is for this machine */
  if (xa->family == FamilyLocal && !auth_streq (hostname, xa->address, xa->address_length))
    return FALSE;

  /* ensure entry is for this session */
  if (xa->number != NULL && !auth_streq (number, xa->number, xa->number_length))
    return FALSE;

  return TRUE;
}

static void
write_xauth (char *number, FILE *output)
{
  Xauth *xa, local_xa;
  char *filename;
  FILE *f;
  struct utsname unames;

  if (uname (&unames))
    {
      g_warning ("uname failed");
      return;
    }

  filename = XauFileName ();
  f = fopen (filename, "rb");
  if (f == NULL)
    return;

  while (TRUE)
    {
      xa = XauReadAuth (f);
      if (xa == NULL)
        break;
      if (xauth_entry_should_propagate (xa, unames.nodename, number))
        {
          local_xa = *xa;
          if (local_xa.number)
            {
              local_xa.number = "99";
              local_xa.number_length = 2;
            }

          if (!XauWriteAuth (output, &local_xa))
            g_warning ("xauth write error");
        }

      XauDisposeAuth (xa);
    }

  fclose (f);
}
#endif /* ENABLE_XAUTH */

static void
append_args (GPtrArray *argv_array, GPtrArray *other_array)
{
  int i;

  for (i = 0; i < other_array->len; i++)
    g_ptr_array_add (argv_array, g_strdup (g_ptr_array_index (other_array, i)));
}

static void
add_args (GPtrArray *argv_array, ...)
{
  va_list args;
  const gchar *arg;

  va_start (args, argv_array);
  while ((arg = va_arg (args, const gchar *)))
    g_ptr_array_add (argv_array, g_strdup (arg));
  va_end (args);
}

static void
add_args_data_fd (GPtrArray *argv_array,
                  GArray    *fd_array,
                  const char *op,
                  int fd,
                  const char *path_optional)
{
  g_autofree char *fd_str = g_strdup_printf ("%d", fd);
  if (fd_array)
    g_array_append_val (fd_array, fd);

  add_args (argv_array,
            op, fd_str, path_optional,
            NULL);
}


/* If memfd_create() is available, generate a sealed memfd with contents of
 * @str. Otherwise use an O_TMPFILE @tmpf in anonymous mode, write @str to
 * @tmpf, and lseek() back to the start. See also similar uses in e.g.
 * rpm-ostree for running dracut.
 */
static gboolean
buffer_to_sealed_memfd_or_tmpfile (GLnxTmpfile *tmpf,
                                   const char  *name,
                                   const char  *str,
                                   size_t       len,
                                   GError     **error)
{
  if (len == -1)
    len = strlen (str);
  glnx_autofd int memfd = memfd_create (name, MFD_CLOEXEC | MFD_ALLOW_SEALING);
  int fd; /* Unowned */
  if (memfd != -1)
    {
      fd = memfd;
    }
  else
    {
      /* We use an anonymous fd (i.e. O_EXCL) since we don't want
       * the target container to potentially be able to re-link it.
       */
      if (!G_IN_SET (errno, ENOSYS, EOPNOTSUPP))
        return glnx_throw_errno_prefix (error, "memfd_create");
      if (!glnx_open_anonymous_tmpfile (O_RDWR | O_CLOEXEC, tmpf, error))
        return FALSE;
      fd = tmpf->fd;
    }
  if (ftruncate (fd, len) < 0)
    return glnx_throw_errno_prefix (error, "ftruncate");
  if (glnx_loop_write (fd, str, len) < 0)
    return glnx_throw_errno_prefix (error, "write");
  if (lseek (fd, 0, SEEK_SET) < 0)
    return glnx_throw_errno_prefix (error, "lseek");
  if (memfd != -1)
    {
      if (fcntl (memfd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL) < 0)
        return glnx_throw_errno_prefix (error, "fcntl(F_ADD_SEALS)");
      /* The other values can stay default */
      tmpf->fd = glnx_steal_fd (&memfd);
      tmpf->initialized = TRUE;
    }
  return TRUE;
}

/* Given a buffer @content of size @content_size, generate a fd (memfd if available)
 * of the data.  The @name parameter is used by memfd_create() as a debugging aid;
 * it has no semantic meaning.  The bwrap command line will inject it into the target
 * container as @path.
 */
static gboolean
add_args_data (GPtrArray *argv_array,
               GArray    *fd_array,
               const char *name,
               const char *content,
               gssize content_size,
               const char *path,
               GError **error)
{
  g_auto(GLnxTmpfile) args_tmpf  = { 0, };

  if (!buffer_to_sealed_memfd_or_tmpfile (&args_tmpf, name, content, content_size, error))
    return FALSE;

  add_args_data_fd (argv_array, fd_array,
                    "--bind-data", glnx_steal_fd (&args_tmpf.fd), path);
  return TRUE;
}

static void
flatpak_run_add_x11_args (GPtrArray *argv_array,
                          GArray    *fd_array,
                          char    ***envp_p,
                          gboolean allowed)
{
  g_autofree char *x11_socket = NULL;
  const char *display;

  /* Always cover /tmp/.X11-unix, that way we never see the host one in case
   * we have access to the host /tmp. If you request X access we'll put the right
   * thing in this anyway.
   */
  add_args (argv_array,
            "--tmpfs", "/tmp/.X11-unix",
            NULL);

  if (!allowed)
    {
      *envp_p = g_environ_unsetenv (*envp_p, "DISPLAY");
      return;
    }

  g_debug ("Allowing x11 access");

  display = g_getenv ("DISPLAY");
  if (display && display[0] == ':' && g_ascii_isdigit (display[1]))
    {
      const char *display_nr = &display[1];
      const char *display_nr_end = display_nr;
      g_autofree char *d = NULL;

      while (g_ascii_isdigit (*display_nr_end))
        display_nr_end++;

      d = g_strndup (display_nr, display_nr_end - display_nr);
      x11_socket = g_strdup_printf ("/tmp/.X11-unix/X%s", d);

      add_args (argv_array,
                "--bind", x11_socket, "/tmp/.X11-unix/X99",
                NULL);
      *envp_p = g_environ_setenv (*envp_p, "DISPLAY", ":99.0", TRUE);

#ifdef ENABLE_XAUTH
      g_auto(GLnxTmpfile) xauth_tmpf  = { 0, };

      if (glnx_open_anonymous_tmpfile (O_RDWR | O_CLOEXEC, &xauth_tmpf, NULL))
        {
          FILE *output = fdopen (xauth_tmpf.fd, "wb");
          if (output != NULL)
            {
	      /* fd is now owned by output, steal it from the tmpfile */
              int tmp_fd = dup (glnx_steal_fd (&xauth_tmpf.fd));
              if (tmp_fd != -1)
                {
                  g_autofree char *dest = g_strdup_printf ("/run/user/%d/Xauthority", getuid ());

                  write_xauth (d, output);
                  add_args_data_fd (argv_array, fd_array,
                                    "--bind-data", tmp_fd, dest);

                  *envp_p = g_environ_setenv (*envp_p, "XAUTHORITY", dest, TRUE);
                }

              fclose (output);

              if (tmp_fd != -1)
                lseek (tmp_fd, 0, SEEK_SET);
            }
        }
#endif
    }
  else
    {
      *envp_p = g_environ_unsetenv (*envp_p, "DISPLAY");
    }

}

static void
flatpak_run_add_wayland_args (GPtrArray *argv_array,
                              char    ***envp_p)
{
  const char *wayland_display;
  g_autofree char *wayland_socket = NULL;
  g_autofree char *sandbox_wayland_socket = NULL;

  wayland_display = g_getenv ("WAYLAND_DISPLAY");
  if (!wayland_display)
    wayland_display = "wayland-0";

  wayland_socket = g_build_filename (g_get_user_runtime_dir (), wayland_display, NULL);
  sandbox_wayland_socket = g_strdup_printf ("/run/user/%d/%s", getuid (), wayland_display);

  if (g_file_test (wayland_socket, G_FILE_TEST_EXISTS))
    {
      add_args (argv_array,
                "--bind", wayland_socket, sandbox_wayland_socket,
                NULL);
    }
}

static void
flatpak_run_add_pulseaudio_args (GPtrArray *argv_array,
                                 GArray    *fd_array,
                                 char    ***envp_p)
{
  g_autofree char *pulseaudio_socket = g_build_filename (g_get_user_runtime_dir (), "pulse/native", NULL);

  *envp_p = g_environ_unsetenv (*envp_p, "PULSE_SERVER");
  if (g_file_test (pulseaudio_socket, G_FILE_TEST_EXISTS))
    {
      gboolean share_shm = FALSE; /* TODO: When do we add this? */
      g_autofree char *client_config = g_strdup_printf ("enable-shm=%s\n", share_shm ? "yes" : "no");
      g_autofree char *sandbox_socket_path = g_strdup_printf ("/run/user/%d/pulse/native", getuid ());
      g_autofree char *pulse_server = g_strdup_printf ("unix:/run/user/%d/pulse/native", getuid ());
      g_autofree char *config_path = g_strdup_printf ("/run/user/%d/pulse/config", getuid ());

      /* FIXME - error handling */
      if (!add_args_data (argv_array, fd_array, "pulseaudio", client_config, -1, config_path, NULL))
        return;

      add_args (argv_array,
                "--bind", pulseaudio_socket, sandbox_socket_path,
                NULL);

      *envp_p = g_environ_setenv (*envp_p, "PULSE_SERVER", pulse_server, TRUE);
      *envp_p = g_environ_setenv (*envp_p, "PULSE_CLIENTCONFIG", config_path, TRUE);
    }
}

static void
flatpak_run_add_journal_args (GPtrArray *argv_array)
{
  g_autofree char *journal_socket_socket = g_strdup ("/run/systemd/journal/socket");
  g_autofree char *journal_stdout_socket = g_strdup ("/run/systemd/journal/stdout");

  if (g_file_test (journal_socket_socket, G_FILE_TEST_EXISTS))
    {
      add_args (argv_array,
                "--bind", journal_socket_socket, journal_socket_socket,
                NULL);
    }
  if (g_file_test (journal_stdout_socket, G_FILE_TEST_EXISTS))
    {
      add_args (argv_array,
                "--bind", journal_stdout_socket, journal_stdout_socket,
                NULL);
    }
}

static char *
create_proxy_socket (char *template)
{
  g_autofree char *proxy_socket_dir = g_build_filename (g_get_user_runtime_dir (), ".dbus-proxy", NULL);
  g_autofree char *proxy_socket = g_build_filename (proxy_socket_dir, template, NULL);
  int fd;

  if (!glnx_shutil_mkdir_p_at (AT_FDCWD, proxy_socket_dir, 0755, NULL, NULL))
    return NULL;

  fd = g_mkstemp (proxy_socket);
  if (fd == -1)
    return NULL;

  close (fd);

  return g_steal_pointer (&proxy_socket);
}

static gboolean
flatpak_run_add_system_dbus_args (FlatpakContext *context,
                                  char         ***envp_p,
                                  GPtrArray      *argv_array,
                                  GPtrArray      *dbus_proxy_argv,
                                  gboolean        unrestricted)
{
  const char *dbus_address = g_getenv ("DBUS_SYSTEM_BUS_ADDRESS");
  g_autofree char *real_dbus_address = NULL;
  g_autofree char *dbus_system_socket = NULL;

  if (dbus_address != NULL)
    dbus_system_socket = extract_unix_path_from_dbus_address (dbus_address);
  else if (g_file_test ("/var/run/dbus/system_bus_socket", G_FILE_TEST_EXISTS))
    dbus_system_socket = g_strdup ("/var/run/dbus/system_bus_socket");

  if (dbus_system_socket != NULL && unrestricted)
    {
      add_args (argv_array,
                "--bind", dbus_system_socket, "/run/dbus/system_bus_socket",
                NULL);
      *envp_p = g_environ_setenv (*envp_p, "DBUS_SYSTEM_BUS_ADDRESS", "unix:path=/run/dbus/system_bus_socket", TRUE);

      return TRUE;
    }
  else if (dbus_proxy_argv &&
           g_hash_table_size (context->system_bus_policy) > 0)
    {
      g_autofree char *proxy_socket = create_proxy_socket ("system-bus-proxy-XXXXXX");

      if (proxy_socket == NULL)
        return FALSE;

      if (dbus_address)
        real_dbus_address = g_strdup (dbus_address);
      else
        real_dbus_address = g_strdup_printf ("unix:path=%s", dbus_system_socket);

      g_ptr_array_add (dbus_proxy_argv, g_strdup (real_dbus_address));
      g_ptr_array_add (dbus_proxy_argv, g_strdup (proxy_socket));


      add_args (argv_array,
                "--bind", proxy_socket, "/run/dbus/system_bus_socket",
                NULL);
      *envp_p = g_environ_setenv (*envp_p, "DBUS_SYSTEM_BUS_ADDRESS", "unix:path=/run/dbus/system_bus_socket", TRUE);

      return TRUE;
    }
  return FALSE;
}

static gboolean
flatpak_run_add_session_dbus_args (GPtrArray *argv_array,
                                   char    ***envp_p,
                                   GPtrArray *dbus_proxy_argv,
                                   gboolean   unrestricted)
{
  const char *dbus_address = g_getenv ("DBUS_SESSION_BUS_ADDRESS");
  char *dbus_session_socket = NULL;
  g_autofree char *sandbox_socket_path = g_strdup_printf ("/run/user/%d/bus", getuid ());
  g_autofree char *sandbox_dbus_address = g_strdup_printf ("unix:path=/run/user/%d/bus", getuid ());

  if (dbus_address == NULL)
    return FALSE;

  dbus_session_socket = extract_unix_path_from_dbus_address (dbus_address);
  if (dbus_session_socket != NULL && unrestricted)
    {

      add_args (argv_array,
                "--bind", dbus_session_socket, sandbox_socket_path,
                NULL);
      *envp_p = g_environ_setenv (*envp_p, "DBUS_SESSION_BUS_ADDRESS", sandbox_dbus_address, TRUE);

      return TRUE;
    }
  else if (dbus_proxy_argv && dbus_address != NULL)
    {
      g_autofree char *proxy_socket = create_proxy_socket ("session-bus-proxy-XXXXXX");

      if (proxy_socket == NULL)
        return FALSE;

      g_ptr_array_add (dbus_proxy_argv, g_strdup (dbus_address));
      g_ptr_array_add (dbus_proxy_argv, g_strdup (proxy_socket));

      add_args (argv_array,
                "--bind", proxy_socket, sandbox_socket_path,
                NULL);
      *envp_p = g_environ_setenv (*envp_p, "DBUS_SESSION_BUS_ADDRESS", sandbox_dbus_address, TRUE);

      return TRUE;
    }

  return FALSE;
}

static void
flatpak_add_bus_filters (GPtrArray      *dbus_proxy_argv,
                         GHashTable     *ht,
                         const char     *app_id,
                         FlatpakContext *context)
{
  GHashTableIter iter;
  gpointer key, value;

  g_ptr_array_add (dbus_proxy_argv, g_strdup ("--filter"));
  if (app_id)
    {
      g_ptr_array_add (dbus_proxy_argv, g_strdup_printf ("--own=%s", app_id));
      g_ptr_array_add (dbus_proxy_argv, g_strdup_printf ("--own=%s.*", app_id));
    }

  g_hash_table_iter_init (&iter, ht);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      FlatpakPolicy policy = GPOINTER_TO_INT (value);

      if (policy > 0)
        g_ptr_array_add (dbus_proxy_argv, g_strdup_printf ("--%s=%s", flatpak_policy_to_string (policy), (char *) key));
    }
}

static int
flatpak_extension_compare_by_path (gconstpointer  _a,
                                   gconstpointer  _b)
{
  const FlatpakExtension *a = _a;
  const FlatpakExtension *b = _b;

  return g_strcmp0 (a->directory, b->directory);
}

gboolean
flatpak_run_add_extension_args (GPtrArray    *argv_array,
                                GArray       *fd_array,
                                char       ***envp_p,
                                GKeyFile     *metakey,
                                const char   *full_ref,
                                gboolean      use_ld_so_cache,
                                char        **extensions_out,
                                GCancellable *cancellable,
                                GError      **error)
{
  g_auto(GStrv) parts = NULL;
  g_autoptr(GString) used_extensions = g_string_new ("");
  gboolean is_app;
  GList *extensions, *path_sorted_extensions, *l;
  g_autoptr(GString) ld_library_path = g_string_new ("");
  int count = 0;
  g_autoptr(GHashTable) mounted_tmpfs =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_autoptr(GHashTable) created_symlink =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  parts = g_strsplit (full_ref, "/", 0);
  if (g_strv_length (parts) != 4)
    return flatpak_fail (error, "Failed to determine parts from ref: %s", full_ref);

  is_app = strcmp (parts[0], "app") == 0;

  extensions = flatpak_list_extensions (metakey,
                                        parts[2], parts[3]);

  /* First we apply all the bindings, they are sorted alphabetically in order for parent directory
     to be mounted before child directories */
  path_sorted_extensions = g_list_copy (extensions);
  path_sorted_extensions = g_list_sort (path_sorted_extensions, flatpak_extension_compare_by_path);

  for (l = path_sorted_extensions; l != NULL; l = l->next)
    {
      FlatpakExtension *ext = l->data;
      g_autofree char *directory = g_build_filename (is_app ? "/app" : "/usr", ext->directory, NULL);
      g_autofree char *full_directory = g_build_filename (directory, ext->subdir_suffix, NULL);
      g_autofree char *ref = g_build_filename (full_directory, ".ref", NULL);
      g_autofree char *real_ref = g_build_filename (ext->files_path, ext->directory, ".ref", NULL);

      if (ext->needs_tmpfs)
        {
          g_autofree char *parent = g_path_get_dirname (directory);

          if (g_hash_table_lookup (mounted_tmpfs, parent) == NULL)
            {
              add_args (argv_array,
                        "--tmpfs", parent,
                        NULL);
              g_hash_table_insert (mounted_tmpfs, g_steal_pointer (&parent), "mounted");
            }
        }

      add_args (argv_array,
                "--ro-bind", ext->files_path, full_directory,
                NULL);

      if (g_file_test (real_ref, G_FILE_TEST_EXISTS))
        add_args (argv_array,
                  "--lock-file", ref,
                  NULL);
    }

  g_list_free (path_sorted_extensions);

  /* Then apply library directories and file merging, in extension prio order */

  for (l = extensions; l != NULL; l = l->next)
    {
      FlatpakExtension *ext = l->data;
      g_autofree char *directory = g_build_filename (is_app ? "/app" : "/usr", ext->directory, NULL);
      g_autofree char *full_directory = g_build_filename (directory, ext->subdir_suffix, NULL);
      int i;

      if (used_extensions->len > 0)
        g_string_append (used_extensions, ";");
      g_string_append (used_extensions, ext->installed_id);
      g_string_append (used_extensions, "=");
      if (ext->commit != NULL)
        g_string_append (used_extensions, ext->commit);
      else
        g_string_append (used_extensions, "local");

      if (ext->add_ld_path)
        {
          g_autofree char *ld_path = g_build_filename (full_directory, ext->add_ld_path, NULL);

          if (use_ld_so_cache)
            {
              g_autofree char *contents = g_strconcat (ld_path, "\n", NULL);
              /* We prepend app or runtime and a counter in order to get the include order correct for the conf files */
              g_autofree char *ld_so_conf_file = g_strdup_printf ("%s-%03d-%s.conf", parts[0], ++count, ext->installed_id);
              g_autofree char *ld_so_conf_file_path = g_build_filename ("/run/flatpak/ld.so.conf.d", ld_so_conf_file, NULL);

              if (!add_args_data (argv_array, fd_array, "ld-so-conf",
                                  contents, -1, ld_so_conf_file_path, error))
                return FALSE;
            }
          else
            {
              if (ld_library_path->len != 0)
                g_string_append (ld_library_path, ":");
              g_string_append (ld_library_path, ld_path);
            }
        }

      for (i = 0; ext->merge_dirs != NULL && ext->merge_dirs[i] != NULL; i++)
        {
          g_autofree char *parent = g_path_get_dirname (directory);
          g_autofree char *merge_dir = g_build_filename (parent, ext->merge_dirs[i], NULL);
          g_autofree char *source_dir = g_build_filename (ext->files_path, ext->merge_dirs[i], NULL);
          g_auto(GLnxDirFdIterator) source_iter = { 0 };
          struct dirent *dent;

          if (glnx_dirfd_iterator_init_at (AT_FDCWD, source_dir, TRUE, &source_iter, NULL))
            {
              while (glnx_dirfd_iterator_next_dent (&source_iter, &dent, NULL, NULL) && dent != NULL)
                {
                  g_autofree char *symlink_path = g_build_filename (merge_dir, dent->d_name, NULL);
                  /* Only create the first, because extensions are listed in prio order */
                  if (g_hash_table_lookup (created_symlink, symlink_path) == NULL)
                    {
                      g_autofree char *symlink = g_build_filename (directory, ext->merge_dirs[i], dent->d_name, NULL);
                      add_args (argv_array,
                                "--symlink", symlink, symlink_path,
                                NULL);
                      g_hash_table_insert (created_symlink, g_steal_pointer (&symlink_path), "created");
                    }
                }
            }
        }
    }

  g_list_free_full (extensions, (GDestroyNotify) flatpak_extension_free);

  if (ld_library_path->len != 0)
    {
      const gchar *old_ld_path = g_environ_getenv (*envp_p, "LD_LIBRARY_PATH");

      if (old_ld_path != NULL && *old_ld_path != 0)
        {
          if (is_app)
            {
              g_string_append (ld_library_path, ":");
              g_string_append (ld_library_path, old_ld_path);
            }
          else
            {
              g_string_prepend (ld_library_path, ":");
              g_string_prepend (ld_library_path, old_ld_path);
            }
        }

      *envp_p = g_environ_setenv (*envp_p, "LD_LIBRARY_PATH", ld_library_path->str , TRUE);
    }

  if (extensions_out)
    *extensions_out = g_string_free (g_steal_pointer (&used_extensions), FALSE);

  return TRUE;
}

static char *
make_relative (const char *base, const char *path)
{
  GString *s = g_string_new ("");

  while (*base != 0)
    {
      while (*base == '/')
        base++;

      if (*base != 0)
        g_string_append (s, "../");

      while (*base != '/' && *base != 0)
        base++;
    }

  while (*path == '/')
    path++;

  g_string_append (s, path);

  return g_string_free (s, FALSE);
}

#define FAKE_MODE_DIR -1 /* Ensure a dir, either on tmpfs or mapped parent */
#define FAKE_MODE_TMPFS 0
#define FAKE_MODE_SYMLINK G_MAXINT

typedef struct {
  char *path;
  gint mode;
} ExportedPath;

struct _FlatpakExports {
  GHashTable *hash;
};

static void
exported_path_free (ExportedPath *exported_path)
{
  g_free (exported_path->path);
  g_free (exported_path);
}

static FlatpakExports *
exports_new (void)
{
  FlatpakExports *exports = g_new0 (FlatpakExports, 1);
  exports->hash = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, (GFreeFunc)exported_path_free);
  return exports;
}

void
flatpak_exports_free (FlatpakExports *exports)
{
  g_hash_table_destroy (exports->hash);
  g_free (exports);
}

/* Returns TRUE if the location of this export
   is not visible due to parents being exported */
static gboolean
path_parent_is_mapped (const char **keys,
                       guint n_keys,
                       GHashTable *hash_table,
                       const char *path)
{
  guint i;
  gboolean is_mapped = FALSE;

  /* The keys are sorted so shorter (i.e. parents) are first */
  for (i = 0; i < n_keys; i++)
    {
      const char *mounted_path = keys[i];
      ExportedPath *ep = g_hash_table_lookup (hash_table, mounted_path);

      if (flatpak_has_path_prefix (path, mounted_path) &&
          (strcmp (path, mounted_path) != 0))
        {
          /* FAKE_MODE_DIR has same mapped value as parent */
          if (ep->mode == FAKE_MODE_DIR)
            continue;

          is_mapped = ep->mode != FAKE_MODE_TMPFS;
        }
    }

  return is_mapped;
}

static gboolean
path_is_mapped (const char **keys,
                guint n_keys,
                GHashTable *hash_table,
                const char *path)
{
  guint i;
  gboolean is_mapped = FALSE;

  /* The keys are sorted so shorter (i.e. parents) are first */
  for (i = 0; i < n_keys; i++)
    {
      const char *mounted_path = keys[i];
      ExportedPath *ep = g_hash_table_lookup (hash_table, mounted_path);

      if (flatpak_has_path_prefix (path, mounted_path))
        {
          /* FAKE_MODE_DIR has same mapped value as parent */
          if (ep->mode == FAKE_MODE_DIR)
            continue;

          if (ep->mode == FAKE_MODE_SYMLINK)
            is_mapped = strcmp (path, mounted_path) == 0;
          else
            is_mapped = ep->mode != FAKE_MODE_TMPFS;
        }
    }

  return is_mapped;
}

static gint
compare_eps (const ExportedPath *a,
             const ExportedPath *b)
{
  return g_strcmp0 (a->path, b->path);
}

/* This differs from g_file_test (path, G_FILE_TEST_IS_DIR) which
   returns true if the path is a symlink to a dir */
static gboolean
path_is_dir (const char *path)
{
  struct stat s;

  if (lstat (path, &s) != 0)
    return FALSE;

  return S_ISDIR (s.st_mode);
}

static gboolean
path_is_symlink (const char *path)
{
  struct stat s;

  if (lstat (path, &s) != 0)
    return FALSE;

  return S_ISLNK (s.st_mode);
}

static void
exports_add_bwrap_args (FlatpakExports *exports,
                        GPtrArray *argv_array)
{
  guint n_keys;
  g_autofree const char **keys = (const char **)g_hash_table_get_keys_as_array (exports->hash, &n_keys);
  g_autoptr(GList) eps = NULL;
  GList *l;

  eps = g_hash_table_get_values (exports->hash);
  eps = g_list_sort (eps, (GCompareFunc)compare_eps);

  g_qsort_with_data (keys, n_keys, sizeof (char *), (GCompareDataFunc) flatpak_strcmp0_ptr, NULL);

  for (l = eps; l != NULL; l = l->next)
    {
      ExportedPath *ep = l->data;
      const char *path = ep->path;

      if (ep->mode == FAKE_MODE_SYMLINK)
        {
          if (!path_parent_is_mapped (keys, n_keys, exports->hash, path))
            {
              g_autofree char *resolved = flatpak_resolve_link (path, NULL);
              if (resolved)
                {
                  g_autofree char *parent = g_path_get_dirname (path);
                  g_autofree char *relative = make_relative (parent, resolved);
                  add_args (argv_array, "--symlink", relative, path,  NULL);
                }
            }
        }
      else if (ep->mode == FAKE_MODE_TMPFS)
        {
          /* Mount a tmpfs to hide the subdirectory, but only if there
             is a pre-existing dir we can mount the path on. */
          if (path_is_dir (path))
            {
              if (!path_parent_is_mapped (keys, n_keys, exports->hash, path))
                /* If the parent is not mapped, it will be a tmpfs, no need to mount another one */
                add_args (argv_array, "--dir", path, NULL);
              else
                add_args (argv_array, "--tmpfs", path, NULL);
            }
        }
      else if (ep->mode == FAKE_MODE_DIR)
        {
          if (path_is_dir (path))
            add_args (argv_array, "--dir", path, NULL);
        }
      else
        {
          add_args (argv_array,
                    (ep->mode == FLATPAK_FILESYSTEM_MODE_READ_ONLY) ? "--ro-bind" : "--bind",
                    path, path, NULL);
        }
    }
}

gboolean
flatpak_exports_path_is_visible (FlatpakExports *exports,
                                 const char *path)
{
  guint n_keys;
  g_autofree const char **keys = (const char **)g_hash_table_get_keys_as_array (exports->hash, &n_keys);
  g_autofree char *canonical = NULL;
  g_auto(GStrv) parts = NULL;
  int i;
  g_autoptr(GString) path_builder = g_string_new ("");
  struct stat st;

  g_qsort_with_data (keys, n_keys, sizeof (char *), (GCompareDataFunc) flatpak_strcmp0_ptr, NULL);

  path = canonical = flatpak_canonicalize_filename (path);

  parts = g_strsplit (path+1, "/", -1);

  /* A path is visible in the sandbox if no parent
   * path element that is mapped in the sandbox is
   * a symlink, and the final element is mapped.
   * If any parent is a symlink we resolve that and
   * continue with that instead.
   */
  for (i = 0; parts[i] != NULL; i++)
    {
      g_string_append (path_builder, "/");
      g_string_append (path_builder, parts[i]);

      if (path_is_mapped (keys, n_keys, exports->hash, path_builder->str))
        {
          if (lstat (path_builder->str, &st) != 0)
            return FALSE;

          if (S_ISLNK (st.st_mode))
            {
              g_autofree char *resolved = flatpak_resolve_link (path_builder->str, NULL);
              g_autoptr(GString) path2_builder = NULL;
              int j;

              if (resolved == NULL)
                return FALSE;
              path2_builder = g_string_new (resolved);

              for (j = i + 1; parts[j] != NULL; j++)
                {
                  g_string_append (path2_builder, "/");
                  g_string_append (path2_builder, parts[j]);
                }


              return flatpak_exports_path_is_visible (exports, path2_builder->str);
            }
        }
      else if (parts[i+1] == NULL)
        return FALSE; /* Last part was not mapped */
    }

  return TRUE;
}

static gboolean
never_export_as_symlink (const char *path)
{
  /* Don't export /tmp as a symlink even if it is on the host, because
     that will fail with the pre-existing directory we created for /tmp,
     and anyway, it being a symlink is not useful in the sandbox */
  if (strcmp (path, "/tmp") == 0)
    return TRUE;

  return FALSE;
}

static void
do_export_path (FlatpakExports *exports,
                const char *path,
                gint mode)
{
  ExportedPath *old_ep = g_hash_table_lookup (exports->hash, path);
  ExportedPath *ep;

  ep = g_new0 (ExportedPath, 1);
  ep->path = g_strdup (path);

  if (old_ep != NULL)
    ep->mode = MAX (old_ep->mode, mode);
  else
    ep->mode = mode;

  g_hash_table_replace (exports->hash, ep->path, ep);
}


/* We use level to avoid infinite recursion */
static gboolean
_exports_path_expose (FlatpakExports *exports,
                      int mode,
                      const char *path,
                      int level)
{
  g_autofree char *canonical = NULL;
  struct stat st;
  char *slash;
  int i;

  if (level > 40) /* 40 is the current kernel ELOOP check */
    {
      g_debug ("Expose too deep, bail");
      return FALSE;
    }

  if (!g_path_is_absolute (path))
    {
      g_debug ("Not exposing relative path %s", path);
      return FALSE;
    }

  /* Check if it exists at all */
  if (lstat (path, &st) != 0)
    return FALSE;

  /* Don't expose weird things */
  if (!(S_ISDIR (st.st_mode) ||
        S_ISREG (st.st_mode) ||
        S_ISLNK (st.st_mode) ||
        S_ISSOCK (st.st_mode)))
    return FALSE;

  path = canonical = flatpak_canonicalize_filename (path);

  for (i = 0; dont_export_in[i] != NULL; i++)
    {
      /* Don't expose files in non-mounted dirs like /app or /usr, as
         they are not the same as on the host, and we generally can't
         create the parents for them anyway */
      if (flatpak_has_path_prefix (path, dont_export_in[i]))
        {
          g_debug ("skipping export for path %s", path);
          return FALSE;
        }
    }

  /* Handle any symlinks prior to the target itself. This includes path itself,
     because we expose the target of the symlink. */
  slash = canonical;
  do
    {
      slash = strchr (slash + 1, '/');
      if (slash)
        *slash = 0;

      if (path_is_symlink (path) && !never_export_as_symlink (path))
        {
          g_autofree char *resolved = flatpak_resolve_link (path, NULL);
          g_autofree char *new_target = NULL;

          if (resolved)
            {
              if (slash)
                new_target = g_build_filename (resolved, slash + 1, NULL);
              else
                new_target = g_strdup (resolved);

              if (_exports_path_expose (exports, mode, new_target, level + 1))
                {
                  do_export_path (exports, path, FAKE_MODE_SYMLINK);
                  return TRUE;
                }
            }

          return FALSE;
        }
      if (slash)
        *slash = '/';
    }
  while (slash != NULL);

  do_export_path (exports, path, mode);
  return TRUE;
}

static void
exports_path_expose (FlatpakExports *exports,
                     FlatpakFilesystemMode mode,
                     const char *path)
{
  _exports_path_expose (exports, mode, path, 0);
}

static void
exports_path_tmpfs (FlatpakExports *exports,
                    const char *path)
{
  _exports_path_expose (exports, FAKE_MODE_TMPFS, path, 0);
}

static void
exports_path_dir (FlatpakExports *exports,
                  const char *path)
{
  _exports_path_expose (exports, FAKE_MODE_DIR, path, 0);
}

static void
export_paths_export_context (FlatpakContext *context,
                             FlatpakExports *exports,
                             GFile *app_id_dir,
                             gboolean do_create,
                             GString *xdg_dirs_conf,
                             gboolean *home_access_out)
{
  gboolean home_access = FALSE;
  FlatpakFilesystemMode fs_mode, home_mode;
  GHashTableIter iter;
  gpointer key, value;

  fs_mode = (FlatpakFilesystemMode) g_hash_table_lookup (context->filesystems, "host");
  if (fs_mode != 0)
    {
      DIR *dir;
      struct dirent *dirent;

      g_debug ("Allowing host-fs access");
      home_access = TRUE;

      /* Bind mount most dirs in / into the new root */
      dir = opendir ("/");
      if (dir != NULL)
        {
          while ((dirent = readdir (dir)))
            {
              g_autofree char *path = NULL;

              if (g_strv_contains (dont_mount_in_root, dirent->d_name))
                continue;

              path = g_build_filename ("/", dirent->d_name, NULL);
              exports_path_expose (exports, fs_mode, path);
            }
          closedir (dir);
        }
      exports_path_expose (exports, fs_mode, "/run/media");
    }

  home_mode = (FlatpakFilesystemMode) g_hash_table_lookup (context->filesystems, "home");
  if (home_mode != 0)
    {
      g_debug ("Allowing homedir access");
      home_access = TRUE;

      exports_path_expose (exports, MAX (home_mode, fs_mode), g_get_home_dir ());
    }

  g_hash_table_iter_init (&iter, context->filesystems);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      const char *filesystem = key;
      FlatpakFilesystemMode mode = GPOINTER_TO_INT (value);

      if (value == NULL ||
          strcmp (filesystem, "host") == 0 ||
          strcmp (filesystem, "home") == 0)
        continue;

      if (g_str_has_prefix (filesystem, "xdg-"))
        {
          const char *path, *rest = NULL;
          const char *config_key = NULL;
          g_autofree char *subpath = NULL;

          if (!get_xdg_user_dir_from_string (filesystem, &config_key, &rest, &path))
            {
              g_warning ("Unsupported xdg dir %s", filesystem);
              continue;
            }

          if (path == NULL)
            continue; /* Unconfigured, ignore */

          if (strcmp (path, g_get_home_dir ()) == 0)
            {
              /* xdg-user-dirs sets disabled dirs to $HOME, and its in general not a good
                 idea to set full access to $HOME other than explicitly, so we ignore
                 these */
              g_debug ("Xdg dir %s is $HOME (i.e. disabled), ignoring", filesystem);
              continue;
            }

          subpath = g_build_filename (path, rest, NULL);

          if (mode == FLATPAK_FILESYSTEM_MODE_CREATE && do_create)
            g_mkdir_with_parents (subpath, 0755);

          if (g_file_test (subpath, G_FILE_TEST_EXISTS))
            {
              if (config_key && xdg_dirs_conf)
                g_string_append_printf (xdg_dirs_conf, "%s=\"%s\"\n",
                                        config_key, path);

              exports_path_expose (exports, mode, subpath);
            }
        }
      else if (g_str_has_prefix (filesystem, "~/"))
        {
          g_autofree char *path = NULL;

          path = g_build_filename (g_get_home_dir (), filesystem + 2, NULL);

          if (mode == FLATPAK_FILESYSTEM_MODE_CREATE && do_create)
            g_mkdir_with_parents (path, 0755);

          if (g_file_test (path, G_FILE_TEST_EXISTS))
            exports_path_expose (exports, mode, path);
        }
      else if (g_str_has_prefix (filesystem, "/"))
        {
          if (mode == FLATPAK_FILESYSTEM_MODE_CREATE && do_create)
            g_mkdir_with_parents (filesystem, 0755);

          if (g_file_test (filesystem, G_FILE_TEST_EXISTS))
            exports_path_expose (exports, mode, filesystem);
        }
      else
        {
          g_warning ("Unexpected filesystem arg %s", filesystem);
        }
    }

  if (app_id_dir)
    {
      g_autoptr(GFile) apps_dir = g_file_get_parent (app_id_dir);
      /* Hide the .var/app dir by default (unless explicitly made visible) */
      exports_path_tmpfs (exports, flatpak_file_get_path_cached (apps_dir));
      /* But let the app write to the per-app dir in it */
      exports_path_expose (exports, FLATPAK_FILESYSTEM_MODE_READ_WRITE,
                           flatpak_file_get_path_cached (app_id_dir));
    }

  if (home_access_out != NULL)
    *home_access_out = home_access;
}

FlatpakExports *
flatpak_exports_from_context (FlatpakContext *context,
                              const char *app_id)
{
  g_autoptr(FlatpakExports) exports = exports_new ();
  g_autoptr(GFile) app_id_dir = flatpak_get_data_dir (app_id);

  export_paths_export_context (context, exports, app_id_dir, FALSE, NULL, NULL);
  return g_steal_pointer (&exports);
}

/* This resolves the target here rather than the destination, because
   it may not resolve in bwrap setup due to absolute relative links
   conflicting with /newroot root. */
static void
add_bind_arg (GPtrArray *argv_array,
              const char *type,
              const char *src,
              const char *dest)
{
  g_autofree char *dest_real = realpath (dest, NULL);

  if (dest_real)
    add_args (argv_array, type, src, dest_real, NULL);
}

gboolean
flatpak_run_add_environment_args (GPtrArray      *argv_array,
                                  GArray         *fd_array,
                                  char         ***envp_p,
                                  const char     *app_info_path,
                                  FlatpakRunFlags flags,
                                  const char     *app_id,
                                  FlatpakContext *context,
                                  GFile          *app_id_dir,
                                  FlatpakExports **exports_out,
                                  GCancellable   *cancellable,
                                  GError        **error)
{
  gboolean home_access = FALSE;
  GHashTableIter iter;
  gpointer key, value;
  gboolean unrestricted_session_bus;
  gboolean unrestricted_system_bus;
  g_autoptr(GString) xdg_dirs_conf = g_string_new ("");
  g_autoptr(GError) my_error = NULL;
  g_autoptr(GFile) user_flatpak_dir = NULL;
  g_autoptr(FlatpakExports) exports = exports_new ();
  g_autoptr(GPtrArray) session_bus_proxy_argv = NULL;
  g_autoptr(GPtrArray) system_bus_proxy_argv = NULL;
  g_autoptr(GPtrArray) a11y_bus_proxy_argv = NULL;
  int sync_fds[2] = {-1, -1};

  if ((flags & FLATPAK_RUN_FLAG_NO_SESSION_BUS_PROXY) == 0)
    session_bus_proxy_argv = g_ptr_array_new_with_free_func (g_free);
  if ((flags & FLATPAK_RUN_FLAG_NO_SYSTEM_BUS_PROXY) == 0)
    system_bus_proxy_argv = g_ptr_array_new_with_free_func (g_free);

 if ((context->shares & FLATPAK_CONTEXT_SHARED_IPC) == 0)
    {
      g_debug ("Disallowing ipc access");
      add_args (argv_array, "--unshare-ipc", NULL);
    }

  if ((context->shares & FLATPAK_CONTEXT_SHARED_NETWORK) == 0)
    {
      g_debug ("Disallowing network access");
      add_args (argv_array, "--unshare-net", NULL);
    }

  if (context->devices & FLATPAK_CONTEXT_DEVICE_ALL)
    {
      add_args (argv_array,
                "--dev-bind", "/dev", "/dev",
                NULL);
    }
  else
    {
      add_args (argv_array,
                "--dev", "/dev",
                NULL);
      if (context->devices & FLATPAK_CONTEXT_DEVICE_DRI)
        {
          g_debug ("Allowing dri access");
          int i;
          char *dri_devices[] = {
            "/dev/dri",
            /* mali */
            "/dev/mali",
            "/dev/umplock",
            /* nvidia */
            "/dev/nvidiactl",
            "/dev/nvidia0",
            "/dev/nvidia-modeset",
          };

          for (i = 0; i < G_N_ELEMENTS(dri_devices); i++)
            {
              if (g_file_test (dri_devices[i], G_FILE_TEST_EXISTS))
                add_args (argv_array, "--dev-bind", dri_devices[i], dri_devices[i], NULL);
            }
        }

      if (context->devices & FLATPAK_CONTEXT_DEVICE_KVM)
        {
          g_debug ("Allowing kvm access");
          if (g_file_test ("/dev/kvm", G_FILE_TEST_EXISTS))
            add_args (argv_array, "--dev-bind", "/dev/kvm", "/dev/kvm", NULL);
        }
    }

  export_paths_export_context (context, exports, app_id_dir, TRUE, xdg_dirs_conf, &home_access);
  if (app_id_dir != NULL)
    *envp_p = flatpak_run_apply_env_appid (*envp_p, app_id_dir);

  if (!home_access)
    {
      /* Enable persistent mapping only if no access to real home dir */

      g_hash_table_iter_init (&iter, context->persistent);
      while (g_hash_table_iter_next (&iter, &key, NULL))
        {
          const char *persist = key;
          g_autofree char *src = g_build_filename (g_get_home_dir (), ".var/app", app_id, persist, NULL);
          g_autofree char *dest = g_build_filename (g_get_home_dir (), persist, NULL);

          g_mkdir_with_parents (src, 0755);

          /* We stick to add_args instead of add_bind_arg because persisted
           * folders don't need to exist outside the chroot.
           */
          add_args (argv_array, "--bind", src, dest, NULL);
        }
    }

  {
    g_autofree char *run_user_app_dst = g_strdup_printf ("/run/user/%d/app/%s", getuid (), app_id);
    g_autofree char *run_user_app_src = g_build_filename (g_get_user_runtime_dir (), "app", app_id, NULL);

    if (glnx_shutil_mkdir_p_at (AT_FDCWD,
                                run_user_app_src,
                                0700,
                                NULL,
                                NULL))
        add_args (argv_array,
                  "--bind", run_user_app_src, run_user_app_dst,
                  NULL);
  }

  /* Hide the flatpak dir by default (unless explicitly made visible) */
  user_flatpak_dir = flatpak_get_user_base_dir_location ();
  exports_path_tmpfs (exports, flatpak_file_get_path_cached (user_flatpak_dir));

  /* Ensure we always have a homedir */
  exports_path_dir (exports, g_get_home_dir ());

  /* This actually outputs the args for the hide/expose operations above */
  exports_add_bwrap_args (exports, argv_array);

  /* Special case subdirectories of the cache, config and data xdg
   * dirs.  If these are accessible explicilty, then we bind-mount
   * these in the app-id dir. This allows applications to explicitly
   * opt out of keeping some config/cache/data in the app-specific
   * directory.
   */
  if (app_id_dir)
    {
      g_hash_table_iter_init (&iter, context->filesystems);
      while (g_hash_table_iter_next (&iter, &key, &value))
        {
          const char *filesystem = key;
          FlatpakFilesystemMode mode = GPOINTER_TO_INT (value);
          g_autofree char *xdg_path = NULL;
          const char *rest, *where;

          xdg_path = get_xdg_dir_from_string (filesystem, &rest, &where);

          if (xdg_path != NULL && *rest != 0 &&
              mode >= FLATPAK_FILESYSTEM_MODE_READ_ONLY)
            {
              g_autoptr(GFile) app_version = g_file_get_child (app_id_dir, where);
              g_autoptr(GFile) app_version_subdir = g_file_resolve_relative_path (app_version, rest);

              if (g_file_test (xdg_path, G_FILE_TEST_IS_DIR) ||
                  g_file_test (xdg_path, G_FILE_TEST_IS_REGULAR))
                {
                  g_autofree char *xdg_path_in_app = g_file_get_path (app_version_subdir);
                  add_bind_arg (argv_array,
                                mode == FLATPAK_FILESYSTEM_MODE_READ_ONLY ? "--ro-bind" : "--bind",
                                xdg_path, xdg_path_in_app);
                }
            }
        }
    }

  if (home_access  && app_id_dir != NULL)
    {
      g_autofree char *src_path = g_build_filename (g_get_user_config_dir (),
                                                    "user-dirs.dirs",
                                                    NULL);
      g_autofree char *path = g_build_filename (flatpak_file_get_path_cached (app_id_dir),
                                                "config/user-dirs.dirs", NULL);
      if (g_file_test (src_path, G_FILE_TEST_EXISTS))
        add_bind_arg (argv_array, "--ro-bind", src_path, path);
    }
  else if (xdg_dirs_conf->len > 0 && app_id_dir != NULL)
    {
      g_autofree char *path =
        g_build_filename (flatpak_file_get_path_cached (app_id_dir),
                          "config/user-dirs.dirs", NULL);

      add_args_data (argv_array, fd_array, "xdg-config-dirs",
                     xdg_dirs_conf->str, xdg_dirs_conf->len, path, NULL);
    }

  flatpak_run_add_x11_args (argv_array, fd_array, envp_p,
                            (context->sockets & FLATPAK_CONTEXT_SOCKET_X11) != 0);

  if (context->sockets & FLATPAK_CONTEXT_SOCKET_WAYLAND)
    {
      g_debug ("Allowing wayland access");
      flatpak_run_add_wayland_args (argv_array, envp_p);
    }

  if (context->sockets & FLATPAK_CONTEXT_SOCKET_PULSEAUDIO)
    {
      g_debug ("Allowing pulseaudio access");
      flatpak_run_add_pulseaudio_args (argv_array, fd_array, envp_p);
    }

  unrestricted_session_bus = (context->sockets & FLATPAK_CONTEXT_SOCKET_SESSION_BUS) != 0;
  if (unrestricted_session_bus)
    g_debug ("Allowing session-dbus access");
  if (flatpak_run_add_session_dbus_args (argv_array, envp_p, session_bus_proxy_argv, unrestricted_session_bus) &&
      !unrestricted_session_bus && session_bus_proxy_argv)
    flatpak_add_bus_filters (session_bus_proxy_argv, context->session_bus_policy, app_id, context);

  unrestricted_system_bus = (context->sockets & FLATPAK_CONTEXT_SOCKET_SYSTEM_BUS) != 0;
  if (unrestricted_system_bus)
    g_debug ("Allowing system-dbus access");
  if (flatpak_run_add_system_dbus_args (context, envp_p, argv_array, system_bus_proxy_argv,
                                        unrestricted_system_bus) &&
      !unrestricted_system_bus && system_bus_proxy_argv)
    flatpak_add_bus_filters (system_bus_proxy_argv, context->system_bus_policy, NULL, context);

  if ((flags & FLATPAK_RUN_FLAG_NO_A11Y_BUS_PROXY) == 0)
    {
      g_autoptr(GDBusConnection) session_bus = NULL;
      g_autofree char *a11y_address = NULL;

      session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
      if (session_bus)
        {
          g_autoptr(GError) local_error = NULL;
          g_autoptr(GDBusMessage) reply = NULL;
          g_autoptr(GDBusMessage) msg =
            g_dbus_message_new_method_call ("org.a11y.Bus",
                                            "/org/a11y/bus",
                                            "org.a11y.Bus",
                                            "GetAddress");
          g_dbus_message_set_body (msg, g_variant_new ("()"));
          reply =
            g_dbus_connection_send_message_with_reply_sync (session_bus, msg,
                                                            G_DBUS_SEND_MESSAGE_FLAGS_NONE,
                                                            30000,
                                                            NULL,
                                                            NULL,
                                                            NULL);
          if (reply)
            {
              if (g_dbus_message_to_gerror (reply, &local_error))
                {
                  if (!g_error_matches (local_error, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN))
                    g_message ("Can't find a11y bus: %s", local_error->message);
                }
              else
                {
                  g_variant_get (g_dbus_message_get_body (reply),
                                 "(s)", &a11y_address);
                }
            }
        }

      if (a11y_address)
        {
          g_autofree char *proxy_socket = create_proxy_socket ("a11y-bus-proxy-XXXXXX");
          if (proxy_socket)
            {
              g_autofree char *sandbox_socket_path = g_strdup_printf ("/run/user/%d/at-spi-bus", getuid ());
              g_autofree char *sandbox_dbus_address = g_strdup_printf ("unix:path=/run/user/%d/at-spi-bus", getuid ());

              a11y_bus_proxy_argv = g_ptr_array_new_with_free_func (g_free);

              g_ptr_array_add (a11y_bus_proxy_argv, g_strdup (a11y_address));
              g_ptr_array_add (a11y_bus_proxy_argv, g_strdup (proxy_socket));
              g_ptr_array_add (a11y_bus_proxy_argv, g_strdup ("--filter"));
              g_ptr_array_add (a11y_bus_proxy_argv, g_strdup ("--sloppy-names"));
              g_ptr_array_add (a11y_bus_proxy_argv,
                               g_strdup ("--filter=org.a11y.atspi.Registry=org.a11y.atspi.Socket.Embed@/org/a11y/atspi/accessible/root"));
              g_ptr_array_add (a11y_bus_proxy_argv,
                               g_strdup ("--filter=org.a11y.atspi.Registry=org.a11y.atspi.Socket.Unembed@/org/a11y/atspi/accessible/root"));
              g_ptr_array_add (a11y_bus_proxy_argv,
                               g_strdup ("--filter=org.a11y.atspi.Registry=org.a11y.atspi.Registry.GetRegisteredEvents@/org/a11y/atspi/registry"));
              g_ptr_array_add (a11y_bus_proxy_argv,
                               g_strdup ("--filter=org.a11y.atspi.Registry=org.a11y.atspi.DeviceEventController.GetKeystrokeListeners@/org/a11y/atspi/registry/deviceeventcontroller"));
              g_ptr_array_add (a11y_bus_proxy_argv,
                               g_strdup ("--filter=org.a11y.atspi.Registry=org.a11y.atspi.DeviceEventController.GetDeviceEventListeners@/org/a11y/atspi/registry/deviceeventcontroller"));
              g_ptr_array_add (a11y_bus_proxy_argv,
                               g_strdup ("--filter=org.a11y.atspi.Registry=org.a11y.atspi.DeviceEventController.NotifyListenersSync@/org/a11y/atspi/registry/deviceeventcontroller"));
              g_ptr_array_add (a11y_bus_proxy_argv,
                               g_strdup ("--filter=org.a11y.atspi.Registry=org.a11y.atspi.DeviceEventController.NotifyListenersAsync@/org/a11y/atspi/registry/deviceeventcontroller"));

              add_args (argv_array,
                        "--bind", proxy_socket, sandbox_socket_path,
                        NULL);
              *envp_p = g_environ_setenv (*envp_p, "AT_SPI_BUS_ADDRESS", sandbox_dbus_address, TRUE);
            }
        }
    }

  if (g_environ_getenv (*envp_p, "LD_LIBRARY_PATH") != NULL)
    {
      /* LD_LIBRARY_PATH is overridden for setuid helper, so pass it as cmdline arg */
      add_args (argv_array,
                "--setenv", "LD_LIBRARY_PATH", g_environ_getenv (*envp_p, "LD_LIBRARY_PATH"),
                NULL);
      *envp_p = g_environ_unsetenv (*envp_p, "LD_LIBRARY_PATH");
    }

  /* Must run this before spawning the dbus proxy, to ensure it
     ends up in the app cgroup */
  if (!flatpak_run_in_transient_unit (app_id, &my_error))
    {
      /* We still run along even if we don't get a cgroup, as nothing
         really depends on it. Its just nice to have */
      g_debug ("Failed to run in transient scope: %s", my_error->message);
      g_clear_error (&my_error);
    }

  if (!add_dbus_proxy_args (argv_array,
                            session_bus_proxy_argv, (flags & FLATPAK_RUN_FLAG_LOG_SESSION_BUS) != 0,
                            system_bus_proxy_argv, (flags & FLATPAK_RUN_FLAG_LOG_SYSTEM_BUS) != 0,
                            a11y_bus_proxy_argv, (flags & FLATPAK_RUN_FLAG_LOG_A11Y_BUS) != 0,
                            sync_fds, app_info_path, error))
    return FALSE;

  if (sync_fds[1] != -1)
    close (sync_fds[1]);

  if (exports_out)
    *exports_out = g_steal_pointer (&exports);

  return TRUE;
}

typedef struct {
  const char *env;
  const char *val;
} ExportData;

static const ExportData default_exports[] = {
  {"PATH", "/app/bin:/usr/bin"},
  /* We always want to unset LD_LIBRARY_PATH to avoid inheriting weird
   * dependencies from the host. But if not using ld.so.cache this is
   * later set. */
  {"LD_LIBRARY_PATH", NULL},
  {"XDG_CONFIG_DIRS", "/app/etc/xdg:/etc/xdg"},
  {"XDG_DATA_DIRS", "/app/share:/usr/share"},
  {"SHELL", "/bin/sh"},
  {"TMPDIR", NULL}, /* Unset TMPDIR as it may not exist in the sandbox */

  /* Some env vars are common enough and will affect the sandbox badly
     if set on the host. We clear these always. */
  {"PYTHONPATH", NULL},
  {"PERLLIB", NULL},
  {"PERL5LIB", NULL},
  {"XCURSOR_PATH", NULL},
};

static const ExportData no_ld_so_cache_exports[] = {
  {"LD_LIBRARY_PATH", "/app/lib"},
};

static const ExportData devel_exports[] = {
  {"ACLOCAL_PATH", "/app/share/aclocal"},
  {"C_INCLUDE_PATH", "/app/include"},
  {"CPLUS_INCLUDE_PATH", "/app/include"},
  {"LDFLAGS", "-L/app/lib "},
  {"PKG_CONFIG_PATH", "/app/lib/pkgconfig:/app/share/pkgconfig:/usr/lib/pkgconfig:/usr/share/pkgconfig"},
  {"LC_ALL", "en_US.utf8"},
};

static void
add_exports (GPtrArray *env_array,
             const ExportData *exports,
             gsize n_exports)
{
  int i;

  for (i = 0; i < n_exports; i++)
    {
      if (exports[i].val)
        g_ptr_array_add (env_array, g_strdup_printf ("%s=%s", exports[i].env, exports[i].val));
    }
}

char **
flatpak_run_get_minimal_env (gboolean devel, gboolean use_ld_so_cache)
{
  GPtrArray *env_array;
  static const char * const copy[] = {
    "PWD",
    "GDMSESSION",
    "XDG_CURRENT_DESKTOP",
    "XDG_SESSION_DESKTOP",
    "DESKTOP_SESSION",
    "EMAIL_ADDRESS",
    "HOME",
    "HOSTNAME",
    "LOGNAME",
    "REAL_NAME",
    "TERM",
    "USER",
    "USERNAME",
  };
  static const char * const copy_nodevel[] = {
    "LANG",
    "LANGUAGE",
    "LC_ALL",
    "LC_ADDRESS",
    "LC_COLLATE",
    "LC_CTYPE",
    "LC_IDENTIFICATION",
    "LC_MEASUREMENT",
    "LC_MESSAGES",
    "LC_MONETARY",
    "LC_NAME",
    "LC_NUMERIC",
    "LC_PAPER",
    "LC_TELEPHONE",
    "LC_TIME",
  };
  int i;

  env_array = g_ptr_array_new_with_free_func (g_free);

  add_exports (env_array, default_exports, G_N_ELEMENTS (default_exports));

  if (!use_ld_so_cache)
    add_exports (env_array, no_ld_so_cache_exports, G_N_ELEMENTS (no_ld_so_cache_exports));

  if (devel)
    add_exports (env_array, devel_exports, G_N_ELEMENTS (devel_exports));

  for (i = 0; i < G_N_ELEMENTS (copy); i++)
    {
      const char *current = g_getenv (copy[i]);
      if (current)
        g_ptr_array_add (env_array, g_strdup_printf ("%s=%s", copy[i], current));
    }

  if (!devel)
    {
      for (i = 0; i < G_N_ELEMENTS (copy_nodevel); i++)
        {
          const char *current = g_getenv (copy_nodevel[i]);
          if (current)
            g_ptr_array_add (env_array, g_strdup_printf ("%s=%s", copy_nodevel[i], current));
        }
    }

  g_ptr_array_add (env_array, NULL);
  return (char **) g_ptr_array_free (env_array, FALSE);
}

static char **
apply_exports (char **envp,
               const ExportData *exports,
               gsize n_exports)
{
  int i;

  for (i = 0; i < n_exports; i++)
    {
      const char *value = exports[i].val;

      if (value)
        envp = g_environ_setenv (envp, exports[i].env, value, TRUE);
      else
        envp = g_environ_unsetenv (envp, exports[i].env);
    }

  return envp;
}

char **
flatpak_run_apply_env_default (char **envp, gboolean use_ld_so_cache)
{
  envp = apply_exports (envp, default_exports, G_N_ELEMENTS (default_exports));

  if (!use_ld_so_cache)
    envp = apply_exports (envp, no_ld_so_cache_exports, G_N_ELEMENTS (no_ld_so_cache_exports));

  return envp;
}

char **
flatpak_run_apply_env_appid (char **envp,
                             GFile *app_dir)
{
  g_autoptr(GFile) app_dir_data = NULL;
  g_autoptr(GFile) app_dir_config = NULL;
  g_autoptr(GFile) app_dir_cache = NULL;

  app_dir_data = g_file_get_child (app_dir, "data");
  app_dir_config = g_file_get_child (app_dir, "config");
  app_dir_cache = g_file_get_child (app_dir, "cache");
  envp = g_environ_setenv (envp, "XDG_DATA_HOME", flatpak_file_get_path_cached (app_dir_data), TRUE);
  envp = g_environ_setenv (envp, "XDG_CONFIG_HOME", flatpak_file_get_path_cached (app_dir_config), TRUE);
  envp = g_environ_setenv (envp, "XDG_CACHE_HOME", flatpak_file_get_path_cached (app_dir_cache), TRUE);

  return envp;
}

char **
flatpak_run_apply_env_vars (char **envp, FlatpakContext *context)
{
  GHashTableIter iter;
  gpointer key, value;

  g_hash_table_iter_init (&iter, context->env_vars);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      const char *var = key;
      const char *val = value;

      if (val && val[0] != 0)
        envp = g_environ_setenv (envp, var, val, TRUE);
      else
        envp = g_environ_unsetenv (envp, var);
    }

  return envp;
}

GFile *
flatpak_get_data_dir (const char *app_id)
{
  g_autoptr(GFile) home = g_file_new_for_path (g_get_home_dir ());
  g_autoptr(GFile) var_app = g_file_resolve_relative_path (home, ".var/app");

  return g_file_get_child (var_app, app_id);
}

GFile *
flatpak_ensure_data_dir (const char   *app_id,
                         GCancellable *cancellable,
                         GError      **error)
{
  g_autoptr(GFile) dir = flatpak_get_data_dir (app_id);
  g_autoptr(GFile) data_dir = g_file_get_child (dir, "data");
  g_autoptr(GFile) cache_dir = g_file_get_child (dir, "cache");
  g_autoptr(GFile) fontconfig_cache_dir = g_file_get_child (cache_dir, "fontconfig");
  g_autoptr(GFile) tmp_dir = g_file_get_child (cache_dir, "tmp");
  g_autoptr(GFile) config_dir = g_file_get_child (dir, "config");

  if (!flatpak_mkdir_p (data_dir, cancellable, error))
    return NULL;

  if (!flatpak_mkdir_p (cache_dir, cancellable, error))
    return NULL;

  if (!flatpak_mkdir_p (fontconfig_cache_dir, cancellable, error))
    return NULL;

  if (!flatpak_mkdir_p (tmp_dir, cancellable, error))
    return NULL;

  if (!flatpak_mkdir_p (config_dir, cancellable, error))
    return NULL;

  return g_object_ref (dir);
}

struct JobData
{
  char      *job;
  GMainLoop *main_loop;
};

static void
job_removed_cb (SystemdManager *manager,
                guint32         id,
                char           *job,
                char           *unit,
                char           *result,
                struct JobData *data)
{
  if (strcmp (job, data->job) == 0)
    g_main_loop_quit (data->main_loop);
}

gboolean
flatpak_run_in_transient_unit (const char *appid, GError **error)
{
  g_autoptr(GDBusConnection) conn = NULL;
  g_autofree char *path = NULL;
  g_autofree char *address = NULL;
  g_autofree char *name = NULL;
  g_autofree char *job = NULL;
  SystemdManager *manager = NULL;
  GVariantBuilder builder;
  GVariant *properties = NULL;
  GVariant *aux = NULL;
  guint32 pid;
  GMainContext *main_context = NULL;
  GMainLoop *main_loop = NULL;
  struct JobData data;
  gboolean res = FALSE;

  path = g_strdup_printf ("/run/user/%d/systemd/private", getuid ());

  if (!g_file_test (path, G_FILE_TEST_EXISTS))
    return flatpak_fail (error,
                         "No systemd user session available, cgroups not available");

  main_context = g_main_context_new ();
  main_loop = g_main_loop_new (main_context, FALSE);

  g_main_context_push_thread_default (main_context);

  address = g_strconcat ("unix:path=", path, NULL);

  conn = g_dbus_connection_new_for_address_sync (address,
                                                 G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT,
                                                 NULL,
                                                 NULL, error);
  if (!conn)
    goto out;

  manager = systemd_manager_proxy_new_sync (conn,
                                            G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                            NULL,
                                            "/org/freedesktop/systemd1",
                                            NULL, error);
  if (!manager)
    goto out;

  name = g_strdup_printf ("flatpak-%s-%d.scope", appid, getpid ());

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(sv)"));

  pid = getpid ();
  g_variant_builder_add (&builder, "(sv)",
                         "PIDs",
                         g_variant_new_fixed_array (G_VARIANT_TYPE ("u"),
                                                    &pid, 1, sizeof (guint32))
                        );

  properties = g_variant_builder_end (&builder);

  aux = g_variant_new_array (G_VARIANT_TYPE ("(sa(sv))"), NULL, 0);

  if (!systemd_manager_call_start_transient_unit_sync (manager,
                                                       name,
                                                       "fail",
                                                       properties,
                                                       aux,
                                                       &job,
                                                       NULL,
                                                       error))
    goto out;

  data.job = job;
  data.main_loop = main_loop;
  g_signal_connect (manager, "job-removed", G_CALLBACK (job_removed_cb), &data);

  g_main_loop_run (main_loop);

  res = TRUE;

out:
  if (main_context)
    {
      g_main_context_pop_thread_default (main_context);
      g_main_context_unref (main_context);
    }
  if (main_loop)
    g_main_loop_unref (main_loop);
  if (manager)
    g_object_unref (manager);

  return res;
}

static void
add_font_path_args (GPtrArray *argv_array)
{
  g_autoptr(GFile) home = NULL;
  g_autoptr(GFile) user_font1 = NULL;
  g_autoptr(GFile) user_font2 = NULL;
  g_autoptr(GFile) user_font_cache = NULL;
  g_auto(GStrv) system_cache_dirs = NULL;
  gboolean found_cache = FALSE;
  int i;

  if (g_file_test (SYSTEM_FONTS_DIR, G_FILE_TEST_EXISTS))
    {
      add_args (argv_array,
                "--ro-bind", SYSTEM_FONTS_DIR, "/run/host/fonts",
                NULL);
    }

  system_cache_dirs = g_strsplit (SYSTEM_FONT_CACHE_DIRS, ":", 0);
  for (i = 0; system_cache_dirs[i] != NULL; i++)
    {
      if (g_file_test (system_cache_dirs[i], G_FILE_TEST_EXISTS))
        {
          add_args (argv_array,
                    "--ro-bind", system_cache_dirs[i], "/run/host/fonts-cache",
                    NULL);
          found_cache = TRUE;
          break;
        }
    }

  if (!found_cache)
    {
      /* We ensure these directories are never writable, or fontconfig
         will use them to write the default cache */
      add_args (argv_array,
                "--tmpfs", "/run/host/fonts-cache",
                "--remount-ro", "/run/host/fonts-cache",
                NULL);
    }

  home = g_file_new_for_path (g_get_home_dir ());
  user_font1 = g_file_resolve_relative_path (home, ".local/share/fonts");
  user_font2 = g_file_resolve_relative_path (home, ".fonts");

  if (g_file_query_exists (user_font1, NULL))
    {
      add_args (argv_array,
                "--ro-bind", flatpak_file_get_path_cached (user_font1), "/run/host/user-fonts",
                NULL);
    }
  else if (g_file_query_exists (user_font2, NULL))
    {
      add_args (argv_array,
                "--ro-bind", flatpak_file_get_path_cached (user_font2), "/run/host/user-fonts",
                NULL);
    }

  user_font_cache = g_file_resolve_relative_path (home, ".cache/fontconfig");
  if (g_file_query_exists (user_font_cache, NULL))
    {
      add_args (argv_array,
                "--ro-bind", flatpak_file_get_path_cached (user_font_cache), "/run/host/user-fonts-cache",
                NULL);
    }
  else
    {
      /* We ensure these directories are never writable, or fontconfig
         will use them to write the default cache */
      add_args (argv_array,
                "--tmpfs", "/run/host/user-fonts-cache",
                "--remount-ro", "/run/host/user-fonts-cache",
                NULL);
    }
}

static void
add_icon_path_args (GPtrArray *argv_array)
{
  if (g_file_test ("/usr/share/icons", G_FILE_TEST_IS_DIR))
    {
      add_args (argv_array,
                "--ro-bind", "/usr/share/icons", "/run/host/share/icons",
                NULL);
    }
}

static void
add_default_permissions (FlatpakContext *app_context)
{
  flatpak_context_set_session_bus_policy (app_context,
                                          "org.freedesktop.portal.*",
                                          FLATPAK_POLICY_TALK);
}

FlatpakContext *
flatpak_app_compute_permissions (GKeyFile *app_metadata,
                                 GKeyFile *runtime_metadata,
                                 GError  **error)
{
  g_autoptr(FlatpakContext) app_context = NULL;

  app_context = flatpak_context_new ();

  add_default_permissions (app_context);

  if (runtime_metadata != NULL &&
      !flatpak_context_load_metadata (app_context, runtime_metadata, error))
    return NULL;

  if (app_metadata != NULL &&
      !flatpak_context_load_metadata (app_context, app_metadata, error))
    return NULL;

  return g_steal_pointer (&app_context);
}

gboolean
flatpak_run_add_app_info_args (GPtrArray      *argv_array,
                               GArray         *fd_array,
                               GFile          *app_files,
                               GVariant       *app_deploy_data,
			       const char     *app_extensions,
                               GFile          *runtime_files,
                               GVariant       *runtime_deploy_data,
			       const char     *runtime_extensions,
                               const char     *app_id,
                               const char     *app_branch,
                               const char     *runtime_ref,
                               FlatpakContext *final_app_context,
                               char          **app_info_path_out,
                               GError        **error)
{
  g_autofree char *tmp_path = NULL;
  int fd, fd2;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autofree char *runtime_path = NULL;
  g_autofree char *old_dest = g_strdup_printf ("/run/user/%d/flatpak-info", getuid ());
  const char *group;

  fd = g_file_open_tmp ("flatpak-context-XXXXXX", &tmp_path, NULL);
  if (fd < 0)
    {
      int errsv = errno;
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errsv),
                   _("Failed to open flatpak-info temp file: %s"), g_strerror (errsv));
      return FALSE;
    }

  close (fd);

  keyfile = g_key_file_new ();

  if (app_files)
    group = FLATPAK_METADATA_GROUP_APPLICATION;
  else
    group = FLATPAK_METADATA_GROUP_RUNTIME;

  g_key_file_set_string (keyfile, group, FLATPAK_METADATA_KEY_NAME, app_id);
  g_key_file_set_string (keyfile, group, FLATPAK_METADATA_KEY_RUNTIME,
                         runtime_ref);

  if (app_files)
    {
      g_autofree char *app_path = g_file_get_path (app_files);
      g_key_file_set_string (keyfile, FLATPAK_METADATA_GROUP_INSTANCE,
                             FLATPAK_METADATA_KEY_APP_PATH, app_path);
    }
  if (app_deploy_data)
    g_key_file_set_string (keyfile, FLATPAK_METADATA_GROUP_INSTANCE,
                           FLATPAK_METADATA_KEY_APP_COMMIT, flatpak_deploy_data_get_commit (app_deploy_data));
  if (app_extensions && *app_extensions != 0)
    g_key_file_set_string (keyfile, FLATPAK_METADATA_GROUP_INSTANCE,
                           FLATPAK_METADATA_KEY_APP_EXTENSIONS, app_extensions);
  runtime_path = g_file_get_path (runtime_files);
  g_key_file_set_string (keyfile, FLATPAK_METADATA_GROUP_INSTANCE,
                         FLATPAK_METADATA_KEY_RUNTIME_PATH, runtime_path);
  if (runtime_deploy_data)
    g_key_file_set_string (keyfile, FLATPAK_METADATA_GROUP_INSTANCE,
                           FLATPAK_METADATA_KEY_RUNTIME_COMMIT, flatpak_deploy_data_get_commit (runtime_deploy_data));
  if (runtime_extensions && *runtime_extensions != 0)
    g_key_file_set_string (keyfile, FLATPAK_METADATA_GROUP_INSTANCE,
                           FLATPAK_METADATA_KEY_RUNTIME_EXTENSIONS, runtime_extensions);
  if (app_branch != NULL)
    g_key_file_set_string (keyfile, FLATPAK_METADATA_GROUP_INSTANCE,
                           FLATPAK_METADATA_KEY_BRANCH, app_branch);

  g_key_file_set_string (keyfile, FLATPAK_METADATA_GROUP_INSTANCE,
                         FLATPAK_METADATA_KEY_FLATPAK_VERSION, PACKAGE_VERSION);

  if ((final_app_context->sockets & FLATPAK_CONTEXT_SOCKET_SESSION_BUS) == 0)
    g_key_file_set_boolean (keyfile, FLATPAK_METADATA_GROUP_INSTANCE,
                            FLATPAK_METADATA_KEY_SESSION_BUS_PROXY, TRUE);

  if ((final_app_context->sockets & FLATPAK_CONTEXT_SOCKET_SYSTEM_BUS) == 0)
    g_key_file_set_boolean (keyfile, FLATPAK_METADATA_GROUP_INSTANCE,
                            FLATPAK_METADATA_KEY_SYSTEM_BUS_PROXY, TRUE);

  flatpak_context_save_metadata (final_app_context, TRUE, keyfile);

  if (!g_key_file_save_to_file (keyfile, tmp_path, error))
    return FALSE;

  /* We want to create a file on /.flatpak-info that the app cannot modify, which
     we do by creating a read-only bind mount. This way one can openat()
     /proc/$pid/root, and if that succeeds use openat via that to find the
     unfakable .flatpak-info file. However, there is a tiny race in that if
     you manage to open /proc/$pid/root, but then the pid dies, then
     every mount but the root is unmounted in the namespace, so the
     .flatpak-info will be empty. We fix this by first creating a real file
     with the real info in, then bind-mounting on top of that, the same info.
     This way even if the bind-mount is unmounted we can find the real data.
  */

  fd = open (tmp_path, O_RDONLY);
  if (fd == -1)
    {
      int errsv = errno;
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errsv),
                   _("Failed to open temp file: %s"), g_strerror (errsv));
      return FALSE;
    }

  fd2 = open (tmp_path, O_RDONLY);
  if (fd2 == -1)
    {
      close (fd);
      int errsv = errno;
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errsv),
                   _("Failed to open temp file: %s"), g_strerror (errsv));
      return FALSE;
    }

  unlink (tmp_path);

  add_args_data_fd (argv_array, fd_array,
                    "--file", fd, "/.flatpak-info");
  add_args_data_fd (argv_array, fd_array,
                    "--ro-bind-data", fd2, "/.flatpak-info");
  add_args (argv_array,
            "--symlink", "../../../.flatpak-info", old_dest,
            NULL);

  if (app_info_path_out != NULL)
    *app_info_path_out = g_strdup_printf ("/proc/self/fd/%d", fd);

  return TRUE;
}

static void
add_monitor_path_args (gboolean use_session_helper,
                       GPtrArray *argv_array)
{
  g_autoptr(AutoFlatpakSessionHelper) session_helper = NULL;
  g_autofree char *monitor_path = NULL;

  if (use_session_helper)
    {
      session_helper =
        flatpak_session_helper_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION,
                                                       G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES | G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS,
                                                       "org.freedesktop.Flatpak",
                                                       "/org/freedesktop/Flatpak/SessionHelper",
                                                       NULL, NULL);
    }

  if (session_helper &&
      flatpak_session_helper_call_request_monitor_sync (session_helper,
                                                        &monitor_path,
                                                        NULL, NULL))
    {
      add_args (argv_array,
                "--ro-bind", monitor_path, "/run/host/monitor",
                "--symlink", "/run/host/monitor/localtime", "/etc/localtime",
                "--symlink", "/run/host/monitor/resolv.conf", "/etc/resolv.conf",
                "--symlink", "/run/host/monitor/host.conf", "/etc/host.conf",
                "--symlink", "/run/host/monitor/hosts", "/etc/hosts",
                NULL);
    }
  else
    {
      /* /etc/localtime and /etc/resolv.conf can not exist (or be symlinks to
       * non-existing targets), in which case we don't want to attempt to create
       * bogus symlinks or bind mounts, as that will cause flatpak run to fail.
       */
      if (g_file_test ("/etc/localtime", G_FILE_TEST_EXISTS))
        {
          char localtime[PATH_MAX + 1];
          ssize_t symlink_size;
          gboolean is_reachable = FALSE;

          symlink_size = readlink ("/etc/localtime", localtime, sizeof (localtime) - 1);
          if (symlink_size > 0)
            {
              g_autoptr(GFile) base_file = NULL;
              g_autoptr(GFile) target_file = NULL;
              g_autofree char *target_canonical = NULL;

              /* readlink() does not append a null byte to the buffer. */
              localtime[symlink_size] = 0;

              base_file = g_file_new_for_path ("/etc");
              target_file = g_file_resolve_relative_path (base_file, localtime);
              target_canonical = g_file_get_path (target_file);

              is_reachable = g_str_has_prefix (target_canonical, "/usr/");
            }

          if (is_reachable)
            {
              add_args (argv_array,
                        "--symlink", localtime, "/etc/localtime",
                        NULL);
            }
          else
            {
              add_args (argv_array,
                        "--ro-bind", "/etc/localtime", "/etc/localtime",
                        NULL);
            }
        }

      if (g_file_test ("/etc/resolv.conf", G_FILE_TEST_EXISTS))
        add_args (argv_array,
                  "--ro-bind", "/etc/resolv.conf", "/etc/resolv.conf",
                  NULL);
      if (g_file_test ("/etc/host.conf", G_FILE_TEST_EXISTS))
        add_args (argv_array,
                  "--ro-bind", "/etc/host.conf", "/etc/host.conf",
                  NULL);
      if (g_file_test ("/etc/hosts", G_FILE_TEST_EXISTS))
        add_args (argv_array,
                  "--ro-bind", "/etc/hosts", "/etc/hosts",
                  NULL);
    }
}

static void
add_document_portal_args (GPtrArray   *argv_array,
                          const char  *app_id,
                          char       **out_mount_path)
{
  g_autoptr(GDBusConnection) session_bus = NULL;
  g_autofree char *doc_mount_path = NULL;

  session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
  if (session_bus)
    {
      g_autoptr(GError) local_error = NULL;
      g_autoptr(GDBusMessage) reply = NULL;
      g_autoptr(GDBusMessage) msg =
        g_dbus_message_new_method_call ("org.freedesktop.portal.Documents",
                                        "/org/freedesktop/portal/documents",
                                        "org.freedesktop.portal.Documents",
                                        "GetMountPoint");
      g_dbus_message_set_body (msg, g_variant_new ("()"));
      reply =
        g_dbus_connection_send_message_with_reply_sync (session_bus, msg,
                                                        G_DBUS_SEND_MESSAGE_FLAGS_NONE,
                                                        30000,
                                                        NULL,
                                                        NULL,
                                                        NULL);
      if (reply)
        {
          if (g_dbus_message_to_gerror (reply, &local_error))
            {
              g_message ("Can't get document portal: %s", local_error->message);
            }
          else
            {
              g_autofree char *src_path = NULL;
              g_autofree char *dst_path = NULL;
              g_variant_get (g_dbus_message_get_body (reply),
                             "(^ay)", &doc_mount_path);

              src_path = g_strdup_printf ("%s/by-app/%s",
                                          doc_mount_path, app_id);
              dst_path = g_strdup_printf ("/run/user/%d/doc", getuid ());
              add_args (argv_array, "--bind", src_path, dst_path, NULL);
            }
        }
    }

  *out_mount_path = g_steal_pointer (&doc_mount_path);
}

static gchar *
join_args (GPtrArray *argv_array, gsize *len_out)
{
  gchar *string;
  gchar *ptr;
  gint i;
  gsize len = 0;

  for (i = 0; i < argv_array->len && argv_array->pdata[i] != NULL; i++)
    len +=  strlen (argv_array->pdata[i]) + 1;

  string = g_new (gchar, len);
  *string = 0;
  ptr = string;
  for (i = 0; i < argv_array->len && argv_array->pdata[i] != NULL; i++)
    ptr = g_stpcpy (ptr, argv_array->pdata[i]) + 1;

  *len_out = len;
  return string;
}

typedef struct {
  int sync_fd;
  int app_info_fd;
  int bwrap_args_fd;
} DbusProxySpawnData;

static void
dbus_spawn_child_setup (gpointer user_data)
{
  DbusProxySpawnData *data = user_data;

  /* Unset CLOEXEC */
  fcntl (data->sync_fd, F_SETFD, 0);
  fcntl (data->app_info_fd, F_SETFD, 0);
  fcntl (data->bwrap_args_fd, F_SETFD, 0);
}

/* This wraps the argv in a bwrap call, primary to allow the
   command to be run with a proper /.flatpak-info with data
   taken from app_info_fd */
static gboolean
prepend_bwrap_argv_wrapper (GPtrArray *argv,
                            int app_info_fd,
                            int *bwrap_fd_out,
                            GError **error)
{
  int i = 0;
  g_auto(GLnxDirFdIterator) dir_iter = { 0 };
  struct dirent *dent;
  g_autoptr(GPtrArray) bwrap_args = g_ptr_array_new_with_free_func (g_free);
  gsize bwrap_args_len;
  g_auto(GLnxTmpfile) args_tmpf  = { 0, };
  g_autofree char *bwrap_args_data = NULL;
  g_autofree char *proxy_socket_dir = g_build_filename (g_get_user_runtime_dir (), ".dbus-proxy/", NULL);

  if (!glnx_dirfd_iterator_init_at (AT_FDCWD, "/", FALSE, &dir_iter, error))
    return FALSE;

  while (TRUE)
    {
      if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&dir_iter, &dent, NULL, error))
        return FALSE;

      if (dent == NULL)
        break;

      if (strcmp (dent->d_name, ".flatpak-info") == 0)
        continue;

      if (dent->d_type == DT_DIR)
        {
          if (strcmp (dent->d_name, "tmp") == 0 ||
              strcmp (dent->d_name, "var") == 0 ||
              strcmp (dent->d_name, "run") == 0)
            g_ptr_array_add (bwrap_args, g_strdup ("--bind"));
          else
            g_ptr_array_add (bwrap_args, g_strdup ("--ro-bind"));
          g_ptr_array_add (bwrap_args, g_strconcat ("/", dent->d_name, NULL));
          g_ptr_array_add (bwrap_args, g_strconcat ("/", dent->d_name, NULL));
        }
      else if (dent->d_type == DT_LNK)
        {
          ssize_t symlink_size;
          char path_buffer[PATH_MAX + 1];

          symlink_size = readlinkat (dir_iter.fd, dent->d_name, path_buffer, sizeof (path_buffer) - 1);
          if (symlink_size < 0)
            {
              glnx_set_error_from_errno (error);
              return FALSE;
            }
          path_buffer[symlink_size] = 0;

          g_ptr_array_add (bwrap_args, g_strdup ("--symlink"));
          g_ptr_array_add (bwrap_args, g_strdup (path_buffer));
          g_ptr_array_add (bwrap_args, g_strconcat ("/", dent->d_name, NULL));
        }
    }

  g_ptr_array_add (bwrap_args, g_strdup ("--bind"));
  g_ptr_array_add (bwrap_args, g_strdup (proxy_socket_dir));
  g_ptr_array_add (bwrap_args, g_strdup (proxy_socket_dir));

  /* This is a file rather than a bind mount, because it will then
     not be unmounted from the namespace when the namespace dies. */
  g_ptr_array_add (bwrap_args, g_strdup ("--file"));
  g_ptr_array_add (bwrap_args, g_strdup_printf ("%d", app_info_fd));
  g_ptr_array_add (bwrap_args, g_strdup ("/.flatpak-info"));
  g_ptr_array_add (bwrap_args, NULL);

  {
    g_autofree char *commandline = flatpak_quote_argv ((const char **) bwrap_args->pdata);
    flatpak_debug2 ("bwrap args '%s'", commandline);
  }

  bwrap_args_data = join_args (bwrap_args, &bwrap_args_len);
  if (!buffer_to_sealed_memfd_or_tmpfile (&args_tmpf, "bwrap-args", bwrap_args_data, bwrap_args_len, error))
    return FALSE;

  g_ptr_array_insert (argv, i++, g_strdup (flatpak_get_bwrap ()));
  g_ptr_array_insert (argv, i++, g_strdup ("--args"));
  g_ptr_array_insert (argv, i++, g_strdup_printf ("%d", args_tmpf.fd));

  *bwrap_fd_out = glnx_steal_fd (&args_tmpf.fd);
  return TRUE;
}

static gboolean
has_args (GPtrArray *args)
{
  return args != NULL && args->len > 0;
}

static void
append_proxy_args (GPtrArray *dbus_proxy_argv,
                   GPtrArray *args,
                   gboolean   enable_logging)
{
  if (has_args (args))
    {
      int i;

      for (i = 0; i < args->len; i++)
        g_ptr_array_add (dbus_proxy_argv, g_strdup (args->pdata[i]));

      if (enable_logging)
        g_ptr_array_add (dbus_proxy_argv, g_strdup ("--log"));
    }
}

static gboolean
add_dbus_proxy_args (GPtrArray *argv_array,
                     GPtrArray *session_dbus_proxy_argv,
                     gboolean   enable_session_logging,
                     GPtrArray *system_dbus_proxy_argv,
                     gboolean   enable_system_logging,
                     GPtrArray *a11y_dbus_proxy_argv,
                     gboolean   enable_a11y_logging,
                     int        sync_fds[2],
                     const char *app_info_path,
                     GError   **error)
{
  char x = 'x';
  const char *proxy;
  g_autofree char *commandline = NULL;
  DbusProxySpawnData spawn_data;
  glnx_autofd int app_info_fd = -1;
  glnx_autofd int bwrap_args_fd = -1;
  g_autoptr(GPtrArray) dbus_proxy_argv = NULL;

  if (!has_args (session_dbus_proxy_argv) &&
      !has_args (system_dbus_proxy_argv) &&
      !has_args (a11y_dbus_proxy_argv))
    return TRUE;

  if (sync_fds[0] == -1)
    {
      if (pipe (sync_fds) < 0)
        {
          g_set_error_literal (error, G_IO_ERROR, g_io_error_from_errno (errno),
                               _("Unable to create sync pipe"));
          return FALSE;
        }

      add_args_data_fd (argv_array, NULL,
                        "--sync-fd", sync_fds[0], NULL);
    }

  proxy = g_getenv ("FLATPAK_DBUSPROXY");
  if (proxy == NULL)
    proxy = DBUSPROXY;

  dbus_proxy_argv = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (dbus_proxy_argv, g_strdup (proxy));
  g_ptr_array_add (dbus_proxy_argv, g_strdup_printf ("--fd=%d", sync_fds[1]));

  append_proxy_args (dbus_proxy_argv, session_dbus_proxy_argv, enable_session_logging);
  append_proxy_args (dbus_proxy_argv, system_dbus_proxy_argv, enable_system_logging);
  append_proxy_args (dbus_proxy_argv, a11y_dbus_proxy_argv, enable_a11y_logging);

  g_ptr_array_add (dbus_proxy_argv, NULL); /* NULL terminate */

  app_info_fd = open (app_info_path, O_RDONLY);
  if (app_info_fd == -1)
    {
      int errsv = errno;
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errsv),
                   _("Failed to open app info file: %s"), g_strerror (errsv));
      return FALSE;
    }

  if (!prepend_bwrap_argv_wrapper (dbus_proxy_argv, app_info_fd, &bwrap_args_fd, error))
    return FALSE;

  commandline = flatpak_quote_argv ((const char **) dbus_proxy_argv->pdata);
  flatpak_debug2 ("Running '%s'", commandline);

  spawn_data.sync_fd = sync_fds[1];
  spawn_data.app_info_fd = app_info_fd;
  spawn_data.bwrap_args_fd = bwrap_args_fd;
  if (!g_spawn_async (NULL,
                      (char **) dbus_proxy_argv->pdata,
                      NULL,
                      G_SPAWN_SEARCH_PATH,
                      dbus_spawn_child_setup,
                      &spawn_data,
                      NULL, error))
    {
      close (sync_fds[0]);
      close (sync_fds[1]);
      return FALSE;
    }

  /* Sync with proxy, i.e. wait until its listening on the sockets */
  if (read (sync_fds[0], &x, 1) != 1)
    {
      g_set_error_literal (error, G_IO_ERROR, g_io_error_from_errno (errno),
                           _("Failed to sync with dbus proxy"));

      close (sync_fds[0]);
      close (sync_fds[1]);
      return FALSE;
    }

  return TRUE;
}

#ifdef ENABLE_SECCOMP
static const uint32_t seccomp_x86_64_extra_arches[] = { SCMP_ARCH_X86, 0, };

#ifdef SCMP_ARCH_AARCH64
static const uint32_t seccomp_aarch64_extra_arches[] = { SCMP_ARCH_ARM, 0 };
#endif

static inline void
cleanup_seccomp (void *p)
{
  scmp_filter_ctx *pp = (scmp_filter_ctx *) p;

  if (*pp)
    seccomp_release (*pp);
}

static gboolean
setup_seccomp (GPtrArray  *argv_array,
               GArray     *fd_array,
               const char *arch,
               gulong      allowed_personality,
               gboolean    multiarch,
               gboolean    devel,
               GError    **error)
{
  __attribute__((cleanup (cleanup_seccomp))) scmp_filter_ctx seccomp = NULL;

  /**** BEGIN NOTE ON CODE SHARING
   *
   * There are today a number of different Linux container
   * implementations.  That will likely continue for long into the
   * future.  But we can still try to share code, and it's important
   * to do so because it affects what library and application writers
   * can do, and we should support code portability between different
   * container tools.
   *
   * This syscall blacklist is copied from linux-user-chroot, which was in turn
   * clearly influenced by the Sandstorm.io blacklist.
   *
   * If you make any changes here, I suggest sending the changes along
   * to other sandbox maintainers.  Using the libseccomp list is also
   * an appropriate venue:
   * https://groups.google.com/forum/#!topic/libseccomp
   *
   * A non-exhaustive list of links to container tooling that might
   * want to share this blacklist:
   *
   *  https://github.com/sandstorm-io/sandstorm
   *    in src/sandstorm/supervisor.c++
   *  http://cgit.freedesktop.org/xdg-app/xdg-app/
   *    in common/flatpak-run.c
   *  https://git.gnome.org/browse/linux-user-chroot
   *    in src/setup-seccomp.c
   *
   **** END NOTE ON CODE SHARING
   */
  struct
  {
    int                  scall;
    struct scmp_arg_cmp *arg;
  } syscall_blacklist[] = {
    /* Block dmesg */
    {SCMP_SYS (syslog)},
    /* Useless old syscall */
    {SCMP_SYS (uselib)},
    /* Don't allow you to switch to bsd emulation or whatnot */
    {SCMP_SYS (personality), &SCMP_A0(SCMP_CMP_NE, allowed_personality)},
    /* Don't allow disabling accounting */
    {SCMP_SYS (acct)},
    /* 16-bit code is unnecessary in the sandbox, and modify_ldt is a
       historic source of interesting information leaks. */
    {SCMP_SYS (modify_ldt)},
    /* Don't allow reading current quota use */
    {SCMP_SYS (quotactl)},

    /* Don't allow access to the kernel keyring */
    {SCMP_SYS (add_key)},
    {SCMP_SYS (keyctl)},
    {SCMP_SYS (request_key)},

    /* Scary VM/NUMA ops */
    {SCMP_SYS (move_pages)},
    {SCMP_SYS (mbind)},
    {SCMP_SYS (get_mempolicy)},
    {SCMP_SYS (set_mempolicy)},
    {SCMP_SYS (migrate_pages)},

    /* Don't allow subnamespace setups: */
    {SCMP_SYS (unshare)},
    {SCMP_SYS (mount)},
    {SCMP_SYS (pivot_root)},
    {SCMP_SYS (clone), &SCMP_A0 (SCMP_CMP_MASKED_EQ, CLONE_NEWUSER, CLONE_NEWUSER)},

    /* Don't allow faking input to the controlling tty (CVE-2017-5226) */
    {SCMP_SYS (ioctl), &SCMP_A1(SCMP_CMP_EQ, (int)TIOCSTI)},
  };

  struct
  {
    int                  scall;
    struct scmp_arg_cmp *arg;
  } syscall_nondevel_blacklist[] = {
    /* Profiling operations; we expect these to be done by tools from outside
     * the sandbox.  In particular perf has been the source of many CVEs.
     */
    {SCMP_SYS (perf_event_open)},
    {SCMP_SYS (ptrace)}
  };
  /* Blacklist all but unix, inet, inet6 and netlink */
  int socket_family_blacklist[] = {
    AF_AX25,
    AF_IPX,
    AF_APPLETALK,
    AF_NETROM,
    AF_BRIDGE,
    AF_ATMPVC,
    AF_X25,
    AF_ROSE,
    AF_DECnet,
    AF_NETBEUI,
    AF_SECURITY,
    AF_KEY,
    AF_NETLINK + 1, /* Last gets CMP_GE, so order is important */
  };
  int i, r;
  g_auto(GLnxTmpfile) seccomp_tmpf  = { 0, };

  seccomp = seccomp_init (SCMP_ACT_ALLOW);
  if (!seccomp)
    return flatpak_fail (error, "Initialize seccomp failed");

  if (arch != NULL)
    {
      uint32_t arch_id = 0;
      const uint32_t *extra_arches = NULL;

      if (strcmp (arch, "i386") == 0)
        {
          arch_id = SCMP_ARCH_X86;
        }
      else if (strcmp (arch, "x86_64") == 0)
        {
          arch_id = SCMP_ARCH_X86_64;
          extra_arches = seccomp_x86_64_extra_arches;
        }
      else if (strcmp (arch, "arm") == 0)
        {
          arch_id = SCMP_ARCH_ARM;
        }
#ifdef SCMP_ARCH_AARCH64
      else if (strcmp (arch, "aarch64") == 0)
        {
          arch_id = SCMP_ARCH_AARCH64;
          extra_arches = seccomp_aarch64_extra_arches;
        }
#endif

      /* We only really need to handle arches on multiarch systems.
       * If only one arch is supported the default is fine */
      if (arch_id != 0)
        {
          /* This *adds* the target arch, instead of replacing the
             native one. This is not ideal, because we'd like to only
             allow the target arch, but we can't really disallow the
             native arch at this point, because then bubblewrap
             couldn't continue running. */
          r = seccomp_arch_add (seccomp, arch_id);
          if (r < 0 && r != -EEXIST)
            return flatpak_fail (error, "Failed to add architecture to seccomp filter");

          if (multiarch && extra_arches != NULL)
            {
              unsigned i;
              for (i = 0; extra_arches[i] != 0; i++)
                {
                  r = seccomp_arch_add (seccomp, extra_arches[i]);
                  if (r < 0 && r != -EEXIST)
                    return flatpak_fail (error, "Failed to add multiarch architecture to seccomp filter");
                }
            }
        }
    }

  /* TODO: Should we filter the kernel keyring syscalls in some way?
   * We do want them to be used by desktop apps, but they could also perhaps
   * leak system stuff or secrets from other apps.
   */

  for (i = 0; i < G_N_ELEMENTS (syscall_blacklist); i++)
    {
      int scall = syscall_blacklist[i].scall;
      if (syscall_blacklist[i].arg)
        r = seccomp_rule_add (seccomp, SCMP_ACT_ERRNO (EPERM), scall, 1, *syscall_blacklist[i].arg);
      else
        r = seccomp_rule_add (seccomp, SCMP_ACT_ERRNO (EPERM), scall, 0);
      if (r < 0 && r == -EFAULT /* unknown syscall */)
        return flatpak_fail (error, "Failed to block syscall %d", scall);
    }

  if (!devel)
    {
      for (i = 0; i < G_N_ELEMENTS (syscall_nondevel_blacklist); i++)
        {
          int scall = syscall_nondevel_blacklist[i].scall;
          if (syscall_nondevel_blacklist[i].arg)
            r = seccomp_rule_add (seccomp, SCMP_ACT_ERRNO (EPERM), scall, 1, *syscall_nondevel_blacklist[i].arg);
          else
            r = seccomp_rule_add (seccomp, SCMP_ACT_ERRNO (EPERM), scall, 0);

          if (r < 0 && r == -EFAULT /* unknown syscall */)
            return flatpak_fail (error, "Failed to block syscall %d", scall);
        }
    }

  /* Socket filtering doesn't work on e.g. i386, so ignore failures here
   * However, we need to user seccomp_rule_add_exact to avoid libseccomp doing
   * something else: https://github.com/seccomp/libseccomp/issues/8 */
  for (i = 0; i < G_N_ELEMENTS (socket_family_blacklist); i++)
    {
      int family = socket_family_blacklist[i];
      if (i == G_N_ELEMENTS (socket_family_blacklist) - 1)
        seccomp_rule_add_exact (seccomp, SCMP_ACT_ERRNO (EAFNOSUPPORT), SCMP_SYS (socket), 1, SCMP_A0 (SCMP_CMP_GE, family));
      else
        seccomp_rule_add_exact (seccomp, SCMP_ACT_ERRNO (EAFNOSUPPORT), SCMP_SYS (socket), 1, SCMP_A0 (SCMP_CMP_EQ, family));
    }

  if (!glnx_open_anonymous_tmpfile (O_RDWR | O_CLOEXEC, &seccomp_tmpf, error))
    return FALSE;

  if (seccomp_export_bpf (seccomp, seccomp_tmpf.fd) != 0)
    return flatpak_fail (error, "Failed to export bpf");

  lseek (seccomp_tmpf.fd, 0, SEEK_SET);

  add_args_data_fd (argv_array, fd_array,
                    "--seccomp", glnx_steal_fd (&seccomp_tmpf.fd), NULL);

  return TRUE;
}
#endif

static void
flatpak_run_setup_usr_links (GPtrArray      *argv_array,
                             GFile          *runtime_files)
{
  const char *usr_links[] = {"lib", "lib32", "lib64", "bin", "sbin"};
  int i;

  if (runtime_files == NULL)
    return;

  for (i = 0; i < G_N_ELEMENTS (usr_links); i++)
    {
      const char *subdir = usr_links[i];
      g_autoptr(GFile) runtime_subdir = g_file_get_child (runtime_files, subdir);
      if (g_file_query_exists (runtime_subdir, NULL))
        {
          g_autofree char *link = g_strconcat ("usr/", subdir, NULL);
          g_autofree char *dest = g_strconcat ("/", subdir, NULL);
          add_args (argv_array,
                    "--symlink", link, dest,
                    NULL);
        }
    }
}

gboolean
flatpak_run_setup_base_argv (GPtrArray      *argv_array,
                             GArray         *fd_array,
                             GFile          *runtime_files,
                             GFile          *app_id_dir,
                             const char     *arch,
                             FlatpakRunFlags flags,
                             GError        **error)
{
  g_autofree char *run_dir = g_strdup_printf ("/run/user/%d", getuid ());
  g_autofree char *passwd_contents = NULL;
  g_autofree char *group_contents = NULL;
  struct group *g = getgrgid (getgid ());
  gulong pers;

  g_autoptr(GFile) etc = NULL;

  passwd_contents = g_strdup_printf ("%s:x:%d:%d:%s:%s:%s\n"
                                     "nfsnobody:x:65534:65534:Unmapped user:/:/sbin/nologin\n",
                                     g_get_user_name (),
                                     getuid (), getgid (),
                                     g_get_real_name (),
                                     g_get_home_dir (),
                                     DEFAULT_SHELL);

  group_contents = g_strdup_printf ("%s:x:%d:%s\n"
                                    "nfsnobody:x:65534:\n",
                                    g->gr_name,
                                    getgid (), g_get_user_name ());

  add_args (argv_array,
            "--unshare-pid",
            "--proc", "/proc",
            "--dir", "/tmp",
            "--dir", "/var/tmp",
            "--dir", "/run/host",
            "--dir", run_dir,
            "--setenv", "XDG_RUNTIME_DIR", run_dir,
            "--symlink", "../run", "/var/run",
            "--ro-bind", "/sys/block", "/sys/block",
            "--ro-bind", "/sys/bus", "/sys/bus",
            "--ro-bind", "/sys/class", "/sys/class",
            "--ro-bind", "/sys/dev", "/sys/dev",
            "--ro-bind", "/sys/devices", "/sys/devices",
            NULL);

  if (flags & FLATPAK_RUN_FLAG_DIE_WITH_PARENT)
    add_args (argv_array,
              "--die-with-parent",
              NULL);

  if (flags & FLATPAK_RUN_FLAG_WRITABLE_ETC)
    add_args (argv_array,
              "--dir", "/usr/etc",
              "--symlink", "usr/etc", "/etc",
              NULL);

  if (!add_args_data (argv_array, fd_array, "passwd", passwd_contents, -1, "/etc/passwd", error))
    return FALSE;

  if (!add_args_data (argv_array, fd_array, "group", group_contents, -1, "/etc/group", error))
    return FALSE;

  if (g_file_test ("/etc/machine-id", G_FILE_TEST_EXISTS))
    add_args (argv_array, "--ro-bind", "/etc/machine-id", "/etc/machine-id", NULL);
  else if (g_file_test ("/var/lib/dbus/machine-id", G_FILE_TEST_EXISTS))
    add_args (argv_array, "--ro-bind", "/var/lib/dbus/machine-id", "/etc/machine-id", NULL);

  if (runtime_files)
    etc = g_file_get_child (runtime_files, "etc");
  if (etc != NULL &&
      (flags & FLATPAK_RUN_FLAG_WRITABLE_ETC) == 0 &&
      g_file_query_exists (etc, NULL))
    {
      g_auto(GLnxDirFdIterator) dfd_iter = { 0, };
      struct dirent *dent;
      char path_buffer[PATH_MAX + 1];
      ssize_t symlink_size;
      gboolean inited;

      inited = glnx_dirfd_iterator_init_at (AT_FDCWD, flatpak_file_get_path_cached (etc), FALSE, &dfd_iter, NULL);

      while (inited)
        {
          g_autofree char *src = NULL;
          g_autofree char *dest = NULL;

          if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&dfd_iter, &dent, NULL, NULL) || dent == NULL)
            break;

          if (strcmp (dent->d_name, "passwd") == 0 ||
              strcmp (dent->d_name, "group") == 0 ||
              strcmp (dent->d_name, "machine-id") == 0 ||
              strcmp (dent->d_name, "resolv.conf") == 0 ||
              strcmp (dent->d_name, "host.conf") == 0 ||
              strcmp (dent->d_name, "hosts") == 0 ||
              strcmp (dent->d_name, "localtime") == 0)
            continue;

          src = g_build_filename (flatpak_file_get_path_cached (etc), dent->d_name, NULL);
          dest = g_build_filename ("/etc", dent->d_name, NULL);
          if (dent->d_type == DT_LNK)
            {
              symlink_size = readlinkat (dfd_iter.fd, dent->d_name, path_buffer, sizeof (path_buffer) - 1);
              if (symlink_size < 0)
                {
                  glnx_set_error_from_errno (error);
                  return FALSE;
                }
              path_buffer[symlink_size] = 0;
              add_args (argv_array, "--symlink", path_buffer, dest, NULL);
            }
          else
            {
              add_args (argv_array, "--bind", src, dest, NULL);
            }
        }
    }

  if (app_id_dir != NULL)
    {
      g_autoptr(GFile) app_cache_dir = g_file_get_child (app_id_dir, "cache");
      g_autoptr(GFile) app_tmp_dir = g_file_get_child (app_cache_dir, "tmp");
      g_autoptr(GFile) app_data_dir = g_file_get_child (app_id_dir, "data");
      g_autoptr(GFile) app_config_dir = g_file_get_child (app_id_dir, "config");

      add_args (argv_array,
                /* These are nice to have as a fixed path */
                "--bind", flatpak_file_get_path_cached (app_cache_dir), "/var/cache",
                "--bind", flatpak_file_get_path_cached (app_data_dir), "/var/data",
                "--bind", flatpak_file_get_path_cached (app_config_dir), "/var/config",
                "--bind", flatpak_file_get_path_cached (app_tmp_dir), "/var/tmp",
                NULL);
    }

  flatpak_run_setup_usr_links (argv_array, runtime_files);

  pers = PER_LINUX;

  if ((flags & FLATPAK_RUN_FLAG_SET_PERSONALITY) &&
      flatpak_is_linux32_arch (arch))
    {
      g_debug ("Setting personality linux32");
      pers = PER_LINUX32;
    }

  /* Always set the personallity, and clear all weird flags */
  personality (pers);

#ifdef ENABLE_SECCOMP
  if (!setup_seccomp (argv_array,
                      fd_array,
                      arch,
                      pers,
                      (flags & FLATPAK_RUN_FLAG_MULTIARCH) != 0,
                      (flags & FLATPAK_RUN_FLAG_DEVEL) != 0,
                      error))
    return FALSE;
#endif

  if ((flags & FLATPAK_RUN_FLAG_WRITABLE_ETC) == 0)
    add_monitor_path_args ((flags & FLATPAK_RUN_FLAG_NO_SESSION_HELPER) == 0, argv_array);

  return TRUE;
}

static void
clear_fd (gpointer data)
{
  int *fd_p = data;
  if (fd_p != NULL && *fd_p != -1)
    close (*fd_p);
}

/* Unset FD_CLOEXEC on the array of fds passed in @user_data */
static void
child_setup (gpointer user_data)
{
  GArray *fd_array = user_data;
  int i;

  /* If no fd_array was specified, don't care. */
  if (fd_array == NULL)
    return;

  /* Otherwise, mark not - close-on-exec all the fds in the array */
  for (i = 0; i < fd_array->len; i++)
    fcntl (g_array_index (fd_array, int, i), F_SETFD, 0);
}

static gboolean
forward_file (XdpDbusDocuments  *documents,
              const char        *app_id,
              const char        *file,
              char             **out_doc_id,
              GError           **error)
{
  int fd, fd_id;
  g_autofree char *doc_id = NULL;
  g_autoptr(GUnixFDList) fd_list = NULL;
  const char *perms[] = { "read", "write", NULL };

  fd = open (file, O_PATH | O_CLOEXEC);
  if (fd == -1)
    return flatpak_fail (error, "Failed to open '%s'", file);

  fd_list = g_unix_fd_list_new ();
  fd_id = g_unix_fd_list_append (fd_list, fd, error);
  close (fd);

  if (!xdp_dbus_documents_call_add_sync (documents,
                                         g_variant_new ("h", fd_id),
                                         TRUE, /* reuse */
                                         FALSE, /* not persistent */
                                         fd_list,
                                         &doc_id,
                                         NULL,
                                         NULL,
                                         error))
    return FALSE;

  if (!xdp_dbus_documents_call_grant_permissions_sync (documents,
                                                       doc_id,
                                                       app_id,
                                                       perms,
                                                       NULL,
                                                       error))
    return FALSE;

  *out_doc_id = g_steal_pointer (&doc_id);

  return TRUE;
}

static gboolean
add_rest_args (const char  *app_id,
               FlatpakExports *exports,
               gboolean     file_forwarding,
               const char  *doc_mount_path,
               GPtrArray   *argv_array,
               char        *args[],
               int          n_args,
               GError     **error)
{
  g_autoptr(XdpDbusDocuments) documents = NULL;
  gboolean forwarding = FALSE;
  gboolean forwarding_uri = FALSE;
  gboolean can_forward = TRUE;
  int i;

  if (file_forwarding && doc_mount_path == NULL)
    {
      g_message ("Can't get document portal mount path");
      can_forward = FALSE;
    }
  else if (file_forwarding)
    {
      g_autoptr(GError) local_error = NULL;

      documents = xdp_dbus_documents_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION, 0,
                                                             "org.freedesktop.portal.Documents",
                                                             "/org/freedesktop/portal/documents",
                                                             NULL,
                                                             &local_error);
      if (documents == NULL)
        {
          g_message ("Can't get document portal: %s", local_error->message);
          can_forward = FALSE;
        }
    }

  for (i = 0; i < n_args; i++)
    {
      g_autoptr(GFile) file = NULL;

      if (file_forwarding &&
          (strcmp (args[i], "@@") == 0 ||
           strcmp (args[i], "@@u") == 0))
        {
          forwarding_uri = strcmp (args[i], "@@u") == 0;
          forwarding = !forwarding;
          continue;
        }

      if (can_forward && forwarding)
        {
          if (forwarding_uri)
            {
              if (g_str_has_prefix (args[i], "file:"))
                file = g_file_new_for_uri (args[i]);
              else if (G_IS_DIR_SEPARATOR(args[i][0]))
                file = g_file_new_for_path (args[i]);
            }
          else
            file = g_file_new_for_path (args[i]);
        }

      if (file && !flatpak_exports_path_is_visible (exports,
                                                    flatpak_file_get_path_cached (file)))
        {
          g_autofree char *doc_id = NULL;
          g_autofree char *basename = NULL;
          char *doc_path;
          if (!forward_file (documents, app_id, flatpak_file_get_path_cached (file),
                             &doc_id, error))
            return FALSE;

          basename = g_file_get_basename (file);
          doc_path = g_build_filename (doc_mount_path, doc_id, basename, NULL);

          if (forwarding_uri)
            {
              g_autofree char *path = doc_path;
              doc_path = g_filename_to_uri (path, NULL, NULL);
              /* This should never fail */
              g_assert (doc_path != NULL);
            }

          g_debug ("Forwarding file '%s' as '%s' to %s", args[i], doc_path, app_id);
          g_ptr_array_add (argv_array, doc_path);
        }
      else
        g_ptr_array_add (argv_array, g_strdup (args[i]));
    }

  return TRUE;
}

FlatpakContext *
flatpak_context_load_for_app (const char     *app_id,
                              GError        **error)
{
  g_autofree char *app_ref = NULL;
  g_autoptr(FlatpakContext) app_context = NULL;
  g_autoptr(FlatpakDeploy) app_deploy = NULL;
  g_autoptr(FlatpakContext) overrides = NULL;
  g_autoptr(GKeyFile) metakey = NULL;

  app_ref = flatpak_find_current_ref (app_id, NULL, error);
  if (app_ref == NULL)
    return NULL;

  app_deploy = flatpak_find_deploy_for_ref (app_ref, NULL, error);
  if (app_deploy == NULL)
    return NULL;

  metakey = flatpak_deploy_get_metadata (app_deploy);
  app_context = flatpak_app_compute_permissions (metakey, NULL, error);
  if (app_context == NULL)
    return NULL;

  overrides = flatpak_deploy_get_overrides (app_deploy);
  flatpak_context_merge (app_context, overrides);

  return g_steal_pointer (&app_context);
}

static char *
calculate_ld_cache_checksum (GVariant *app_deploy_data,
                             GVariant *runtime_deploy_data,
                             const char *app_extensions,
                             const char *runtime_extensions)
{
  g_autoptr(GChecksum) ld_so_checksum = g_checksum_new (G_CHECKSUM_SHA256);
  if (app_deploy_data)
    g_checksum_update (ld_so_checksum, (guchar *)flatpak_deploy_data_get_commit (app_deploy_data), -1);
  g_checksum_update (ld_so_checksum, (guchar *)flatpak_deploy_data_get_commit (runtime_deploy_data), -1);
  if (app_extensions)
    g_checksum_update (ld_so_checksum, (guchar *)app_extensions, -1);
  if (runtime_extensions)
    g_checksum_update (ld_so_checksum, (guchar *)runtime_extensions, -1);

  return g_strdup (g_checksum_get_string (ld_so_checksum));
}

static gboolean
add_ld_so_conf (GPtrArray      *argv_array,
                GArray         *fd_array,
                GError        **error)
{
  const char *contents =
    "include /run/flatpak/ld.so.conf.d/app-*.conf\n"
    "include /app/etc/ld.so.conf\n"
    "/app/lib\n"
    "include /run/flatpak/ld.so.conf.d/runtime-*.conf\n";

  return add_args_data (argv_array, fd_array, "ld-so-conf",
                        contents, -1, "/etc/ld.so.conf", error);
}

static int
regenerate_ld_cache (GPtrArray      *base_argv_array,
                     GArray         *base_fd_array,
                     GFile          *app_id_dir,
                     const char     *checksum,
                     GFile          *runtime_files,
                     gboolean        generate_ld_so_conf,
                     GCancellable   *cancellable,
                     GError        **error)
{
  g_autoptr(GPtrArray) argv_array = NULL;
  g_autoptr(GArray) fd_array = NULL;
  g_autoptr(GArray) combined_fd_array = NULL;
  g_autoptr(GFile) ld_so_cache = NULL;
  g_autofree char *sandbox_cache_path = NULL;
  g_auto(GStrv) envp = NULL;
  g_autofree char *commandline = NULL;
  int exit_status;
  glnx_autofd int ld_so_fd = -1;
  g_autoptr(GFile) ld_so_dir = NULL;

  if (app_id_dir)
    ld_so_dir = g_file_get_child (app_id_dir, ".ld.so");
  else
    {
      g_autoptr(GFile) base_dir = g_file_new_for_path (g_get_user_cache_dir ());
      ld_so_dir = g_file_resolve_relative_path (base_dir, "flatpak/ld.so");
    }

  ld_so_cache = g_file_get_child (ld_so_dir, checksum);
  ld_so_fd = open (flatpak_file_get_path_cached (ld_so_cache), O_RDONLY);
  if (ld_so_fd >= 0)
    return glnx_steal_fd (&ld_so_fd);

  g_debug ("Regenerating ld.so.cache %s", flatpak_file_get_path_cached (ld_so_cache));

  if (!flatpak_mkdir_p (ld_so_dir, cancellable, error))
    return FALSE;

  argv_array = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (argv_array, g_strdup (flatpak_get_bwrap ()));
  append_args (argv_array, base_argv_array);

  fd_array = g_array_new (FALSE, TRUE, sizeof (int));
  g_array_set_clear_func (fd_array, clear_fd);

  envp = flatpak_run_get_minimal_env (FALSE, FALSE);

  flatpak_run_setup_usr_links (argv_array, runtime_files);

  if (generate_ld_so_conf)
    {
      if (!add_ld_so_conf(argv_array, fd_array, error))
        return -1;
    }
  else
    add_args (argv_array,
              "--symlink", "../usr/etc/ld.so.conf", "/etc/ld.so.conf",
              NULL);

  sandbox_cache_path = g_build_filename ("/run/ld-so-cache-dir", checksum, NULL);

  add_args (argv_array,
            "--unshare-pid",
            "--unshare-ipc",
            "--unshare-net",
            "--proc", "/proc",
            "--dev", "/dev",
            "--bind", flatpak_file_get_path_cached (ld_so_dir), "/run/ld-so-cache-dir",
            "ldconfig", "-X", "-C", sandbox_cache_path, NULL);

  g_ptr_array_add (argv_array, NULL);

  commandline = flatpak_quote_argv ((const char **) argv_array->pdata);
  flatpak_debug2 ("Running: '%s'", commandline);

  combined_fd_array = g_array_new (FALSE, TRUE, sizeof (int));
  g_array_append_vals (combined_fd_array, base_fd_array->data, base_fd_array->len);
  g_array_append_vals (combined_fd_array, fd_array->data, fd_array->len);

  if (!g_spawn_sync (NULL,
                     (char **) argv_array->pdata,
                     envp,
                     G_SPAWN_SEARCH_PATH,
                     child_setup, combined_fd_array,
                     NULL, NULL,
                     &exit_status,
                     error))
    return -1;

  if (!WIFEXITED(exit_status) || WEXITSTATUS(exit_status) != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   _("ldconfig failed, exit status %d"), exit_status);
      return -1;
    }

  ld_so_fd = open (flatpak_file_get_path_cached (ld_so_cache), O_RDONLY);
  if (ld_so_fd < 0)
    {
      flatpak_fail (error, "Can't open generated ld.so.cache");
      return -1;
    }

  if (app_id_dir == NULL)
    {
      /* For runs without an app id dir we always regenerate the ld.so.cache */
      unlink (flatpak_file_get_path_cached (ld_so_cache));
    }
  else
    {
      g_autoptr(GFile) active = g_file_get_child (ld_so_dir, "active");

      /* For app-dirs we keep one checksum alive, by pointing the active symlink to it */

      if (!flatpak_switch_symlink_and_remove (flatpak_file_get_path_cached (active),
                                              checksum, error))
        return -1;
    }

  return glnx_steal_fd (&ld_so_fd);
}

gboolean
flatpak_run_app (const char     *app_ref,
                 FlatpakDeploy  *app_deploy,
                 FlatpakContext *extra_context,
                 const char     *custom_runtime,
                 const char     *custom_runtime_version,
                 FlatpakRunFlags flags,
                 const char     *custom_command,
                 char           *args[],
                 int             n_args,
                 GCancellable   *cancellable,
                 GError        **error)
{
  g_autoptr(FlatpakDeploy) runtime_deploy = NULL;
  g_autoptr(GVariant) runtime_deploy_data = NULL;
  g_autoptr(GVariant) app_deploy_data = NULL;
  g_autoptr(GFile) app_files = NULL;
  g_autoptr(GFile) runtime_files = NULL;
  g_autoptr(GFile) app_id_dir = NULL;
  g_autofree char *default_runtime = NULL;
  g_autofree char *default_command = NULL;
  g_autofree char *runtime_ref = NULL;
  g_autoptr(GKeyFile) metakey = NULL;
  g_autoptr(GKeyFile) runtime_metakey = NULL;
  g_autoptr(GPtrArray) argv_array = NULL;
  g_auto(GLnxTmpfile) arg_tmpf = { 0, };
  g_autoptr(GArray) fd_array = NULL;
  g_autoptr(GPtrArray) real_argv_array = NULL;
  g_auto(GStrv) envp = NULL;
  const char *command = "/bin/sh";
  g_autoptr(GError) my_error = NULL;
  g_auto(GStrv) runtime_parts = NULL;
  int i;
  g_autofree char *app_info_path = NULL;
  g_autoptr(FlatpakContext) app_context = NULL;
  g_autoptr(FlatpakContext) overrides = NULL;
  g_autoptr(FlatpakExports) exports = NULL;
  g_auto(GStrv) app_ref_parts = NULL;
  g_autofree char *commandline = NULL;
  int commandline_2_start;
  g_autofree char *commandline2 = NULL;
  g_autofree char *doc_mount_path = NULL;
  g_autofree char *app_extensions = NULL;
  g_autofree char *runtime_extensions = NULL;
  g_autofree char *checksum = NULL;
  int ld_so_fd = -1;
  g_autoptr(GFile) runtime_ld_so_conf = NULL;
  gboolean generate_ld_so_conf = TRUE;
  gboolean use_ld_so_cache = TRUE;
  struct stat s;

  app_ref_parts = flatpak_decompose_ref (app_ref, error);
  if (app_ref_parts == NULL)
    return FALSE;

  argv_array = g_ptr_array_new_with_free_func (g_free);
  fd_array = g_array_new (FALSE, TRUE, sizeof (int));
  g_array_set_clear_func (fd_array, clear_fd);

  if (app_deploy == NULL)
    {
      g_assert (g_str_has_prefix (app_ref, "runtime/"));
      default_runtime = g_strdup (app_ref + strlen ("runtime/"));
    }
  else
    {
      const gchar *key;

      app_deploy_data = flatpak_deploy_get_deploy_data (app_deploy, cancellable, error);
      if (app_deploy_data == NULL)
        return FALSE;

      if ((flags & FLATPAK_RUN_FLAG_DEVEL) != 0)
        key = FLATPAK_METADATA_KEY_SDK;
      else
        key = FLATPAK_METADATA_KEY_RUNTIME;

      metakey = flatpak_deploy_get_metadata (app_deploy);
      default_runtime = g_key_file_get_string (metakey,
                                               FLATPAK_METADATA_GROUP_APPLICATION,
                                               key, &my_error);
      if (my_error)
        {
          g_propagate_error (error, g_steal_pointer (&my_error));
          return FALSE;
        }
    }

  runtime_parts = g_strsplit (default_runtime, "/", 0);
  if (g_strv_length (runtime_parts) != 3)
    return flatpak_fail (error, "Wrong number of components in runtime %s", default_runtime);

  if (custom_runtime)
    {
      g_auto(GStrv) custom_runtime_parts = g_strsplit (custom_runtime, "/", 0);

      for (i = 0; i < 3 && custom_runtime_parts[i] != NULL; i++)
        {
          if (strlen (custom_runtime_parts[i]) > 0)
            {
              g_free (runtime_parts[i]);
              runtime_parts[i] = g_steal_pointer (&custom_runtime_parts[i]);
            }
        }
    }

  if (custom_runtime_version)
    {
      g_free (runtime_parts[2]);
      runtime_parts[2] = g_strdup (custom_runtime_version);
    }

  runtime_ref = flatpak_compose_ref (FALSE,
                                     runtime_parts[0],
                                     runtime_parts[2],
                                     runtime_parts[1],
                                     error);
  if (runtime_ref == NULL)
    return FALSE;

  runtime_deploy = flatpak_find_deploy_for_ref (runtime_ref, cancellable, error);
  if (runtime_deploy == NULL)
    return FALSE;

  runtime_deploy_data = flatpak_deploy_get_deploy_data (runtime_deploy, cancellable, error);
  if (runtime_deploy_data == NULL)
    return FALSE;

  runtime_metakey = flatpak_deploy_get_metadata (runtime_deploy);

  app_context = flatpak_app_compute_permissions (metakey, runtime_metakey, error);
  if (app_context == NULL)
    return FALSE;

  if (app_deploy != NULL)
    {
      overrides = flatpak_deploy_get_overrides (app_deploy);
      flatpak_context_merge (app_context, overrides);
    }

  if (extra_context)
    flatpak_context_merge (app_context, extra_context);

  runtime_files = flatpak_deploy_get_files (runtime_deploy);
  if (app_deploy != NULL)
    {
      app_files = flatpak_deploy_get_files (app_deploy);
      if ((app_id_dir = flatpak_ensure_data_dir (app_ref_parts[1], cancellable, error)) == NULL)
        return FALSE;
    }

  envp = g_get_environ ();
  envp = flatpak_run_apply_env_default (envp, use_ld_so_cache);
  envp = flatpak_run_apply_env_vars (envp, app_context);

  add_args (argv_array,
            "--ro-bind", flatpak_file_get_path_cached (runtime_files), "/usr",
            "--lock-file", "/usr/.ref",
            NULL);

  if (app_files != NULL)
    add_args (argv_array,
              "--ro-bind", flatpak_file_get_path_cached (app_files), "/app",
              "--lock-file", "/app/.ref",
              NULL);
  else
    add_args (argv_array,
              "--dir", "/app",
              NULL);

  if (metakey != NULL &&
      !flatpak_run_add_extension_args (argv_array, fd_array, &envp, metakey, app_ref, use_ld_so_cache, &app_extensions, cancellable, error))
    return FALSE;

  if (!flatpak_run_add_extension_args (argv_array, fd_array, &envp, runtime_metakey, runtime_ref, use_ld_so_cache, &runtime_extensions, cancellable, error))
    return FALSE;

  runtime_ld_so_conf = g_file_resolve_relative_path (runtime_files, "etc/ld.so.conf");
  if (lstat (flatpak_file_get_path_cached (runtime_ld_so_conf), &s) == 0)
    generate_ld_so_conf = S_ISREG (s.st_mode) && s.st_size == 0;

  /* At this point we have the minimal argv set up, with just the app, runtime and extensions.
     We can reuse this to generate the ld.so.cache (if needed) */
  checksum = calculate_ld_cache_checksum (app_deploy_data, runtime_deploy_data,
                                                  app_extensions, runtime_extensions);
  ld_so_fd = regenerate_ld_cache (argv_array,
                                  fd_array,
                                  app_id_dir,
                                  checksum,
                                  runtime_files,
                                  generate_ld_so_conf,
                                  cancellable, error);
  if (ld_so_fd == -1)
    return FALSE;
  if (fd_array)
    g_array_append_val (fd_array, ld_so_fd);

  if (app_context->features & FLATPAK_CONTEXT_FEATURE_DEVEL)
    flags |= FLATPAK_RUN_FLAG_DEVEL;

  if (app_context->features & FLATPAK_CONTEXT_FEATURE_MULTIARCH)
    flags |= FLATPAK_RUN_FLAG_MULTIARCH;

  if (!flatpak_run_setup_base_argv (argv_array, fd_array, runtime_files, app_id_dir, app_ref_parts[2], flags, error))
    return FALSE;

  if (generate_ld_so_conf)
    {
      if (!add_ld_so_conf(argv_array, fd_array, error))
        return FALSE;
    }

  if (ld_so_fd != -1)
    {
      /* Don't add to fd_array, its already there */
      add_args_data_fd (argv_array, NULL, "--ro-bind-data", ld_so_fd, "/etc/ld.so.cache");
    }

  if (!flatpak_run_add_app_info_args (argv_array, fd_array,
                                      app_files, app_deploy_data, app_extensions,
                                      runtime_files, runtime_deploy_data, runtime_extensions,
                                      app_ref_parts[1], app_ref_parts[3],
                                      runtime_ref, app_context, &app_info_path, error))
    return FALSE;

  add_document_portal_args (argv_array, app_ref_parts[1], &doc_mount_path);

  if (!flatpak_run_add_environment_args (argv_array, fd_array, &envp,
                                         app_info_path, flags,
                                         app_ref_parts[1], app_context, app_id_dir, &exports, cancellable, error))
    return FALSE;

  flatpak_run_add_journal_args (argv_array);
  add_font_path_args (argv_array);
  add_icon_path_args (argv_array);

  add_args (argv_array,
            /* Not in base, because we don't want this for flatpak build */
            "--symlink", "/app/lib/debug/source", "/run/build",
            "--symlink", "/usr/lib/debug/source", "/run/build-runtime",
            NULL);

  if (custom_command)
    {
      command = custom_command;
    }
  else if (metakey)
    {
      default_command = g_key_file_get_string (metakey,
                                               FLATPAK_METADATA_GROUP_APPLICATION,
                                               FLATPAK_METADATA_KEY_COMMAND,
                                               &my_error);
      if (my_error)
        {
          g_propagate_error (error, g_steal_pointer (&my_error));
          return FALSE;
        }
      command = default_command;
    }

  real_argv_array = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (real_argv_array, g_strdup (flatpak_get_bwrap ()));

  {
    gsize len;
    g_autofree char *args = join_args (argv_array, &len);

    if (!buffer_to_sealed_memfd_or_tmpfile (&arg_tmpf, "bwrap-args", args, len, error))
      return FALSE;

    add_args_data_fd (real_argv_array, fd_array,
                      "--args", glnx_steal_fd (&arg_tmpf.fd), NULL);
  }

  commandline_2_start = real_argv_array->len;

  g_ptr_array_add (real_argv_array, g_strdup (command));
  if (!add_rest_args (app_ref_parts[1], exports, (flags & FLATPAK_RUN_FLAG_FILE_FORWARDING) != 0,
                      doc_mount_path,
                      real_argv_array, args, n_args, error))
    return FALSE;

  g_ptr_array_add (real_argv_array, NULL);
  g_ptr_array_add (argv_array, NULL);

  commandline = flatpak_quote_argv ((const char **) argv_array->pdata);
  commandline2 = flatpak_quote_argv (((const char **) real_argv_array->pdata) + commandline_2_start);
  flatpak_debug2 ("Running '%s %s'", commandline, commandline2);

  if ((flags & FLATPAK_RUN_FLAG_BACKGROUND) != 0)
    {
      if (!g_spawn_async (NULL,
                          (char **) real_argv_array->pdata,
                          envp,
                          G_SPAWN_SEARCH_PATH,
                          child_setup, fd_array,
                          NULL,
                          error))
        return FALSE;
    }
  else
    {
      /* Ensure we unset O_CLOEXEC */
      child_setup (fd_array);
      if (execvpe (flatpak_get_bwrap (), (char **) real_argv_array->pdata, envp) == -1)
        {
          g_set_error_literal (error, G_IO_ERROR, g_io_error_from_errno (errno),
                               _("Unable to start app"));
          return FALSE;
        }
      /* Not actually reached... */
    }

  return TRUE;
}
