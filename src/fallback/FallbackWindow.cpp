#include "FallbackWindow.hpp"
#include "prompt/PromptModelBuilder.hpp"
#include "prompt/TextNormalize.hpp"

#include <QCloseEvent>
#include <QAction>
#include <QCoreApplication>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QKeySequence>
#include <QLineEdit>
#include <QLoggingCategory>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QShortcut>
#include <QSizePolicy>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>

namespace {
    bool fallbackGeometryDebugEnabled() {
        static const int enabled = qEnvironmentVariableIntValue("BB_AUTH_FALLBACK_DEBUG_GEOMETRY");
        return enabled > 0;
    }

    void logFallbackGeometry(const char* phase, const QWidget* widget, const QWidget* contentWidget, const QWindow* window) {
        if (!fallbackGeometryDebugEnabled() || !widget) {
            return;
        }

        const QSize  widgetSize  = widget->size();
        const QRect  contentRect = contentWidget ? contentWidget->geometry() : QRect();
        const QSize  windowSize  = window ? window->size() : QSize();
        const QRect  frameGeom   = window ? window->frameGeometry() : QRect();
        const QString platform   = QGuiApplication::platformName();
        qWarning().nospace() << "[fallback-geo] phase=" << phase << " platform=" << platform << " widget=" << widgetSize.width() << "x"
                             << widgetSize.height() << " contentRect=" << contentRect.x() << "," << contentRect.y() << " " << contentRect.width() << "x"
                             << contentRect.height() << " window=" << windowSize.width() << "x" << windowSize.height() << " frame=" << frameGeom.x() << ","
                             << frameGeom.y() << " " << frameGeom.width() << "x" << frameGeom.height();
    }

