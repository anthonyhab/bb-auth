#pragma once

// sudo/doas -> pkexec translation, the one subset every channel shares:
// -n/--non-interactive drops (the supervised GUI prompt is the
// non-interactive path), -u/--user maps to pkexec --user, -- ends options.
// Other options, VAR=val, shell metacharacters, and probe-only invocations
// decline. tests/fixtures/escalation-translation.json pins the behavior.

#include <QString>
#include <QStringList>

namespace bb::agent {

    struct Translation {
        QStringList argv; // empty => declined
        QString     note; // what was changed, or why it declined
        bool        ok() const {
            return !argv.isEmpty();
        }
    };

    // argv-level (aisudo CLI, PATH shim): `launcher args...` -> pkexec argv.
    Translation translateArgv(const QString &launcher, const QStringList &args);

    // String-level (harness hook): a leading `sudo`/`doas` command line ->
    // pkexec command line, preserving the rest byte-for-byte (quoting
    // included). Empty when declined.
    QString rewriteCommandLine(const QString &command);

} // namespace bb::agent
