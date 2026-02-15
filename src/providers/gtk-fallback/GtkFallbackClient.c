#include "GtkFallbackClient.h"

#include <gio/gio.h>
#include <json-glib/json-glib.h>

struct _GtkFallbackClient {
    char*                      socket_path;
    GtkFallbackClientCallbacks callbacks;

    GSocketClient*             socket_client;
    GSocketConnection*         connection;
    GDataInputStream*          input;
    GOutputStream*             output;

    guint                      heartbeat_source_id;
    guint                      reconnect_source_id;

    gboolean                   connected;
    gboolean                   registered;
    gboolean                   active;
    char*                      provider_id;
};

static void client_emit_status(GtkFallbackClient* client, const char* message) {
    if (client->callbacks.on_status != NULL) {
        client->callbacks.on_status(message, client->callbacks.user_data);
    }
}

static void client_emit_provider_state(GtkFallbackClient* client, gboolean active) {
    if (client->active == active) {
        return;
    }

    client->active = active;
    if (client->callbacks.on_provider_state != NULL) {
        client->callbacks.on_provider_state(active, client->callbacks.user_data);
    }
}

static gboolean client_send_json(GtkFallbackClient* client, JsonObject* obj) {
    if (!client->connected || client->output == NULL) {
        return FALSE;
    }

    g_autoptr(JsonNode) root = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(root, obj);

    g_autoptr(JsonGenerator) generator = json_generator_new();
    json_generator_set_root(generator, root);
    g_autofree char* payload = json_generator_to_data(generator, NULL);
    g_autofree char* line    = g_strconcat(payload, "\n", NULL);

    gsize            bytes_written = 0;
    GError*          error         = NULL;

    if (!g_output_stream_write_all(client->output, line, strlen(line), &bytes_written, NULL, &error)) {
        client_emit_status(client, error->message);
        g_clear_error(&error);
        return FALSE;
    }

    if (!g_output_stream_flush(client->output, NULL, &error)) {
        client_emit_status(client, error->message);
        g_clear_error(&error);
        return FALSE;
    }

    return TRUE;
}

static void client_send_register(GtkFallbackClient* client) {
    g_autoptr(JsonObject) obj = json_object_new();
    json_object_set_string_member(obj, "type", "ui.register");
    json_object_set_string_member(obj, "name", "bb-auth-gtk-fallback");
    json_object_set_string_member(obj, "kind", "gtk-fallback");
    json_object_set_int_member(obj, "priority", 8);
    client_send_json(client, obj);
}

static void client_send_subscribe(GtkFallbackClient* client) {
    g_autoptr(JsonObject) obj = json_object_new();
    json_object_set_string_member(obj, "type", "subscribe");
    client_send_json(client, obj);
}

static gboolean client_send_heartbeat(gpointer user_data) {
    GtkFallbackClient* client = (GtkFallbackClient*)user_data;

    if (!client->connected || !client->registered || client->provider_id == NULL) {
        return G_SOURCE_CONTINUE;
    }

    g_autoptr(JsonObject) obj = json_object_new();
    json_object_set_string_member(obj, "type", "ui.heartbeat");
    json_object_set_string_member(obj, "id", client->provider_id);
    client_send_json(client, obj);

    return G_SOURCE_CONTINUE;
}

static void     client_disconnect(GtkFallbackClient* client);
static gboolean client_schedule_reconnect(gpointer user_data);

