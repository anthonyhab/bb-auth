#include "ProviderDiscovery.hpp"

#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QSet>

namespace bb::providers {

    namespace {

        QString normalizeDir(const QString& dir) {
            if (dir.isEmpty()) {
                return QString();
            }

            return QDir::cleanPath(dir);
        }

    } // namespace

    QStringList ProviderDiscovery::defaultSearchDirs(const QString& systemDir) {
        QStringList   dirs;

        const QString explicitDir = normalizeDir(QString::fromLocal8Bit(qgetenv("BB_AUTH_PROVIDER_DIR")));
        if (!explicitDir.isEmpty()) {
            dirs.push_back(explicitDir);
        }

        const QString configDir = normalizeDir(QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/bb-auth/providers.d");
        const QString dataDir   = normalizeDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/bb-auth/providers.d");

        if (!configDir.isEmpty() && !dirs.contains(configDir)) {
            dirs.push_back(configDir);
        }
        if (!dataDir.isEmpty() && !dirs.contains(dataDir)) {
            dirs.push_back(dataDir);
        }

        const QString normalizedSystemDir = normalizeDir(systemDir);
        if (!normalizedSystemDir.isEmpty() && !dirs.contains(normalizedSystemDir)) {
            dirs.push_back(normalizedSystemDir);
        }

        return dirs;
    }

    DiscoveryResult ProviderDiscovery::discover(const QStringList& searchDirs) {
        DiscoveryResult result;

        QSet<QString>   seenIds;

        for (const QString& rawDir : searchDirs) {
            const QString dirPath = normalizeDir(rawDir);
            if (dirPath.isEmpty()) {
                continue;
            }

            QDir dir(dirPath);
            if (!dir.exists()) {
                continue;
            }

            const QFileInfoList manifests = dir.entryInfoList(QStringList{"*.json"}, QDir::Files | QDir::Readable, QDir::Name);

            for (const QFileInfo& manifestInfo : manifests) {
                QFile file(manifestInfo.absoluteFilePath());
                if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                    result.warnings.push_back(QStringLiteral("Skipping manifest %1: cannot read file").arg(manifestInfo.absoluteFilePath()));
                    continue;
                }

                const auto parseResult = parseProviderManifest(file.readAll(), manifestInfo.absoluteFilePath());
                if (!parseResult.ok) {
                    result.warnings.push_back(QStringLiteral("Skipping manifest %1: %2").arg(manifestInfo.absoluteFilePath(), parseResult.error));
                    continue;
                }

                const ProviderManifest& manifest = parseResult.manifest;
                if (seenIds.contains(manifest.id)) {
                    result.warnings.push_back(QStringLiteral("Skipping manifest %1: duplicate id '%2' already selected from higher precedence directory")
                                                  .arg(manifestInfo.absoluteFilePath(), manifest.id));
                    continue;
                }

                seenIds.insert(manifest.id);
                result.manifests.push_back(manifest);
            }
        }

        return result;
    }

} // namespace bb::providers
