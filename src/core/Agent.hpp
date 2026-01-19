#pragma once

#include <QCoreApplication>
#include <QDBusInterface>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QQueue>
#include <QString>

#include "PolkitListener.hpp"
#include <polkitqt1-subject.h>

#include <memory>

class CAgent {
  public:
    CAgent()  = default;
    ~CAgent() = default;

    bool start(QCoreApplication& app, const QString& socketPath);
    void initAuthPrompt(const QString& cookie);
    void enqueueRequest(const QString& cookie);
    void enqueueError(const QString& cookie, const QString& error);
    void enqueueComplete(const QString& cookie, const QString& result);
    bool handleRespond(const QString& cookie, const QString& password);
    bool handleCancel(const QString& cookie);

  private:
    // Keyring request tracking
    struct KeyringRequest {
        QString                cookie;
        QString                title;
        QString                message;
        QString                description;
        QString                warning;
        qint64                 originPid   = 0;
        bool                   passwordNew = false;
        bool                   confirmOnly = false;
        QPointer<QLocalSocket> replySocket;
    };

    QHash<QString, KeyringRequest>                 pendingKeyringRequests;

    CPolkitListener                                listener;
    std::shared_ptr<PolkitQt1::UnixSessionSubject> sessionSubject;

    QLocalServer*                                  ipcServer = nullptr;
    QQueue<QJsonObject>                            eventQueue;
    QString                                        ipcSocketPath;
    bool                                           fingerprintAvailable = false;

    void                                           setupIpcServer();
    bool                                           checkFingerprintAvailable();
    void                                           handleSocketLine(QLocalSocket* socket, const QByteArray& line);
    void                                           sendJson(QLocalSocket* socket, const QJsonObject& obj, bool disconnect = true, bool secure = false);
    void                                           enqueueEvent(const QJsonObject& event);
    QJsonObject                                    buildRequestEvent(const QString& cookie) const;
    QJsonObject                                    buildKeyringRequestEvent(const KeyringRequest& req) const;
    void                                           cleanupKeyringRequestsForSocket(QLocalSocket* socket);

    // Keyring request handlers
    void handleKeyringRequest(QLocalSocket* socket, const QJsonObject& obj);
    void respondToKeyringRequest(const QString& cookie, const QString& password);
    void cancelKeyringRequest(const QString& cookie);

    friend class CPolkitListener;
};

inline std::unique_ptr<CAgent> g_pAgent;
