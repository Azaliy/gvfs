#include <config.h>

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>

#include "gdaemonfile.h"
#include "gvfsdaemondbus.h"
#include <gvfsdaemonprotocol.h>
#include <gdaemonfileinputstream.h>
#include <gdaemonfileoutputstream.h>
#include <gdaemonfileenumerator.h>
#include <glib/gi18n-lib.h>
#include "gdbusutils.h"
#include "gmountoperationdbus.h"
#include <gio/gsimpleasyncresult.h>

static void g_daemon_file_file_iface_init (GFileIface       *iface);

struct _GDaemonFile
{
  GObject parent_instance;

  GMountSpec *mount_spec;
  char *path;
};

static void g_daemon_file_read_async (GFile *file,
				      int io_priority,
				      GCancellable *cancellable,
				      GAsyncReadyCallback callback,
				      gpointer callback_data);

G_DEFINE_TYPE_WITH_CODE (GDaemonFile, g_daemon_file, G_TYPE_OBJECT,
			 G_IMPLEMENT_INTERFACE (G_TYPE_FILE,
						g_daemon_file_file_iface_init))

static void
g_daemon_file_finalize (GObject *object)
{
  GDaemonFile *daemon_file;

  daemon_file = G_DAEMON_FILE (object);

  g_mount_spec_unref (daemon_file->mount_spec);
  g_free (daemon_file->path);
  
  if (G_OBJECT_CLASS (g_daemon_file_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_daemon_file_parent_class)->finalize) (object);
}

static void
g_daemon_file_class_init (GDaemonFileClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  
  gobject_class->finalize = g_daemon_file_finalize;
}

static void
g_daemon_file_init (GDaemonFile *daemon_file)
{
}

static char *
canonicalize_path (const char *path)
{
  char *canon, *start, *p, *q;

  if (*path != '/')
    canon = g_strconcat ("/", path, NULL);
  else
    canon = g_strdup (path);

  /* Skip initial slash */
  start = canon + 1;

  p = start;
  while (*p != 0)
    {
      if (p[0] == '.' && (p[1] == 0 || p[1] == '/'))
	{
	  memmove (p, p+1, strlen (p+1)+1);
	}
      else if (p[0] == '.' && p[1] == '.' && (p[2] == 0 || p[2] == '/'))
	{
	  q = p + 2;
	  /* Skip previous separator */
	  p = p - 2;
	  if (p < start)
	    p = start;
	  while (p > start && *p != '/')
	    p--;
	  if (*p == '/')
	    p++;
	  memmove (p, q, strlen (q)+1);
	}
      else
	{
	  /* Skip until next separator */
	  while (*p != 0 && *p != '/')
	    p++;

	  /* Keep one separator */
	  if (*p != 0)
	    p++;
	}

      /* Remove additional separators */
      q = p;
      while (*q && *q == '/')
	q++;

      if (p != q)
	memmove (p, q, strlen (q)+1);
    }

  /* Remove trailing slashes */
  if (p > start && *(p-1) == '/')
    *(p-1) = 0;
  
  return canon;
}

GFile *
g_daemon_file_new (GMountSpec *mount_spec,
		   const char *path)
{
  GDaemonFile *daemon_file;

  daemon_file = g_object_new (G_TYPE_DAEMON_FILE, NULL);
  /* TODO: These should be construct only properties */
  daemon_file->mount_spec = g_mount_spec_ref (mount_spec);
  daemon_file->path = canonicalize_path (path);
 
  return G_FILE (daemon_file);
}

static gboolean
g_daemon_file_is_native (GFile *file)
{
  return FALSE;
}

static char *
g_daemon_file_get_basename (GFile *file)
{
  GDaemonFile *daemon_file = G_DAEMON_FILE (file);
  char *last_slash;    

  /* This code relies on the path being canonicalized */
  
  last_slash = strrchr (daemon_file->path, '/');
  /* If no slash, or only "/" fallback to full path */
  if (last_slash == NULL ||
      last_slash[1] == '\0')
    return g_strdup (daemon_file->path);

  return g_strdup (last_slash + 1);
}

static char *
g_daemon_file_get_path (GFile *file)
{
  return NULL;
}

static char *
g_daemon_file_get_uri (GFile *file)
{
  GDaemonFile *daemon_file = G_DAEMON_FILE (file);
  GDecodedUri *uri;

  uri = _g_daemon_vfs_get_uri_for_mountspec (daemon_file->mount_spec,
					     daemon_file->path);

  if (uri == NULL)
    return NULL;

  return _g_encode_uri (uri, FALSE);
}

static char *
g_daemon_file_get_parse_name (GFile *file)
{
  GDaemonFile *daemon_file = G_DAEMON_FILE (file);
  GDecodedUri *uri;

  uri = _g_daemon_vfs_get_uri_for_mountspec (daemon_file->mount_spec,
					     daemon_file->path);

  if (uri == NULL)
    return NULL;

  return _g_encode_uri (uri, TRUE);
}

static GFile *
g_daemon_file_get_parent (GFile *file)
{
  GDaemonFile *daemon_file = G_DAEMON_FILE (file);
  const char *path;
  GDaemonFile *parent;
  const char *base;
  char *parent_path;
  gsize len;    

  path = daemon_file->path;
  base = strrchr (path, '/');
  if (base == NULL || base == path)
    return NULL;

  while (base > path && *base == '/')
    base--;

  len = (guint) 1 + base - path;
  
  parent_path = g_new (gchar, len + 1);
  g_memmove (parent_path, path, len);
  parent_path[len] = 0;

  parent = g_object_new (G_TYPE_DAEMON_FILE, NULL);
  parent->mount_spec = g_mount_spec_ref (daemon_file->mount_spec);
  parent->path = parent_path;
  
  return G_FILE (parent);
}

