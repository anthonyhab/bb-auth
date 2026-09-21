#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace bb::providers {

    struct ProviderManifest {
        QString            id;
        QString            name;
        QString            kind;
        int                priority = 0;
        QString            exec;
        QStringList        args;
        QJsonObject        env;
        bool               autostart = true;
        // Resident providers are kept hot by eager launches (daemon startup,
        // post-disconnect). Non-resident providers still serve sessions but are
        // only launched on demand — they exit when idle instead of staying hot.
        bool               resident = true;
        QStringList        capabilities;
        QString            sourcePath;

        [[nodiscard]] bool isValid() const;
    };

    struct ParseResult {
        bool             ok = false;
        ProviderManifest manifest;
        QString          error;
    };

    ParseResult parseProviderManifest(const QByteArray& jsonBytes, const QString& sourcePath = QString());
    ParseResult parseProviderManifest(const QJsonObject& json, const QString& sourcePath = QString());

} // namespace bb::providers
