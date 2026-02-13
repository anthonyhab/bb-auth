#pragma once

#include "FallbackClient.hpp"

#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;
class QTimer;

namespace bb {

    class FallbackWindow : public QWidget {
        Q_OBJECT

      public:
        explicit FallbackWindow(FallbackClient* client, QWidget* parent = nullptr);

        friend class FallbackWindowTouchModelTest;

      protected:
        void closeEvent(QCloseEvent* event) override;

      private:
        enum class PromptIntent {
            Generic,
            Unlock,
            RunCommand,
            OpenPgp,
            Fingerprint,
            Fido2
        };

        struct PromptDisplayModel {
            PromptIntent intent = PromptIntent::Generic;
            QString      title;
            QString      summary;
            QString      requestor;
            QString      details;
            QString      prompt;
            bool         passphrasePrompt   = false;
            bool         allowEmptyResponse = false;
        };

        void               setBusy(bool busy);
        void               clearSession();
        void               setErrorText(const QString& text);
        void               setStatusText(const QString& text);
        void               setDetailsText(const QString& text);
        void               setDetailsExpanded(bool expanded);
        void               ensureContentFits();
        void               configureSizingForIntent(PromptIntent intent);
        PromptDisplayModel buildDisplayModel(const QJsonObject& event) const;

        FallbackClient*    m_client = nullptr;

        QWidget*           m_contentWidget       = nullptr;
        QLabel*            m_titleLabel          = nullptr;
        QLabel*            m_summaryLabel        = nullptr;
        QLabel*            m_requestorLabel      = nullptr;
        QLabel*            m_contextLabel        = nullptr;
        QPushButton*       m_contextToggleButton = nullptr;
        QLabel*            m_promptLabel         = nullptr;
        QLabel*            m_errorLabel          = nullptr;
        QLabel*            m_statusLabel         = nullptr;
        QLineEdit*         m_input               = nullptr;
        QPushButton*       m_submitButton        = nullptr;
        QPushButton*       m_cancelButton        = nullptr;

        QString            m_currentSessionId;
        QString            m_fullContextText;
        QString            m_collapsedContextText;
        PromptIntent       m_activeIntent       = PromptIntent::Generic;
        int                m_baseWidth          = 500;
        int                m_baseHeight         = 334;
        int                m_minWidth           = 450;
        int                m_minHeight          = 320;
        bool               m_contextExpandable  = false;
        bool               m_contextExpanded    = false;
        bool               m_confirmOnly        = false;
        bool               m_busy               = false;
        bool               m_allowEmptyResponse = false;

        // Idle exit timer - process exits when hidden with no active session
        QTimer* m_idleExitTimer = nullptr;

        void    startIdleExitTimer();
        void    stopIdleExitTimer();
    };

} // namespace bb
