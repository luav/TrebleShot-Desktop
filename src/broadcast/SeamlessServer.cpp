//
// Created by veli on 2/9/19.
//

#include <src/database/object/NetworkDevice.h>
#include <src/util/AppUtils.h>
#include <src/database/object/TransferGroup.h>
#include <src/database/object/TransferObject.h>
#include <QtCore/QFile>
#include "SeamlessServer.h"

SeamlessServer::SeamlessServer(QObject *parent)
        : CoolSocket::Server(QHostAddress::Any, PORT_SEAMLESS, TIMEOUT_SOCKET_DEFAULT, parent)
{

}

void SeamlessServer::connected(CoolSocket::ActiveConnection *connection)
{
    try {
        const auto &mainRequest = connection->receive();
        const auto &mainRequestJSON = mainRequest.asJson();
        QString deviceId = mainRequestJSON.contains(KEYWORD_TRANSFER_DEVICE_ID)
                           ? mainRequestJSON.value(KEYWORD_TRANSFER_DEVICE_ID).toString()
                           : nullptr;
        groupid groupId = mainRequestJSON.value(KEYWORD_TRANSFER_GROUP_ID).toVariant().toUInt();

        {
            QJsonObject reply;

            reply.insert(KEYWORD_RESULT, false);

            auto *device = deviceId != nullptr ? new NetworkDevice(deviceId) : nullptr;
            auto *group = new TransferGroup(groupId);

            // device might be null because this was introduced in build 91
            if (device != nullptr && !gDbSignal->reconstruct(*device)) {
                reply.insert(KEYWORD_ERROR, KEYWORD_ERROR_NOT_ALLOWED);
            } else if (!gDbSignal->reconstruct(*group)) {
                reply.insert(KEYWORD_ERROR, KEYWORD_ERROR_NOT_FOUND);
            } else {
                reply.insert(KEYWORD_RESULT, true);
            }

            connection->reply(reply);

            if (!reply.value(KEYWORD_RESULT).toBool(false))
                return;
        }

        while (connection->getSocket()->isOpen()) {
            const auto &response = connection->receive();

            if (response.response == nullptr || response.length <= 0)
                return;

            const QJsonObject &request = response.asJson();
            QJsonObject reply;

            qDebug() << request;

            {
                if (request.contains(KEYWORD_RESULT) && !request.value(KEYWORD_RESULT).toBool(false)) {
                    if (request.contains(KEYWORD_TRANSFER_JOB_DONE)
                        && request.value(KEYWORD_TRANSFER_JOB_DONE).toBool(false)) {
                        qDebug() << "Received notified that the transfer is done";
                    } else
                        interrupt();
                } else {
                    if (interrupted())
                        break;

                    qDebug() << "Entering send phase";

                    TransferObject transferObject(request.value(KEYWORD_TRANSFER_REQUEST_ID).toVariant().toUInt(),
                                                  deviceId, TransferObject::Type::Outgoing);

                    if (gDbSignal->reconstruct(transferObject)) {
                        quint16 serverPort = static_cast<quint16>(request.value(KEYWORD_TRANSFER_SOCKET_PORT)
                                .toVariant()
                                .toUInt());
                        size_t skippedBytes = request.contains(KEYWORD_SKIPPED_BYTES)
                                              ? request.value(KEYWORD_SKIPPED_BYTES).toVariant().toUInt()
                                              : 0;

                        qDebug() << "File sending about to being" << serverPort << skippedBytes << transferObject.file;
                        gDbSignal->update(transferObject);

                        QFile file(transferObject.file);
                        file.open(QFile::OpenModeFlag::ReadOnly);

                        if (file.exists()) {
                            transferObject.accessPort = serverPort;
                            transferObject.skippedBytes = skippedBytes;

                            reply.insert(KEYWORD_RESULT, true);

                            if (transferObject.fileSize != file.size()) {
                                reply.insert(KEYWORD_SIZE_CHANGED, file.size());
                                transferObject.fileSize = static_cast<size_t>(file.size());
                            }

                            connection->reply(reply);

                            if (skippedBytes > 0)
                                file.seek(static_cast<qint64>(skippedBytes));

                            qDebug() << "About to connect";
                            QTcpSocket socket;
                            socket.open(QIODevice::OpenModeFlag::WriteOnly);

                            {
                                // Establish the connection to the socket that will receive the file
                                socket.connectToHost(connection->getSocket()->peerAddress(), serverPort);

                                while (QAbstractSocket::SocketState::ConnectingState == socket.state())
                                    socket.waitForConnected(TIMEOUT_SOCKET_DEFAULT_LARGE);

                                if (QAbstractSocket::SocketState::ConnectedState != socket.state())
                                    throw exception();
                            }

                            qDebug() << "Connected & will receive";

                            while (!file.atEnd()) {
                                if (socket.bytesToWrite() == 0) {
                                    socket.write(file.read(BUFFER_LENGTH_DEFAULT));
                                    socket.flush();
                                } else if (socket.bytesToWrite() > 0
                                           && !socket.waitForBytesWritten(TIMEOUT_SOCKET_DEFAULT)) {
                                    qDebug() << "Timed out";
                                    break;
                                }
                            }

                            transferObject.flag = TransferObject::Flag::Done;

                            gDbSignal->update(transferObject);

                            socket.close();
                            file.close();
                        }
                    } else {
                        reply.insert(KEYWORD_RESULT, false);
                        reply.insert(KEYWORD_ERROR, KEYWORD_ERROR_NOT_FOUND);
                        reply.insert(KEYWORD_FLAG_GROUP_EXISTS, true);

                        transferObject.flag = TransferObject::Flag::Removed;
                    }
                }
            }
        }
    } catch (...) {
        qDebug() << "Error occurred";
    }
}
