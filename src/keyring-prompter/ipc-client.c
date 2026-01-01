#include "ipc-client.h"

#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <json-glib/json-glib.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static gchar *
get_socket_path (void)
{
    const gchar *runtime_dir = g_get_user_runtime_dir ();
    return g_build_filename (runtime_dir, "noctalia-polkit-agent.sock", NULL);
}

static GSocketConnection *
connect_to_socket (void)
{
    g_autoptr(GSocketClient) client = NULL;
    g_autoptr(GSocketAddress) address = NULL;
    g_autofree gchar *socket_path = NULL;
    GSocketConnection *connection = NULL;
    GError *error = NULL;

    socket_path = get_socket_path ();

    client = g_socket_client_new ();
    address = g_unix_socket_address_new (socket_path);

    connection = g_socket_client_connect (client,
                                          G_SOCKET_CONNECTABLE (address),
                                          NULL, &error);

    if (error) {
        g_debug ("Failed to connect to socket %s: %s", socket_path, error->message);
        g_error_free (error);
        return NULL;
    }

    return connection;
}

static gchar *
send_json_command (const gchar *json_request)
{
    g_autoptr(GSocketConnection) connection = NULL;
    GOutputStream *output;
    GInputStream *input;
    g_autoptr(GDataInputStream) data_input = NULL;
    GError *error = NULL;
    g_autofree gchar *request = NULL;
    gchar *line = NULL;

    connection = connect_to_socket ();
    if (!connection)
        return NULL;

    output = g_io_stream_get_output_stream (G_IO_STREAM (connection));
    input = g_io_stream_get_input_stream (G_IO_STREAM (connection));
    data_input = g_data_input_stream_new (input);

    request = g_strdup_printf ("%s\n", json_request);

    if (!g_output_stream_write_all (output, request, strlen (request),
                                    NULL, NULL, &error)) {
        g_debug ("Failed to write command: %s", error->message);
        g_error_free (error);
        return NULL;
    }

    line = g_data_input_stream_read_line (data_input, NULL, NULL, &error);
    if (!line) {
        g_debug ("Failed to read response: %s", error->message);
        g_error_free (error);
        return NULL;
    }

    return line;
}

gboolean
noctalia_ipc_ping (void)
{
    g_autoptr(JsonBuilder) builder = NULL;
    g_autoptr(JsonGenerator) generator = NULL;
    g_autoptr(JsonNode) root = NULL;
    g_autofree gchar *json_str = NULL;
    g_autofree gchar *response = NULL;
    g_autoptr(JsonParser) parser = NULL;
    GError *error = NULL;

    builder = json_builder_new ();
    json_builder_begin_object (builder);
    json_builder_set_member_name (builder, "type");
    json_builder_add_string_value (builder, "ping");
    json_builder_end_object (builder);

    root = json_builder_get_root (builder);
    generator = json_generator_new ();
    json_generator_set_root (generator, root);
    json_str = json_generator_to_data (generator, NULL);

    response = send_json_command (json_str);
    if (!response)
        return FALSE;

    parser = json_parser_new ();
    if (!json_parser_load_from_data (parser, response, -1, &error)) {
        g_debug ("Failed to parse ping response: %s", error->message);
        g_error_free (error);
        return FALSE;
    }

    JsonNode *resp_root = json_parser_get_root (parser);
    if (!JSON_NODE_HOLDS_OBJECT (resp_root))
        return FALSE;

    JsonObject *resp_obj = json_node_get_object (resp_root);
    if (!json_object_has_member (resp_obj, "type"))
        return FALSE;

    const gchar *type = json_object_get_string_member (resp_obj, "type");
    return type && g_strcmp0 (type, "pong") == 0;
}

