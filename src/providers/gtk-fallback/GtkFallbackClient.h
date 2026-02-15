#pragma once

#include <stdbool.h>

#include <glib.h>

G_BEGIN_DECLS

typedef struct _GtkFallbackClient GtkFallbackClient;

typedef void (*GtkFallbackSessionCreatedCb)(const char* session_id, const char* source, const char* message, gpointer user_data);
typedef void (*GtkFallbackSessionUpdatedCb)(const char* session_id, const char* prompt, gboolean echo, const char* error, const char* info, gpointer user_data);
typedef void (*GtkFallbackSessionClosedCb)(const char* session_id, const char* result, const char* error, gpointer user_data);
typedef void (*GtkFallbackProviderStateCb)(gboolean active, gpointer user_data);
typedef void (*GtkFallbackStatusCb)(const char* status, gpointer user_data);

typedef struct {
    GtkFallbackSessionCreatedCb on_session_created;
    GtkFallbackSessionUpdatedCb on_session_updated;
    GtkFallbackSessionClosedCb  on_session_closed;
    GtkFallbackProviderStateCb  on_provider_state;
    GtkFallbackStatusCb         on_status;
    gpointer                    user_data;
} GtkFallbackClientCallbacks;

GtkFallbackClient* gtk_fallback_client_new(const char* socket_path, const GtkFallbackClientCallbacks* callbacks);
void               gtk_fallback_client_free(GtkFallbackClient* client);

void               gtk_fallback_client_start(GtkFallbackClient* client);
void               gtk_fallback_client_send_response(GtkFallbackClient* client, const char* session_id, const char* response);
void               gtk_fallback_client_send_cancel(GtkFallbackClient* client, const char* session_id);

G_END_DECLS