static GFile *
g_daemon_file_dup (GFile *file)
{
  GDaemonFile *daemon_file = G_DAEMON_FILE (file);

  return g_daemon_file_new (daemon_file->mount_spec,
			    daemon_file->path);
}

static guint
g_daemon_file_hash (GFile *file)
{
  GDaemonFile *daemon_file = G_DAEMON_FILE (file);

  return
    g_str_hash (daemon_file->path) ^
    g_mount_spec_hash (daemon_file->mount_spec);
}

static gboolean
g_daemon_file_equal (GFile *file1,
		     GFile *file2)
{
  GDaemonFile *daemon_file1 = G_DAEMON_FILE (file1);
  GDaemonFile *daemon_file2 = G_DAEMON_FILE (file2);

  return g_str_equal (daemon_file1->path, daemon_file2->path);
}

static GFile *
g_daemon_file_resolve_relative (GFile *file,
				const char *relative_path)
{
  GDaemonFile *daemon_file = G_DAEMON_FILE (file);
  char *path;
  GFile *child;

  if (*relative_path == '/')
    return g_daemon_file_new (daemon_file->mount_spec, relative_path);
  
  path = g_build_path ("/", daemon_file->path, relative_path, NULL);
  child = g_daemon_file_new (daemon_file->mount_spec, path);
  g_free (path);
  
  return child;
}

static DBusMessage *
create_empty_message (GFile *file,
		      const char *op)
{
  GDaemonFile *daemon_file = G_DAEMON_FILE (file);
  DBusMessage *message;
  GMountRef *mount_ref;
  const char *path;

  mount_ref = _g_daemon_vfs_get_mount_ref_sync (daemon_file->mount_spec,
						daemon_file->path,
						NULL);
  if (mount_ref == NULL)
    return NULL;
  
  message =
    dbus_message_new_method_call (mount_ref->dbus_id,
				  mount_ref->object_path,
				  G_VFS_DBUS_MOUNT_INTERFACE,
				  op);

  path = _g_mount_ref_resolve_path (mount_ref,
				    daemon_file->path);
  _g_dbus_message_append_args (message, G_DBUS_TYPE_CSTRING, &path, 0);

  _g_mount_ref_unref (mount_ref);
  return message;
}

static DBusMessage *
do_sync_path_call (GFile *file,
		   const char *op,
		   DBusConnection **connection_out,
		   GCancellable *cancellable,
		   GError **error,
		   int first_arg_type,
		   ...)
{
  DBusMessage *message, *reply;
  va_list var_args;

  message = create_empty_message (file, op);
  if (!message)
    return NULL;

  va_start (var_args, first_arg_type);
  _g_dbus_message_append_args_valist (message,
				      first_arg_type,
				      var_args);
  va_end (var_args);

  reply = _g_vfs_daemon_call_sync (message,
				   connection_out,
				   NULL, NULL, NULL,
				   cancellable, error);
  dbus_message_unref (message);

  return reply;
}

static DBusMessage *
do_sync_2_path_call (GFile *file1,
		     GFile *file2,
		     const char *op,
		     const char *callback_obj_path,
		     DBusObjectPathMessageFunction callback,
		     gpointer callback_user_data, 
		     DBusConnection **connection_out,
		     GCancellable *cancellable,
		     GError **error,
		     int first_arg_type,
		     ...)
{
  GDaemonFile *daemon_file1 = G_DAEMON_FILE (file1);
  GDaemonFile *daemon_file2 = G_DAEMON_FILE (file2);
  DBusMessage *message, *reply;
  GMountRef *mount_ref1, *mount_ref2;
  const char *path1, *path2;
  va_list var_args;

  mount_ref1 = _g_daemon_vfs_get_mount_ref_sync (daemon_file1->mount_spec,
						 daemon_file1->path,
						 error);
  if (mount_ref1 == NULL)
    return NULL;

  mount_ref2 = _g_daemon_vfs_get_mount_ref_sync (daemon_file2->mount_spec,
						 daemon_file2->path,
						 error);
  if (mount_ref2 == NULL)
    return NULL;

  if (mount_ref1 != mount_ref2)
    {
      /* For copy this will cause the fallback code to be involved */
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
		   _("Operation not supported, files on different mounts"));
      return FALSE;
    }
  
  message =
    dbus_message_new_method_call (mount_ref1->dbus_id,
				  mount_ref1->object_path,
				  G_VFS_DBUS_MOUNT_INTERFACE,
				  op);

  path1 = _g_mount_ref_resolve_path (mount_ref1,
				     daemon_file1->path);
  _g_dbus_message_append_args (message, G_DBUS_TYPE_CSTRING, &path1, 0);

  path2 = _g_mount_ref_resolve_path (mount_ref1,
				     daemon_file2->path);
  _g_dbus_message_append_args (message, G_DBUS_TYPE_CSTRING, &path2, 0);

  va_start (var_args, first_arg_type);
  _g_dbus_message_append_args_valist (message,
				      first_arg_type,
				      var_args);
  va_end (var_args);

  reply = _g_vfs_daemon_call_sync (message,
				   connection_out,
				   callback_obj_path,
				   callback,
				   callback_user_data, 
				   cancellable, error);
  dbus_message_unref (message);

  _g_mount_ref_unref (mount_ref1);
  _g_mount_ref_unref (mount_ref2);
  
  return reply;
}

