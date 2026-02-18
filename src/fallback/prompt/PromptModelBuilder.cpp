#include "PromptModelBuilder.hpp"

#include "PromptExtractors.hpp"
#include "PromptHeuristics.hpp"
#include "TextNormalize.hpp"

#include <QRegularExpression>

namespace bb::fallback::prompt {

    namespace {

        bool isLowSignalCommand(const QString& commandName) {
            const QString normalized = commandName.trimmed().toLower();
            if (normalized.isEmpty()) {
                return true;
            }

            static const QStringList lowSignal = {QStringLiteral("true"), QStringLiteral("sh"), QStringLiteral("bash")};
            return lowSignal.contains(normalized);
        }

        bool isIdentityLine(const QString& line) {
            return line.contains('"') && line.contains('<') && line.contains('>');
        }

        bool isKeyMetadataLine(const QString& line) {
            const QString lower = line.toLower();
            return (lower.contains(" id ") || lower.startsWith("id ")) && lower.contains("created");
        }

        QString captureFirst(const QString& text, const QRegularExpression& regex) {
            const QRegularExpressionMatch match = regex.match(text);
            if (!match.hasMatch()) {
                return QString();
            }

            return match.captured(1).trimmed();
        }

        QString cleanIdentity(QString identity) {
            identity = identity.simplified();
            identity.replace(" (github)", "", Qt::CaseInsensitive);
            return identity.trimmed();
        }

        QString humanizeActionId(QString actionId) {
            actionId = actionId.trimmed();
            if (actionId.isEmpty()) {
                return QString();
            }

            const qsizetype lastDot = actionId.lastIndexOf('.');
            if (lastDot >= 0 && (lastDot + 1) < actionId.size()) {
                actionId = actionId.mid(lastDot + 1);
            }

            actionId.replace('-', ' ');
            actionId.replace('_', ' ');
            actionId.replace('/', ' ');
            actionId = actionId.simplified();
            if (actionId.isEmpty()) {
                return QString();
            }

            QStringList words = actionId.split(' ', Qt::SkipEmptyParts);
            for (QString& word : words) {
                word[0] = word[0].toUpper();
            }
            return words.join(" ");
        }

        bool isGenericAuthSummary(const QString& summary) {
            const QString normalized = normalizeCompareText(summary);
            if (normalized.isEmpty()) {
                return true;
            }

            static const QStringList genericSummaries = {
                QStringLiteral("authentication is required"),
                QStringLiteral("authentication required"),
                QStringLiteral("authorization is required"),
                QStringLiteral("authorization required"),
                QStringLiteral("authenticate to continue"),
                QStringLiteral("authentication is required to continue"),
                QStringLiteral("authorization is required to continue"),
            };

            return genericSummaries.contains(normalized);
        }

    } // namespace

