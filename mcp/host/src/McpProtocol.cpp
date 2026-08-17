#include "McpProtocol.hpp"

#include <utility>

namespace qtllm::infrastructure::mcp
{
namespace
{
const QStringList supportedVersions{QStringLiteral("2025-06-18"),
                                    QStringLiteral("2025-03-26"),
                                    QStringLiteral("2024-11-05")};

bool readCapabilityObject(const QJsonObject& capabilities, const QString& name,
                          QJsonObject& value, QString& errorMessage)
{
    const auto capability = capabilities.value(name);
    if (capability.isUndefined()) return false;
    if (!capability.isObject())
    {
        errorMessage =
            QStringLiteral("initialize capability '%1' must be an object.")
                .arg(name);
        return false;
    }
    value = capability.toObject();
    return true;
}
}  // namespace

QString latestSupportedProtocolVersion()
{
    return supportedVersions.constFirst();
}

QStringList supportedProtocolVersions()
{
    return supportedVersions;
}

bool isSupportedProtocolVersion(const QString& version)
{
    return supportedProtocolVersions().contains(version);
}

bool parseInitializeResult(const QJsonObject& object,
                           McpInitializeResult& result, QString& errorMessage)
{
    const auto protocolVersion =
        object.value(QStringLiteral("protocolVersion"));
    const auto capabilitiesValue = object.value(QStringLiteral("capabilities"));
    const auto serverInfo = object.value(QStringLiteral("serverInfo"));
    const auto instructions = object.value(QStringLiteral("instructions"));
    if (!protocolVersion.isString() ||
        protocolVersion.toString().trimmed().isEmpty())
    {
        errorMessage =
            QStringLiteral("initialize result has no protocolVersion.");
        return false;
    }
    if (!isSupportedProtocolVersion(protocolVersion.toString()))
    {
        errorMessage = QStringLiteral("Unsupported MCP protocol version: %1")
                           .arg(protocolVersion.toString());
        return false;
    }
    if (!capabilitiesValue.isObject())
    {
        errorMessage =
            QStringLiteral("initialize result has no capabilities object.");
        return false;
    }
    if (!serverInfo.isObject() ||
        serverInfo.toObject()
            .value(QStringLiteral("name"))
            .toString()
            .trimmed()
            .isEmpty() ||
        serverInfo.toObject()
            .value(QStringLiteral("version"))
            .toString()
            .trimmed()
            .isEmpty())
    {
        errorMessage = QStringLiteral(
            "initialize result has invalid serverInfo name or version.");
        return false;
    }
    if (!instructions.isUndefined() && !instructions.isString())
    {
        errorMessage =
            QStringLiteral("initialize instructions must be a string.");
        return false;
    }

    McpInitializeResult parsed;
    parsed.protocolVersion = protocolVersion.toString();
    parsed.serverInfo = serverInfo.toObject();
    parsed.instructions = instructions.toString();
    parsed.capabilities.raw = capabilitiesValue.toObject();

    QJsonObject capability;
    QString capabilityError;
    parsed.capabilities.tools =
        readCapabilityObject(parsed.capabilities.raw, QStringLiteral("tools"),
                             capability, capabilityError);
    if (!capabilityError.isEmpty())
    {
        errorMessage = capabilityError;
        return false;
    }
    if (parsed.capabilities.tools)
        parsed.capabilities.toolsListChanged =
            capability.value(QStringLiteral("listChanged")).toBool();

    parsed.capabilities.resources = readCapabilityObject(
        parsed.capabilities.raw, QStringLiteral("resources"), capability,
        capabilityError);
    if (!capabilityError.isEmpty())
    {
        errorMessage = capabilityError;
        return false;
    }
    if (parsed.capabilities.resources)
    {
        parsed.capabilities.resourcesSubscribe =
            capability.value(QStringLiteral("subscribe")).toBool();
        parsed.capabilities.resourcesListChanged =
            capability.value(QStringLiteral("listChanged")).toBool();
    }

    parsed.capabilities.prompts =
        readCapabilityObject(parsed.capabilities.raw, QStringLiteral("prompts"),
                             capability, capabilityError);
    if (!capabilityError.isEmpty())
    {
        errorMessage = capabilityError;
        return false;
    }
    if (parsed.capabilities.prompts)
        parsed.capabilities.promptsListChanged =
            capability.value(QStringLiteral("listChanged")).toBool();

    parsed.capabilities.logging =
        readCapabilityObject(parsed.capabilities.raw, QStringLiteral("logging"),
                             capability, capabilityError);
    if (!capabilityError.isEmpty())
    {
        errorMessage = capabilityError;
        return false;
    }
    parsed.capabilities.completions = readCapabilityObject(
        parsed.capabilities.raw, QStringLiteral("completions"), capability,
        capabilityError);
    if (!capabilityError.isEmpty())
    {
        errorMessage = capabilityError;
        return false;
    }

    result = std::move(parsed);
    return true;
}
}  // namespace qtllm::infrastructure::mcp