typedef void (*AsyncPathCallCallback) (DBusMessage *reply,
				       DBusConnection *connection,
				       GSimpleAsyncResult *result,
				       GCancellable *cancellable,
				       gpointer callback_data);


typedef struct {
  GSimpleAsyncResult *result;
  GFile *file;
  char *op;
  GCancellable *cancellable;
  DBusMessage *args;
  AsyncPathCallCallback callback;
  gpointer callback_data;
  GDestroyNotify notify;
} AsyncPathCall;

static void
async_path_call_free (AsyncPathCall *data)
{
  if (data->notify)
    data->notify (data->callback_data);

  if (data->result)
    g_object_unref (data->result);
  g_object_unref (data->file);
  g_free (data->op);
  if (data->cancellable)
    g_object_unref (data->cancellable);
  if (data->args)
    dbus_message_unref (data->args);
  g_free (data);
}

static void
async_path_call_done (DBusMessage *reply,
		      DBusConnection *connection,
		      GError *io_error,
		      gpointer _data)
{
  AsyncPathCall *data = _data;
  GSimpleAsyncResult *result;

  if (io_error != NULL)
    {
      g_simple_async_result_set_from_error (data->result, io_error);
      g_simple_async_result_complete (data->result);
      async_path_call_free (data);
    }
  else
    {
      result = data->result;
      g_object_weak_ref (G_OBJECT (result), (GWeakNotify)async_path_call_free, data);
      data->result = NULL;
      
      data->callback (reply, connection,
		      result,
		      data->cancellable,
		      data->callback_data);

      /* Free data here, or later if callback ref:ed the result */
      g_object_unref (result);
    }
}

static void
do_async_path_call_callback (GMountRef *mount_ref,
			     gpointer _data,
			     GError *error)
{
  AsyncPathCall *data = _data;
  GDaemonFile *daemon_file = G_DAEMON_FILE (data->file);
  const char *path;
  DBusMessage *message;
  DBusMessageIter arg_source, arg_dest;
  
  if (error != NULL)
    {
      g_simple_async_result_set_from_error (data->result, error);      
      g_simple_async_result_complete (data->result);
      async_path_call_free (data);
      return;
    }

  message =
    dbus_message_new_method_call (mount_ref->dbus_id,
				  mount_ref->object_path,
				  G_VFS_DBUS_MOUNT_INTERFACE,
				  data->op);
  
  path = _g_mount_ref_resolve_path (mount_ref, daemon_file->path);
  _g_dbus_message_append_args (message, G_DBUS_TYPE_CSTRING, &path, 0);

  /* Append more args from data->args */

  if (data->args)
    {
      dbus_message_iter_init (data->args, &arg_source);
      dbus_message_iter_init_append (message, &arg_dest);

      _g_dbus_message_iter_copy (&arg_dest, &arg_source);
    }

  _g_vfs_daemon_call_async (message,
			    async_path_call_done, data,
			    data->cancellable);
  
  dbus_message_unref (message);
}

static void
do_async_path_call (GFile *file,
		    const char *op,
		    GCancellable *cancellable,
		    GAsyncReadyCallback op_callback,
		    gpointer op_callback_data,
		    AsyncPathCallCallback callback,
		    gpointer callback_data,
		    GDestroyNotify notify,
		    int first_arg_type,
		    ...)
{
  GDaemonFile *daemon_file = G_DAEMON_FILE (file);
  va_list var_args;
  GError *error;
  AsyncPathCall *data;

  data = g_new0 (AsyncPathCall, 1);

  data->result = g_simple_async_result_new (G_OBJECT (file),
					    op_callback, op_callback_data,
					    NULL);

  data->file = g_object_ref (file);
  data->op = g_strdup (op);
  if (data->cancellable)
    data->cancellable = g_object_ref (cancellable);
  data->callback = callback;
  data->callback_data = callback_data;
  data->notify = notify;
  
  error = NULL;

  if (first_arg_type != 0)
    {
      data->args = dbus_message_new (DBUS_MESSAGE_TYPE_METHOD_CALL);
      if (data->args == NULL)
	_g_dbus_oom ();
      
      va_start (var_args, first_arg_type);
      _g_dbus_message_append_args_valist (data->args,
					  first_arg_type,
					  var_args);
      va_end (var_args);
    }
  
  
  _g_daemon_vfs_get_mount_ref_async (daemon_file->mount_spec,
				     daemon_file->path,
				     do_async_path_call_callback,
				     data);
}


static GFileEnumerator *
g_daemon_file_enumerate_children (GFile      *file,
				  const char *attributes,
				  GFileGetInfoFlags flags,
				  GCancellable *cancellable,
				  GError **error)
{
  DBusMessage *reply;
  dbus_uint32_t flags_dbus;
  char *obj_path;
  GDaemonFileEnumerator *enumerator;
  DBusConnection *connection;

  enumerator = g_daemon_file_enumerator_new ();
  obj_path = g_daemon_file_enumerator_get_object_path (enumerator);
						       
  if (attributes == NULL)
    attributes = "";
  flags_dbus = flags;
  reply = do_sync_path_call (file, 
			     G_VFS_DBUS_MOUNT_OP_ENUMERATE,
			     &connection, cancellable, error,
			     DBUS_TYPE_STRING, &obj_path,
			     DBUS_TYPE_STRING, &attributes,
			     DBUS_TYPE_UINT32, &flags_dbus,
			     0);
  
  g_free (obj_path);

  if (reply == NULL)
    goto error;

  dbus_message_unref (reply);

  g_daemon_file_enumerator_set_sync_connection (enumerator, connection);
  
  return G_FILE_ENUMERATOR (enumerator);

 error:
  if (reply)
    dbus_message_unref (reply);
  g_object_unref (enumerator);
  return NULL;
}

