#pragma once

#include <gtk/gtk.h>

#include "GtkFallbackClient.h"

G_BEGIN_DECLS

typedef struct _GtkFallbackWindow GtkFallbackWindow;

GtkFallbackWindow*                gtk_fallback_window_new(GtkApplication* app, GtkFallbackClient* client);
void                              gtk_fallback_window_free(GtkFallbackWindow* window);

void                              gtk_fallback_window_on_session_created(GtkFallbackWindow* window, const char* session_id, const char* source, const char* message);
void gtk_fallback_window_on_session_updated(GtkFallbackWindow* window, const char* session_id, const char* prompt, gboolean echo, const char* error, const char* info);
void gtk_fallback_window_on_session_closed(GtkFallbackWindow* window, const char* session_id, const char* result, const char* error);
void gtk_fallback_window_on_provider_state(GtkFallbackWindow* window, gboolean active);
void gtk_fallback_window_on_status(GtkFallbackWindow* window, const char* status);

G_END_DECLS
