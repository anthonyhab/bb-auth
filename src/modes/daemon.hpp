#pragma once

#include <QCoreApplication>
#include <QString>

namespace modes {

QString defaultSocketPath();
int runDaemon(QCoreApplication& app, const QString& socketPathOverride = {});

} // namespace modes