static GFileInfo *
g_daemon_file_get_info (GFile                *file,
			const char           *attributes,
			GFileGetInfoFlags     flags,
			GCancellable         *cancellable,
			GError              **error)
{
  DBusMessage *reply;
  dbus_uint32_t flags_dbus;
  DBusMessageIter iter;
  GFileInfo *info;

  if (attributes == NULL)
    attributes = "";
  flags_dbus = flags;
  reply = do_sync_path_call (file, 
			     G_VFS_DBUS_MOUNT_OP_GET_INFO,
			     NULL, cancellable, error,
			     DBUS_TYPE_STRING, &attributes,
			     DBUS_TYPE_UINT32, &flags,
			     0);
  if (reply == NULL)
    return NULL;

  info = NULL;
  
  if (!dbus_message_iter_init (reply, &iter) ||
      (dbus_message_iter_get_arg_type (&iter) != DBUS_TYPE_STRUCT))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		   _("Invalid return value from get_info"));
      goto out;
    }

  info = _g_dbus_get_file_info (&iter, error);

 out:
  dbus_message_unref (reply);
  return info;
}

typedef struct {
  GSimpleAsyncResult *result;
  gboolean can_seek;
} GetFDData;

static void
read_async_get_fd_cb (int fd,
		      gpointer callback_data)
{
  GetFDData *data = callback_data;
  GFileInputStream *stream;
  
  if (fd == -1)
    {
      g_simple_async_result_set_error (data->result,
				       G_IO_ERROR, G_IO_ERROR_FAILED,
				       _("Couldn't get stream file descriptor"));
    }
  else
    {
      stream = g_daemon_file_input_stream_new (fd, data->can_seek);
      g_simple_async_result_set_op_res_gpointer (data->result, stream, g_object_unref);
    }

  g_simple_async_result_complete (data->result);

  g_object_unref (data->result);
  g_free (data);
}

static void
read_async_cb (DBusMessage *reply,
	       DBusConnection *connection,
	       GSimpleAsyncResult *result,
	       GCancellable *cancellable,
	       gpointer callback_data)
{
  guint32 fd_id;
  dbus_bool_t can_seek;
  GetFDData *get_fd_data;
  
  if (!dbus_message_get_args (reply, NULL,
			      DBUS_TYPE_UINT32, &fd_id,
			      DBUS_TYPE_BOOLEAN, &can_seek,
			      DBUS_TYPE_INVALID))
    {
      g_simple_async_result_set_error (result,
				       G_IO_ERROR, G_IO_ERROR_FAILED,
				       _("Invalid return value from open"));
      g_simple_async_result_complete (result);
      return;
    }
  
  get_fd_data = g_new0 (GetFDData, 1);
  get_fd_data->result = g_object_ref (result);
  get_fd_data->can_seek = can_seek;
  
  _g_dbus_connection_get_fd_async (connection, fd_id,
				   read_async_get_fd_cb, get_fd_data);
}

static void
g_daemon_file_read_async (GFile *file,
			  int io_priority,
			  GCancellable *cancellable,
			  GAsyncReadyCallback callback,
			  gpointer callback_data)
{
  do_async_path_call (file,
		      G_VFS_DBUS_MOUNT_OP_OPEN_FOR_READ,
		      cancellable,
		      callback, callback_data,
		      read_async_cb, NULL, NULL,
		      0);
}

static GFileInputStream *
g_daemon_file_read_finish (GFile                  *file,
			   GAsyncResult           *res,
			   GError                **error)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (res);
  gpointer op;

  op = g_simple_async_result_get_op_res_gpointer (simple);
  if (op)
    return g_object_ref (op);
  
  return NULL;
}


static GFileInputStream *
g_daemon_file_read (GFile *file,
		    GCancellable *cancellable,
		    GError **error)
{
  DBusConnection *connection;
  int fd;
  DBusMessage *reply;
  guint32 fd_id;
  dbus_bool_t can_seek;

  reply = do_sync_path_call (file, 
			     G_VFS_DBUS_MOUNT_OP_OPEN_FOR_READ,
			     &connection, cancellable, error,
			     0);
  if (reply == NULL)
    return NULL;

  if (!dbus_message_get_args (reply, NULL,
			      DBUS_TYPE_UINT32, &fd_id,
			      DBUS_TYPE_BOOLEAN, &can_seek,
			      DBUS_TYPE_INVALID))
    {
      dbus_message_unref (reply);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		   _("Invalid return value from open"));
      return NULL;
    }
  
  dbus_message_unref (reply);

  fd = _g_dbus_connection_get_fd_sync (connection, fd_id);
  if (fd == -1)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		   _("Didn't get stream file descriptor"));
      return NULL;
    }
  
  return g_daemon_file_input_stream_new (fd, can_seek);
}

