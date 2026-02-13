#include "FallbackWindow.hpp"

#include <QCloseEvent>
#include <QAction>
#include <QCoreApplication>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QKeySequence>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSizePolicy>
#include <QTimer>
#include <QVBoxLayout>

namespace {

    QString normalizeDetailText(const QString& text) {
        QString normalized = text;
        normalized.replace('\r', '\n');
        QStringList lines = normalized.split('\n');
        QStringList cleaned;
        cleaned.reserve(lines.size());
        for (const QString& line : lines) {
            const QString simplified = line.simplified();
            if (!simplified.isEmpty()) {
                cleaned << simplified;
            }
        }

        return cleaned.join("\n");
    }

    QString normalizeCompareText(const QString& text) {
        QString normalized = normalizeDetailText(text).toLower();
        normalized.replace('`', ' ');
        normalized.replace('"', ' ');
        normalized.replace(',', ' ');
        normalized.replace('.', ' ');
        normalized = normalized.simplified();
        return normalized;
    }

    bool textEquivalent(const QString& left, const QString& right) {
        const QString a = normalizeCompareText(left);
        const QString b = normalizeCompareText(right);
        if (a.isEmpty() || b.isEmpty()) {
            return false;
        }

        return a == b || a.startsWith(b) || b.startsWith(a);
    }

