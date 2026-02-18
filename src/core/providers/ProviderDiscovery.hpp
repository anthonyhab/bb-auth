#pragma once

#include "ProviderManifest.hpp"

#include <QList>
#include <QString>
#include <QStringList>

namespace bb::providers {

    struct DiscoveryResult {
        QList<ProviderManifest> manifests;
        QStringList             warnings;
    };

    class ProviderDiscovery {
      public:
        static QStringList     defaultSearchDirs(const QString& systemDir = QString());
        static DiscoveryResult discover(const QStringList& searchDirs);
    };

} // namespace bb::providers