static GFileOutputStream *
g_daemon_file_append_to (GFile *file,
			 GCancellable *cancellable,
			 GError **error)
{
  DBusConnection *connection;
  int fd;
  DBusMessage *reply;
  guint32 fd_id;
  dbus_bool_t can_seek;
  guint16 mode;
  guint64 mtime, initial_offset;
  dbus_bool_t make_backup;

  mode = 1;
  mtime = 0;
  make_backup = FALSE;
  
  reply = do_sync_path_call (file, 
			     G_VFS_DBUS_MOUNT_OP_OPEN_FOR_WRITE,
			     &connection, cancellable, error,
			     DBUS_TYPE_UINT16, &mode,
			     DBUS_TYPE_UINT64, &mtime,
			     DBUS_TYPE_BOOLEAN, &make_backup,
			     0);
  if (reply == NULL)
    return NULL;

  if (!dbus_message_get_args (reply, NULL,
			      DBUS_TYPE_UINT32, &fd_id,
			      DBUS_TYPE_BOOLEAN, &can_seek,
			      DBUS_TYPE_UINT64, &initial_offset,
			      DBUS_TYPE_INVALID))
    {
      dbus_message_unref (reply);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		   _("Invalid return value from open"));
      return NULL;
    }
  
  dbus_message_unref (reply);

  fd = _g_dbus_connection_get_fd_sync (connection, fd_id);
  if (fd == -1)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		   _("Didn't get stream file descriptor"));
      return NULL;
    }
  
  return g_daemon_file_output_stream_new (fd, can_seek, initial_offset);
}

static GFileOutputStream *
g_daemon_file_create (GFile *file,
		      GCancellable *cancellable,
		      GError **error)
{
  DBusConnection *connection;
  int fd;
  DBusMessage *reply;
  guint32 fd_id;
  dbus_bool_t can_seek;
  guint16 mode;
  guint64 mtime, initial_offset;
  dbus_bool_t make_backup;

  mode = 0;
  mtime = 0;
  make_backup = FALSE;
  
  reply = do_sync_path_call (file, 
			     G_VFS_DBUS_MOUNT_OP_OPEN_FOR_WRITE,
			     &connection, cancellable, error,
			     DBUS_TYPE_UINT16, &mode,
			     DBUS_TYPE_UINT64, &mtime,
			     DBUS_TYPE_BOOLEAN, &make_backup,
			     0);
  if (reply == NULL)
    return NULL;

  if (!dbus_message_get_args (reply, NULL,
			      DBUS_TYPE_UINT32, &fd_id,
			      DBUS_TYPE_BOOLEAN, &can_seek,
			      DBUS_TYPE_UINT64, &initial_offset,
			      DBUS_TYPE_INVALID))
    {
      dbus_message_unref (reply);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		   _("Invalid return value from open"));
      return NULL;
    }
  
  dbus_message_unref (reply);

  fd = _g_dbus_connection_get_fd_sync (connection, fd_id);
  if (fd == -1)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		   _("Didn't get stream file descriptor"));
      return NULL;
    }
  
  return g_daemon_file_output_stream_new (fd, can_seek, initial_offset);
}

static GFileOutputStream *
g_daemon_file_replace (GFile *file,
		       time_t mtime,
		       gboolean make_backup,
		       GCancellable *cancellable,
		       GError **error)
{
  DBusConnection *connection;
  int fd;
  DBusMessage *reply;
  guint32 fd_id;
  dbus_bool_t can_seek;
  guint16 mode;
  guint64 dbus_mtime, initial_offset;
  dbus_bool_t dbus_make_backup;

  mode = 2;
  dbus_mtime = mtime;
  dbus_make_backup = make_backup;
  
  reply = do_sync_path_call (file, 
			     G_VFS_DBUS_MOUNT_OP_OPEN_FOR_WRITE,
			     &connection, cancellable, error,
			     DBUS_TYPE_UINT16, &mode,
			     DBUS_TYPE_UINT64, &dbus_mtime,
			     DBUS_TYPE_BOOLEAN, &dbus_make_backup,
			     0);
  if (reply == NULL)
    return NULL;

  if (!dbus_message_get_args (reply, NULL,
			      DBUS_TYPE_UINT32, &fd_id,
			      DBUS_TYPE_BOOLEAN, &can_seek,
			      DBUS_TYPE_UINT64, &initial_offset,
			      DBUS_TYPE_INVALID))
    {
      dbus_message_unref (reply);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		   _("Invalid return value from open"));
      return NULL;
    }
  
  dbus_message_unref (reply);

  fd = _g_dbus_connection_get_fd_sync (connection, fd_id);
  if (fd == -1)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		   _("Didn't get stream file descriptor"));
      return NULL;
    }
  
  return g_daemon_file_output_stream_new (fd, can_seek, initial_offset);
}

static void
mount_mountable_location_mounted_cb (GObject *source_object,
				     GAsyncResult *res,
				     gpointer user_data)
{
  GSimpleAsyncResult *result = user_data;
  GError *error = NULL;
  
  if (!g_mount_for_location_finish (G_FILE (source_object), res, &error))
    {
      g_simple_async_result_set_from_error (result, error);
      g_error_free (error);
    }

  g_simple_async_result_complete (result);
  g_object_unref (result);

}

