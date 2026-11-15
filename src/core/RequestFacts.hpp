#pragma once

#include <QJsonObject>
#include <QMap>
#include <QString>
#include <functional>
#include <optional>

#include "../common/ProcAgent.hpp"

namespace bb {

    // What a polkit request will actually do, as reported by the privileged
    // mechanism that asked (pkexec, systemd, ...), never by the requester. The
    // prompt builds its per-command screens from this; `context.intent` stays
    // the requester's claim beside it.
    struct RequestFactsInput {
        QString                                  actionId;
        QString                                  message;    // polkit's message, as rendered for the agent
        QMap<QString, QString>                   details;    // polkit details, as strings
        std::optional<ProcInfo>                  caller;     // polkit.caller-pid, read from /proc
        QString                                  subjectCwd; // requester's working directory, if readable
        std::function<QString(const QString &)>  homeForUser;
    };

    // Returns an empty object unless the caller is privileged (a user process
    // can put anything in polkit details) and, for pkexec, its argv reproduces
    // the command line pkexec itself reported, in its details or its message.
    QJsonObject buildRequestFacts(const RequestFactsInput &input);

} // namespace bb
