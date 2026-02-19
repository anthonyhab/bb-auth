#include "../src/fallback/FallbackClient.hpp"
#include "../src/fallback/FallbackWindow.hpp"

#include <QtTest/QtTest>

#include <QApplication>
#include <QColor>
#include <QFontMetrics>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QShortcut>
#include <QtMath>

#include <algorithm>

namespace bb {

    namespace {

        class EnvVarGuard {
          public:
            EnvVarGuard(const char* name, const QByteArray& value) : m_name(name), m_hadValue(qEnvironmentVariableIsSet(name)), m_oldValue(qgetenv(name)) {
                qputenv(name, value);
            }

            ~EnvVarGuard() {
                if (m_hadValue) {
                    qputenv(m_name.constData(), m_oldValue);
                } else {
                    qunsetenv(m_name.constData());
                }
            }

          private:
            QByteArray m_name;
            bool       m_hadValue = false;
            QByteArray m_oldValue;
        };

        class AppFontGuard {
          public:
            AppFontGuard() : m_oldFont(QApplication::font()) {}
            ~AppFontGuard() { QApplication::setFont(m_oldFont); }

          private:
            QFont m_oldFont;
        };

        double linearizeChannel(double channel) {
            if (channel <= 0.03928) {
                return channel / 12.92;
            }

            return qPow((channel + 0.055) / 1.055, 2.4);
        }

        double relativeLuminance(const QColor& color) {
            const double r = linearizeChannel(color.redF());
            const double g = linearizeChannel(color.greenF());
            const double b = linearizeChannel(color.blueF());
            return (0.2126 * r) + (0.7152 * g) + (0.0722 * b);
        }

        double contrastRatio(const QColor& a, const QColor& b) {
            const double lighter = qMax(relativeLuminance(a), relativeLuminance(b));
            const double darker  = qMin(relativeLuminance(a), relativeLuminance(b));
            return (lighter + 0.05) / (darker + 0.05);
        }

        QJsonObject makeCreatedEvent(const QString& id) {
            const QJsonObject context{{"message", "Authentication is required"}, {"requestor", QJsonObject{{"name", "test-app"}, {"pid", 101}}}};
            return QJsonObject{{"type", "session.created"}, {"id", id}, {"source", "polkit"}, {"context", context}};
        }

    } // namespace

    class FallbackWindowTouchModelTest : public QObject {
        Q_OBJECT

      private slots:
        void background_isOpaqueToAvoidBlurBleed();
        void keyboardEnter_submitsWithoutMouse();
        void keyboardEscape_cancelsWithoutMouse();
        void keyboardEscapeShortcut_registeredAtWindowLevel();
        void keyboardCancel_recoversFromDisconnectedSubmit();
        void keyboardTabOrder_cyclesPrimaryControls();
        void scaling_largeFontExpandsControlHeights();
        void contrast_textUsesReadablePaletteContrast();
        void tallResize_keepsControlsGrouped();
        void compositorResize_keepsControlsGroupedWithoutInteraction();
        void submitTimeout_releasesBusyAndShowsRetry();
        void cancelTimeout_releasesBusyAndShowsError();
        void closedError_autoDismissesToAvoidDeadEnd();
    };

    void FallbackWindowTouchModelTest::background_isOpaqueToAvoidBlurBleed() {
        FallbackClient client("/tmp/non-existent-bb-auth.sock");
        FallbackWindow window(&client);

        QVERIFY(window.autoFillBackground());
        QVERIFY(window.testAttribute(Qt::WA_StyledBackground));

        const QColor background = window.palette().color(QPalette::Window);
        QCOMPARE(background.alpha(), 255);
    }

