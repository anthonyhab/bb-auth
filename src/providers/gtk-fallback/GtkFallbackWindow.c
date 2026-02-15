#include "GtkFallbackWindow.h"

#include <string.h>

struct _GtkFallbackWindow {
    GtkApplication*    app;
    GtkFallbackClient* client;

    GtkWidget*         window;
    GtkWidget*         source_label;
    GtkWidget*         message_label;
    GtkWidget*         prompt_label;
    GtkWidget*         status_label;
    GtkWidget*         entry;
    GtkWidget*         submit_button;
    GtkWidget*         cancel_button;

    gboolean           active;
    char*              current_session_id;
};

static void window_set_status(GtkFallbackWindow* window, const char* status) {
    gtk_label_set_text(GTK_LABEL(window->status_label), status != NULL ? status : "");
}

static void on_submit_clicked(GtkButton* button, gpointer user_data) {
    (void)button;

    GtkFallbackWindow* window = (GtkFallbackWindow*)user_data;
    if (!window->active || window->current_session_id == NULL) {
        return;
    }

    const char* text = gtk_editable_get_text(GTK_EDITABLE(window->entry));
    gtk_fallback_client_send_response(window->client, window->current_session_id, text);
    gtk_editable_set_text(GTK_EDITABLE(window->entry), "");
}

static void on_cancel_clicked(GtkButton* button, gpointer user_data) {
    (void)button;

    GtkFallbackWindow* window = (GtkFallbackWindow*)user_data;
    if (!window->active || window->current_session_id == NULL) {
        return;
    }

    gtk_fallback_client_send_cancel(window->client, window->current_session_id);
}

GtkFallbackWindow* gtk_fallback_window_new(GtkApplication* app, GtkFallbackClient* client) {
    GtkFallbackWindow* window = g_new0(GtkFallbackWindow, 1);
    window->app               = app;
    window->client            = client;

    window->window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window->window), "Authentication Required");
    gtk_window_set_default_size(GTK_WINDOW(window->window), 460, 230);
    gtk_window_set_resizable(GTK_WINDOW(window->window), FALSE);

    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);
    gtk_widget_set_margin_top(box, 16);
    gtk_widget_set_margin_bottom(box, 16);
    gtk_window_set_child(GTK_WINDOW(window->window), box);

    window->source_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(window->source_label), 0.0f);
    gtk_box_append(GTK_BOX(box), window->source_label);

    window->message_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(window->message_label), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(window->message_label), TRUE);
    gtk_box_append(GTK_BOX(box), window->message_label);

    window->prompt_label = gtk_label_new("Password:");
    gtk_label_set_xalign(GTK_LABEL(window->prompt_label), 0.0f);
    gtk_box_append(GTK_BOX(box), window->prompt_label);

    window->entry = gtk_entry_new();
    gtk_box_append(GTK_BOX(box), window->entry);

    window->status_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(window->status_label), 0.0f);
    gtk_box_append(GTK_BOX(box), window->status_label);

    GtkWidget* actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(box), actions);

    window->submit_button = gtk_button_new_with_label("Submit");
    window->cancel_button = gtk_button_new_with_label("Cancel");

    gtk_box_append(GTK_BOX(actions), window->submit_button);
    gtk_box_append(GTK_BOX(actions), window->cancel_button);

    g_signal_connect(window->submit_button, "clicked", G_CALLBACK(on_submit_clicked), window);
    g_signal_connect(window->cancel_button, "clicked", G_CALLBACK(on_cancel_clicked), window);

    gtk_widget_set_sensitive(window->submit_button, FALSE);
    gtk_widget_set_sensitive(window->cancel_button, FALSE);

    gtk_widget_set_visible(window->window, FALSE);

    return window;
}

void gtk_fallback_window_free(GtkFallbackWindow* window) {
    if (window == NULL) {
        return;
    }

    g_free(window->current_session_id);
    g_free(window);
}

void gtk_fallback_window_on_session_created(GtkFallbackWindow* window, const char* session_id, const char* source, const char* message) {
    g_return_if_fail(window != NULL);

    g_free(window->current_session_id);
    window->current_session_id = g_strdup(session_id);

    gtk_label_set_text(GTK_LABEL(window->source_label), source != NULL ? source : "");
    gtk_label_set_text(GTK_LABEL(window->message_label), message != NULL ? message : "");

    gtk_widget_set_sensitive(window->submit_button, window->active);
    gtk_widget_set_sensitive(window->cancel_button, window->active);

    gtk_widget_set_visible(window->window, TRUE);
    gtk_window_present(GTK_WINDOW(window->window));
}

void gtk_fallback_window_on_session_updated(GtkFallbackWindow* window, const char* session_id, const char* prompt, gboolean echo, const char* error, const char* info) {
    g_return_if_fail(window != NULL);

    if (window->current_session_id == NULL || g_strcmp0(window->current_session_id, session_id) != 0) {
        return;
    }

    gtk_label_set_text(GTK_LABEL(window->prompt_label), prompt != NULL ? prompt : "Password:");
    gtk_entry_set_visibility(GTK_ENTRY(window->entry), echo);
    gtk_widget_set_sensitive(window->entry, window->active);

    if (error != NULL && strlen(error) > 0) {
        window_set_status(window, error);
    } else if (info != NULL && strlen(info) > 0) {
        window_set_status(window, info);
    } else {
        window_set_status(window, "");
    }
}

void gtk_fallback_window_on_session_closed(GtkFallbackWindow* window, const char* session_id, const char* result, const char* error) {
    g_return_if_fail(window != NULL);

    if (window->current_session_id == NULL || g_strcmp0(window->current_session_id, session_id) != 0) {
        return;
    }

    if (error != NULL && strlen(error) > 0) {
        window_set_status(window, error);
    } else if (result != NULL && strlen(result) > 0) {
        window_set_status(window, result);
    }

    g_free(window->current_session_id);
    window->current_session_id = NULL;
    gtk_editable_set_text(GTK_EDITABLE(window->entry), "");
    gtk_widget_set_sensitive(window->submit_button, FALSE);
    gtk_widget_set_sensitive(window->cancel_button, FALSE);
    gtk_widget_set_visible(window->window, FALSE);
}

void gtk_fallback_window_on_provider_state(GtkFallbackWindow* window, gboolean active) {
    g_return_if_fail(window != NULL);

    window->active             = active;
    const gboolean interactive = active && (window->current_session_id != NULL);

    gtk_widget_set_sensitive(window->entry, interactive);
    gtk_widget_set_sensitive(window->submit_button, interactive);
    gtk_widget_set_sensitive(window->cancel_button, interactive);

    if (!active) {
        window_set_status(window, "Standby: not active provider");
    }
}

void gtk_fallback_window_on_status(GtkFallbackWindow* window, const char* status) {
    g_return_if_fail(window != NULL);
    window_set_status(window, status);
}