gboolean
noctalia_ipc_send_keyring_request (const gchar *cookie,
                                   const gchar *title,
                                   const gchar *message,
                                   const gchar *description,
                                   gboolean     password_new,
                                   gchar      **out_password)
{
    GError *error = NULL;
    g_autoptr(JsonBuilder) builder = NULL;
    g_autoptr(JsonGenerator) generator = NULL;
    g_autoptr(JsonNode) root = NULL;
    g_autofree gchar *json_str = NULL;
    g_autofree gchar *response = NULL;
    g_autoptr(JsonParser) parser = NULL;

    *out_password = NULL;

    /* Build JSON payload */
    builder = json_builder_new ();
    json_builder_begin_object (builder);

    json_builder_set_member_name (builder, "type");
    json_builder_add_string_value (builder, "keyring_request");

    json_builder_set_member_name (builder, "cookie");
    json_builder_add_string_value (builder, cookie);

    json_builder_set_member_name (builder, "title");
    json_builder_add_string_value (builder, title ? title : "Unlock Keyring");

    json_builder_set_member_name (builder, "message");
    json_builder_add_string_value (builder, message ? message : "Password required");

    if (description) {
        json_builder_set_member_name (builder, "description");
        json_builder_add_string_value (builder, description);
    }

    json_builder_set_member_name (builder, "password_new");
    json_builder_add_boolean_value (builder, password_new);

    json_builder_set_member_name (builder, "confirm_only");
    json_builder_add_boolean_value (builder, FALSE);

    json_builder_end_object (builder);

    root = json_builder_get_root (builder);
    generator = json_generator_new ();
    json_generator_set_root (generator, root);
    json_str = json_generator_to_data (generator, NULL);

    response = send_json_command (json_str);
    if (!response) {
        g_warning ("Failed to connect to noctalia-polkit socket");
        return FALSE;
    }

    parser = json_parser_new ();
    if (!json_parser_load_from_data (parser, response, -1, &error)) {
        g_warning ("Failed to parse keyring response: %s", error->message);
        g_error_free (error);
        return FALSE;
    }

    JsonNode *resp_root = json_parser_get_root (parser);
    if (!JSON_NODE_HOLDS_OBJECT (resp_root))
        return FALSE;

    JsonObject *resp_obj = json_node_get_object (resp_root);
    if (!json_object_has_member (resp_obj, "type"))
        return FALSE;

    const gchar *type = json_object_get_string_member (resp_obj, "type");
    if (!type || g_strcmp0 (type, "keyring_response") != 0)
        return FALSE;

    if (!json_object_has_member (resp_obj, "result"))
        return FALSE;

    const gchar *result = json_object_get_string_member (resp_obj, "result");
    if (result && g_strcmp0 (result, "ok") == 0) {
        if (!json_object_has_member (resp_obj, "password"))
            return FALSE;

        const gchar *password = json_object_get_string_member (resp_obj, "password");
        if (password) {
            *out_password = g_strdup (password);
            return TRUE;
        }
        return FALSE;
    }

    if (result && g_strcmp0 (result, "cancelled") == 0) {
        g_debug ("Keyring request cancelled by user");
        return FALSE;
    }

    g_warning ("Unexpected response from noctalia-polkit: %s", response);
    return FALSE;
}

gboolean
noctalia_ipc_send_confirm_request (const gchar *cookie,
                                   const gchar *title,
                                   const gchar *message,
                                   const gchar *description)
{
    GError *error = NULL;
    g_autoptr(JsonBuilder) builder = NULL;
    g_autoptr(JsonGenerator) generator = NULL;
    g_autoptr(JsonNode) root = NULL;
    g_autofree gchar *json_str = NULL;
    g_autofree gchar *response = NULL;
    g_autoptr(JsonParser) parser = NULL;

    /* Build JSON payload */
    builder = json_builder_new ();
    json_builder_begin_object (builder);

    json_builder_set_member_name (builder, "type");
    json_builder_add_string_value (builder, "keyring_request");

    json_builder_set_member_name (builder, "cookie");
    json_builder_add_string_value (builder, cookie);

    json_builder_set_member_name (builder, "title");
    json_builder_add_string_value (builder, title ? title : "Confirm");

    json_builder_set_member_name (builder, "message");
    json_builder_add_string_value (builder, message ? message : "Please confirm");

    if (description) {
        json_builder_set_member_name (builder, "description");
        json_builder_add_string_value (builder, description);
    }

    json_builder_set_member_name (builder, "confirm_only");
    json_builder_add_boolean_value (builder, TRUE);

    json_builder_end_object (builder);

    root = json_builder_get_root (builder);
    generator = json_generator_new ();
    json_generator_set_root (generator, root);
    json_str = json_generator_to_data (generator, NULL);

    response = send_json_command (json_str);
    if (!response)
        return FALSE;

    parser = json_parser_new ();
    if (!json_parser_load_from_data (parser, response, -1, &error)) {
        g_warning ("Failed to parse confirm response: %s", error->message);
        g_error_free (error);
        return FALSE;
    }

    JsonNode *resp_root = json_parser_get_root (parser);
    if (!JSON_NODE_HOLDS_OBJECT (resp_root))
        return FALSE;

    JsonObject *resp_obj = json_node_get_object (resp_root);
    if (!json_object_has_member (resp_obj, "type"))
        return FALSE;

    const gchar *type = json_object_get_string_member (resp_obj, "type");
    if (!type || g_strcmp0 (type, "keyring_response") != 0)
        return FALSE;

    if (!json_object_has_member (resp_obj, "result"))
        return FALSE;

    const gchar *result = json_object_get_string_member (resp_obj, "result");
    return result && g_strcmp0 (result, "confirmed") == 0;
}

void
noctalia_ipc_send_cancel (const gchar *cookie)
{
    g_autoptr(JsonBuilder) builder = NULL;
    g_autoptr(JsonGenerator) generator = NULL;
    g_autoptr(JsonNode) root = NULL;
    g_autofree gchar *json_str = NULL;
    g_autofree gchar *response = NULL;

    builder = json_builder_new ();
    json_builder_begin_object (builder);
    json_builder_set_member_name (builder, "type");
    json_builder_add_string_value (builder, "cancel");
    json_builder_set_member_name (builder, "id");
    json_builder_add_string_value (builder, cookie);
    json_builder_end_object (builder);

    root = json_builder_get_root (builder);
    generator = json_generator_new ();
    json_generator_set_root (generator, root);
    json_str = json_generator_to_data (generator, NULL);

    response = send_json_command (json_str);
    /* Ignore response - best effort cancel */
}
