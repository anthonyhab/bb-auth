#ifndef __IPC_CLIENT_H__
#define __IPC_CLIENT_H__

#include <glib.h>

G_BEGIN_DECLS

/* Check if noctalia-polkit socket is available */
gboolean noctalia_ipc_ping (void);

/* Send a keyring password request, blocks until response received
 * Returns TRUE on success with password in out_password (caller must free)
 * Returns FALSE on cancel or error */
gboolean noctalia_ipc_send_keyring_request (const gchar *cookie,
                                            const gchar *title,
                                            const gchar *message,
                                            const gchar *description,
                                            const gchar *warning,
                                            gboolean     password_new,
                                            gchar      **out_password);

/* Send a confirm request, blocks until response
 * Returns TRUE if confirmed, FALSE if cancelled */
gboolean noctalia_ipc_send_confirm_request (const gchar *cookie,
                                            const gchar *title,
                                            const gchar *message,
                                            const gchar *description);

/* Send cancel for a pending request */
void noctalia_ipc_send_cancel (const gchar *cookie);

G_END_DECLS

#endif /* __IPC_CLIENT_H__ */
