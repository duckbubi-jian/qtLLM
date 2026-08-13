#include "StdinReader.hpp"

#include <cstdio>

namespace qtllm::worker
{
void StdinReader::readLines()
{
    QByteArray line;
    while (true)
    {
        const auto character = std::fgetc(stdin);
        if (character == EOF) break;
        if (character == '\n')
        {
            emit lineReceived(line);
            line.clear();
        }
        else if (character != '\r')
        {
            line.append(static_cast<char>(character));
        }
    }
    if (!line.isEmpty()) emit lineReceived(line);
    emit inputClosed();
}
}  // namespace qtllm::worker
