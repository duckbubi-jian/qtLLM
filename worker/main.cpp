#include "models/ModelDescriptor.hpp"
#include "protocol/ProtocolVersion.hpp"

#include <QCoreApplication>
#include <QTextStream>

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("qtllm-worker"));

    [[maybe_unused]] qtllm::models::ModelDescriptor modelDescriptor;
    modelDescriptor.origin = qtllm::models::ModelOrigin::DirectGguf;

    QTextStream output(stdout);
    output << "qtllm-worker protocol " << qtllm::protocol::version << Qt::endl;

    return 0;
}
