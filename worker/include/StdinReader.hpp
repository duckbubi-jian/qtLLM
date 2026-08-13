#pragma once

#include <QByteArray>
#include <QObject>

namespace qtllm::worker
{
class StdinReader final : public QObject
{
    Q_OBJECT

   public slots:
    void readLines();

   signals:
    void lineReceived(const QByteArray& line);
    void inputClosed();
};
}  // namespace qtllm::worker
