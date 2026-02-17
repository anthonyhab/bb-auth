#include "pinentry.hpp"

#include "../common/Constants.hpp"
#include "../common/IpcClient.hpp"
#include "../common/Paths.hpp"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QUuid>

#include <cstring>
#include <iostream>
#include <print>
#include <string>

namespace {

    // Assuan percent-decoding
    QString assuanDecode(const QString& input) {
        QString result;
        result.reserve(input.size());

        for (int i = 0; i < input.size(); ++i) {
            if (input[i] == '%' && i + 2 < input.size()) {
                bool ok;
                int  code = input.mid(i + 1, 2).toInt(&ok, 16);
                if (ok) {
                    result += QChar(code);
                    i += 2;
                    continue;
                }
            }
            result += input[i];
        }
        return result;
    }

    // Assuan percent-encoding for data response
    QString assuanEncode(const QString& input) {
        QString result;
        result.reserve(input.size() * 3);

        static const char hexChars[] = "0123456789ABCDEF";

        for (const QChar& ch : input) {
            char c = ch.toLatin1();
            if (c == '%' || c == '\n' || c == '\r') {
                result.append('%');
                unsigned char uc = static_cast<unsigned char>(c);
                result.append(hexChars[(uc >> 4) & 0xF]);
                result.append(hexChars[uc & 0xF]);
            } else {
                result.append(ch);
            }
        }
        return result;
    }

    struct PinentryState {
        QString description;
        QString prompt;
        QString title;
        QString error;
        QString okText;
        QString cancelText;
        QString notOkText;
        QString keyinfo;
        QString repeat;
        bool    confirmMode = false;
    };

    class PinentrySession {
      public:
        PinentrySession() = default;

        int run() {
            // Send initial greeting
            sendOk("BB Auth Pinentry");

            std::string line;
            while (std::getline(std::cin, line)) {
                if (line.empty())
                    continue;

                // Remove trailing \r if present (Windows line endings)
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();

                if (!handleCommand(QString::fromStdString(line)))
                    break;
            }

            finalizeOnStreamClose();

            return 0;
        }

      private:
        PinentryState state;
        QString       flowCookie;
        bool          awaitingTerminalResult = false;

        QString       ensureFlowCookie() {
            if (flowCookie.isEmpty()) {
                flowCookie = QUuid::createUuid().toString(QUuid::WithoutBraces);
            }
            return flowCookie;
        }

        void clearSubmitState() {
            awaitingTerminalResult = false;
        }

        void resetFlow() {
            clearSubmitState();
            flowCookie.clear();
        }

        void finalizeOnStreamClose() {
            if (awaitingTerminalResult) {
                if (!state.error.isEmpty()) {
                    reportTerminalResult("error", state.error);
                } else {
                    reportTerminalResult("success");
                }
                return;
            }

            if (!flowCookie.isEmpty()) {
                if (!state.error.isEmpty()) {
                    reportTerminalResult("error", state.error);
                } else {
                    reportTerminalResult("cancelled");
                }
            }
        }

        void reportTerminalResult(const QString& result, const QString& error = {}) {
            if (flowCookie.isEmpty()) {
                return;
            }

            bb::IpcClient client(bb::socketPath());
            QJsonObject   request;
            request["type"]   = "pinentry_result";
            request["id"]     = flowCookie;
            request["result"] = result;
            if (!error.isEmpty()) {
                request["error"] = error;
            }

            auto response = client.sendRequest(request, bb::IPC_READ_TIMEOUT_MS);
            if (!response || (*response)["type"].toString() == "error") {
                std::print(stderr, "pinentry: failed to report terminal result for cookie {}\n", flowCookie.toStdString());
            }

            if (result == "retry") {
                clearSubmitState();
            } else {
                resetFlow();
            }
        }

        void sendOk(const QString& comment = {}) {
            if (comment.isEmpty())
                std::cout << "OK\n";
            else
                std::cout << "OK " << comment.toStdString() << "\n";
            std::cout.flush();
        }

        void sendError(int code, const QString& message) {
            std::cout << "ERR " << code << " " << message.toStdString() << "\n";
            std::cout.flush();
        }

        void sendData(const QString& data) {
            std::cout << "D " << assuanEncode(data).toStdString() << "\n";
            std::cout.flush();
        }