static void
mount_mountable_async_cb (DBusMessage *reply,
			  DBusConnection *connection,
			  GSimpleAsyncResult *result,
			  GCancellable *cancellable,
			  gpointer callback_data)
{
  GMountOperation *mount_operation = callback_data;
  GMountSpec *mount_spec;
  char *path;
  DBusMessageIter iter;
  GFile *file;
  dbus_bool_t must_mount_location;

  path = NULL;
  
  dbus_message_iter_init (reply, &iter);
  mount_spec = g_mount_spec_from_dbus (&iter);
  
  if (mount_spec == NULL ||
      !_g_dbus_message_iter_get_args (&iter, NULL,
				      G_DBUS_TYPE_CSTRING, &path,
				      DBUS_TYPE_BOOLEAN, &must_mount_location,
				      0))
    {
      g_simple_async_result_set_error (result,
				       G_IO_ERROR, G_IO_ERROR_FAILED,
				       _("Invalid return value from call"));
      g_simple_async_result_complete (result);
    }
  else
    {
      file = g_daemon_file_new (mount_spec, path);
      g_mount_spec_unref (mount_spec);
      g_free (path);
      g_simple_async_result_set_op_res_gpointer (result, file, g_object_unref);

      if (must_mount_location)
	{
	  g_mount_for_location (file,
				mount_operation,
				cancellable,
				mount_mountable_location_mounted_cb,
				g_object_ref (result));
	  
	}
      else
	g_simple_async_result_complete (result);
    }
}

static void
g_daemon_file_mount_mountable (GFile               *file,
			       GMountOperation     *mount_operation,
			       GCancellable        *cancellable,
			       GAsyncReadyCallback  callback,
			       gpointer             user_data)
{
  GMountSource *mount_source;
  const char *dbus_id, *obj_path;
  
  mount_source = g_mount_operation_dbus_wrap (mount_operation);
  
  dbus_id = g_mount_source_get_dbus_id (mount_source);
  obj_path = g_mount_source_get_obj_path (mount_source);

  if (mount_operation)
    g_object_ref (mount_operation);
  
  do_async_path_call (file,
		      G_VFS_DBUS_MOUNT_OP_MOUNT_MOUNTABLE,
		      cancellable,
		      callback, user_data,
		      mount_mountable_async_cb,
		      mount_operation, mount_operation ? g_object_unref : NULL,
		      DBUS_TYPE_STRING, &dbus_id,
		      DBUS_TYPE_OBJECT_PATH, &obj_path,
		      0);

  g_object_unref (mount_source);
}

static GFile *
g_daemon_file_mount_mountable_finish (GFile               *file,
				      GAsyncResult        *result,
				      GError             **error)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (result);
  GFile *result_file;
  
  result_file = g_simple_async_result_get_op_res_gpointer (simple);
  if (result_file)
    return g_object_ref (result_file);
  
  return NULL;
}

typedef struct {
  GFile *file;
  GMountOperation *mount_operation;
  GAsyncReadyCallback callback;
  gpointer user_data;
} MountData;

static void g_daemon_file_mount_for_location (GFile *location,
					      GMountOperation *mount_operation,
					      GCancellable *cancellable,
					      GAsyncReadyCallback callback,
					      gpointer user_data);

static void
mount_reply (DBusMessage *reply,
	     GError *error,
	     gpointer user_data)
{
  MountData *data = user_data;
  GSimpleAsyncResult *res;

  if (reply == NULL)
    {
      res = g_simple_async_result_new_from_error (G_OBJECT (data->file),
						  data->callback,
						  data->user_data,
						  error);
    }
  else
    {
      res = g_simple_async_result_new (G_OBJECT (data->file),
				       data->callback,
				       data->user_data,
				       g_daemon_file_mount_for_location);
    }

  g_simple_async_result_complete (res);
  
  g_object_unref (data->file);
  if (data->mount_operation)
    g_object_unref (data->mount_operation);
  g_free (data);
}

static void
g_daemon_file_mount_for_location (GFile *location,
				  GMountOperation *mount_operation,
				  GCancellable *cancellable,
				  GAsyncReadyCallback callback,
				  gpointer user_data)
{
  GDaemonFile *daemon_file;
  DBusMessage *message;
  GMountSpec *spec;
  GMountSource *mount_source;
  DBusMessageIter iter;
  MountData *data;
  
  daemon_file = G_DAEMON_FILE (location);
  
  message = dbus_message_new_method_call (G_VFS_DBUS_DAEMON_NAME,
					  G_VFS_DBUS_MOUNTTRACKER_PATH,
					  G_VFS_DBUS_MOUNTTRACKER_INTERFACE,
					  G_VFS_DBUS_MOUNTTRACKER_OP_MOUNT_LOCATION);

  spec = g_mount_spec_copy (daemon_file->mount_spec);
  g_mount_spec_set_mount_prefix (spec, daemon_file->path);
  dbus_message_iter_init_append (message, &iter);
  g_mount_spec_to_dbus (&iter, spec);
  g_mount_spec_unref (spec);

  mount_source = g_mount_operation_dbus_wrap (mount_operation);
  g_mount_source_to_dbus (mount_source, message);
  g_object_unref (mount_source);

  data = g_new0 (MountData, 1);
  data->callback = callback;
  data->user_data = user_data;
  data->file = g_object_ref (location);
  if (mount_operation)
    data->mount_operation = g_object_ref (mount_operation);

  /* TODO: Ignoring cancellable here */
  _g_dbus_connection_call_async (NULL, message,
				 G_VFS_DBUS_MOUNT_TIMEOUT_MSECS,
				 mount_reply, data);
 
  dbus_message_unref (message);
}

static gboolean
g_daemon_file_mount_for_location_finish (GFile                  *location,
					 GAsyncResult           *result,
					 GError                **error)
{
  /* Errors handled in generic code */
  return TRUE;
}