    PromptDisplayModel PromptModelBuilder::build(const QJsonObject& event) const {
        PromptDisplayModel model;
        const QString      source                = event.value("source").toString();
        const QJsonObject  context               = event.value("context").toObject();
        const QJsonObject  requestor             = context.value("requestor").toObject();
        const QString      message               = context.value("message").toString();
        const QString      description           = context.value("description").toString();
        const QString      requestorName         = requestor.value("name").toString().trimmed();
        const qint64       requestorPid          = requestor.value("pid").toInteger();
        const QString      actionId              = context.value("actionId").toString().trimmed();
        const QString      actionSummary         = (source == "polkit") ? humanizeActionId(actionId) : QString();
        const QString      actionUser            = context.value("user").toString().trimmed();
        const QString      infoText              = normalizeDetailText(event.value("info").toString());
        const QString      livePromptText        = normalizeDetailText(event.value("prompt").toString());
        const QString      normalizedMessage     = normalizeDetailText(message);
        const QString      normalizedDescription = normalizeDetailText(description);
        const QString      detailText            = (normalizedDescription + " " + normalizedMessage).toLower();
        const QString      authHintText          = (detailText + " " + infoText + " " + livePromptText).toLower();
        const QString      commandName           = (source == "polkit") ? extractCommandName(message) : QString();
        QString            unlockTarget          = (source == "polkit" || source == "keyring") ? extractUnlockTargetFromContext(context) : QString();
        const bool         fingerprintHint       = looksLikeFingerprintPrompt(authHintText);
        const bool         fidoHint              = looksLikeFidoPrompt(authHintText);
        const bool         touchHint             = fingerprintHint || fidoHint || looksLikeTouchPrompt(authHintText);
        if (source == "keyring" && unlockTarget.isEmpty()) {
            unlockTarget = requestorName;
        }
        if (source == "polkit" && fingerprintHint) {
            model.intent = PromptIntent::Fingerprint;
        } else if (source == "polkit" && fidoHint) {
            model.intent = PromptIntent::Fido2;
        } else if (source == "pinentry" && (detailText.contains("openpgp") || detailText.contains("gpg"))) {
            model.intent = PromptIntent::OpenPgp;
        } else if (source == "polkit" && !commandName.isEmpty()) {
            model.intent = PromptIntent::RunCommand;
        } else if ((source == "polkit" || source == "keyring") && !unlockTarget.isEmpty()) {
            model.intent = PromptIntent::Unlock;
        }
        if (model.intent == PromptIntent::Unlock) {
            model.title   = QString("Unlock %1").arg(unlockTarget);
            model.summary = QString("Use your password to unlock %1").arg(unlockTarget);
            model.details = buildUnlockDetails(context, unlockTarget);
        } else if (model.intent == PromptIntent::Fingerprint) {
            model.title   = QString("Verify Fingerprint");
            model.summary = infoText.isEmpty() ? QString("Use your fingerprint sensor to continue") : firstMeaningfulLine(infoText);
            model.details = normalizeDetailText(description);
        } else if (model.intent == PromptIntent::Fido2) {
            model.title   = QString("Use Security Key");
            model.summary = infoText.isEmpty() ? QString("Touch your security key to continue") : firstMeaningfulLine(infoText);
            model.details = normalizeDetailText(description);
        } else if (model.intent == PromptIntent::RunCommand) {
            model.title   = QString("Authorization Required");
            model.summary = firstMeaningfulLine(normalizedDescription);
            if (model.summary.isEmpty()) {
                model.summary = firstMeaningfulLine(normalizedMessage);
            }
            if (model.summary.isEmpty()) {
                model.summary = isLowSignalCommand(commandName) ? QString("Administrative privileges required") : QString("Run %1 as superuser").arg(commandName);
            }
            model.details.clear();
        } else if (source == "pinentry") {
            if (model.intent == PromptIntent::OpenPgp) {
                model.title = QString("Unlock OpenPGP Key");
            } else if (detailText.contains("ssh")) {
                model.title = QString("Unlock SSH Key");
            } else {
                model.title = QString("Authentication Required");
            }
            const QString referenceText = description.isEmpty() ? message : description;
            const QString identity      = cleanIdentity(captureFirst(referenceText, QRegularExpression(QStringLiteral("\"([^\"]+)\""))));
            const QString keyId         = captureFirst(referenceText, QRegularExpression(R"(ID\s+([A-F0-9]{8,}))", QRegularExpression::CaseInsensitiveOption));
            const QString keyType       = captureFirst(referenceText, QRegularExpression(R"((\d{3,5}-bit\s+[A-Za-z0-9-]+\s+key))", QRegularExpression::CaseInsensitiveOption));
            const QString created       = captureFirst(referenceText, QRegularExpression(R"(created\s+([0-9]{4}-[0-9]{2}-[0-9]{2}))", QRegularExpression::CaseInsensitiveOption));
            QStringList   pieces;
            if (!identity.isEmpty()) {
                pieces << trimToLength(identity, 72);
            } else if (!keyType.isEmpty()) {
                pieces << keyType;
            }
            if (!keyId.isEmpty()) {
                pieces << keyId;
            }
            if (!created.isEmpty()) {
                pieces << ("created " + created);
            }
            if (!pieces.isEmpty()) {
                model.summary = pieces.join("  •  ");
            } else {
                model.summary = firstMeaningfulLine(referenceText);
            }
            const QString pinText = normalizeDetailText(description.isEmpty() ? message : description);
            if (!pinText.isEmpty()) {
                QStringList       filtered;
                const QStringList lines = pinText.split('\n');
                filtered.reserve(lines.size());
                for (const QString& line : lines) {
                    if (isIdentityLine(line) || isKeyMetadataLine(line)) {
                        continue;
                    }
                    filtered << line;
                }
                model.details = filtered.isEmpty() ? pinText : filtered.join("\n");
            }
        } else {
            model.title   = (source == "polkit") ? QString("Authorization Required") : QString("Authentication Required");
            model.summary = firstMeaningfulLine(normalizedMessage);
            if (model.summary.isEmpty()) {
                model.summary = firstMeaningfulLine(normalizedDescription);
            }
            if (!normalizedDescription.isEmpty() && !textEquivalent(normalizedDescription, model.summary)) {
                model.details = normalizedDescription;
            } else if (!normalizedMessage.isEmpty() && !textEquivalent(normalizedMessage, model.summary)) {
                model.details = normalizedMessage;
            }
        }
        if (!requestorName.isEmpty()) {
            const bool duplicateUnlockRequestor = (model.intent == PromptIntent::Unlock) && (requestorName.compare(unlockTarget, Qt::CaseInsensitive) == 0);
            if (!duplicateUnlockRequestor) {
                const bool weakIdentity = (source == "polkit") && (requestorName.compare("unknown", Qt::CaseInsensitive) == 0) && (requestorPid > 0);
                model.requestor         = weakIdentity ? QString("Requested by process %1").arg(requestorPid) : QString("Requested by %1").arg(requestorName);
            }
        } else if ((source == "polkit") && (requestorPid > 0)) {
            model.requestor = QString("Requested by process %1").arg(requestorPid);
        }
        if (model.summary.isEmpty() && !model.details.isEmpty()) {
            const QString normalizedDetails = normalizeDetailText(model.details);
            const qsizetype newline         = normalizedDetails.indexOf('\n');
            if (newline == -1) {
                model.summary = normalizedDetails;
                model.details.clear();
            } else {
                model.summary = normalizedDetails.left(newline).trimmed();
                model.details = normalizedDetails.mid(newline + 1).trimmed();
            }
        }
        if (!model.summary.isEmpty() && !model.details.isEmpty()) {
            const QString normalizedDetails = normalizeDetailText(model.details);
            QStringList   detailLines       = normalizedDetails.split('\n');
            if (!detailLines.isEmpty() && textEquivalent(detailLines.first(), model.summary)) {
                detailLines.removeFirst();
                model.details = detailLines.join("\n").trimmed();
            }
            if (textEquivalent(model.summary, model.details)) {
                model.details.clear();
            }
        }
        if (!infoText.isEmpty() && !textEquivalent(infoText, model.summary) && !textEquivalent(infoText, model.details)) {
            model.details = model.details.isEmpty() ? infoText : uniqueJoined(QStringList{model.details, infoText});
        }
        if (source == "polkit") {
            if (isGenericAuthSummary(model.summary) && !actionSummary.isEmpty()) {
                model.summary = actionSummary;
            }

            QStringList actionLines;
            if (!actionSummary.isEmpty()) {
                actionLines << QString("Action: %1").arg(actionSummary);
            }
            if (!actionId.isEmpty() && actionId.compare(actionSummary, Qt::CaseInsensitive) != 0) {
                actionLines << QString("Policy: %1").arg(actionId);
            }
            if (!actionUser.isEmpty()) {
                actionLines << QString("Authenticate as %1").arg(actionUser);
            }

            const QString actionDetails = uniqueJoined(actionLines);
            if (!actionDetails.isEmpty()) {
                model.details = model.details.isEmpty() ? actionDetails : uniqueJoined(QStringList{model.details, actionDetails});
            }
        }
        if (source == "pinentry") {
            const QString eventPrompt = event.value("prompt").toString().trimmed();
            const QString pinPrompt   = eventPrompt.isEmpty() ? context.value("message").toString().trimmed() : eventPrompt;
            model.prompt            = pinPrompt.isEmpty() ? QString("Passphrase:") : pinPrompt;
        } else {
            model.prompt = QString("Password:");
            if (source == "polkit" && touchHint) {
                model.prompt             = QString("Press Enter to continue (or wait)");
                model.allowEmptyResponse = true;
            }
        }
        model.passphrasePrompt = (source == "pinentry") || model.prompt.contains("passphrase", Qt::CaseInsensitive);
        if (source == "polkit" && touchHint) {
            model.passphrasePrompt = false;
        }
        return model;
    }

} // namespace bb::fallback::prompt