    QString firstMeaningfulLine(const QString& text) {
        const QString cleaned = normalizeDetailText(text);
        if (cleaned.isEmpty()) {
            return QString();
        }

        const int newline = cleaned.indexOf('\n');
        if (newline == -1) {
            return cleaned;
        }

        return cleaned.left(newline);
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

    QString trimToLength(const QString& text, int maxChars) {
        if (text.size() <= maxChars) {
            return text;
        }

        return text.left(maxChars - 3).trimmed() + "...";
    }

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

    bool containsAnyTerm(const QString& text, std::initializer_list<const char*> terms) {
        const QString lower = text.toLower();
        for (const char* term : terms) {
            if (lower.contains(QString::fromLatin1(term))) {
                return true;
            }
        }
        return false;
    }

    bool looksLikeFingerprintPrompt(const QString& text) {
        return containsAnyTerm(text, {"fingerprint", "finger print", "fprint", "swipe", "scan your finger"});
    }

    bool looksLikeFidoPrompt(const QString& text) {
        return containsAnyTerm(text, {"fido", "fido2", "webauthn", "security key", "yubikey", "hardware token", "user presence"});
    }

    bool looksLikeTouchPrompt(const QString& text) {
        return containsAnyTerm(text, {"touch", "tap", "insert", "use your security key", "verify your identity"});
    }

    QString extractCommandName(const QString& message) {
        const QRegularExpression explicitRunRegex(R"(run\s+[`'"]([^`'"\s]+)[`'"])", QRegularExpression::CaseInsensitiveOption);
        QString                  command = captureFirst(message, explicitRunRegex);

        if (command.isEmpty()) {
            const QRegularExpression pathRegex(R"((/[A-Za-z0-9_\-\./]+))");
            command = captureFirst(message, pathRegex);
        }

        if (command.isEmpty()) {
            return QString();
        }

        const QString commandName = QFileInfo(command).fileName();
        return commandName.isEmpty() ? command : commandName;
    }

    QString extractUnlockTarget(const QString& text) {
        const QString normalized = normalizeDetailText(text);
        if (normalized.isEmpty()) {
            return QString();
        }

        const QRegularExpression unlockRegex(R"(unlock\s+([^\n]+))", QRegularExpression::CaseInsensitiveOption);
        QString                  target = captureFirst(normalized, unlockRegex);
        if (target.isEmpty()) {
            return QString();
        }

        target = target.trimmed();
        if (target.endsWith('.')) {
            target.chop(1);
        }

        return target.trimmed();
    }

    QString uniqueJoined(const QStringList& values) {
        QStringList filtered;
        filtered.reserve(values.size());
        for (const QString& value : values) {
            const QString simplified = value.trimmed();
            if (simplified.isEmpty()) {
                continue;
            }

            bool duplicate = false;
            for (const QString& existing : filtered) {
                if (textEquivalent(existing, simplified)) {
                    duplicate = true;
                    break;
                }
            }

            if (!duplicate) {
                filtered << simplified;
            }
        }

        return filtered.join("\n");
    }

    QString extractUnlockTargetFromContext(const QJsonObject& context) {
        QString target = extractUnlockTarget(context.value("keyringName").toString());
        if (target.isEmpty()) {
            target = extractUnlockTarget(context.value("message").toString());
        }
        if (target.isEmpty()) {
            target = extractUnlockTarget(context.value("description").toString());
        }
        return target;
    }

    bool isTemplateUnlockLine(const QString& line, const QString& target) {
        const QString normalized = normalizeCompareText(line);
        if (normalized.isEmpty()) {
            return true;
        }

        const QString normalizedTarget = normalizeCompareText(target);
        if (!normalizedTarget.isEmpty() && normalized == normalizedTarget) {
            return true;
        }

        if (!normalizedTarget.isEmpty() && normalized.contains("unlock") && normalized.contains(normalizedTarget)) {
            if (normalized.startsWith("authenticate to unlock") || normalized.startsWith("unlock") || normalized.startsWith("use your password to unlock") ||
                normalized.startsWith("use your account password to unlock")) {
                return true;
            }
        }

        return false;
    }

    QString buildUnlockDetails(const QJsonObject& context, const QString& target) {
        QStringList       details;

        const QStringList candidates = {normalizeDetailText(context.value("description").toString()), normalizeDetailText(context.value("message").toString()),
                                        normalizeDetailText(context.value("keyringName").toString())};

        for (const QString& candidate : candidates) {
            if (candidate.isEmpty()) {
                continue;
            }

            const QStringList lines = candidate.split('\n');
            for (const QString& rawLine : lines) {
                const QString line = rawLine.trimmed();
                if (line.isEmpty() || isTemplateUnlockLine(line, target)) {
                    continue;
                }
                details << line;
            }
        }

        return uniqueJoined(details);
    }

    QPair<QString, bool> collapseDetailText(const QString& text, int maxLines, int maxChars) {
        if (text.isEmpty()) {
            return qMakePair(QString(), false);
        }

        const QStringList lines = text.split('\n');
        QStringList       collapsedLines;
        collapsedLines.reserve(lines.size());

        int  usedChars = 0;
        bool truncated = false;

        for (const QString& line : lines) {
            if (collapsedLines.size() >= maxLines) {
                truncated = true;
                break;
            }

            QString clipped = line;
            if ((usedChars + clipped.size()) > maxChars) {
                const int remaining = qMax(0, maxChars - usedChars);
                clipped             = remaining > 0 ? clipped.left(remaining).trimmed() : QString();
                if (!clipped.isEmpty()) {
                    collapsedLines << clipped;
                }
                truncated = true;
                break;
            }

            collapsedLines << clipped;
            usedChars += clipped.size();
        }

        if (lines.size() > collapsedLines.size()) {
            truncated = true;
        }

        QString collapsed = collapsedLines.join("\n");
        if (truncated && !collapsed.isEmpty() && !collapsed.endsWith("...")) {
            collapsed += "...";
        }

        return qMakePair(collapsed, truncated);
    }

} // namespace

namespace bb {

    FallbackWindow::FallbackWindow(FallbackClient* client, QWidget* parent) : QWidget(parent), m_client(client) {

        setWindowTitle("Authentication Required");
        setObjectName("BBAuthFallback");
        setWindowFlags(Qt::Window | Qt::WindowTitleHint | Qt::WindowCloseButtonHint);
        configureSizingForIntent(PromptIntent::Generic);
        resize(m_baseWidth, m_baseHeight);

        auto* rootLayout = new QVBoxLayout(this);
        rootLayout->setContentsMargins(20, 20, 20, 20);
        rootLayout->setSpacing(0);

        m_contentWidget = new QWidget(this);
        m_contentWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
        m_contentWidget->setMaximumWidth(680);
        rootLayout->addWidget(m_contentWidget, 0);
        auto* layout = new QVBoxLayout(m_contentWidget);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        auto* headerLayout = new QVBoxLayout();
        headerLayout->setContentsMargins(0, 0, 0, 0);
        headerLayout->setSpacing(10);
        auto* promptLayout = new QVBoxLayout();
        promptLayout->setContentsMargins(0, 0, 0, 0);
        promptLayout->setSpacing(10);
        m_titleLabel = new QLabel("Authentication Required", m_contentWidget);
        m_titleLabel->setStyleSheet("font-weight: 700; font-size: 17px;");
        m_summaryLabel = new QLabel(m_contentWidget);
        m_summaryLabel->setWordWrap(true);
        m_summaryLabel->setStyleSheet("font-weight: 600; color: #d1d7d3; font-size: 14px;");
        m_summaryLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        m_summaryLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
        m_summaryLabel->hide();
        m_requestorLabel = new QLabel(m_contentWidget);
        m_requestorLabel->setWordWrap(true);
        m_requestorLabel->setStyleSheet("color: #aeb6b1; font-size: 13px;");
        m_requestorLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        m_requestorLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
        m_requestorLabel->hide();
        m_contextLabel = new QLabel(m_contentWidget);
        m_contextLabel->setWordWrap(true);
        m_contextLabel->setStyleSheet("color: #bcc4bf; font-size: 13px;");
        m_contextLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        m_contextLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
        m_contextLabel->hide();
        m_contextToggleButton = new QPushButton("Show more", m_contentWidget);
        m_contextToggleButton->setCursor(Qt::PointingHandCursor);
        m_contextToggleButton->setFlat(true);
        m_contextToggleButton->setStyleSheet(
            "QPushButton { color: #8fb59a; border: none; padding: 0; text-align: left; } QPushButton:hover { color: #a6c8af; text-decoration: underline; }");
        m_contextToggleButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        m_contextToggleButton->hide();
        m_promptLabel = new QLabel("Password:", m_contentWidget);
        m_promptLabel->setStyleSheet("font-weight: 600;");
        m_input = new QLineEdit(m_contentWidget);
        m_input->setEchoMode(QLineEdit::Password);
        m_input->setMinimumHeight(38);
        m_input->setTextMargins(12, 0, 12, 0);
        m_input->setPlaceholderText("Enter password");
        auto* togglePasswordAction = m_input->addAction("Show", QLineEdit::TrailingPosition);
        togglePasswordAction->setCheckable(true);
        togglePasswordAction->setToolTip("Show password");
        connect(togglePasswordAction, &QAction::toggled, this, [this, togglePasswordAction](bool checked) {
            m_input->setEchoMode(checked ? QLineEdit::Normal : QLineEdit::Password);
            togglePasswordAction->setText(checked ? "Hide" : "Show");
            togglePasswordAction->setToolTip(checked ? "Hide password" : "Show password");
        });
        m_errorLabel = new QLabel(m_contentWidget);
        m_errorLabel->setWordWrap(true);
        m_errorLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        m_errorLabel->setStyleSheet("color: #cc4a4a;");
        m_errorLabel->hide();
        m_statusLabel = new QLabel(m_contentWidget);
        m_statusLabel->setWordWrap(true);
        m_statusLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        m_statusLabel->setStyleSheet("color: #999;");
        m_statusLabel->hide();
        auto* buttonRow = new QHBoxLayout();
        buttonRow->setSpacing(8);
        m_cancelButton = new QPushButton("Cancel", m_contentWidget);
        m_cancelButton->setShortcut(QKeySequence::Cancel);
        m_submitButton = new QPushButton("Authenticate", m_contentWidget);
        m_cancelButton->setMinimumHeight(34);
        m_submitButton->setMinimumHeight(34);
        m_cancelButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        m_submitButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        m_submitButton->setDefault(true);
        buttonRow->addWidget(m_cancelButton, 1);
        buttonRow->addWidget(m_submitButton, 1);
        headerLayout->addWidget(m_titleLabel);
        headerLayout->addWidget(m_summaryLabel);
        headerLayout->addWidget(m_requestorLabel);
        headerLayout->addWidget(m_contextLabel);
        headerLayout->addWidget(m_contextToggleButton, 0, Qt::AlignLeft);
        promptLayout->addWidget(m_promptLabel);
        promptLayout->addWidget(m_input);
        promptLayout->addWidget(m_errorLabel);
        promptLayout->addWidget(m_statusLabel);
        layout->addLayout(headerLayout);
        layout->addSpacing(12);
        layout->addLayout(promptLayout);
        layout->addStretch(1);
        layout->addLayout(buttonRow);
        hide();
        ensureContentFits();

        // Setup idle exit timer - exit process when hidden with no active session
        m_idleExitTimer = new QTimer(this);
        m_idleExitTimer->setSingleShot(true);
        const QByteArray idleTimeoutEnv = qgetenv("BB_AUTH_FALLBACK_IDLE_MS");
        const int        idleTimeoutMs  = idleTimeoutEnv.isEmpty() ? 30000 : QString::fromLatin1(idleTimeoutEnv).toInt();
        m_idleExitTimer->setInterval(qMax(5000, idleTimeoutMs)); // Minimum 5s safety
        connect(m_idleExitTimer, &QTimer::timeout, this, []() { QCoreApplication::quit(); });

        connect(m_input, &QLineEdit::returnPressed, this, [this]() {
            if (m_submitButton->isEnabled()) {
                m_submitButton->click();
            }
        });

        connect(m_submitButton, &QPushButton::clicked, this, [this]() {
            if (!m_client || m_currentSessionId.isEmpty()) {
                return;
            }

            if (!m_confirmOnly && !m_allowEmptyResponse && m_input->text().isEmpty()) {
                const bool passphrasePrompt = m_promptLabel->text().contains("passphrase", Qt::CaseInsensitive);
                setErrorText(passphrasePrompt ? "Please enter your passphrase." : "Please enter your password.");
                return;
            }

            setErrorText("");
            setStatusText("Verifying...");

            const QString response = m_confirmOnly ? QString("confirm") : m_input->text();
            m_client->sendResponse(m_currentSessionId, response);

            if (!m_confirmOnly) {
                m_input->clear();
            }

            setBusy(true);
        });

        connect(m_cancelButton, &QPushButton::clicked, this, [this]() {
            if (!m_client || m_currentSessionId.isEmpty()) {
                hide();
                return;
            }

            setStatusText("Cancelling...");
            setBusy(true);
            m_client->sendCancel(m_currentSessionId);
        });

        connect(m_contextToggleButton, &QPushButton::clicked, this, [this]() {
            if (!m_contextExpandable) {
                return;
            }

            setDetailsExpanded(!m_contextExpanded);
        });

        connect(m_client, &FallbackClient::connectionStateChanged, this, [this](bool connected) {
            if (connected) {
                if (!m_currentSessionId.isEmpty()) {
                    setStatusText("Connected");
                }
            } else {
                setStatusText("Disconnected from daemon, reconnecting...");
                setBusy(true);
            }
        });

        connect(m_client, &FallbackClient::providerStateChanged, this, [this](bool active) {
            if (active) {
                setStatusText("");
                if (!m_currentSessionId.isEmpty()) {
                    setBusy(false);
                }
                return;
            }

            if (!m_currentSessionId.isEmpty()) {
                clearSession();
            }
            hide();
            startIdleExitTimer();
        });

        connect(m_client, &FallbackClient::statusMessage, this, [this](const QString& status) { setStatusText(status); });

        connect(m_client, &FallbackClient::sessionCreated, this, [this, togglePasswordAction](const QJsonObject& event) {
            const QString id = event.value("id").toString();
            if (id.isEmpty()) {
                return;
            }

            m_currentSessionId             = id;
            m_confirmOnly                  = event.value("context").toObject().value("confirmOnly").toBool();
            m_allowEmptyResponse           = false;
            const PromptDisplayModel model = buildDisplayModel(event);
            configureSizingForIntent(model.intent);

            m_titleLabel->setText(model.title);
            m_summaryLabel->setText(model.summary);
            m_summaryLabel->setVisible(!model.summary.isEmpty());
            m_requestorLabel->setText(model.requestor);
            m_requestorLabel->setVisible(!model.requestor.isEmpty());
            setDetailsText(model.details);
            m_promptLabel->setText(model.prompt);

            m_input->clear();
            m_input->setEchoMode(QLineEdit::Password);
            togglePasswordAction->setChecked(false);
            togglePasswordAction->setText("Show");
            togglePasswordAction->setToolTip("Show password");
            togglePasswordAction->setVisible(!m_confirmOnly);
            togglePasswordAction->setEnabled(!m_confirmOnly);
            m_input->setVisible(!m_confirmOnly);
            m_promptLabel->setVisible(!m_confirmOnly);
            const bool    passphrasePrompt = model.passphrasePrompt;
            const QString placeholder =
                model.allowEmptyResponse ? QString("Press Enter to continue (optional)") : (passphrasePrompt ? QString("Enter passphrase") : QString("Enter password"));
            m_input->setPlaceholderText(m_confirmOnly ? QString() : placeholder);
            m_submitButton->setText(m_confirmOnly ? "Confirm" : (model.allowEmptyResponse ? "Continue" : "Authenticate"));
            m_allowEmptyResponse = model.allowEmptyResponse;

            setErrorText("");
            setStatusText("");
            setBusy(false);
            ensureContentFits();

            stopIdleExitTimer();
            show();
            raise();
            activateWindow();
            QTimer::singleShot(0, this, [this]() { ensureContentFits(); });
            QTimer::singleShot(30, this, [this]() { ensureContentFits(); });

            if (!m_confirmOnly) {
                m_input->setFocus();
            }
        });

        connect(m_client, &FallbackClient::sessionUpdated, this, [this, togglePasswordAction](const QJsonObject& event) {
            const QString id = event.value("id").toString();
            if (id.isEmpty() || id != m_currentSessionId) {
                return;
            }

            const QString prompt = event.value("prompt").toString();
            if (!prompt.isEmpty()) {
                m_promptLabel->setText(prompt);
            }

            const QString info              = event.value("info").toString().trimmed();
            const QString hint              = prompt + "\n" + info;
            const bool    fingerprintPrompt = looksLikeFingerprintPrompt(hint);
            const bool    fidoPrompt        = looksLikeFidoPrompt(hint);
            const bool    touchPrompt       = fingerprintPrompt || fidoPrompt || looksLikeTouchPrompt(hint);

            if (fingerprintPrompt) {
                m_titleLabel->setText("Verify Fingerprint");
            } else if (fidoPrompt) {
                m_titleLabel->setText("Use Security Key");
            }

            if (!m_confirmOnly) {
                m_allowEmptyResponse = touchPrompt;
                if (touchPrompt) {
                    m_promptLabel->setText("Press Enter to continue (or wait)");
                    m_input->setPlaceholderText("Press Enter to continue (optional)");
                    m_submitButton->setText("Continue");
                } else {
                    const bool passphrasePrompt = m_promptLabel->text().contains("passphrase", Qt::CaseInsensitive);
                    m_input->setPlaceholderText(passphrasePrompt ? QString("Enter passphrase") : QString("Enter password"));
                    m_submitButton->setText("Authenticate");
                }
            }

            if (!info.isEmpty()) {
                setStatusText(info);
            }

            if (event.contains("echo")) {
                const bool echo = event.value("echo").toBool();
                togglePasswordAction->setChecked(echo);
            }

            const QString error = event.value("error").toString();
            if (!error.isEmpty()) {
                setErrorText(error);
            } else {
                setErrorText("");
            }

            if (info.isEmpty()) {
                setStatusText("");
            }

            setBusy(false);

            if (!m_confirmOnly) {
                m_input->setFocus();
            }
        });

        connect(m_client, &FallbackClient::sessionClosed, this, [this](const QJsonObject& event) {
            const QString id = event.value("id").toString();
            if (id.isEmpty() || id != m_currentSessionId) {
                return;
            }

            const QString result = event.value("result").toString();
            const QString error  = event.value("error").toString();

            if (result == "success") {
                setErrorText("");
                setStatusText("Authentication successful.");
                QTimer::singleShot(300, this, [this]() {
                    clearSession();
                    hide();
                    startIdleExitTimer();
                });
                return;
            }

            if (result == "cancelled" || result == "canceled") {
                clearSession();
                hide();
                startIdleExitTimer();
                return;
            }

            if (!error.isEmpty()) {
                setErrorText(error);
            } else {
                setErrorText("Authentication failed.");
            }

            setStatusText("");
            setBusy(false);
        });
    }

    void FallbackWindow::closeEvent(QCloseEvent* event) {
        if (!m_currentSessionId.isEmpty() && m_client) {
            m_client->sendCancel(m_currentSessionId);
            clearSession();
        }

        event->accept();
    }

    void FallbackWindow::setBusy(bool busy) {
        m_busy = busy;
        m_submitButton->setEnabled(!m_busy);
        m_cancelButton->setEnabled(!m_busy);
        m_input->setEnabled(!m_busy);
    }

    void FallbackWindow::clearSession() {
        m_currentSessionId.clear();
        m_confirmOnly        = false;
        m_activeIntent       = PromptIntent::Generic;
        m_allowEmptyResponse = false;
        configureSizingForIntent(m_activeIntent);
        m_titleLabel->setText("Authentication Required");
        m_summaryLabel->clear();
        m_summaryLabel->hide();
        m_requestorLabel->clear();
        m_requestorLabel->hide();
        setDetailsText("");
        setErrorText("");
        setStatusText("");
        m_input->clear();
        setBusy(false);
    }

    void FallbackWindow::setErrorText(const QString& text) {
        if (text.isEmpty()) {
            m_errorLabel->clear();
            m_errorLabel->hide();
            ensureContentFits();
            return;
        }

        m_errorLabel->setText(text);
        m_errorLabel->show();
        ensureContentFits();
    }

    void FallbackWindow::setStatusText(const QString& text) {
        if (text.isEmpty()) {
            m_statusLabel->clear();
            m_statusLabel->hide();
            ensureContentFits();
            return;
        }

        m_statusLabel->setText(text);
        m_statusLabel->show();
        ensureContentFits();
    }

    void FallbackWindow::setDetailsText(const QString& text) {
        m_fullContextText = normalizeDetailText(text);
        if (m_fullContextText.isEmpty()) {
            m_collapsedContextText.clear();
            m_contextExpandable = false;
            m_contextExpanded   = false;
            m_contextLabel->clear();
            m_contextLabel->hide();
            m_contextToggleButton->hide();
            return;
        }

        const QPair<QString, bool> collapsed = collapseDetailText(m_fullContextText, 3, 220);
        m_collapsedContextText               = collapsed.first;
        m_contextExpandable                  = collapsed.second;
        setDetailsExpanded(false);
    }

    void FallbackWindow::setDetailsExpanded(bool expanded) {
        m_contextExpanded = expanded && m_contextExpandable;

        if (m_fullContextText.isEmpty()) {
            m_contextLabel->clear();
            m_contextLabel->hide();
            m_contextToggleButton->hide();
            return;
        }

        const QString text = (m_contextExpanded || !m_contextExpandable) ? m_fullContextText : m_collapsedContextText;
        m_contextLabel->setText(text);
        m_contextLabel->setVisible(!text.isEmpty());
        ensureContentFits();

        if (m_contextExpandable) {
            m_contextToggleButton->setText(m_contextExpanded ? "Show less" : "Show more");
            m_contextToggleButton->show();
        } else {
            m_contextToggleButton->hide();
        }
    }

    void FallbackWindow::ensureContentFits() {
        if (!m_contentWidget) {
            return;
        }

        auto* rootLayout = qobject_cast<QVBoxLayout*>(layout());
        if (!rootLayout) {
            return;
        }

        auto* contentLayout = m_contentWidget->layout();
        if (contentLayout) {
            contentLayout->activate();
        }
        rootLayout->activate();

        int left   = 0;
        int top    = 0;
        int right  = 0;
        int bottom = 0;
        rootLayout->getContentsMargins(&left, &top, &right, &bottom);

        const int currentContentWidth   = qMax(360, qMax(m_baseWidth, width()) - left - right);
        int       requiredContentHeight = m_contentWidget->sizeHint().height();
        int       requiredContentWidth  = m_contentWidget->sizeHint().width();

        if (contentLayout) {
            if (contentLayout->hasHeightForWidth()) {
                requiredContentHeight = contentLayout->heightForWidth(currentContentWidth);
            } else {
                requiredContentHeight = contentLayout->sizeHint().height();
            }

            requiredContentWidth = contentLayout->sizeHint().width();
        }

        const int baselineWidth = m_baseWidth;
        const int targetWidth   = qBound(m_minWidth, qMax(baselineWidth, requiredContentWidth + left + right), 680);
        const int targetHeight  = qBound(m_minHeight, requiredContentHeight + top + bottom + 8, 640);
        if (targetWidth != width() || targetHeight != height()) {
            resize(targetWidth, targetHeight);
        }
    }

    void FallbackWindow::configureSizingForIntent(PromptIntent intent) {
        m_activeIntent = intent;

        if (intent == PromptIntent::OpenPgp) {
            m_baseWidth  = 540;
            m_baseHeight = 360;
            m_minWidth   = 500;
            m_minHeight  = 336;
        } else if (intent == PromptIntent::Unlock || intent == PromptIntent::RunCommand || intent == PromptIntent::Fingerprint || intent == PromptIntent::Fido2) {
            m_baseWidth  = 540;
            m_baseHeight = 280;
            m_minWidth   = 500;
            m_minHeight  = 240;
        } else {
            m_baseWidth  = 540;
            m_baseHeight = 290;
            m_minWidth   = 500;
            m_minHeight  = 240;
        }

        setMinimumSize(m_minWidth, m_minHeight);
    }

    FallbackWindow::PromptDisplayModel FallbackWindow::buildDisplayModel(const QJsonObject& event) const {
        PromptDisplayModel model;
        const QString      source                = event.value("source").toString();
        const QJsonObject  context               = event.value("context").toObject();
        const QJsonObject  requestor             = context.value("requestor").toObject();
        const QString      message               = context.value("message").toString();
        const QString      description           = context.value("description").toString();
        const QString      requestorName         = requestor.value("name").toString().trimmed();
        const QString      infoText              = normalizeDetailText(event.value("info").toString());
        const QString      normalizedMessage     = normalizeDetailText(message);
        const QString      normalizedDescription = normalizeDetailText(description);
        const QString      detailText            = (normalizedDescription + " " + normalizedMessage).toLower();
        const QString      authHintText          = (detailText + " " + infoText).toLower();
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
                model.requestor = QString("Requested by %1").arg(requestorName);
            }
        }
        if (model.summary.isEmpty() && !model.details.isEmpty()) {
            const QString normalizedDetails = normalizeDetailText(model.details);
            const int     newline           = normalizedDetails.indexOf('\n');
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
        if (source == "pinentry") {
            const QString pinPrompt = context.value("message").toString().trimmed();
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

    void FallbackWindow::startIdleExitTimer() {
        // Start countdown to exit when hidden with no active session
        if (m_idleExitTimer && m_currentSessionId.isEmpty() && !isVisible()) {
            m_idleExitTimer->start();
        }
    }

    void FallbackWindow::stopIdleExitTimer() {
        // Cancel exit countdown when showing or receiving a session
        if (m_idleExitTimer) {
            m_idleExitTimer->stop();
        }
    }

} // namespace bb
