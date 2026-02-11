#define GCR_API_SUBJECT_TO_CHANGE 1
#include "bb-prompt.h"
#include "ipc-client.h"

#include <string.h>
#include <unistd.h>

struct _BbAuthPrompt {
    GObject parent_instance;

    /* GcrPrompt properties */
    gchar *title;
    gchar *message;
    gchar *description;
    gchar *warning;
    gchar *choice_label;
    gboolean choice_chosen;
    gboolean password_new;
    gchar *caller_window;
    gchar *continue_label;
    gchar *cancel_label;

    /* Internal state */
    gchar *password;
    gchar *request_cookie;
    gboolean cancelled;
    gint cookie_counter;
};

static void bb_auth_prompt_iface_init (GcrPromptInterface *iface);

G_DEFINE_TYPE_WITH_CODE (BbAuthPrompt, bb_auth_prompt, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (GCR_TYPE_PROMPT, bb_auth_prompt_iface_init))

enum {
    PROP_0,
    PROP_TITLE,
    PROP_MESSAGE,
    PROP_DESCRIPTION,
    PROP_WARNING,
    PROP_CHOICE_LABEL,
    PROP_CHOICE_CHOSEN,
    PROP_PASSWORD_NEW,
    PROP_PASSWORD_STRENGTH,
    PROP_CALLER_WINDOW,
    PROP_CONTINUE_LABEL,
    PROP_CANCEL_LABEL,
    N_PROPS
};