        bool handleCommand(const QString& line) {
            // Split command and argument
            qsizetype spaceIdx = line.indexOf(' ');
            QString   cmd      = (spaceIdx > 0) ? line.left(spaceIdx).toUpper() : line.toUpper();
            QString   arg      = (spaceIdx > 0) ? assuanDecode(line.mid(static_cast<int>(spaceIdx + 1))) : QString();

            if (cmd == "BYE") {
                if (awaitingTerminalResult) {
                    if (!state.error.isEmpty()) {
                        reportTerminalResult("error", state.error);
                    } else {
                        reportTerminalResult("success");
                    }
                } else if (!flowCookie.isEmpty()) {
                    if (!state.error.isEmpty()) {
                        reportTerminalResult("error", state.error);
                    } else {
                        reportTerminalResult("cancelled");
                    }
                }
                sendOk("closing connection");
                return false;
            }

            if (cmd == "SETDESC") {
                state.description = arg;
                sendOk();
                return true;
            }

            if (cmd == "SETPROMPT") {
                state.prompt = arg;
                sendOk();
                return true;
            }

            if (cmd == "SETTITLE") {
                state.title = arg;
                sendOk();
                return true;
            }

            if (cmd == "SETERROR") {
                state.error = arg;
                sendOk();
                return true;
            }

            if (cmd == "SETOK") {
                state.okText = arg;
                sendOk();
                return true;
            }

            if (cmd == "SETCANCEL") {
                state.cancelText = arg;
                sendOk();
                return true;
            }

            if (cmd == "SETNOTOK") {
                state.notOkText = arg;
                sendOk();
                return true;
            }

            if (cmd == "SETKEYINFO") {
                state.keyinfo = arg;
                sendOk();
                return true;
            }

            if (cmd == "SETREPEAT") {
                state.repeat = arg;
                sendOk();
                return true;
            }

            if (cmd == "OPTION") {
                // Options like "ttyname", "ttytype", "lc-ctype", etc.
                // We acknowledge but don't use them
                sendOk();
                return true;
            }

            if (cmd == "GETINFO") {
                // Return info about this pinentry
                if (arg == "pid") {
                    sendData(QString::number(getpid()));
                    sendOk();
                } else if (arg == "version") {
                    sendData("1.0.0");
                    sendOk();
                } else if (arg == "flavor") {
                    sendData("bb");
                    sendOk();
                } else if (arg == "ttyinfo") {
                    sendData("");
                    sendOk();
                } else {
                    sendOk();
                }
                return true;
            }

            if (cmd == "GETPIN") {
                return handleGetPin();
            }

            if (cmd == "CONFIRM") {
                return handleConfirm();
            }

            if (cmd == "MESSAGE") {
                return handleMessage();
            }

            if (cmd == "RESET") {
                state = PinentryState{};
                sendOk();
                return true;
            }

            if (cmd == "NOP") {
                sendOk();
                return true;
            }

            // Unknown command - still OK per Assuan spec
            sendOk();
            return true;
        }

        bool handleGetPin() {
            if (awaitingTerminalResult) {
                const QString retryError = state.error.isEmpty() ? QString("Authentication failed") : state.error;
                reportTerminalResult("retry", retryError);
            }

            QString password;
            bool    success = requestPasswordFromDaemon(password);

            if (success && !password.isEmpty()) {
                sendData(password);
                // Securely clear password string
                password.fill('\0');
                sendOk();
            } else {
                // User cancelled or error - use Operation cancelled error code
                sendError(83886179, "Operation cancelled");
            }

            // Clear state for next request
            state.error.clear();
            return true;
        }

        bool handleConfirm() {
            bool confirmed = requestConfirmFromDaemon();

            if (confirmed) {
                sendOk();
            } else {
                sendError(83886179, "Operation cancelled");
            }

            state.error.clear();
            return true;
        }

        bool handleMessage() {
            // MESSAGE just shows the description and waits for OK
            // For now, we just acknowledge it
            sendOk();
            return true;
        }

        bool requestPasswordFromDaemon(QString& password) {
            bb::IpcClient client(bb::socketPath());

            const QString cookie = ensureFlowCookie();

            // Build request JSON
            QJsonObject request;
            request["type"]        = "pinentry_request";
            request["cookie"]      = cookie;
            request["title"]       = state.title.isEmpty() ? "GPG Key" : state.title;
            request["prompt"]      = state.prompt.isEmpty() ? "Enter passphrase:" : state.prompt;
            request["description"] = state.description;
            request["repeat"]      = !state.repeat.isEmpty();

            if (!state.error.isEmpty()) {
                request["error"] = state.error;
            }

            if (!state.keyinfo.isEmpty())
                request["keyinfo"] = state.keyinfo;

            auto response = client.sendRequest(request, bb::PINENTRY_REQUEST_TIMEOUT_MS);

            if (!response) {
                std::print(stderr, "pinentry: failed to communicate with daemon\n");
                resetFlow();
                return false;
            }

            QString type = (*response)["type"].toString();

            if (type == "pinentry_response") {
                QString result = (*response)["result"].toString();
                if (result == "ok") {
                    password               = (*response)["password"].toString();
                    awaitingTerminalResult = true;
                    return true;
                }
                // cancelled or error
                resetFlow();
                return false;
            }

            if (type == "error") {
                std::print(stderr, "pinentry: daemon error: {}\n", (*response)["error"].toString().toStdString());
                resetFlow();
                return false;
            }

            resetFlow();
            return false;
        }

        bool requestConfirmFromDaemon() {
            bb::IpcClient client(bb::socketPath());

            const QString cookie = ensureFlowCookie();

            QJsonObject   request;
            request["type"]         = "pinentry_request";
            request["cookie"]       = cookie;
            request["title"]        = state.title.isEmpty() ? "Confirm" : state.title;
            request["prompt"]       = state.description.isEmpty() ? "Please confirm" : state.description;
            request["confirm_only"] = true;

            auto response = client.sendRequest(request, bb::PINENTRY_REQUEST_TIMEOUT_MS);

            if (!response) {
                resetFlow();
                return false;
            }

            const bool confirmed = (*response)["type"].toString() == "pinentry_response" && (*response)["result"].toString() == "confirmed";
            if (confirmed) {
                awaitingTerminalResult = true;
            } else {
                resetFlow();
            }

            return confirmed;
        }
    };

} // namespace

namespace modes {

    int runPinentry() {
        PinentrySession session;
        return session.run();
    }

} // namespace modes
