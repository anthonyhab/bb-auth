#include "GtkFallbackClient.h"
#include "GtkFallbackWindow.h"

#include <gtk/gtk.h>

typedef struct {
    GtkFallbackClient* client;
    GtkFallbackWindow* window;
    char*              socket_path;
} AppState;

static void on_session_created(const char* session_id, const char* source, const char* message, gpointer user_data) {
    AppState* state = (AppState*)user_data;
    gtk_fallback_window_on_session_created(state->window, session_id, source, message);
}

static void on_session_updated(const char* session_id, const char* prompt, gboolean echo, const char* error, const char* info, gpointer user_data) {
    AppState* state = (AppState*)user_data;
    gtk_fallback_window_on_session_updated(state->window, session_id, prompt, echo, error, info);
}

static void on_session_closed(const char* session_id, const char* result, const char* error, gpointer user_data) {
    AppState* state = (AppState*)user_data;
    gtk_fallback_window_on_session_closed(state->window, session_id, result, error);
}

static void on_provider_state(gboolean active, gpointer user_data) {
    AppState* state = (AppState*)user_data;
    gtk_fallback_window_on_provider_state(state->window, active);
}

static void on_status(const char* status, gpointer user_data) {
    AppState* state = (AppState*)user_data;
    gtk_fallback_window_on_status(state->window, status);
}

static void app_activate(GApplication* app, gpointer user_data) {
    AppState*                  state = (AppState*)user_data;

    GtkFallbackClientCallbacks callbacks = {
        .on_session_created = on_session_created,
        .on_session_updated = on_session_updated,
        .on_session_closed  = on_session_closed,
        .on_provider_state  = on_provider_state,
        .on_status          = on_status,
        .user_data          = state,
    };

    state->client = gtk_fallback_client_new(state->socket_path, &callbacks);
    state->window = gtk_fallback_window_new(GTK_APPLICATION(app), state->client);
    gtk_fallback_client_start(state->client);
}

static void app_shutdown(GApplication* app, gpointer user_data) {
    (void)app;

    AppState* state = (AppState*)user_data;
    gtk_fallback_window_free(state->window);
    gtk_fallback_client_free(state->client);
}

int main(int argc, char** argv) {
    g_autofree char* runtime_dir    = g_strdup(g_getenv("XDG_RUNTIME_DIR"));
    g_autofree char* default_socket = NULL;
    char*            filtered_argv[argc + 1];
    int              filtered_argc = 1;

    if (runtime_dir != NULL) {
        default_socket = g_build_filename(runtime_dir, "bb-auth.sock", NULL);
    }

    g_autofree char* socket_path = g_strdup(default_socket != NULL ? default_socket : "");
    filtered_argv[0]             = argv[0];

    for (int i = 1; i < argc; ++i) {
        if (g_strcmp0(argv[i], "--socket") == 0 && (i + 1) < argc) {
            g_free(socket_path);
            socket_path = g_strdup(argv[i + 1]);
            i++;
            continue;
        }

        filtered_argv[filtered_argc++] = argv[i];
    }
    filtered_argv[filtered_argc] = NULL;

    if (socket_path == NULL || socket_path[0] == '\0') {
        return 1;
    }

    AppState state = {
        .socket_path = socket_path,
    };

    g_autoptr(GtkApplication) app = gtk_application_new("org.bb.auth.gtk-fallback", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(app_activate), &state);
    g_signal_connect(app, "shutdown", G_CALLBACK(app_shutdown), &state);

    return g_application_run(G_APPLICATION(app), filtered_argc, filtered_argv);
}