static void     client_on_read_line(GObject* source_object, GAsyncResult* result, gpointer user_data) {
    GtkFallbackClient* client = (GtkFallbackClient*)user_data;

    gsize              length = 0;
    GError*            error  = NULL;
    char*              line   = g_data_input_stream_read_line_finish(client->input, result, &length, &error);

    if (error != NULL) {
        client_emit_status(client, error->message);
        g_clear_error(&error);
        client_disconnect(client);
        client_schedule_reconnect(client);
        return;
    }

    if (line == NULL) {
        client_disconnect(client);
        client_schedule_reconnect(client);
        return;
    }

    if (length > 0) {
        g_autoptr(JsonParser) parser = json_parser_new();
        if (json_parser_load_from_data(parser, line, (gssize)length, &error)) {
            JsonNode* root = json_parser_get_root(parser);
            if (JSON_NODE_HOLDS_OBJECT(root)) {
                JsonObject* msg  = json_node_get_object(root);
                const char* type = json_object_get_string_member_with_default(msg, "type", "");

                if (g_strcmp0(type, "ui.registered") == 0) {
                    g_free(client->provider_id);
                    client->provider_id = g_strdup(json_object_get_string_member_with_default(msg, "id", ""));
                    client->registered  = TRUE;
                    client_emit_provider_state(client, json_object_get_boolean_member_with_default(msg, "active", FALSE));
                    client_send_subscribe(client);
                } else if (g_strcmp0(type, "subscribed") == 0) {
                    if (json_object_has_member(msg, "active")) {
                        client_emit_provider_state(client, json_object_get_boolean_member(msg, "active"));
                    }
                } else if (g_strcmp0(type, "ui.active") == 0) {
                    gboolean active = json_object_get_boolean_member_with_default(msg, "active", FALSE);
                    if (!active) {
                        client_emit_provider_state(client, FALSE);
                    } else {
                        const char* active_id = json_object_get_string_member_with_default(msg, "id", "");
                        client_emit_provider_state(client, g_strcmp0(active_id, client->provider_id) == 0);
                    }
                } else if (g_strcmp0(type, "session.created") == 0) {
                    if (client->callbacks.on_session_created != NULL && client->active) {
                        const char* session_id = json_object_get_string_member_with_default(msg, "id", "");
                        const char* source     = json_object_get_string_member_with_default(msg, "source", "");

                        const char* message = "";
                        if (json_object_has_member(msg, "context")) {
                            JsonObject* context = json_object_get_object_member(msg, "context");
                            message             = json_object_get_string_member_with_default(context, "message", "");
                        }

                        client->callbacks.on_session_created(session_id, source, message, client->callbacks.user_data);
                    }
                } else if (g_strcmp0(type, "session.updated") == 0) {
                    if (client->callbacks.on_session_updated != NULL && client->active) {
                        const char* session_id = json_object_get_string_member_with_default(msg, "id", "");
                        const char* prompt     = json_object_get_string_member_with_default(msg, "prompt", "");
                        const char* error_msg  = json_object_get_string_member_with_default(msg, "error", "");
                        const char* info_msg   = json_object_get_string_member_with_default(msg, "info", "");
                        gboolean    echo       = json_object_get_boolean_member_with_default(msg, "echo", FALSE);

                        client->callbacks.on_session_updated(session_id, prompt, echo, error_msg, info_msg, client->callbacks.user_data);
                    }
                } else if (g_strcmp0(type, "session.closed") == 0) {
                    if (client->callbacks.on_session_closed != NULL) {
                        const char* session_id = json_object_get_string_member_with_default(msg, "id", "");
                        const char* close_type = json_object_get_string_member_with_default(msg, "result", "");
                        const char* error_msg  = json_object_get_string_member_with_default(msg, "error", "");
                        client->callbacks.on_session_closed(session_id, close_type, error_msg, client->callbacks.user_data);
                    }
                } else if (g_strcmp0(type, "error") == 0) {
                    const char* message = json_object_get_string_member_with_default(msg, "message", "Error");
                    if (g_strcmp0(message, "Not active UI provider") == 0) {
                        client_emit_provider_state(client, FALSE);
                    }
                    client_emit_status(client, message);
                }
            }
        } else {
            client_emit_status(client, error->message);
            g_clear_error(&error);
        }
    }

    g_free(line);

    if (client->connected && client->input != NULL) {
        g_data_input_stream_read_line_async(client->input, G_PRIORITY_DEFAULT, NULL, client_on_read_line, client);
    }
}