    void FallbackWindowTouchModelTest::keyboardEnter_submitsWithoutMouse() {
        EnvVarGuard   timeoutGuard("BB_AUTH_FALLBACK_ACTION_TIMEOUT_MS", "5000");
        FallbackClient client("/tmp/non-existent-bb-auth.sock");
        FallbackWindow window(&client);

        const QJsonObject created = makeCreatedEvent("kbd-enter");
        QVERIFY(QMetaObject::invokeMethod(&client, "sessionCreated", Qt::DirectConnection, Q_ARG(QJsonObject, created)));

        window.m_input->setText("secret");
        QTest::keyClick(window.m_input, Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(window.m_busy, 1000);
        QCOMPARE(window.m_pendingAction, FallbackWindow::PendingAction::Submit);
    }

    void FallbackWindowTouchModelTest::keyboardEscape_cancelsWithoutMouse() {
        EnvVarGuard   timeoutGuard("BB_AUTH_FALLBACK_ACTION_TIMEOUT_MS", "5000");
        FallbackClient client("/tmp/non-existent-bb-auth.sock");
        FallbackWindow window(&client);

        const QJsonObject created = makeCreatedEvent("kbd-escape");
        QVERIFY(QMetaObject::invokeMethod(&client, "sessionCreated", Qt::DirectConnection, Q_ARG(QJsonObject, created)));

        QTest::keyClick(window.m_cancelButton, Qt::Key_Space);
        QTRY_VERIFY_WITH_TIMEOUT(window.m_busy, 1000);
        QCOMPARE(window.m_pendingAction, FallbackWindow::PendingAction::Cancel);
    }

    void FallbackWindowTouchModelTest::keyboardEscapeShortcut_registeredAtWindowLevel() {
        EnvVarGuard   timeoutGuard("BB_AUTH_FALLBACK_ACTION_TIMEOUT_MS", "5000");
        FallbackClient client("/tmp/non-existent-bb-auth.sock");
        FallbackWindow window(&client);

        const QJsonObject created = makeCreatedEvent("kbd-escape-shortcut");
        QVERIFY(QMetaObject::invokeMethod(&client, "sessionCreated", Qt::DirectConnection, Q_ARG(QJsonObject, created)));

        const QList<QShortcut*> shortcuts = window.findChildren<QShortcut*>();
        const auto hasCancelWindowShortcut = std::any_of(shortcuts.cbegin(), shortcuts.cend(), [](const QShortcut* shortcut) {
            return shortcut && shortcut->key() == QKeySequence::Cancel && shortcut->context() == Qt::WindowShortcut;
        });
        QVERIFY(hasCancelWindowShortcut);
    }

    void FallbackWindowTouchModelTest::keyboardCancel_recoversFromDisconnectedSubmit() {
        EnvVarGuard   timeoutGuard("BB_AUTH_FALLBACK_ACTION_TIMEOUT_MS", "5000");
        FallbackClient client("/tmp/non-existent-bb-auth.sock");
        FallbackWindow window(&client);

        const QJsonObject created = makeCreatedEvent("kbd-submit-recover");
        QVERIFY(QMetaObject::invokeMethod(&client, "sessionCreated", Qt::DirectConnection, Q_ARG(QJsonObject, created)));

        window.m_input->setText("secret");
        QTest::keyClick(window.m_input, Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(window.m_busy, 1000);
        QCOMPARE(window.m_pendingAction, FallbackWindow::PendingAction::Submit);
        QVERIFY(window.m_cancelButton->isEnabled());

        window.m_cancelButton->setFocus();
        QTest::keyClick(window.m_cancelButton, Qt::Key_Space);
        QTRY_VERIFY_WITH_TIMEOUT(window.m_currentSessionId.isEmpty(), 1000);
        QVERIFY(!window.m_busy);
        QVERIFY(!window.isVisible());
    }

    void FallbackWindowTouchModelTest::keyboardTabOrder_cyclesPrimaryControls() {
        EnvVarGuard   timeoutGuard("BB_AUTH_FALLBACK_ACTION_TIMEOUT_MS", "5000");
        FallbackClient client("/tmp/non-existent-bb-auth.sock");
        FallbackWindow window(&client);

        const QJsonObject created = makeCreatedEvent("kbd-tab");
        QVERIFY(QMetaObject::invokeMethod(&client, "sessionCreated", Qt::DirectConnection, Q_ARG(QJsonObject, created)));

        window.m_input->setFocus();
        if (!window.m_input->hasFocus()) {
            QSKIP("Keyboard focus not supported by current Qt platform plugin");
        }

        QTest::keyClick(window.m_input, Qt::Key_Tab);
        QTRY_VERIFY_WITH_TIMEOUT(window.m_cancelButton->hasFocus(), 1000);

        QTest::keyClick(window.m_cancelButton, Qt::Key_Tab);
        QTRY_VERIFY_WITH_TIMEOUT(window.m_submitButton->hasFocus(), 1000);

        QTest::keyClick(window.m_submitButton, Qt::Key_Backtab);
        QTRY_VERIFY_WITH_TIMEOUT(window.m_cancelButton->hasFocus(), 1000);
    }

    void FallbackWindowTouchModelTest::scaling_largeFontExpandsControlHeights() {
        AppFontGuard fontGuard;

        QFont largeFont = QApplication::font();
        if (largeFont.pointSizeF() > 0.0) {
            largeFont.setPointSizeF(largeFont.pointSizeF() + 6.0);
        } else {
            largeFont.setPixelSize(largeFont.pixelSize() + 6);
        }
        QApplication::setFont(largeFont);

        FallbackClient client("/tmp/non-existent-bb-auth.sock");
        FallbackWindow window(&client);

        const QJsonObject created = makeCreatedEvent("scaled-font");
        QVERIFY(QMetaObject::invokeMethod(&client, "sessionCreated", Qt::DirectConnection, Q_ARG(QJsonObject, created)));

        const int expectedInputHeight  = qMax(38, QFontMetrics(window.m_input->font()).height() + 16);
        const int expectedButtonHeight = qMax(34, QFontMetrics(window.m_submitButton->font()).height() + 14);
        QCOMPARE(window.m_input->minimumHeight(), expectedInputHeight);
        QCOMPARE(window.m_submitButton->minimumHeight(), expectedButtonHeight);
        QCOMPARE(window.m_cancelButton->minimumHeight(), expectedButtonHeight);
    }

    void FallbackWindowTouchModelTest::contrast_textUsesReadablePaletteContrast() {
        FallbackClient client("/tmp/non-existent-bb-auth.sock");
        FallbackWindow window(&client);

        const QJsonObject created = makeCreatedEvent("contrast");
        QVERIFY(QMetaObject::invokeMethod(&client, "sessionCreated", Qt::DirectConnection, Q_ARG(QJsonObject, created)));

        const QColor background = window.palette().color(QPalette::Window);

        const auto checkLabelContrast = [&](QLabel* label) {
            const QColor text = label->palette().color(QPalette::WindowText);
            QVERIFY2(contrastRatio(text, background) >= 4.5, qPrintable(QString("Insufficient text contrast: %1").arg(label->objectName())));
        };

        checkLabelContrast(window.m_titleLabel);
        checkLabelContrast(window.m_summaryLabel);
        checkLabelContrast(window.m_requestorLabel);
        checkLabelContrast(window.m_contextLabel);
        checkLabelContrast(window.m_promptLabel);
        checkLabelContrast(window.m_errorLabel);
        checkLabelContrast(window.m_statusLabel);
    }

    void FallbackWindowTouchModelTest::tallResize_keepsControlsGrouped() {
        FallbackClient client("/tmp/non-existent-bb-auth.sock");
        FallbackWindow window(&client);

        const QJsonObject created = makeCreatedEvent("resize-grouped");
        QVERIFY(QMetaObject::invokeMethod(&client, "sessionCreated", Qt::DirectConnection, Q_ARG(QJsonObject, created)));
        QTRY_VERIFY_WITH_TIMEOUT(window.isVisible(), 1000);

        const int oversizedHeight = qMin(760, window.height() + 300);
        window.resize(window.width(), oversizedHeight);
        QTest::qWait(150);

        const int controlGap = window.m_cancelButton->geometry().top() - window.m_input->geometry().bottom();
        QVERIFY2(controlGap < 120, qPrintable(QString("Control gap too large after tall resize: %1").arg(controlGap)));
    }

    void FallbackWindowTouchModelTest::compositorResize_keepsControlsGroupedWithoutInteraction() {
        FallbackClient client("/tmp/non-existent-bb-auth.sock");
        FallbackWindow window(&client);

        const QJsonObject created = makeCreatedEvent("resize-refit");
        QVERIFY(QMetaObject::invokeMethod(&client, "sessionCreated", Qt::DirectConnection, Q_ARG(QJsonObject, created)));
        QTRY_VERIFY_WITH_TIMEOUT(window.isVisible(), 1000);

        const int oversizedHeight = qMin(760, window.height() + 300);

        window.resize(window.width(), oversizedHeight);
        QTest::qWait(150);

        // Compositor-sized/tiled windows should keep the prompt block compact.
        const int controlGap = window.m_cancelButton->geometry().top() - window.m_input->geometry().bottom();
        QVERIFY2(controlGap < 120, qPrintable(QString("Control gap too large after compositor resize: %1").arg(controlGap)));
    }

    void FallbackWindowTouchModelTest::submitTimeout_releasesBusyAndShowsRetry() {
        EnvVarGuard   timeoutGuard("BB_AUTH_FALLBACK_ACTION_TIMEOUT_MS", "300");
        FallbackClient client("/tmp/non-existent-bb-auth.sock");
        FallbackWindow window(&client);

        const QJsonObject created = makeCreatedEvent("submit-timeout");
        QVERIFY(QMetaObject::invokeMethod(&client, "sessionCreated", Qt::DirectConnection, Q_ARG(QJsonObject, created)));

        QCOMPARE(window.m_currentSessionId, QString("submit-timeout"));
        window.m_input->setText("secret");
        window.m_submitButton->click();

        QVERIFY(window.m_busy);
        QTRY_VERIFY_WITH_TIMEOUT(!window.m_busy, 2000);
        QVERIFY(window.m_errorLabel->isVisible());
        QVERIFY(window.m_errorLabel->text().contains("timed out", Qt::CaseInsensitive));
        QCOMPARE(window.m_submitButton->text(), QString("Retry"));
    }

    void FallbackWindowTouchModelTest::cancelTimeout_releasesBusyAndShowsError() {
        EnvVarGuard   timeoutGuard("BB_AUTH_FALLBACK_ACTION_TIMEOUT_MS", "300");
        FallbackClient client("/tmp/non-existent-bb-auth.sock");
        FallbackWindow window(&client);

        const QJsonObject created = makeCreatedEvent("cancel-timeout");
        QVERIFY(QMetaObject::invokeMethod(&client, "sessionCreated", Qt::DirectConnection, Q_ARG(QJsonObject, created)));

        QCOMPARE(window.m_currentSessionId, QString("cancel-timeout"));
        window.m_cancelButton->click();

        QVERIFY(window.m_busy);
        QTRY_VERIFY_WITH_TIMEOUT(!window.m_busy, 2000);
        QVERIFY(window.m_errorLabel->isVisible());
        QVERIFY(window.m_errorLabel->text().contains("Cancel request timed out", Qt::CaseInsensitive));
    }

    void FallbackWindowTouchModelTest::closedError_autoDismissesToAvoidDeadEnd() {
        FallbackClient client("/tmp/non-existent-bb-auth.sock");
        FallbackWindow window(&client);

        const QJsonObject created = makeCreatedEvent("closed-error");
        QVERIFY(QMetaObject::invokeMethod(&client, "sessionCreated", Qt::DirectConnection, Q_ARG(QJsonObject, created)));
        QVERIFY(window.isVisible());

        const QJsonObject closed{{"type", "session.closed"}, {"id", "closed-error"}, {"result", "error"}, {"error", "Too many failed attempts"}};
        QVERIFY(QMetaObject::invokeMethod(&client, "sessionClosed", Qt::DirectConnection, Q_ARG(QJsonObject, closed)));

        QVERIFY(window.m_errorLabel->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(window.m_currentSessionId.isEmpty(), 3000);
        QVERIFY(!window.isVisible());
    }

} // namespace bb

int runFallbackWindowStateTests(int argc, char** argv) {
    bb::FallbackWindowTouchModelTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_fallback_window_states.moc"
