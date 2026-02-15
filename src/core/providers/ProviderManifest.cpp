#include "ProviderManifest.hpp"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>

namespace bb::providers {

    namespace {

        inline constexpr int      PRIORITY_MIN = -1000;
        inline constexpr int      PRIORITY_MAX = 1000;

        [[nodiscard]] ParseResult failure(const QString& error) {
            ParseResult result;
            result.ok    = false;
            result.error = error;
            return result;
        }

        [[nodiscard]] ParseResult success(const ProviderManifest& manifest) {
            ParseResult result;
            result.ok       = true;
            result.manifest = manifest;
            return result;
        }

        [[nodiscard]] bool isValidId(const QString& id) {
            static const QRegularExpression kPattern(QStringLiteral("^[a-z0-9][a-z0-9._-]*$"));
            return kPattern.match(id).hasMatch();
        }

        [[nodiscard]] bool isValidExec(const QString& exec) {
            if (exec.isEmpty()) {
                return false;
            }

            if (exec.contains('/')) {
                return QDir::isAbsolutePath(exec);
            }

            return true;
        }

        [[nodiscard]] bool parseStringArray(const QJsonValue& value, QStringList& out, const QString& fieldName, QString& error) {
            if (value.isUndefined() || value.isNull()) {
                out.clear();
                return true;
            }

            if (!value.isArray()) {
                error = QStringLiteral("%1 must be an array").arg(fieldName);
                return false;
            }

            const QJsonArray arr = value.toArray();
            out.clear();
            out.reserve(arr.size());
            for (const auto& entry : arr) {
                if (!entry.isString()) {
                    error = QStringLiteral("%1 must contain only strings").arg(fieldName);
                    return false;
                }
                out.push_back(entry.toString());
            }
            return true;
        }

        [[nodiscard]] bool parseEnvMap(const QJsonValue& value, QJsonObject& out, QString& error) {
            if (value.isUndefined() || value.isNull()) {
                out = QJsonObject{};
                return true;
            }

            if (!value.isObject()) {
                error = QStringLiteral("env must be an object of string values");
                return false;
            }

            const QJsonObject input = value.toObject();
            QJsonObject       envObj;

            for (auto it = input.begin(); it != input.end(); ++it) {
                if (!it.value().isString()) {
                    error = QStringLiteral("env values must be strings");
                    return false;
                }
                envObj.insert(it.key(), it.value().toString());
            }

            out = envObj;
            return true;
        }

    } // namespace

    bool ProviderManifest::isValid() const {
        return !id.isEmpty() && isValidId(id) && !name.isEmpty() && !kind.isEmpty() && priority >= PRIORITY_MIN && priority <= PRIORITY_MAX && isValidExec(exec);
    }

    ParseResult parseProviderManifest(const QByteArray& jsonBytes, const QString& sourcePath) {
        QJsonParseError parseError;
        const auto      doc = QJsonDocument::fromJson(jsonBytes, &parseError);
        if (parseError.error != QJsonParseError::NoError) {
            return failure(QStringLiteral("invalid JSON: %1").arg(parseError.errorString()));
        }

        if (!doc.isObject()) {
            return failure(QStringLiteral("manifest root must be an object"));
        }

        return parseProviderManifest(doc.object(), sourcePath);
    }

    ParseResult parseProviderManifest(const QJsonObject& json, const QString& sourcePath) {
        ProviderManifest manifest;
        manifest.sourcePath = sourcePath;

        manifest.id   = json.value("id").toString().trimmed();
        manifest.name = json.value("name").toString().trimmed();
        manifest.kind = json.value("kind").toString().trimmed();
        manifest.exec = json.value("exec").toString().trimmed();

        if (json.contains("priority") && !json.value("priority").isDouble()) {
            return failure(QStringLiteral("priority must be an integer"));
        }
        manifest.priority = json.value("priority").toInt();

        if (json.contains("autostart") && !json.value("autostart").isBool()) {
            return failure(QStringLiteral("autostart must be a boolean"));
        }
        manifest.autostart = json.contains("autostart") ? json.value("autostart").toBool() : true;

        QString parseError;
        if (!parseStringArray(json.value("args"), manifest.args, QStringLiteral("args"), parseError)) {
            return failure(parseError);
        }

        if (!parseEnvMap(json.value("env"), manifest.env, parseError)) {
            return failure(parseError);
        }

        if (!parseStringArray(json.value("capabilities"), manifest.capabilities, QStringLiteral("capabilities"), parseError)) {
            return failure(parseError);
        }

        if (manifest.id.isEmpty()) {
            return failure(QStringLiteral("id is required"));
        }
        if (!isValidId(manifest.id)) {
            return failure(QStringLiteral("id must match [a-z0-9][a-z0-9._-]*"));
        }
        if (manifest.name.isEmpty()) {
            return failure(QStringLiteral("name is required"));
        }
        if (manifest.kind.isEmpty()) {
            return failure(QStringLiteral("kind is required"));
        }
        if (manifest.priority < PRIORITY_MIN || manifest.priority > PRIORITY_MAX) {
            return failure(QStringLiteral("priority must be within [-1000, 1000]"));
        }
        if (manifest.exec.isEmpty()) {
            return failure(QStringLiteral("exec is required"));
        }
        if (!isValidExec(manifest.exec)) {
            return failure(QStringLiteral("exec must be absolute path or basename"));
        }

        return success(manifest);
    }

} // namespace bb::providers