static void client_disconnect(GtkFallbackClient* client) {
    client->connected  = FALSE;
    client->registered = FALSE;
    client_emit_provider_state(client, FALSE);

    g_clear_object(&client->input);
    g_clear_object(&client->output);
    g_clear_object(&client->connection);
}

static gboolean client_connect_once(GtkFallbackClient* client) {
    if (client->connected) {
        return TRUE;
    }

    GError* error = NULL;

    g_autoptr(GSocketAddress) address = g_unix_socket_address_new(client->socket_path);
    client->connection                = g_socket_client_connect(client->socket_client, G_SOCKET_CONNECTABLE(address), NULL, &error);
    if (client->connection == NULL) {
        client_emit_status(client, error->message);
        g_clear_error(&error);
        return FALSE;
    }

    client->input  = g_data_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(client->connection)));
    client->output = g_io_stream_get_output_stream(G_IO_STREAM(client->connection));
    g_object_ref(client->output);

    client->connected = TRUE;
    client_emit_status(client, "Connected to auth daemon");

    client_send_register(client);
    client_send_subscribe(client);

    g_data_input_stream_read_line_async(client->input, G_PRIORITY_DEFAULT, NULL, client_on_read_line, client);

    return TRUE;
}

static gboolean client_schedule_reconnect(gpointer user_data) {
    GtkFallbackClient* client   = (GtkFallbackClient*)user_data;
    client->reconnect_source_id = 0;

    if (!client_connect_once(client)) {
        client->reconnect_source_id = g_timeout_add_seconds(2, client_schedule_reconnect, client);
    }

    return G_SOURCE_REMOVE;
}

GtkFallbackClient* gtk_fallback_client_new(const char* socket_path, const GtkFallbackClientCallbacks* callbacks) {
    GtkFallbackClient* client = g_new0(GtkFallbackClient, 1);

    client->socket_path   = g_strdup(socket_path);
    client->socket_client = g_socket_client_new();

    if (callbacks != NULL) {
        client->callbacks = *callbacks;
    }

    return client;
}

void gtk_fallback_client_free(GtkFallbackClient* client) {
    if (client == NULL) {
        return;
    }

    if (client->heartbeat_source_id != 0) {
        g_source_remove(client->heartbeat_source_id);
    }
    if (client->reconnect_source_id != 0) {
        g_source_remove(client->reconnect_source_id);
    }

    client_disconnect(client);

    g_clear_object(&client->socket_client);

    g_free(client->provider_id);
    g_free(client->socket_path);
    g_free(client);
}

void gtk_fallback_client_start(GtkFallbackClient* client) {
    g_return_if_fail(client != NULL);

    if (!client_connect_once(client) && client->reconnect_source_id == 0) {
        client->reconnect_source_id = g_timeout_add_seconds(2, client_schedule_reconnect, client);
    }

    if (client->heartbeat_source_id == 0) {
        client->heartbeat_source_id = g_timeout_add_seconds(4, client_send_heartbeat, client);
    }
}

void gtk_fallback_client_send_response(GtkFallbackClient* client, const char* session_id, const char* response) {
    if (client == NULL || session_id == NULL) {
        return;
    }

    g_autoptr(JsonObject) obj = json_object_new();
    json_object_set_string_member(obj, "type", "session.respond");
    json_object_set_string_member(obj, "id", session_id);
    json_object_set_string_member(obj, "response", response != NULL ? response : "");
    client_send_json(client, obj);
}

void gtk_fallback_client_send_cancel(GtkFallbackClient* client, const char* session_id) {
    if (client == NULL || session_id == NULL) {
        return;
    }

    g_autoptr(JsonObject) obj = json_object_new();
    json_object_set_string_member(obj, "type", "session.cancel");
    json_object_set_string_member(obj, "id", session_id);
    client_send_json(client, obj);
}
