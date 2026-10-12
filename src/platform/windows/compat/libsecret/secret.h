#pragma once
#include <gio/gio.h>
#define SECRET_SCHEMA_NONE 0
#define SECRET_SCHEMA_ATTRIBUTE_STRING 0
#define SECRET_COLLECTION_DEFAULT "default"
typedef struct {
    const gchar *name;
    int flags;
    struct { const gchar *name; int type; } attributes[32];
    int reserved;
    gpointer reserved1, reserved2, reserved3, reserved4, reserved5, reserved6, reserved7;
} SecretSchema;
gboolean secret_password_store_sync (const SecretSchema *, const gchar *, const gchar *, const gchar *, GCancellable *, GError **, ...);
gchar *secret_password_lookup_sync (const SecretSchema *, GCancellable *, GError **, ...);
gboolean secret_password_clear_sync (const SecretSchema *, GCancellable *, GError **, ...);
void secret_password_lookup (const SecretSchema *, GCancellable *, GAsyncReadyCallback, gpointer, ...);
void secret_password_clear (const SecretSchema *, GCancellable *, GAsyncReadyCallback, gpointer, ...);
gchar *secret_password_lookup_finish (GAsyncResult *, GError **);
gboolean secret_password_clear_finish (GAsyncResult *, GError **);
gboolean secret_password_store_finish (GAsyncResult *, GError **);
void secret_password_free (gchar *password);
