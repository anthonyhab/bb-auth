#pragma once

#include "FallbackClient.hpp"
#include "prompt/PromptModel.hpp"

#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;
class QResizeEvent;
class QTimer;
class QShowEvent;

namespace bb {

    class FallbackWindow : public QWidget {
        Q_OBJECT

      public:
        explicit FallbackWindow(FallbackClient* client, QWidget* parent = nullptr);

        friend class FallbackWindowTouchModelTest;

      protected:
        void closeEvent(QCloseEvent* event) override;
        void showEvent(QShowEvent* event) override;
        void resizeEvent(QResizeEvent* event) override;

      private:
        using PromptIntent       = bb::fallback::prompt::PromptIntent;
        using PromptDisplayModel = bb::fallback::prompt::PromptDisplayModel;
        enum class PendingAction {
            None,
            Submit,
            Cancel
        };

        void               setBusy(bool busy);
        void               handleCancelRequest();
        void               startPendingAction(PendingAction action);
        void               clearPendingAction();
        void               clearSession();
        void               setErrorText(const QString& text);
        void               setStatusText(const QString& text);
        void               setDetailsText(const QString& text);
        void               setDetailsExpanded(bool expanded);
        void               ensureContentFits();
        void               scheduleEnsureContentFits(int delayMs = 0);
        void               syncToCompositorSize();
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
        QString            m_currentSource;
        QJsonObject        m_currentContext;
        PendingAction      m_pendingAction = PendingAction::None;

        // Idle exit timer - process exits when hidden with no active session
        QTimer* m_idleExitTimer = nullptr;
        QTimer* m_actionTimeoutTimer = nullptr;
        bool    m_fitRefreshScheduled = false;
        bool    m_syncingToCompositor = false;

        void    startIdleExitTimer();
        void    stopIdleExitTimer();
    };

} // namespace bb