static void
bb_auth_prompt_set_property (GObject      *obj,
                              guint         prop_id,
                              const GValue *value,
                              GParamSpec   *pspec)
{
    BbAuthPrompt *self = BB_AUTH_PROMPT (obj);

    switch (prop_id) {
    case PROP_TITLE:
        g_free (self->title);
        self->title = g_value_dup_string (value);
        break;
    case PROP_MESSAGE:
        g_free (self->message);
        self->message = g_value_dup_string (value);
        break;
    case PROP_DESCRIPTION:
        g_free (self->description);
        self->description = g_value_dup_string (value);
        break;
    case PROP_WARNING:
        g_free (self->warning);
        self->warning = g_value_dup_string (value);
        break;
    case PROP_CHOICE_LABEL:
        g_free (self->choice_label);
        self->choice_label = g_value_dup_string (value);
        break;
    case PROP_CHOICE_CHOSEN:
        self->choice_chosen = g_value_get_boolean (value);
        break;
    case PROP_PASSWORD_NEW:
        self->password_new = g_value_get_boolean (value);
        break;
    case PROP_CALLER_WINDOW:
        g_free (self->caller_window);
        self->caller_window = g_value_dup_string (value);
        break;
    case PROP_CONTINUE_LABEL:
        g_free (self->continue_label);
        self->continue_label = g_value_dup_string (value);
        break;
    case PROP_CANCEL_LABEL:
        g_free (self->cancel_label);
        self->cancel_label = g_value_dup_string (value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (obj, prop_id, pspec);
    }
}

static void
bb_auth_prompt_get_property (GObject    *obj,
                              guint       prop_id,
                              GValue     *value,
                              GParamSpec *pspec)
{
    BbAuthPrompt *self = BB_AUTH_PROMPT (obj);

    switch (prop_id) {
    case PROP_TITLE:
        g_value_set_string (value, self->title);
        break;
    case PROP_MESSAGE:
        g_value_set_string (value, self->message);
        break;
    case PROP_DESCRIPTION:
        g_value_set_string (value, self->description);
        break;
    case PROP_WARNING:
        g_value_set_string (value, self->warning);
        break;
    case PROP_CHOICE_LABEL:
        g_value_set_string (value, self->choice_label);
        break;
    case PROP_CHOICE_CHOSEN:
        g_value_set_boolean (value, self->choice_chosen);
        break;
    case PROP_PASSWORD_NEW:
        g_value_set_boolean (value, self->password_new);
        break;
    case PROP_PASSWORD_STRENGTH:
        g_value_set_int (value, 0); /* Not implemented */
        break;
    case PROP_CALLER_WINDOW:
        g_value_set_string (value, self->caller_window);
        break;
    case PROP_CONTINUE_LABEL:
        g_value_set_string (value, self->continue_label);
        break;
    case PROP_CANCEL_LABEL:
        g_value_set_string (value, self->cancel_label);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (obj, prop_id, pspec);
    }
}

static void
bb_auth_prompt_finalize (GObject *obj)
{
    BbAuthPrompt *self = BB_AUTH_PROMPT (obj);

    g_free (self->title);
    g_free (self->message);
    g_free (self->description);
    g_free (self->warning);
    g_free (self->choice_label);
    g_free (self->caller_window);
    g_free (self->continue_label);
    g_free (self->cancel_label);
    g_free (self->password);
    g_free (self->request_cookie);

    G_OBJECT_CLASS (bb_auth_prompt_parent_class)->finalize (obj);
}

static gchar *
generate_cookie (BbAuthPrompt *self)
{
    self->cookie_counter++;
    return g_strdup_printf ("keyring-%ld-%d-%d",
                            (long)getpid (),
                            g_random_int (),
                            self->cookie_counter);
}

/* Thread function for async password prompt */
static void
password_request_thread (GTask        *task,
                         gpointer      source_object,
                         gpointer      task_data,
                         GCancellable *cancellable)
{
    BbAuthPrompt *self = BB_AUTH_PROMPT (source_object);
    gchar *password = NULL;
    gboolean success;

    g_message ("Thread: Sending keyring password request: cookie=%s",
               self->request_cookie);

    /* Send request to bb-auth - this blocks until user responds */
    success = bb_auth_ipc_send_keyring_request (
        self->request_cookie,
        self->title ? self->title : "Unlock Keyring",
        self->message ? self->message : "Password required",
        self->description,
        self->warning,
        self->password_new,
        &password
    );

    if (g_task_return_error_if_cancelled (task)) {
        g_free (password);
        return;
    }

    if (success && password != NULL) {
        g_free (self->password);
        self->password = password;
        self->cancelled = FALSE;
        g_message ("Thread: Keyring request successful");
        g_task_return_pointer (task, (gpointer)self->password, NULL);
    } else {
        self->cancelled = TRUE;
        g_free (password);
        g_message ("Thread: Keyring request cancelled or failed");
        g_task_return_pointer (task, NULL, NULL);
    }
}

/* Async password prompt implementation */
static void
bb_auth_prompt_password_async (GcrPrompt          *prompt,
                                GCancellable       *cancellable,
                                GAsyncReadyCallback callback,
                                gpointer            user_data)
{
    BbAuthPrompt *self = BB_AUTH_PROMPT (prompt);
    GTask *task;

    task = g_task_new (prompt, cancellable, callback, user_data);

    /* Generate a unique cookie for this request */
    g_free (self->request_cookie);
    self->request_cookie = generate_cookie (self);

    g_message ("Async: Starting keyring password request: cookie=%s title=%s message=%s warning=%s",
               self->request_cookie,
               self->title ? self->title : "(null)",
               self->message ? self->message : "(null)",
               self->warning ? self->warning : "(null)");

    /* Run the blocking IPC in a thread to not block the main loop */
    g_task_run_in_thread (task, password_request_thread);
    g_object_unref (task);
}

static const gchar *
bb_auth_prompt_password_finish (GcrPrompt    *prompt,
                                 GAsyncResult *result,
                                 GError      **error)
{
    g_return_val_if_fail (g_task_is_valid (result, prompt), NULL);

    return g_task_propagate_pointer (G_TASK (result), error);
}

/* Thread function for async confirm prompt */
static void
confirm_request_thread (GTask        *task,
                        gpointer      source_object,
                        gpointer      task_data,
                        GCancellable *cancellable)
{
    BbAuthPrompt *self = BB_AUTH_PROMPT (source_object);
    gboolean confirmed;

    g_message ("Thread: Sending keyring confirm request: cookie=%s",
               self->request_cookie);

    /* Send confirm request to bb-auth */
    confirmed = bb_auth_ipc_send_confirm_request (
        self->request_cookie,
        self->title ? self->title : "Confirm",
        self->message ? self->message : "Please confirm",
        self->description
    );

    if (g_task_return_error_if_cancelled (task))
        return;

    g_task_return_int (task, confirmed ? GCR_PROMPT_REPLY_CONTINUE : GCR_PROMPT_REPLY_CANCEL);
}

/* Async confirm prompt implementation */
static void
bb_auth_prompt_confirm_async (GcrPrompt          *prompt,
                               GCancellable       *cancellable,
                               GAsyncReadyCallback callback,
                               gpointer            user_data)
{
    BbAuthPrompt *self = BB_AUTH_PROMPT (prompt);
    GTask *task;

    task = g_task_new (prompt, cancellable, callback, user_data);

    /* Generate cookie */
    g_free (self->request_cookie);
    self->request_cookie = generate_cookie (self);

    g_message ("Async: Starting keyring confirm request: cookie=%s title=%s",
               self->request_cookie,
               self->title ? self->title : "(null)");

    /* Run the blocking IPC in a thread to not block the main loop */
    g_task_run_in_thread (task, confirm_request_thread);
    g_object_unref (task);
}

static GcrPromptReply
bb_auth_prompt_confirm_finish (GcrPrompt    *prompt,
                                GAsyncResult *result,
                                GError      **error)
{
    g_return_val_if_fail (g_task_is_valid (result, prompt), GCR_PROMPT_REPLY_CANCEL);

    return (GcrPromptReply)g_task_propagate_int (G_TASK (result), error);
}

static void
bb_auth_prompt_close (GcrPrompt *prompt)
{
    BbAuthPrompt *self = BB_AUTH_PROMPT (prompt);

    g_debug ("Closing prompt, cookie=%s", self->request_cookie ? self->request_cookie : "(null)");

    if (self->request_cookie) {
        bb_auth_ipc_send_cancel (self->request_cookie);
    }
}

static void
bb_auth_prompt_iface_init (GcrPromptInterface *iface)
{
    iface->prompt_password_async = bb_auth_prompt_password_async;
    iface->prompt_password_finish = bb_auth_prompt_password_finish;
    iface->prompt_confirm_async = bb_auth_prompt_confirm_async;
    iface->prompt_confirm_finish = bb_auth_prompt_confirm_finish;
    iface->prompt_close = bb_auth_prompt_close;
}

static void
bb_auth_prompt_class_init (BbAuthPromptClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->set_property = bb_auth_prompt_set_property;
    object_class->get_property = bb_auth_prompt_get_property;
    object_class->finalize = bb_auth_prompt_finalize;

    /* Install GcrPrompt interface properties */
    g_object_class_override_property (object_class, PROP_TITLE, "title");
    g_object_class_override_property (object_class, PROP_MESSAGE, "message");
    g_object_class_override_property (object_class, PROP_DESCRIPTION, "description");
    g_object_class_override_property (object_class, PROP_WARNING, "warning");
    g_object_class_override_property (object_class, PROP_CHOICE_LABEL, "choice-label");
    g_object_class_override_property (object_class, PROP_CHOICE_CHOSEN, "choice-chosen");
    g_object_class_override_property (object_class, PROP_PASSWORD_NEW, "password-new");
    g_object_class_override_property (object_class, PROP_PASSWORD_STRENGTH, "password-strength");
    g_object_class_override_property (object_class, PROP_CALLER_WINDOW, "caller-window");
    g_object_class_override_property (object_class, PROP_CONTINUE_LABEL, "continue-label");
    g_object_class_override_property (object_class, PROP_CANCEL_LABEL, "cancel-label");
}

static void
bb_auth_prompt_init (BbAuthPrompt *self)
{
    self->title = NULL;
    self->message = NULL;
    self->description = NULL;
    self->warning = NULL;
    self->choice_label = NULL;
    self->choice_chosen = FALSE;
    self->password_new = FALSE;
    self->caller_window = NULL;
    self->continue_label = g_strdup ("Unlock");
    self->cancel_label = g_strdup ("Cancel");
    self->password = NULL;
    self->request_cookie = NULL;
    self->cancelled = FALSE;
    self->cookie_counter = 0;
}

BbAuthPrompt *
bb_auth_prompt_new (void)
{
    return g_object_new (BB_AUTH_TYPE_PROMPT, NULL);
}