    QPair<QString, bool> collapseDetailText(const QString& text, int maxLines, int maxChars) {
        const QString normalizedText = bb::fallback::prompt::normalizeDetailText(text);
        if (normalizedText.isEmpty()) {
            return qMakePair(QString(), false);
        }

        const QStringList lines = normalizedText.split('\n');
        QStringList       collapsedLines;
        collapsedLines.reserve(lines.size());

        qsizetype usedChars = 0;
        bool      truncated = false;
        const qsizetype maxCharsBound = static_cast<qsizetype>(maxChars);

        for (const QString& line : lines) {
            if (collapsedLines.size() >= maxLines) {
                truncated = true;
                break;
            }

            QString clipped = line;
            if ((usedChars + clipped.size()) > maxCharsBound) {
                const qsizetype remaining = qMax(static_cast<qsizetype>(0), maxCharsBound - usedChars);
                clipped                   = remaining > 0 ? clipped.left(remaining).trimmed() : QString();
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
        setAutoFillBackground(true);
        setAttribute(Qt::WA_StyledBackground, true);
        QPalette windowPalette = palette();
        QColor   windowColor   = windowPalette.color(QPalette::Window);
        if (windowColor.alpha() < 255) {
            windowColor.setAlpha(255);
            windowPalette.setColor(QPalette::Window, windowColor);
            setPalette(windowPalette);
        }
        configureSizingForIntent(PromptIntent::Generic);
        resize(m_baseWidth, m_baseHeight);

        auto* rootLayout = new QVBoxLayout(this);
        rootLayout->setContentsMargins(20, 20, 20, 20);
        rootLayout->setSpacing(0);

        m_contentWidget = new QWidget(this);
        m_contentWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        m_contentWidget->setAutoFillBackground(true);
        m_contentWidget->setAttribute(Qt::WA_StyledBackground, true);
        rootLayout->addWidget(m_contentWidget, 1);
        auto* layout = new QVBoxLayout(m_contentWidget);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        layout->setAlignment(Qt::AlignTop);
        auto* headerLayout = new QVBoxLayout();
        headerLayout->setContentsMargins(0, 0, 0, 0);
        headerLayout->setSpacing(10);
        auto* promptLayout = new QVBoxLayout();
        promptLayout->setContentsMargins(0, 0, 0, 0);
        promptLayout->setSpacing(10);
        m_titleLabel = new QLabel("Authentication Required", m_contentWidget);
        m_titleLabel->setStyleSheet("font-weight: 700;");
        m_summaryLabel = new QLabel(m_contentWidget);
        m_summaryLabel->setWordWrap(true);
        m_summaryLabel->setStyleSheet("font-weight: 600;");
        m_summaryLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        m_summaryLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
        m_summaryLabel->hide();
        m_requestorLabel = new QLabel(m_contentWidget);
        m_requestorLabel->setWordWrap(true);
        m_requestorLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        m_requestorLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
        m_requestorLabel->hide();
        m_contextLabel = new QLabel(m_contentWidget);
        m_contextLabel->setWordWrap(true);
        m_contextLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        m_contextLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
        m_contextLabel->hide();
        m_contextToggleButton = new QPushButton("Show more", m_contentWidget);
        m_contextToggleButton->setCursor(Qt::PointingHandCursor);
        m_contextToggleButton->setFlat(true);
        m_contextToggleButton->setStyleSheet(
            "QPushButton { border: none; padding: 0; text-align: left; } QPushButton:hover { text-decoration: underline; }");
        m_contextToggleButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        m_contextToggleButton->hide();
        m_promptLabel = new QLabel("Password:", m_contentWidget);
        m_promptLabel->setStyleSheet("font-weight: 600;");
        m_input = new QLineEdit(m_contentWidget);
        m_input->setEchoMode(QLineEdit::Password);
        const int inputHeight = qMax(38, QFontMetrics(m_input->font()).height() + 16);
        m_input->setMinimumHeight(inputHeight);
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
        m_errorLabel->setStyleSheet("font-weight: 600;");
        m_errorLabel->hide();
        m_statusLabel = new QLabel(m_contentWidget);
        m_statusLabel->setWordWrap(true);
        m_statusLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        m_statusLabel->hide();
        auto* buttonRow = new QHBoxLayout();
        buttonRow->setSpacing(8);
        m_cancelButton = new QPushButton("Cancel", m_contentWidget);
        m_cancelButton->setShortcut(QKeySequence::Cancel);
        m_submitButton = new QPushButton("Authenticate", m_contentWidget);
        const int buttonHeight = qMax(34, QFontMetrics(m_submitButton->font()).height() + 14);
        m_cancelButton->setMinimumHeight(buttonHeight);
        m_submitButton->setMinimumHeight(buttonHeight);
        m_cancelButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        m_submitButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        m_submitButton->setDefault(true);
        m_submitButton->setAutoDefault(true);
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
        layout->addSpacing(14);
        layout->addLayout(buttonRow);

        // Keep keyboard traversal deterministic for input-only operation.
        setTabOrder(m_input, m_contextToggleButton);
        setTabOrder(m_contextToggleButton, m_cancelButton);
        setTabOrder(m_cancelButton, m_submitButton);

        hide();
        ensureContentFits();

        // Setup idle exit timer - exit process when hidden with no active session
        m_idleExitTimer = new QTimer(this);
        m_idleExitTimer->setSingleShot(true);
        const QByteArray idleTimeoutEnv = qgetenv("BB_AUTH_FALLBACK_IDLE_MS");
        const int        idleTimeoutMs  = idleTimeoutEnv.isEmpty() ? 30000 : QString::fromLatin1(idleTimeoutEnv).toInt();
        m_idleExitTimer->setInterval(qMax(5000, idleTimeoutMs)); // Minimum 5s safety
        connect(m_idleExitTimer, &QTimer::timeout, this, []() { QCoreApplication::quit(); });

        m_actionTimeoutTimer = new QTimer(this);
        m_actionTimeoutTimer->setSingleShot(true);
        const QByteArray actionTimeoutEnv = qgetenv("BB_AUTH_FALLBACK_ACTION_TIMEOUT_MS");
        const int        actionTimeoutMs  = actionTimeoutEnv.isEmpty() ? 12000 : QString::fromLatin1(actionTimeoutEnv).toInt();
        m_actionTimeoutTimer->setInterval(qBound(250, actionTimeoutMs, 120000));
        connect(m_actionTimeoutTimer, &QTimer::timeout, this, [this]() {
            if (m_currentSessionId.isEmpty() || m_pendingAction == PendingAction::None) {
                return;
            }

            const PendingAction timedOutAction = m_pendingAction;
            clearPendingAction();
            setBusy(false);
            setStatusText("");

            if (timedOutAction == PendingAction::Cancel) {
                setErrorText("Cancel request timed out. Try again.");
                m_cancelButton->setFocus();
            } else {
                setErrorText("Authentication request timed out. Review details and retry.");
                if (!m_confirmOnly) {
                    m_submitButton->setText("Retry");
                    m_input->setFocus();
                } else {
                    m_submitButton->setFocus();
                }
            }
        });

        connect(m_input, &QLineEdit::returnPressed, this, [this]() {
            if (m_submitButton->isEnabled()) {
                m_submitButton->click();
            }
        });

        auto* cancelShortcut = new QShortcut(QKeySequence::Cancel, this);
        cancelShortcut->setContext(Qt::WindowShortcut);
        connect(cancelShortcut, &QShortcut::activated, this, [this]() { handleCancelRequest(); });

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
            startPendingAction(PendingAction::Submit);

            const QString response = m_confirmOnly ? QString("confirm") : m_input->text();
            m_client->sendResponse(m_currentSessionId, response);

            if (!m_confirmOnly) {
                m_input->clear();
            }

            setBusy(true);
        });

        connect(m_cancelButton, &QPushButton::clicked, this, [this]() { handleCancelRequest(); });

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
            m_currentSource                = event.value("source").toString();
            m_currentContext               = event.value("context").toObject();
            m_confirmOnly                  = m_currentContext.value("confirmOnly").toBool();
            m_allowEmptyResponse           = false;
            const PromptDisplayModel model = buildDisplayModel(event);
            configureSizingForIntent(model.intent);
            clearPendingAction();

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
            scheduleEnsureContentFits(0);
            scheduleEnsureContentFits(30);
            scheduleEnsureContentFits(120);
            scheduleEnsureContentFits(260);

            if (!m_confirmOnly) {
                m_input->setFocus();
            } else {
                m_submitButton->setFocus();
            }
        });

        connect(m_client, &FallbackClient::sessionUpdated, this, [this, togglePasswordAction](const QJsonObject& event) {
            const QString id = event.value("id").toString();
            if (id.isEmpty() || id != m_currentSessionId) {
                return;
            }

            const QString prompt = event.value("prompt").toString();
            const QString info   = event.value("info").toString().trimmed();
            const QString source = m_currentSource.isEmpty() ? event.value("source").toString() : m_currentSource;
            QJsonObject   context = m_currentContext.isEmpty() ? event.value("context").toObject() : m_currentContext;
            QJsonObject   merged = QJsonObject{{"source", source}, {"context", context}};
            if (!prompt.isEmpty()) {
                merged.insert("prompt", prompt);
            }
            if (!info.isEmpty()) {
                merged.insert("info", info);
            }
            const PromptDisplayModel model = buildDisplayModel(merged);
            clearPendingAction();

            m_titleLabel->setText(model.title);

            if (!m_confirmOnly) {
                m_allowEmptyResponse = model.allowEmptyResponse;
                m_promptLabel->setText(model.prompt);
                if (model.allowEmptyResponse) {
                    m_input->setPlaceholderText("Press Enter to continue (optional)");
                    m_submitButton->setText("Continue");
                } else {
                    const bool passphrasePrompt = model.passphrasePrompt;
                    m_input->setPlaceholderText(passphrasePrompt ? QString("Enter passphrase") : QString("Enter password"));
                    m_submitButton->setText("Authenticate");
                }
            }

            if (event.contains("echo")) {
                const bool echo = event.value("echo").toBool();
                togglePasswordAction->setChecked(echo);
            }

            const QString error = event.value("error").toString();
            if (!error.isEmpty()) {
                setErrorText(error);
                if (!m_confirmOnly) {
                    m_submitButton->setText("Retry");
                }
            } else {
                setErrorText("");
            }

            if (!info.isEmpty()) {
                setStatusText(info);
            } else {
                const int curRetry = event.value("curRetry").toInt();
                const int maxRetry = event.value("maxRetries").toInt();
                if (m_currentSource == "pinentry" && curRetry > 0 && maxRetry > 0) {
                    setStatusText(QString("Attempt %1 of %2").arg(curRetry).arg(maxRetry));
                } else {
                    setStatusText("");
                }
            }

            setBusy(false);

            if (!m_confirmOnly) {
                m_input->setFocus();
            } else {
                m_submitButton->setFocus();
            }
        });

        connect(m_client, &FallbackClient::sessionClosed, this, [this](const QJsonObject& event) {
            const QString id = event.value("id").toString();
            if (id.isEmpty() || id != m_currentSessionId) {
                return;
            }

            const QString result = event.value("result").toString();
            const QString error  = event.value("error").toString();
            clearPendingAction();

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

            setStatusText("Request ended.");
            setBusy(false);
            QTimer::singleShot(1200, this, [this, id]() {
                if (m_currentSessionId == id) {
                    clearSession();
                    hide();
                    startIdleExitTimer();
                }
            });
        });
    }

    void FallbackWindow::closeEvent(QCloseEvent* event) {
        if (!m_currentSessionId.isEmpty() && m_client) {
            m_client->sendCancel(m_currentSessionId);
            clearSession();
        }

        event->accept();
    }

    void FallbackWindow::showEvent(QShowEvent* event) {
        QWidget::showEvent(event);
        logFallbackGeometry("showEvent:beforeSync", this, m_contentWidget, windowHandle());
        syncToCompositorSize();
        logFallbackGeometry("showEvent:afterSync", this, m_contentWidget, windowHandle());

        // Wayland compositors can apply initial/tiled geometry after map; run
        // multiple fit passes to converge without requiring user interaction.
        scheduleEnsureContentFits(0);
        scheduleEnsureContentFits(40);
        scheduleEnsureContentFits(140);
        QTimer::singleShot(0, this, [this]() { syncToCompositorSize(); });
        QTimer::singleShot(120, this, [this]() { syncToCompositorSize(); });
        QTimer::singleShot(260, this, [this]() { syncToCompositorSize(); });
    }

    void FallbackWindow::resizeEvent(QResizeEvent* event) {
        QWidget::resizeEvent(event);
        logFallbackGeometry("resizeEvent:beforeSync", this, m_contentWidget, windowHandle());
        syncToCompositorSize();
        logFallbackGeometry("resizeEvent:afterSync", this, m_contentWidget, windowHandle());
        if (isVisible()) {
            scheduleEnsureContentFits(0);
            scheduleEnsureContentFits(90);
            scheduleEnsureContentFits(260);
        }
    }

    void FallbackWindow::setBusy(bool busy) {
        m_busy = busy;
        m_submitButton->setEnabled(!m_busy);
        const bool hasSession           = !m_currentSessionId.isEmpty();
        const bool cancelInFlight       = m_busy && m_pendingAction == PendingAction::Cancel;
        const bool cancelButtonEnabled  = hasSession && !cancelInFlight;
        m_cancelButton->setEnabled(cancelButtonEnabled);
        m_input->setEnabled(!m_busy);
    }

    void FallbackWindow::handleCancelRequest() {
        if (m_currentSessionId.isEmpty()) {
            hide();
            startIdleExitTimer();
            return;
        }

        if (m_busy && m_pendingAction == PendingAction::Cancel) {
            return;
        }

        // If submit is already in-flight and transport is down, exiting locally avoids a keyboard dead-end.
        if (m_busy && (!m_client || !m_client->isConnected())) {
            clearSession();
            hide();
            startIdleExitTimer();
            return;
        }

        setErrorText("");
        setStatusText("Cancelling...");
        startPendingAction(PendingAction::Cancel);
        setBusy(true);
        m_client->sendCancel(m_currentSessionId);
    }

    void FallbackWindow::startPendingAction(PendingAction action) {
        m_pendingAction = action;
        if (m_actionTimeoutTimer) {
            m_actionTimeoutTimer->start();
        }
    }

    void FallbackWindow::clearPendingAction() {
        m_pendingAction = PendingAction::None;
        if (m_actionTimeoutTimer) {
            m_actionTimeoutTimer->stop();
        }
    }

    void FallbackWindow::clearSession() {
        clearPendingAction();
        m_currentSessionId.clear();
        m_currentSource.clear();
        m_currentContext = QJsonObject{};
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
            scheduleEnsureContentFits();
            return;
        }

        m_errorLabel->setText(text);
        m_errorLabel->show();
        scheduleEnsureContentFits();
    }

    void FallbackWindow::setStatusText(const QString& text) {
        if (text.isEmpty()) {
            m_statusLabel->clear();
            m_statusLabel->hide();
            scheduleEnsureContentFits();
            return;
        }

        m_statusLabel->setText(text);
        m_statusLabel->show();
        scheduleEnsureContentFits();
    }

    void FallbackWindow::setDetailsText(const QString& text) {
        m_fullContextText = bb::fallback::prompt::normalizeDetailText(text);
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
        scheduleEnsureContentFits();

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

        const QScreen* screen = windowHandle() ? windowHandle()->screen() : QGuiApplication::primaryScreen();
        const int      availableWidth  = screen ? qMax(320, screen->availableGeometry().width() - 32) : 680;
        const int      availableHeight = screen ? qMax(220, screen->availableGeometry().height() - 32) : 640;

        const int minWidth  = qMin(m_minWidth, availableWidth);
        const int minHeight = qMin(m_minHeight, availableHeight);

        const int softMaxWidth  = qMin(680, availableWidth);
        const int softMaxHeight = qMin(640, availableHeight);
        const int widthForHeight = qBound(minWidth, qMax(width(), m_baseWidth), softMaxWidth);
        const int currentContentWidth = qMax(320, widthForHeight - left - right);
        int       requiredContentHeight = m_contentWidget->sizeHint().height();
        int       requiredContentWidth  = m_contentWidget->sizeHint().width();

        if (contentLayout) {
            const int layoutSizeHintHeight = contentLayout->sizeHint().height();
            requiredContentHeight          = layoutSizeHintHeight;
            if (contentLayout->hasHeightForWidth()) {
                const int heightForWidth = contentLayout->heightForWidth(currentContentWidth);
                if (heightForWidth > 0 && heightForWidth < (QWIDGETSIZE_MAX / 2)) {
                    requiredContentHeight = heightForWidth;
                }
            }

            requiredContentWidth = contentLayout->sizeHint().width();
        }

        const int baselineWidth = qBound(minWidth, m_baseWidth, softMaxWidth);
        const int targetWidth   = qBound(minWidth, qMax(baselineWidth, requiredContentWidth + left + right), softMaxWidth);
        const int targetHeight  = qBound(minHeight, requiredContentHeight + top + bottom + 8, softMaxHeight);

        if (minimumWidth() != minWidth || minimumHeight() != minHeight) {
            setMinimumSize(minWidth, minHeight);
        }
        if (isVisible()) {
            return;
        }

        if (targetWidth != width() || targetHeight != height()) {
            resize(targetWidth, targetHeight);
        }
        logFallbackGeometry("ensureContentFits", this, m_contentWidget, windowHandle());
    }

    void FallbackWindow::scheduleEnsureContentFits(int delayMs) {
        if (delayMs <= 0) {
            if (m_fitRefreshScheduled) {
                return;
            }
            m_fitRefreshScheduled = true;
            QTimer::singleShot(0, this, [this]() {
                m_fitRefreshScheduled = false;
                ensureContentFits();
            });
            return;
        }

        QTimer::singleShot(delayMs, this, [this]() { ensureContentFits(); });
    }

    void FallbackWindow::syncToCompositorSize() {
        if (m_syncingToCompositor) {
            return;
        }

        QWindow* handle = windowHandle();
        if (!handle) {
            return;
        }

        const QSize compositorSize = handle->size();
        if (!compositorSize.isValid() || compositorSize.width() <= 0 || compositorSize.height() <= 0) {
            return;
        }

        if (size() == compositorSize) {
            return;
        }

        m_syncingToCompositor = true;
        if (fallbackGeometryDebugEnabled()) {
            qWarning().nospace() << "[fallback-geo] sync resize " << size().width() << "x" << size().height() << " -> " << compositorSize.width() << "x"
                                 << compositorSize.height();
        }
        resize(compositorSize);
        m_syncingToCompositor = false;
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
        const bb::fallback::prompt::PromptModelBuilder builder;
        return builder.build(event);
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