static GFileInfo *
g_daemon_file_get_filesystem_info (GFile                *file,
				   const char           *attributes,
				   GCancellable         *cancellable,
				   GError              **error)
{
  DBusMessage *reply;
  DBusMessageIter iter;
  GFileInfo *info;

  if (attributes == NULL)
    attributes = "";
  reply = do_sync_path_call (file, 
			     G_VFS_DBUS_MOUNT_OP_GET_FILESYSTEM_INFO,
			     NULL, cancellable, error,
			     DBUS_TYPE_STRING, &attributes,
			     0);
  if (reply == NULL)
    return NULL;

  info = NULL;
  
  if (!dbus_message_iter_init (reply, &iter) ||
      (dbus_message_iter_get_arg_type (&iter) != DBUS_TYPE_STRUCT))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		   _("Invalid return value from get_filesystem_info"));
      goto out;
    }

  info = _g_dbus_get_file_info (&iter, error);

 out:
  dbus_message_unref (reply);
  return info;
}

static GFile *
g_daemon_file_set_display_name (GFile *file,
				const char *display_name,
				GCancellable *cancellable,
				GError **error)
{
  GDaemonFile *daemon_file;
  DBusMessage *reply;
  DBusMessageIter iter;
  char *new_path;

  daemon_file = G_DAEMON_FILE (file);
  
  reply = do_sync_path_call (file, 
			     G_VFS_DBUS_MOUNT_OP_SET_DISPLAY_NAME,
			     NULL, cancellable, error,
			     DBUS_TYPE_STRING, &display_name,
			     0);
  if (reply == NULL)
    return NULL;


  if (!dbus_message_iter_init (reply, &iter) ||
      !_g_dbus_message_iter_get_args (&iter, NULL,
				      G_DBUS_TYPE_CSTRING, &new_path,
				      0))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		   _("Invalid return value from get_filesystem_info"));
      goto out;
    }

  file = g_daemon_file_new (daemon_file->mount_spec, new_path);
  g_free (new_path);

 out:
  dbus_message_unref (reply);
  return file;
}

static gboolean
g_daemon_file_delete (GFile *file,
		      GCancellable *cancellable,
		      GError **error)
{
  GDaemonFile *daemon_file;
  DBusMessage *reply;

  daemon_file = G_DAEMON_FILE (file);
  
  reply = do_sync_path_call (file, 
			     G_VFS_DBUS_MOUNT_OP_DELETE,
			     NULL, cancellable, error,
			     0);
  if (reply == NULL)
    return FALSE;

  dbus_message_unref (reply);
  return TRUE;
}

static gboolean
g_daemon_file_trash (GFile *file,
		     GCancellable *cancellable,
		     GError **error)
{
  GDaemonFile *daemon_file;
  DBusMessage *reply;

  daemon_file = G_DAEMON_FILE (file);
  
  reply = do_sync_path_call (file, 
			     G_VFS_DBUS_MOUNT_OP_TRASH,
			     NULL, cancellable, error,
			     0);
  if (reply == NULL)
    return FALSE;

  dbus_message_unref (reply);
  return TRUE;
}

static gboolean
g_daemon_file_make_directory (GFile *file,
			      GCancellable *cancellable,
			      GError **error)
{
  GDaemonFile *daemon_file;
  DBusMessage *reply;

  daemon_file = G_DAEMON_FILE (file);

  reply = do_sync_path_call (file, 
			     G_VFS_DBUS_MOUNT_OP_MAKE_DIRECTORY,
			     NULL, cancellable, error,
			     0);
  if (reply == NULL)
    return FALSE;

  dbus_message_unref (reply);
  return TRUE;
}

static gboolean
g_daemon_file_set_attribute (GFile *file,
			     const char *attribute,
			     GFileAttributeType type,
			     gconstpointer value_ptr,
			     GFileGetInfoFlags flags,
			     GCancellable *cancellable,
			     GError **error)
{
#if 0
  GDaemonFile *daemon_file;
  DBusMessage *message, *reply;
  DBusMessageIter iter;
  dbus_uint32_t flags_dbus;

  daemon_file = G_DAEMON_FILE (file);

  message = create_empty_message (file, G_VFS_DBUS_MOUNT_OP_SET_ATTRIBUTE);
  if (!message)
    return FALSE;

  dbus_message_iter_init_append (message, &iter);

  flags_dbus = flags;
  dbus_message_iter_append_basic (&iter,
				  DBUS_TYPE_UINT32,
				  &flags_dbus);


  reply = do_sync_path_call (file, 
			     G_VFS_DBUS_MOUNT_OP_SET_ATTRIBUTE,
			     NULL, cancellable, error,
			     DBUS_TYPE_STRING, &attribute,
			     DBUS_TYPE_UINT, &type,
			     DBUS_TYPE_UINT32, &flags_dbus,
			     g_dbus_type_from_file_attribute_type (type), &value,
			     0);
  if (reply == NULL)
    return FALSE;

  dbus_message_unref (reply);
#endif
  return TRUE;
}

struct ProgressCallbackData {
  GFileProgressCallback progress_callback;
  gpointer progress_callback_data;
};

static DBusHandlerResult
progress_callback_message (DBusConnection  *connection,
			   DBusMessage     *message,
			   void            *user_data)
{
  struct ProgressCallbackData *data = user_data;
  dbus_uint64_t current_dbus, total_dbus;
  
  if (dbus_message_is_method_call (message,
				   G_VFS_DBUS_PROGRESS_INTERFACE,
				   G_VFS_DBUS_PROGRESS_OP_PROGRESS))
    {
      if (dbus_message_get_args (message, NULL, 
				 DBUS_TYPE_UINT64, &current_dbus,
				 DBUS_TYPE_UINT64, &total_dbus,
				 0))
	data->progress_callback (current_dbus, total_dbus, data->progress_callback_data);
    }
  else
    g_warning ("Unknown progress callback message type\n");
  
  /* TODO: demarshal args and call reall callback */
  return DBUS_HANDLER_RESULT_HANDLED;
}

static gboolean
g_daemon_file_copy (GFile                  *source,
		    GFile                  *destination,
		    GFileCopyFlags          flags,
		    GCancellable           *cancellable,
		    GFileProgressCallback   progress_callback,
		    gpointer                progress_callback_data,
		    GError                **error)
{
  GDaemonFile *daemon_source, *daemon_dest;
  DBusMessage *reply;
  char *obj_path, *dbus_obj_path;
  dbus_uint32_t flags_dbus;
  struct ProgressCallbackData data;

  daemon_source = G_DAEMON_FILE (source);
  daemon_dest = G_DAEMON_FILE (destination);

  if (progress_callback)
    {
      obj_path = g_strdup_printf ("/org/gtk/vfs/callback/%p", &obj_path);
      dbus_obj_path = obj_path;
    }
  else
    {
      obj_path = NULL;
      /* Can't pass NULL obj path as arg */
      dbus_obj_path = "/org/gtk/vfs/void";
    }

  data.progress_callback = progress_callback;
  data.progress_callback_data = progress_callback_data;

  flags_dbus = flags;
  reply = do_sync_2_path_call (source, destination, 
			       G_VFS_DBUS_MOUNT_OP_COPY,
			       obj_path, progress_callback_message, &data,
			       NULL, cancellable, error,
			       DBUS_TYPE_UINT32, &flags_dbus,
			       DBUS_TYPE_OBJECT_PATH, &dbus_obj_path,
			       0);

  g_free (obj_path);

  if (reply == NULL)
    return FALSE;

  dbus_message_unref (reply);
  return TRUE;
}

static gboolean
g_daemon_file_move (GFile                  *source,
		    GFile                  *destination,
		    GFileCopyFlags          flags,
		    GCancellable           *cancellable,
		    GFileProgressCallback   progress_callback,
		    gpointer                progress_callback_data,
		    GError                **error)
{
  GDaemonFile *daemon_source, *daemon_dest;
  DBusMessage *reply;
  char *obj_path, *dbus_obj_path;
  dbus_uint32_t flags_dbus;
  struct ProgressCallbackData data;

  daemon_source = G_DAEMON_FILE (source);
  daemon_dest = G_DAEMON_FILE (destination);

  if (progress_callback)
    {
      obj_path = g_strdup_printf ("/org/gtk/vfs/callback/%p", &obj_path);
      dbus_obj_path = obj_path;
    }
  else
    {
      obj_path = NULL;
      /* Can't pass NULL obj path as arg */
      dbus_obj_path = "/org/gtk/vfs/void";
    }

  data.progress_callback = progress_callback;
  data.progress_callback_data = progress_callback_data;

  flags_dbus = flags;
  reply = do_sync_2_path_call (source, destination, 
			       G_VFS_DBUS_MOUNT_OP_MOVE,
			       obj_path, progress_callback_message, &data,
			       NULL, cancellable, error,
			       DBUS_TYPE_UINT32, &flags_dbus,
			       DBUS_TYPE_OBJECT_PATH, &dbus_obj_path,
			       0);

  g_free (obj_path);

  if (reply == NULL)
    return FALSE;

  dbus_message_unref (reply);
  return TRUE;
}

static void
g_daemon_file_file_iface_init (GFileIface *iface)
{
  iface->dup = g_daemon_file_dup;
  iface->hash = g_daemon_file_hash;
  iface->equal = g_daemon_file_equal;
  iface->is_native = g_daemon_file_is_native;
  iface->get_basename = g_daemon_file_get_basename;
  iface->get_path = g_daemon_file_get_path;
  iface->get_uri = g_daemon_file_get_uri;
  iface->get_parse_name = g_daemon_file_get_parse_name;
  iface->get_parent = g_daemon_file_get_parent;
  iface->resolve_relative = g_daemon_file_resolve_relative;
  iface->enumerate_children = g_daemon_file_enumerate_children;
  iface->get_info = g_daemon_file_get_info;
  iface->read = g_daemon_file_read;
  iface->append_to = g_daemon_file_append_to;
  iface->create = g_daemon_file_create;
  iface->replace = g_daemon_file_replace;
  iface->read_async = g_daemon_file_read_async;
  iface->read_finish = g_daemon_file_read_finish;
  iface->mount_for_location = g_daemon_file_mount_for_location;
  iface->mount_for_location_finish = g_daemon_file_mount_for_location_finish;
  iface->mount_mountable = g_daemon_file_mount_mountable;
  iface->mount_mountable_finish = g_daemon_file_mount_mountable_finish;
  iface->get_filesystem_info = g_daemon_file_get_filesystem_info;
  iface->set_display_name = g_daemon_file_set_display_name;
  iface->delete_file = g_daemon_file_delete;
  iface->trash = g_daemon_file_trash;
  iface->make_directory = g_daemon_file_make_directory;
  iface->copy = g_daemon_file_copy;
  iface->move = g_daemon_file_move;
  iface->set_attribute = g_daemon_file_set_attribute;
}
