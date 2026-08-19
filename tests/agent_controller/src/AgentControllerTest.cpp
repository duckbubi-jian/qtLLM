#include "AgentController.hpp"
#include "AgentContextCompactor.hpp"
#include "AgentRunMetrics.hpp"
#include "ToolCatalogBuilder.hpp"
#include "ToolResultStatus.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QtTest>

#include <algorithm>

namespace qtllm::tests
{
class AgentControllerTest final : public QObject
{
    Q_OBJECT

   private slots:
    void completesMultiStepToolRun();
    void publishesStructuredProgressSnapshots();
    void publishesApprovalRecoveryAndCancellationProgress();
    void simpleToolRunCompletesWithoutReview();
    void simpleTaskRecoversFromFailureWithoutStructuredReview();
    void waitsForApprovalAndHonorsRejection();
    void repairsOnlyOneInvalidAction();
    void cancelsAndIgnoresLateResponses();
    void cancellationRemainsTerminalAcrossApprovalAndToolResult();
    void doesNotRetryUncertainMutationAfterTransportFailure();
    void retriesReadOnlyTransportFailureOnce();
    void exportsStructuredRunMetrics();
    void classifiesRunMetricFailures();
    void keepsConversationHistoryAndClearsIt();
    void emptyToolPromptForbidsToolCalls();
    void casualPromptPrefersFinalWithoutTools();
    void includesRuntimeContextInPrompt();
    void toolCatalogIsStableAndValid();
    void toolCatalogOmitsWholeDefinitions();
    void compactCatalogPreservesComplexOmittedContract();
    void compactCatalogPreservesDynamicKeyContract();
    void repairsToolValidationWithFocusedContract();
    void rejectsUnchangedRetryAfterToolError();
    void repairsDynamicPropertyNameValidation();
    void repairsMissingOrderedPlanMetadata();
    void blocksOrderedPlanWhenRequiredInputIsMissing();
    void successfulStructuredMutationAdvancesPlanWithoutReadBack();
    void keepsSharedMutationWithinCurrentPlanStep();
    void keepsOrderedStepPendingWhenEvidenceMissesRequestedTarget();
    void advancesPlanOneStepPerTerminalToolResult();
    void rejectsOutOfOrderCompletionReview();
    void rejectsRepeatedSuccessfulToolCall();
    void limitsConsecutiveDiscoveryBreadth();
    void preservesSummaryWhenCompletionReviewFormattingFails();
    void allowsRepeatedPollingUntilTerminalStatus();
    void classifiesStructuredLifecycleStates();
    void cancelsPendingStatusPoll();
    void rejectsAlternatingCompletedToolCycle();
    void reviewsPrematureFinalBeforeAnyToolCall();
    void reviewsCompletionAfterEachEvidenceRevision();
    void restoresRecordedPlanWhenCompletionReviewDrifts();
    void rejectsRepeatedFinalDuringCompletionReview();
    void completionReviewRejectsNonTerminalEvidence();
    void doesNotUsePreparedFinalWhenReviewStalls();
    void stagnationRecoveryIsIndependentFromActionRepair();
    void preservesStructuredScalarEvidence();
    void compactsLongAgentContextAndPreservesEvidence();
    void tracksRunScopedLedgerAndRequiresVerification();
    void requiresMatchingTargetForMutationSelfVerification();
    void treatsDefaultModifyRiskAsMutation();
    void rejectsCompletionWithUnverifiedMutation();
    void hasNoToolCallCountLimit();
    void longRunWarningDoesNotStopAgent();
};

agent::ToolDefinition echoTool()
{
    return {QStringLiteral("fake.echo"),
            QStringLiteral("fake"),
            QStringLiteral("echo"),
            QStringLiteral("Echo values"),
            {{QStringLiteral("type"), QStringLiteral("object")}}};
}

agent::ToolDefinition namedTool(const QString& name, const QString& description)
{
    return {QStringLiteral("fake.") + name,
            QStringLiteral("fake"),
            name,
            description,
            {{QStringLiteral("type"), QStringLiteral("object")},
             {QStringLiteral("properties"),
              QJsonObject{{QStringLiteral("value"),
                           QJsonObject{{QStringLiteral("type"),
                                        QStringLiteral("string")}}}}}}};
}

agent::ToolDefinition ledgerTool(const QString& name, bool readOnly)
{
    return {QStringLiteral("fake.") + name,
            QStringLiteral("fake"),
            name,
            QStringLiteral("Ledger test tool"),
            {{QStringLiteral("type"), QStringLiteral("object")}},
            {},
            {{QStringLiteral("readOnlyHint"), readOnly}},
            false};
}

agent::ToolDefinition inletTool()
{
    const QJsonObject geometrySource{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("additionalProperties"), false},
        {QStringLiteral("properties"),
         QJsonObject{
             {QStringLiteral("type"),
              QJsonObject{
                  {QStringLiteral("type"), QStringLiteral("string")},
                  {QStringLiteral("const"), QStringLiteral("geometry_file")}}},
             {QStringLiteral("file_path"),
              QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
             {QStringLiteral("scale_ratio"),
              QJsonObject{
                  {QStringLiteral("type"), QStringLiteral("number")}}}}},
        {QStringLiteral("required"),
         QJsonArray{QStringLiteral("type"), QStringLiteral("file_path")}}};
    const QJsonObject circleSource{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("additionalProperties"), false},
        {QStringLiteral("properties"),
         QJsonObject{
             {QStringLiteral("type"),
              QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                          {QStringLiteral("const"), QStringLiteral("circle")}}},
             {QStringLiteral("radius"),
              QJsonObject{
                  {QStringLiteral("type"), QStringLiteral("number")}}}}},
        {QStringLiteral("required"),
         QJsonArray{QStringLiteral("type"), QStringLiteral("radius")}}};
    const QJsonObject inletSpec{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("additionalProperties"), false},
        {QStringLiteral("properties"),
         QJsonObject{
             {QStringLiteral("name"),
              QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
             {QStringLiteral("source"),
              QJsonObject{
                  {QStringLiteral("discriminator"),
                   QJsonObject{{QStringLiteral("propertyName"),
                                QStringLiteral("type")}}},
                  {QStringLiteral("oneOf"),
                   QJsonArray{QJsonObject{{QStringLiteral("$ref"),
                                           QStringLiteral(
                                               "#/$defs/GeometryFileSource")}},
                              QJsonObject{{QStringLiteral("$ref"),
                                           QStringLiteral(
                                               "#/$defs/CircleSource")}}}}}}}},
        {QStringLiteral("required"),
         QJsonArray{QStringLiteral("name"), QStringLiteral("source")}}};
    const QJsonObject schema{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("additionalProperties"), false},
        {QStringLiteral("properties"),
         QJsonObject{{QStringLiteral("spec"),
                      QJsonObject{{QStringLiteral("$ref"),
                                   QStringLiteral("#/$defs/InletSpec")}}}}},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("spec")}},
        {QStringLiteral("$defs"),
         QJsonObject{{QStringLiteral("InletSpec"), inletSpec},
                     {QStringLiteral("GeometryFileSource"), geometrySource},
                     {QStringLiteral("CircleSource"), circleSource}}}};
    return {QStringLiteral("shondy.create_inlet"), QStringLiteral("shondy"),
            QStringLiteral("create_inlet"),
            QStringLiteral("Create an inlet from a structured definition"),
            schema};
}

agent::ToolValidationResult acceptsToolArguments(const QString&,
                                                 const QJsonObject&)
{
    return {true, {}};
}

QJsonObject planStep(const QString& id, const QString& description,
                     bool requiresTool, QStringList allowedTools = {})
{
    if (requiresTool && allowedTools.isEmpty())
        allowedTools.append(QStringLiteral("fake.echo"));
    QJsonArray allowedToolValues;
    for (const auto& tool : allowedTools)
        allowedToolValues.append(tool);
    return {{QStringLiteral("id"), id},
            {QStringLiteral("description"), description},
            {QStringLiteral("requires_tool"), requiresTool},
            {QStringLiteral("allowed_tools"), allowedToolValues}};
}

QJsonObject reviewedStep(const QString& id, const QString& description,
                         bool requiresTool, const QString& status,
                         const QJsonArray& evidence = {})
{
    return {{QStringLiteral("id"), id},
            {QStringLiteral("description"), description},
            {QStringLiteral("requires_tool"), requiresTool},
            {QStringLiteral("status"), status},
            {QStringLiteral("evidence"), evidence}};
}

QByteArray taskPlanAction(const QJsonArray& steps)
{
    return QJsonDocument(QJsonObject{{QStringLiteral("action"),
                                      QStringLiteral("task_plan")},
                                     {QStringLiteral("steps"), steps},
                                     {QStringLiteral("ordered"), false}})
        .toJson(QJsonDocument::Compact);
}

QByteArray orderedTaskPlanAction(const QJsonArray& steps)
{
    return QJsonDocument(QJsonObject{{QStringLiteral("action"),
                                      QStringLiteral("task_plan")},
                                     {QStringLiteral("steps"), steps},
                                     {QStringLiteral("ordered"), true}})
        .toJson(QJsonDocument::Compact);
}

QByteArray completionReviewAction(const QString& verdict,
                                  const QJsonArray& steps,
                                  const QString& detail)
{
    return QJsonDocument(QJsonObject{{QStringLiteral("action"),
                                      QStringLiteral("review_completion")},
                                     {QStringLiteral("verdict"), verdict},
                                     {QStringLiteral("steps"), steps},
                                     {QStringLiteral("detail"), detail}})
        .toJson(QJsonDocument::Compact);
}

QByteArray planStepReviewAction(const QString& stepId, const QString& status,
                                const QJsonArray& evidence,
                                const QString& detail)
{
    return QJsonDocument(QJsonObject{{QStringLiteral("action"),
                                      QStringLiteral("review_plan_step")},
                                     {QStringLiteral("step_id"), stepId},
                                     {QStringLiteral("status"), status},
                                     {QStringLiteral("evidence"), evidence},
                                     {QStringLiteral("detail"), detail}})
        .toJson(QJsonDocument::Compact);
}

QByteArray toolCallReviewAction(const QString& verdict, const QString& detail)
{
    return QJsonDocument(QJsonObject{{QStringLiteral("action"),
                                      QStringLiteral("review_tool_call")},
                                     {QStringLiteral("verdict"), verdict},
                                     {QStringLiteral("detail"), detail}})
        .toJson(QJsonDocument::Compact);
}

void AgentControllerTest::toolCatalogIsStableAndValid()
{
    auto alpha =
        namedTool(QStringLiteral("alpha"), QStringLiteral("First tool"));
    alpha.outputSchema = {{QStringLiteral("type"), QStringLiteral("object")}};
    alpha.annotations = {{QStringLiteral("readOnlyHint"), true}};
    alpha.hasOutputSchema = true;
    const auto middle =
        namedTool(QStringLiteral("middle"), QStringLiteral("Middle tool"));
    const auto zeta =
        namedTool(QStringLiteral("zeta"), QStringLiteral("Last tool"));
    const auto first =
        application::ToolCatalogBuilder::build({zeta, alpha, middle}, 4'096);
    const auto second =
        application::ToolCatalogBuilder::build({middle, zeta, alpha}, 4'096);

    QCOMPARE(first.json, second.json);
    QCOMPARE(first.indexJson, second.indexJson);
    QCOMPARE(first.includedToolCount, 3);
    QCOMPARE(first.omittedToolCount, 0);
    QCOMPARE(first.indexedToolCount, 3);
    QCOMPARE(first.unindexedToolCount, 0);
    QVERIFY(first.json.size() <= 4'096);
    QVERIFY(first.indexJson.size() <=
            application::ToolCatalogBuilder::defaultMaximumIndexBytes);

    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(first.json, &error);
    QCOMPARE(error.error, QJsonParseError::NoError);
    QVERIFY(document.isArray());
    const auto definitions = document.array();
    QCOMPARE(definitions.size(), 3);
    QCOMPARE(definitions.at(0).toObject().value(QStringLiteral("name")),
             QJsonValue(QStringLiteral("fake.alpha")));
    QCOMPARE(definitions.at(1).toObject().value(QStringLiteral("name")),
             QJsonValue(QStringLiteral("fake.middle")));
    QCOMPARE(definitions.at(2).toObject().value(QStringLiteral("name")),
             QJsonValue(QStringLiteral("fake.zeta")));
    QCOMPARE(definitions.at(0)
                 .toObject()
                 .value(QStringLiteral("inputSchema"))
                 .toObject(),
             alpha.inputSchema);
    QCOMPARE(definitions.at(0)
                 .toObject()
                 .value(QStringLiteral("outputSchema"))
                 .toObject(),
             alpha.outputSchema);
    QCOMPARE(definitions.at(0)
                 .toObject()
                 .value(QStringLiteral("annotations"))
                 .toObject(),
             alpha.annotations);
}

void AgentControllerTest::toolCatalogOmitsWholeDefinitions()
{
    const auto alpha =
        namedTool(QStringLiteral("alpha"), QStringLiteral("Small A"));
    const auto oversized =
        namedTool(QStringLiteral("middle"), QString(8'192, QLatin1Char('x')));
    const auto zeta =
        namedTool(QStringLiteral("zeta"), QStringLiteral("Small Z"));
    const auto expected =
        application::ToolCatalogBuilder::build({alpha, zeta}, 4'096);
    const auto actual = application::ToolCatalogBuilder::build(
        {zeta, oversized, alpha}, expected.json.size());

    QCOMPARE(actual.json, expected.json);
    QCOMPARE(actual.includedToolCount, 2);
    QCOMPARE(actual.omittedToolCount, 1);
    QCOMPARE(actual.omittedToolNames,
             QStringList{QStringLiteral("fake.middle")});
    QCOMPARE(actual.indexedToolCount, 3);
    QVERIFY(actual.json.size() <= expected.json.size());
    QVERIFY(QJsonDocument::fromJson(actual.json).isArray());

    const auto empty = application::ToolCatalogBuilder::build({oversized}, 32);
    QCOMPARE(empty.json, QByteArrayLiteral("[]"));
    QCOMPARE(empty.includedToolCount, 0);
    QCOMPARE(empty.omittedToolCount, 1);
}

void AgentControllerTest::compactCatalogPreservesComplexOmittedContract()
{
    const auto tool = inletTool();
    const auto catalog =
        application::ToolCatalogBuilder::build({tool}, 2, 16'384);

    QCOMPARE(catalog.json, QByteArrayLiteral("[]"));
    QCOMPARE(catalog.includedToolCount, 0);
    QCOMPARE(catalog.omittedToolCount, 1);
    QCOMPARE(catalog.omittedToolNames,
             QStringList{QStringLiteral("shondy.create_inlet")});
    QCOMPARE(catalog.indexedToolCount, 1);
    QCOMPARE(catalog.unindexedToolCount, 0);
    QVERIFY(catalog.indexJson.size() <= 16'384);

    const auto document = QJsonDocument::fromJson(catalog.indexJson);
    QVERIFY(document.isArray());
    QCOMPARE(document.array().size(), 1);
    const auto indexText = QString::fromUtf8(catalog.indexJson);
    QVERIFY(indexText.contains(QStringLiteral("shondy.create_inlet")));
    QVERIFY(indexText.contains(QStringLiteral("spec")));
    QVERIFY(indexText.contains(QStringLiteral("geometry_file")));
    QVERIFY(indexText.contains(QStringLiteral("file_path")));
    QVERIFY(indexText.contains(QStringLiteral("circle")));
}

void AgentControllerTest::compactCatalogPreservesDynamicKeyContract()
{
    auto tool = ledgerTool(QStringLiteral("update_fields"), false);
    tool.annotations = {{QStringLiteral("destructiveHint"), true},
                        {QStringLiteral("idempotentHint"), false}};
    tool.inputSchema = QJsonObject{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"),
         QJsonObject{
             {QStringLiteral("updates"),
              QJsonObject{
                  {QStringLiteral("type"), QStringLiteral("object")},
                  {QStringLiteral("minProperties"), 1},
                  {QStringLiteral("propertyNames"),
                   QJsonObject{{QStringLiteral("pattern"),
                                QStringLiteral("^(?:/(?:[^~/]|~0|~1)*)+$")}}},
                  {QStringLiteral("additionalProperties"),
                   QJsonObject{
                       {QStringLiteral("type"), QStringLiteral("number")}}}}}}},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("updates")}}};

    const auto compact =
        application::ToolCatalogBuilder::compactDefinition(tool);
    const auto updates = compact.value(QStringLiteral("arguments"))
                             .toObject()
                             .value(QStringLiteral("properties"))
                             .toObject()
                             .value(QStringLiteral("updates"))
                             .toObject();
    QVERIFY(compact.value(QStringLiteral("destructive")).toBool());
    QVERIFY(compact.contains(QStringLiteral("idempotent")));
    QVERIFY(!compact.value(QStringLiteral("idempotent")).toBool());
    QCOMPARE(updates.value(QStringLiteral("minProperties")).toInt(), 1);
    QCOMPARE(updates.value(QStringLiteral("propertyNames"))
                 .toObject()
                 .value(QStringLiteral("pattern"))
                 .toString(),
             QStringLiteral("^(?:/(?:[^~/]|~0|~1)*)+$"));
    QCOMPARE(updates.value(QStringLiteral("additionalProperties"))
                 .toObject()
                 .value(QStringLiteral("type"))
                 .toString(),
             QStringLiteral("number"));
}

void AgentControllerTest::repairsToolValidationWithFocusedContract()
{
    auto generationCount = 0;
    auto toolCallCount = 0;
    QList<chat::Message> generatedMessages;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [&](const QList<chat::Message>& messages,
                const models::InferencePreset&, int)
            {
                ++generationCount;
                generatedMessages = messages;
            },
            [] {},
            [&](const QString&, const QJsonObject&)
            {
                ++toolCallCount;
                return QStringLiteral("tool-request-1");
            },
            [](const QString&) {},
            [](const QString& toolName, const QJsonObject& arguments)
            {
                const auto source = arguments.value(QStringLiteral("spec"))
                                        .toObject()
                                        .value(QStringLiteral("source"))
                                        .toObject();
                if (source.value(QStringLiteral("type")).toString() ==
                    QLatin1String("geometry_file"))
                    return agent::ToolValidationResult{true, {}};
                return agent::ToolValidationResult{
                    false,
                    {toolName, QStringLiteral("arguments.spec.source.type"),
                     QStringLiteral(
                         "#/properties/spec/properties/source/oneOf"),
                     QStringLiteral("discriminator"),
                     QStringLiteral("arguments.spec.source.type must be one of "
                                    "[\"geometry_file\",\"circle\"].")}};
            },
            [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});
    QVERIFY(controller.start(QStringLiteral("Create the inlet"), {},
                             {inletTool()}));
    controller.receiveToken(QByteArrayLiteral(
        R"({"action":"call_tool","tool":"shondy.create_inlet","arguments":{"spec":{"name":"inlet_shaft","source":{"type":"geometryFile","file_path":"E:/stl_case1/inlet_shaft.stl"}}}})"));
    controller.completeGeneration(false);

    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QCOMPARE(generationCount, 2);
    QCOMPARE(toolCallCount, 0);
    QCOMPARE(controller.activeRun()->repairAttempts, 0);
    QCOMPARE(controller.activeRun()->validationRepairs, 1);
    QCOMPARE(controller.activeRun()->consecutiveValidationFailures, 1);
    const auto correction = generatedMessages.constLast().content;
    QVERIFY(correction.contains(QStringLiteral("was not executed")));
    QVERIFY(correction.contains(QStringLiteral("shondy.create_inlet")));
    QVERIFY(correction.contains(QStringLiteral("geometryFile")));
    QVERIFY(correction.contains(QStringLiteral("geometry_file")));
    QVERIFY(correction.contains(QStringLiteral("file_path")));
    QVERIFY(correction.contains(QStringLiteral("<compact_contract>")));
    QVERIFY(correction.contains(QStringLiteral("<exact_input_schema>")));
    QVERIFY(correction.contains(QStringLiteral("<validation_issue>")));
    QVERIFY(correction.contains(QStringLiteral("instancePath")));
    QVERIFY(correction.contains(QStringLiteral("schemaPath")));
    QVERIFY(correction.contains(QStringLiteral("discriminator")));
    QVERIFY(
        correction.contains(QStringLiteral("do not call list or describe")));

    controller.receiveToken(QByteArrayLiteral(
        R"({"action":"call_tool","tool":"shondy.create_inlet","arguments":{"spec":{"name":"inlet_shaft","source":{"type":"geometry_file","file_path":"E:/stl_case1/inlet_shaft.stl"}}}})"));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::ExecutingTool);
    QCOMPARE(toolCallCount, 1);
    QCOMPARE(controller.activeRun()->consecutiveValidationFailures, 0);
    controller.cancel();
}

void AgentControllerTest::completesMultiStepToolRun()
{
    auto generationCount = 0;
    auto toolCallCount = 0;
    auto toolArgumentsValid = false;
    QList<chat::Message> lastMessages;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [&](const QList<chat::Message>& messages,
                const models::InferencePreset&, int maxTokens)
            {
                ++generationCount;
                lastMessages = messages;
                QCOMPARE(maxTokens, 4'096);
            },
            [] {},
            [&](const QString& name, const QJsonObject& arguments)
            {
                ++toolCallCount;
                toolArgumentsValid =
                    name == QStringLiteral("fake.echo") &&
                    arguments.value(QStringLiteral("value")).toString() ==
                        QStringLiteral("hello");
                return QStringLiteral("tool-request-1");
            },
            [](const QString&) {}, acceptsToolArguments, [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});
    QSignalSpy finalSpy(&controller,
                        &application::AgentController::finalAnswerReady);
    QSignalSpy finishedSpy(&controller,
                           &application::AgentController::runFinished);

    const auto request =
        QStringLiteral("1. Use a tool.\n2. Report the result.");
    QVERIFY(controller.start(request, {}, {echoTool()}));
    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QCOMPARE(generationCount, 1);
    QCOMPARE(lastMessages.size(), 2);
    QCOMPARE(lastMessages.at(0).role, chat::Role::System);
    QCOMPARE(lastMessages.at(1).role, chat::Role::User);
    QVERIFY(lastMessages.constFirst().content.contains(
        QStringLiteral("fake.echo")));
    QVERIFY(
        lastMessages.constLast().content.contains(QStringLiteral("task_plan")));

    const QJsonArray plan{
        planStep(QStringLiteral("step-1"), QStringLiteral("Use a tool"), true),
        planStep(QStringLiteral("step-2"), QStringLiteral("Report the result"),
                 false)};
    controller.receiveToken(taskPlanAction(plan));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QCOMPARE(generationCount, 2);

    controller.receiveToken(QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.echo","arguments":{"value":"hello"}})"));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::ExecutingTool);
    QCOMPARE(toolCallCount, 1);
    QVERIFY(toolArgumentsValid);

    agent::ToolResult result;
    result.requestId = QStringLiteral("tool-request-1");
    result.serverId = QStringLiteral("fake");
    result.toolName = QStringLiteral("echo");
    result.result = {
        {QStringLiteral("content"), QStringLiteral("duplicate-marker")},
        {QStringLiteral("structuredContent"),
         QJsonObject{{QStringLiteral("value"),
                      QStringLiteral("legacy-structured-marker")}}}};
    result.structuredContent =
        QJsonObject{{QStringLiteral("value"), QStringLiteral("hello")}};
    controller.receiveToolResult(result);
    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QCOMPARE(generationCount, 3);
    QVERIFY(lastMessages.constLast().content.contains(
        QStringLiteral("tool_result")));
    QVERIFY(lastMessages.constLast().content.contains(
        QStringLiteral("Do not repeat this exact call")));
    QVERIFY(lastMessages.constLast().content.contains(
        QStringLiteral("only this operation is complete")));
    QVERIFY(lastMessages.constLast().content.contains(QStringLiteral("hello")));
    QVERIFY(!lastMessages.constLast().content.contains(
        QStringLiteral("duplicate-marker")));
    QVERIFY(!lastMessages.constLast().content.contains(
        QStringLiteral("legacy-structured-marker")));

    controller.receiveToken(
        QByteArrayLiteral(R"({"action":"final","content":"Task complete"})"));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QCOMPARE(generationCount, 4);
    QCOMPARE(finalSpy.count(), 0);
    QVERIFY(lastMessages.constLast().content.contains(
        QStringLiteral("Completion review required")));
    QVERIFY(lastMessages.constLast().content.contains(request));

    const QJsonArray completedSteps{
        reviewedStep(QStringLiteral("step-1"), QStringLiteral("Use a tool"),
                     true, QStringLiteral("satisfied"), QJsonArray{1}),
        reviewedStep(QStringLiteral("step-2"),
                     QStringLiteral("Report the result"), false,
                     QStringLiteral("satisfied"), QJsonArray{1})};
    controller.receiveToken(completionReviewAction(
        QStringLiteral("complete"), completedSteps,
        QStringLiteral("All requested work is complete.")));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Completed);
    QCOMPARE(finalSpy.count(), 1);
    QCOMPARE(finalSpy.at(0).at(1).toString(), QStringLiteral("Task complete"));
    QCOMPARE(finishedSpy.count(), 1);
    QVERIFY(!controller.hasActiveRun());
    QCOMPARE(controller.conversationMessages().size(), 2);
}

void AgentControllerTest::publishesStructuredProgressSnapshots()
{
    application::AgentController controller(
        application::AgentController::Dependencies{
            [](const QList<chat::Message>&, const models::InferencePreset&,
               int) {},
            [] {}, [](const QString&, const QJsonObject&)
            { return QStringLiteral("tool-request-1"); }, [](const QString&) {},
            acceptsToolArguments, [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});
    QSignalSpy progressSpy(&controller,
                           &application::AgentController::progressChanged);
    const auto latest = [&progressSpy]
    {
        return qvariant_cast<application::AgentProgressSnapshot>(
            progressSpy.constLast().constFirst());
    };

    QVERIFY(controller.start(
        QStringLiteral("1. Use a tool.\n2. Report the result."), {},
        {echoTool()}));
    QVERIFY(!progressSpy.isEmpty());
    QCOMPARE(latest().state, application::AgentRun::State::Deciding);
    QCOMPARE(latest().operation, QStringLiteral("Planning the request"));

    const QJsonArray plan{
        planStep(QStringLiteral("step-1"), QStringLiteral("Use a tool"), true),
        planStep(QStringLiteral("step-2"), QStringLiteral("Report the result"),
                 false)};
    controller.receiveToken(taskPlanAction(plan));
    controller.completeGeneration(false);
    auto snapshot = latest();
    QCOMPARE(snapshot.steps.size(), 2);
    QCOMPARE(snapshot.currentStepId, QStringLiteral("step-1"));
    QCOMPARE(snapshot.steps.constFirst().status,
             application::AgentProgressStepStatus::Current);
    QCOMPARE(snapshot.recentActivity.constLast().type,
             agent::EventType::DecisionStarted);
    QVERIFY(std::any_of(
        snapshot.recentActivity.cbegin(), snapshot.recentActivity.cend(),
        [](const application::AgentProgressActivity& activity)
        { return activity.type == agent::EventType::TaskPlanAccepted; }));

    controller.receiveToken(QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.echo","arguments":{"value":"hello"}})"));
    controller.completeGeneration(false);
    snapshot = latest();
    QCOMPARE(snapshot.state, application::AgentRun::State::ExecutingTool);
    QCOMPARE(snapshot.operation, QStringLiteral("Calling fake.echo"));
    QCOMPARE(snapshot.waitingReason,
             QStringLiteral("Waiting for the tool result"));

    agent::ToolResult result;
    result.requestId = QStringLiteral("tool-request-1");
    result.serverId = QStringLiteral("fake");
    result.toolName = QStringLiteral("echo");
    result.structuredContent =
        QJsonObject{{QStringLiteral("value"), QStringLiteral("hello")}};
    controller.receiveToolResult(result);
    snapshot = latest();
    QCOMPARE(snapshot.completedToolCount, 1);
    QCOMPARE(snapshot.evidenceCount, 1);

    controller.receiveToken(
        QByteArrayLiteral(R"({"action":"final","content":"Done"})"));
    controller.completeGeneration(false);
    const QJsonArray completedSteps{
        reviewedStep(QStringLiteral("step-1"), QStringLiteral("Use a tool"),
                     true, QStringLiteral("satisfied"), QJsonArray{1}),
        reviewedStep(QStringLiteral("step-2"),
                     QStringLiteral("Report the result"), false,
                     QStringLiteral("satisfied"), QJsonArray{1})};
    controller.receiveToken(completionReviewAction(
        QStringLiteral("complete"), completedSteps,
        QStringLiteral("All requested work is complete.")));
    controller.completeGeneration(false);
    snapshot = latest();
    QCOMPARE(snapshot.state, application::AgentRun::State::Completed);
    QCOMPARE(snapshot.steps.constFirst().status,
             application::AgentProgressStepStatus::Completed);
    QCOMPARE(snapshot.steps.constLast().status,
             application::AgentProgressStepStatus::Completed);
    QVERIFY(snapshot.currentStepId.isEmpty());
}

void AgentControllerTest::publishesApprovalRecoveryAndCancellationProgress()
{
    application::AgentController approvalController(
        application::AgentController::Dependencies{
            [](const QList<chat::Message>&, const models::InferencePreset&,
               int) {},
            [] {}, [](const QString&, const QJsonObject&)
            { return QStringLiteral("tool-request-1"); }, [](const QString&) {},
            acceptsToolArguments, [](const QString&)
            { return infrastructure::mcp::ToolDecision::RequireApproval; }});
    QVERIFY(approvalController.start(QStringLiteral("Read the current value."),
                                     {}, {echoTool()}));
    approvalController.receiveToken(QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.echo","arguments":{"value":"hello"}})"));
    approvalController.completeGeneration(false);
    auto snapshot = approvalController.progressSnapshot();
    QCOMPARE(snapshot.state, application::AgentRun::State::WaitingForApproval);
    QCOMPARE(snapshot.operation, QStringLiteral("Calling fake.echo"));
    QCOMPARE(snapshot.waitingReason,
             QStringLiteral("Waiting for your approval"));

    approvalController.resolveApproval(true);
    QCOMPARE(approvalController.progressSnapshot().state,
             application::AgentRun::State::ExecutingTool);
    approvalController.cancel();
    snapshot = approvalController.progressSnapshot();
    QCOMPARE(snapshot.state, application::AgentRun::State::Cancelled);
    QCOMPARE(snapshot.finishCode, QStringLiteral("cancelled"));

    application::AgentController recoveryController(
        application::AgentController::Dependencies{
            [](const QList<chat::Message>&, const models::InferencePreset&,
               int) {},
            [] {}, [](const QString&, const QJsonObject&)
            { return QStringLiteral("unused"); }, [](const QString&) {},
            acceptsToolArguments, [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});
    QVERIFY(recoveryController.start(QStringLiteral("Read the current value."),
                                     {}, {echoTool()}));
    recoveryController.receiveToken(
        QByteArrayLiteral(R"({"action":"unsupported"})"));
    recoveryController.completeGeneration(false);
    snapshot = recoveryController.progressSnapshot();
    QVERIFY(std::any_of(
        snapshot.recentActivity.cbegin(), snapshot.recentActivity.cend(),
        [](const application::AgentProgressActivity& activity)
        { return activity.type == agent::EventType::RecoveryStarted; }));
    QCOMPARE(snapshot.state, application::AgentRun::State::Deciding);
}

void AgentControllerTest::simpleToolRunCompletesWithoutReview()
{
    auto generationCount = 0;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [&](const QList<chat::Message>&, const models::InferencePreset&,
                int) { ++generationCount; },
            [] {}, [](const QString&, const QJsonObject&)
            { return QStringLiteral("tool-request-1"); }, [](const QString&) {},
            acceptsToolArguments, [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});
    QSignalSpy finalSpy(&controller,
                        &application::AgentController::finalAnswerReady);

    QVERIFY(controller.start(QStringLiteral("Read the current file."), {},
                             {echoTool()}));
    controller.receiveToken(QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.echo","arguments":{"value":1}})"));
    controller.completeGeneration(false);
    agent::ToolResult result;
    result.requestId = QStringLiteral("tool-request-1");
    result.serverId = QStringLiteral("fake");
    result.toolName = QStringLiteral("echo");
    controller.receiveToolResult(result);
    QCOMPARE(generationCount, 2);

    controller.receiveToken(
        QByteArrayLiteral(R"({"action":"final","content":"Read complete"})"));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Completed);
    QCOMPARE(generationCount, 2);
    QCOMPARE(finalSpy.count(), 1);
}

void AgentControllerTest::simpleTaskRecoversFromFailureWithoutStructuredReview()
{
    auto generationCount = 0;
    auto toolCallCount = 0;
    QList<chat::Message> generatedMessages;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [&](const QList<chat::Message>& messages,
                const models::InferencePreset&, int)
            {
                ++generationCount;
                generatedMessages = messages;
            },
            [] {},
            [&](const QString&, const QJsonObject&)
            {
                ++toolCallCount;
                return QStringLiteral("tool-request-%1").arg(toolCallCount);
            },
            [](const QString&) {}, acceptsToolArguments, [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});
    QSignalSpy finalSpy(&controller,
                        &application::AgentController::finalAnswerReady);

    QVERIFY(controller.start(QStringLiteral("Set the inlet flow rate."), {},
                             {echoTool()}));
    controller.receiveToken(QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.echo","arguments":{"value":1}})"));
    controller.completeGeneration(false);
    agent::ToolResult failedResult;
    failedResult.requestId = QStringLiteral("tool-request-1");
    failedResult.serverId = QStringLiteral("fake");
    failedResult.toolName = QStringLiteral("echo");
    failedResult.isError = true;
    failedResult.errorMessage = QStringLiteral("Invalid value.");
    controller.receiveToolResult(failedResult);

    controller.receiveToken(QByteArrayLiteral(
        R"({"action":"final","content":"The flow rate is set."})"));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QCOMPARE(finalSpy.count(), 0);
    QVERIFY(generatedMessages.constLast().content.contains(
        QStringLiteral("latest operation is unfinished")));

    const auto completeSuccessfulCall = [&](int value, const QString& requestId)
    {
        controller.receiveToken(
            QStringLiteral(
                R"({"action":"call_tool","tool":"fake.echo","arguments":{"value":%1}})")
                .arg(value)
                .toUtf8());
        controller.completeGeneration(false);
        agent::ToolResult result;
        result.requestId = requestId;
        result.serverId = QStringLiteral("fake");
        result.toolName = QStringLiteral("echo");
        controller.receiveToolResult(result);
    };
    completeSuccessfulCall(2, QStringLiteral("tool-request-2"));
    completeSuccessfulCall(3, QStringLiteral("tool-request-3"));
    QCOMPARE(controller.activeRun()->successfulToolResults, 2);

    controller.receiveToken(QByteArrayLiteral(
        R"({"action":"final","content":"The flow rate is set and verified."})"));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Completed);
    QCOMPARE(finalSpy.count(), 1);
    QCOMPARE(generationCount, 5);
}

void AgentControllerTest::waitsForApprovalAndHonorsRejection()
{
    auto toolCallCount = 0;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [](const QList<chat::Message>&, const models::InferencePreset&,
               int) {},
            [] {},
            [&](const QString&, const QJsonObject&)
            {
                ++toolCallCount;
                return QStringLiteral("unexpected");
            },
            [](const QString&) {}, acceptsToolArguments, [](const QString&)
            { return infrastructure::mcp::ToolDecision::RequireApproval; }});
    QSignalSpy approvalSpy(&controller,
                           &application::AgentController::approvalRequested);
    QSignalSpy finishedSpy(&controller,
                           &application::AgentController::runFinished);

    QVERIFY(controller.start(QStringLiteral("Use a tool"), {}, {echoTool()}));
    controller.receiveToken(QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.echo","arguments":{}})"));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(),
             application::AgentRun::State::WaitingForApproval);
    QCOMPARE(approvalSpy.count(), 1);
    QCOMPARE(toolCallCount, 0);

    controller.resolveApproval(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Failed);
    QCOMPARE(toolCallCount, 0);
    QCOMPARE(finishedSpy.at(0).at(2).toString(),
             QStringLiteral("approval_denied"));
}

void AgentControllerTest::repairsOnlyOneInvalidAction()
{
    auto generationCount = 0;
    auto toolCallCount = 0;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [&](const QList<chat::Message>&, const models::InferencePreset&,
                int) { ++generationCount; },
            [] {},
            [&](const QString&, const QJsonObject&)
            {
                ++toolCallCount;
                return QStringLiteral("tool");
            },
            [](const QString&) {}, acceptsToolArguments, [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});
    QSignalSpy finishedSpy(&controller,
                           &application::AgentController::runFinished);

    QVERIFY(controller.start(QStringLiteral("Do work"), {}, {echoTool()}));
    controller.receiveToken(QByteArrayLiteral("not-json"));
    controller.completeGeneration(false);
    QCOMPARE(generationCount, 2);
    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);

    controller.receiveToken(QByteArrayLiteral("still-not-json"));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Failed);
    QCOMPARE(toolCallCount, 0);
    QCOMPARE(finishedSpy.at(0).at(2).toString(),
             QStringLiteral("invalid_agent_action"));
}

void AgentControllerTest::cancelsAndIgnoresLateResponses()
{
    auto cancellationCount = 0;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [](const QList<chat::Message>&, const models::InferencePreset&,
               int) {},
            [&] { ++cancellationCount; },
            [](const QString&, const QJsonObject&) { return QString{}; },
            [](const QString&) {}, acceptsToolArguments, [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});
    QSignalSpy finalSpy(&controller,
                        &application::AgentController::finalAnswerReady);

    QVERIFY(controller.start(QStringLiteral("Do work"), {}, {echoTool()}));
    controller.cancel();
    QCOMPARE(controller.state(), application::AgentRun::State::Cancelled);
    QCOMPARE(cancellationCount, 1);
    controller.receiveToken(
        QByteArrayLiteral(R"({"action":"final","content":"late"})"));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Cancelled);
    QCOMPARE(finalSpy.count(), 0);
}

void AgentControllerTest::
    cancellationRemainsTerminalAcrossApprovalAndToolResult()
{
    auto toolCallCount = 0;
    auto toolCancellationCount = 0;
    application::AgentController approvalController(
        application::AgentController::Dependencies{
            [](const QList<chat::Message>&, const models::InferencePreset&,
               int) {},
            [] {},
            [&](const QString&, const QJsonObject&)
            {
                ++toolCallCount;
                return QStringLiteral("approval-request");
            },
            [&](const QString&) { ++toolCancellationCount; },
            acceptsToolArguments, [](const QString&)
            { return infrastructure::mcp::ToolDecision::RequireApproval; }});

    const auto action = QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.echo","arguments":{}})");
    QVERIFY(
        approvalController.start(QStringLiteral("Do work"), {}, {echoTool()}));
    approvalController.receiveToken(action);
    approvalController.completeGeneration(false);
    QCOMPARE(approvalController.state(),
             application::AgentRun::State::WaitingForApproval);
    approvalController.cancel();
    approvalController.resolveApproval(true);
    QCOMPARE(approvalController.state(),
             application::AgentRun::State::Cancelled);
    QCOMPARE(toolCallCount, 0);

    application::AgentController executionController(
        application::AgentController::Dependencies{
            [](const QList<chat::Message>&, const models::InferencePreset&,
               int) {},
            [] {},
            [&](const QString&, const QJsonObject&)
            {
                ++toolCallCount;
                return QStringLiteral("tool-request-1");
            },
            [&](const QString&) { ++toolCancellationCount; },
            acceptsToolArguments, [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});
    QVERIFY(
        executionController.start(QStringLiteral("Do work"), {}, {echoTool()}));
    executionController.receiveToken(action);
    executionController.completeGeneration(false);
    QCOMPARE(executionController.state(),
             application::AgentRun::State::ExecutingTool);
    executionController.cancel();
    QCOMPARE(toolCancellationCount, 1);

    agent::ToolResult lateResult;
    lateResult.requestId = QStringLiteral("tool-request-1");
    lateResult.serverId = QStringLiteral("fake");
    lateResult.toolName = QStringLiteral("echo");
    executionController.receiveToolResult(lateResult);
    QCOMPARE(executionController.state(),
             application::AgentRun::State::Cancelled);
    QCOMPARE(executionController.activeRun()->successfulToolResults, 0);
}

void AgentControllerTest::doesNotRetryUncertainMutationAfterTransportFailure()
{
    auto toolCallCount = 0;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [](const QList<chat::Message>&, const models::InferencePreset&,
               int) {},
            [] {},
            [&](const QString&, const QJsonObject&)
            {
                ++toolCallCount;
                return QStringLiteral("tool-request-%1").arg(toolCallCount);
            },
            [](const QString&) {}, acceptsToolArguments, [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});
    QSignalSpy finishedSpy(&controller,
                           &application::AgentController::runFinished);

    const auto action = QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.mutate","arguments":{}})");
    QVERIFY(controller.start(QStringLiteral("Change the remote value"), {},
                             {ledgerTool(QStringLiteral("mutate"), false)}));
    controller.receiveToken(action);
    controller.completeGeneration(false);
    QCOMPARE(toolCallCount, 1);

    agent::ToolResult timeout;
    timeout.requestId = QStringLiteral("tool-request-1");
    timeout.serverId = QStringLiteral("fake");
    timeout.toolName = QStringLiteral("mutate");
    timeout.isError = true;
    timeout.errorCode = QStringLiteral("timeout");
    timeout.errorMessage = QStringLiteral("MCP request timed out.");
    timeout.failureKind = agent::ToolFailureKind::Transport;
    controller.receiveToolResult(timeout);

    QCOMPARE(controller.state(), application::AgentRun::State::Failed);
    QCOMPARE(toolCallCount, 1);
    QCOMPARE(finishedSpy.count(), 1);
    QCOMPARE(finishedSpy.constFirst().at(2).toString(),
             QStringLiteral("tool_side_effect_uncertain"));
    QCOMPARE(controller.activeRun()->successfulToolResults, 0);
}

void AgentControllerTest::retriesReadOnlyTransportFailureOnce()
{
    auto generationCount = 0;
    auto toolCallCount = 0;
    QList<chat::Message> generatedMessages;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [&](const QList<chat::Message>& messages,
                const models::InferencePreset&, int)
            {
                ++generationCount;
                generatedMessages = messages;
            },
            [] {},
            [&](const QString&, const QJsonObject&)
            {
                ++toolCallCount;
                return QStringLiteral("tool-request-%1").arg(toolCallCount);
            },
            [](const QString&) {}, acceptsToolArguments, [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});

    const auto action = QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.inspect","arguments":{}})");
    QVERIFY(controller.start(QStringLiteral("Inspect the remote value"), {},
                             {ledgerTool(QStringLiteral("inspect"), true)}));
    controller.receiveToken(action);
    controller.completeGeneration(false);
    QCOMPARE(toolCallCount, 1);

    const auto deliverTimeout = [&](const QString& requestId)
    {
        agent::ToolResult timeout;
        timeout.requestId = requestId;
        timeout.serverId = QStringLiteral("fake");
        timeout.toolName = QStringLiteral("inspect");
        timeout.isError = true;
        timeout.errorCode = QStringLiteral("timeout");
        timeout.errorMessage = QStringLiteral("MCP request timed out.");
        timeout.failureKind = agent::ToolFailureKind::Transport;
        controller.receiveToolResult(timeout);
    };
    deliverTimeout(QStringLiteral("tool-request-1"));
    QCOMPARE(controller.state(), application::AgentRun::State::ExecutingTool);
    QCOMPARE(toolCallCount, 2);
    QCOMPARE(generationCount, 1);

    deliverTimeout(QStringLiteral("tool-request-2"));
    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QCOMPARE(toolCallCount, 2);
    QCOMPARE(generationCount, 2);
    QVERIFY(generatedMessages.constLast().content.contains(
        QStringLiteral("side effect is unknown")));

    controller.receiveToken(action);
    controller.completeGeneration(false);
    QCOMPARE(toolCallCount, 2);
    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    controller.cancel();
}

void AgentControllerTest::exportsStructuredRunMetrics()
{
    auto toolCallCount = 0;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [](const QList<chat::Message>&, const models::InferencePreset&,
               int) {},
            [] {},
            [&](const QString&, const QJsonObject&)
            {
                ++toolCallCount;
                return QStringLiteral("tool-request-%1").arg(toolCallCount);
            },
            [](const QString&) {},
            [](const QString& toolName, const QJsonObject& arguments)
            {
                if (arguments.value(QStringLiteral("valid")).toBool())
                    return agent::ToolValidationResult{true, {}};
                return agent::ToolValidationResult{
                    false,
                    {toolName, QStringLiteral("arguments.valid"),
                     QStringLiteral("#/properties/valid"),
                     QStringLiteral("required"),
                     QStringLiteral("arguments.valid is required.")}};
            },
            [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});
    QSignalSpy metricsSpy(&controller,
                          &application::AgentController::metricsReady);
    const auto invalidAction = QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.list_items","arguments":{}})");
    const auto validAction = QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.list_items","arguments":{"valid":true}})");

    QVERIFY(controller.start(QStringLiteral("Inspect items"), {},
                             {ledgerTool(QStringLiteral("list_items"), true)}));
    controller.receiveToken(invalidAction);
    controller.completeGeneration(false);
    controller.receiveToken(validAction);
    controller.completeGeneration(false);

    agent::ToolResult result;
    result.requestId = QStringLiteral("tool-request-1");
    result.serverId = QStringLiteral("fake");
    result.toolName = QStringLiteral("list_items");
    controller.receiveToolResult(result);

    controller.receiveToken(validAction);
    controller.completeGeneration(false);
    QCOMPARE(toolCallCount, 1);
    controller.receiveToken(
        QByteArrayLiteral(R"({"action":"final","content":"Done"})"));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Completed);

    const auto metrics =
        application::AgentRunMetrics::fromRun(*controller.activeRun());
    QCOMPARE(metrics.state, QStringLiteral("completed"));
    QCOMPARE(metrics.firstToolChoice, QStringLiteral("fake.list_items"));
    QCOMPARE(metrics.decisionCount, 4);
    QCOMPARE(metrics.toolActionAttempts, 3);
    QCOMPARE(metrics.toolValidationAttempts, 2);
    QCOMPARE(metrics.toolValidationFailures, 1);
    QCOMPARE(metrics.validationRepairs, 1);
    QCOMPARE(metrics.executedToolCalls, 1);
    QCOMPARE(metrics.duplicateToolActions, 1);
    QCOMPARE(metrics.duplicateMutationActions, 0);
    QCOMPARE(metrics.redundantDiscoveryCalls, 1);
    QCOMPARE(metrics.schemaValidArgumentRate, 0.5);
    QVERIFY(!metrics.completionReviewSucceeded);

    const auto json = metrics.toJson();
    QCOMPARE(json.value(QStringLiteral("schemaVersion")).toInt(), 1);
    QCOMPARE(json.value(QStringLiteral("toolEvidenceCount")).toInt(), 1);
    QCOMPARE(json.value(QStringLiteral("finishCode")).toString(), QString{});
    const auto taskTimings =
        json.value(QStringLiteral("taskTimings")).toArray();
    QCOMPARE(taskTimings.size(), 1);
    QCOMPARE(taskTimings.constFirst()
                 .toObject()
                 .value(QStringLiteral("kind"))
                 .toString(),
             QStringLiteral("execution"));
    QCOMPARE(taskTimings.constFirst()
                 .toObject()
                 .value(QStringLiteral("status"))
                 .toString(),
             QStringLiteral("completed"));
    QCOMPARE(metricsSpy.count(), 1);
    const auto emittedMetrics =
        qvariant_cast<QJsonObject>(metricsSpy.constFirst().at(1));
    QCOMPARE(emittedMetrics.value(QStringLiteral("state")).toString(),
             QStringLiteral("completed"));
}

void AgentControllerTest::classifiesRunMetricFailures()
{
    application::AgentRun run;
    run.state = application::AgentRun::State::Failed;
    const auto category = [&run](const QString& code)
    {
        run.finishCode = code;
        return application::AgentRunMetrics::fromRun(run).failureCategory;
    };

    QCOMPARE(category(QStringLiteral("invalid_tool_arguments")),
             QStringLiteral("agent_action"));
    QCOMPARE(category(QStringLiteral("decision_too_large")),
             QStringLiteral("agent_action"));
    QCOMPARE(category(QStringLiteral("tool_denied")),
             QStringLiteral("authorization"));
    QCOMPARE(category(QStringLiteral("tool_side_effect_uncertain")),
             QStringLiteral("transport_or_uncertain_side_effect"));
    QCOMPARE(category(QStringLiteral("completion_unverified")),
             QStringLiteral("verification"));
    QCOMPARE(category(QStringLiteral("worker_crashed")),
             QStringLiteral("generation"));
    QCOMPARE(category(QStringLiteral("model_context_failed")),
             QStringLiteral("generation"));
    QCOMPARE(category(QStringLiteral("tool_call_failed")),
             QStringLiteral("tool"));
}

void AgentControllerTest::keepsConversationHistoryAndClearsIt()
{
    QList<chat::Message> generatedMessages;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [&](const QList<chat::Message>& messages,
                const models::InferencePreset&, int)
            { generatedMessages = messages; },
            [] {}, [](const QString&, const QJsonObject&)
            { return QStringLiteral("unused"); }, [](const QString&) {},
            acceptsToolArguments, [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});
    QSignalSpy clearedSpy(&controller,
                          &application::AgentController::conversationCleared);

    QVERIFY(controller.start(QStringLiteral("First question"), {}, {}));
    controller.receiveToken(
        QByteArrayLiteral(R"({"action":"final","content":"First answer"})"));
    controller.completeGeneration(false);
    QVERIFY(controller.hasConversation());

    QVERIFY(controller.start(QStringLiteral("Follow-up"), {}, {}));
    QCOMPARE(generatedMessages.size(), 4);
    QCOMPARE(generatedMessages.at(1).content, QStringLiteral("First question"));
    QCOMPARE(generatedMessages.at(2).content, QStringLiteral("First answer"));
    QCOMPARE(generatedMessages.at(3).content, QStringLiteral("Follow-up"));
    QVERIFY(!controller.setConversationMessages({}));
    controller.cancel();

    const QList<chat::Message> sharedHistory{
        {chat::Role::User, QStringLiteral("Shared question")},
        {chat::Role::Assistant, QStringLiteral("Shared answer")}};
    QVERIFY(controller.setConversationMessages(sharedHistory));
    QCOMPARE(controller.conversationMessages(), sharedHistory);

    QVERIFY(controller.clearConversation());
    QVERIFY(!controller.hasConversation());
    QCOMPARE(clearedSpy.count(), 1);
}

void AgentControllerTest::emptyToolPromptForbidsToolCalls()
{
    QList<chat::Message> generatedMessages;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [&](const QList<chat::Message>& messages,
                const models::InferencePreset&, int)
            { generatedMessages = messages; },
            [] {}, [](const QString&, const QJsonObject&)
            { return QStringLiteral("unused"); }, [](const QString&) {},
            acceptsToolArguments, [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});

    QVERIFY(controller.start(QStringLiteral("Write a C++ file"), {}, {}));
    QVERIFY(!generatedMessages.isEmpty());
    const auto prompt = generatedMessages.constFirst().content;
    QVERIFY(prompt.contains(QStringLiteral("No tools are available")));
    QVERIFY(prompt.contains(QStringLiteral("must not call a tool")));
    QVERIFY(!prompt.contains(QStringLiteral("server.tool")));
    controller.cancel();
}

void AgentControllerTest::casualPromptPrefersFinalWithoutTools()
{
    QList<chat::Message> generatedMessages;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [&](const QList<chat::Message>& messages,
                const models::InferencePreset&, int)
            { generatedMessages = messages; },
            [] {}, [](const QString&, const QJsonObject&)
            { return QStringLiteral("unused"); }, [](const QString&) {},
            acceptsToolArguments, [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});

    QVERIFY(controller.start(QStringLiteral("Hello"), {}, {echoTool()}));
    QVERIFY(!generatedMessages.isEmpty());
    const auto prompt = generatedMessages.constFirst().content;
    QVERIFY(prompt.contains(QStringLiteral("Greetings, casual conversation")));
    QVERIFY(prompt.contains(
        QStringLiteral("must return final without calling a tool")));
    QVERIFY(prompt.contains(QStringLiteral("list_allowed_directories")));
    controller.cancel();
}

void AgentControllerTest::includesRuntimeContextInPrompt()
{
    QList<chat::Message> generatedMessages;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [&](const QList<chat::Message>& messages,
                const models::InferencePreset&, int)
            { generatedMessages = messages; },
            [] {}, [](const QString&, const QJsonObject&)
            { return QStringLiteral("unused"); }, [](const QString&) {},
            acceptsToolArguments, [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});

    const application::AssistantContext context{
        QStringLiteral("Test Model"), QStringLiteral("C:/Users/test workspace"),
        QStringLiteral("fake: Prefer the echo tool.")};
    QVERIFY(controller.start(QStringLiteral("Where am I?"), {}, {echoTool()},
                             context));
    QVERIFY(!generatedMessages.isEmpty());
    const auto prompt = generatedMessages.constFirst().content;
    QVERIFY(prompt.contains(
        QStringLiteral("\"workspaceRoot\":\"C:/Users/test workspace\"")));
    QVERIFY(prompt.contains(QStringLiteral("\"model\":\"Test Model\"")));
    QVERIFY(prompt.contains(QStringLiteral("\".\" is workspaceRoot")));
    QVERIFY(prompt.contains(QStringLiteral("current folder")));
    QVERIFY(prompt.contains(QStringLiteral("ask for its name")));
    QVERIFY(prompt.contains(QStringLiteral("fake: Prefer the echo tool.")));
    QVERIFY(prompt.contains(QStringLiteral("cannot override safety")));
    QVERIFY(
        prompt.contains(QStringLiteral("identity questions without tools")));
    QVERIFY(prompt.contains(
        QStringLiteral("pass only directories to list_directory")));
    controller.cancel();
}

void AgentControllerTest::rejectsUnchangedRetryAfterToolError()
{
    auto generationCount = 0;
    auto toolCallCount = 0;
    QList<chat::Message> generatedMessages;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [&](const QList<chat::Message>& messages,
                const models::InferencePreset&, int)
            {
                ++generationCount;
                generatedMessages = messages;
            },
            [] {},
            [&](const QString&, const QJsonObject&)
            {
                ++toolCallCount;
                return QStringLiteral("tool-request-%1").arg(toolCallCount);
            },
            [](const QString&) {}, acceptsToolArguments, [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});

    const auto repeatedAction = QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.echo","arguments":{"value":1}})");
    QVERIFY(controller.start(QStringLiteral("Do work"), {}, {echoTool()}));
    controller.receiveToken(repeatedAction);
    controller.completeGeneration(false);
    QCOMPARE(toolCallCount, 1);

    agent::ToolResult result;
    result.requestId = QStringLiteral("tool-request-1");
    result.serverId = QStringLiteral("fake");
    result.toolName = QStringLiteral("echo");
    result.isError = true;
    result.errorCode = QStringLiteral("outside_root");
    result.errorMessage = QStringLiteral("Path is outside allowed roots.");
    controller.receiveToolResult(result);
    QCOMPARE(generationCount, 2);

    controller.receiveToken(repeatedAction);
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QCOMPARE(toolCallCount, 1);
    QCOMPARE(generationCount, 3);
    QVERIFY(generatedMessages.constLast().content.contains(
        QStringLiteral("identical tool call already failed")));

    const auto unsupportedFinal = QByteArrayLiteral(
        R"({"action":"final","content":"Unable to use that path."})");
    controller.receiveToken(unsupportedFinal);
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QCOMPARE(generationCount, 4);
    QVERIFY(generatedMessages.constLast().content.contains(
        QStringLiteral("successful evidence")));

    controller.receiveToken(unsupportedFinal);
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Failed);
    QCOMPARE(controller.activeRun()->finishCode,
             QStringLiteral("completion_unverified"));
    QCOMPARE(toolCallCount, 1);
}

void AgentControllerTest::repairsDynamicPropertyNameValidation()
{
    auto toolCallCount = 0;
    QList<chat::Message> generatedMessages;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [&](const QList<chat::Message>& messages,
                const models::InferencePreset&, int)
            { generatedMessages = messages; },
            [] {},
            [&](const QString&, const QJsonObject&)
            {
                ++toolCallCount;
                return QStringLiteral("tool-request-%1").arg(toolCallCount);
            },
            [](const QString&) {},
            [](const QString& toolName, const QJsonObject& arguments)
            {
                const auto updates =
                    arguments.value(QStringLiteral("updates")).toObject();
                if (!updates.isEmpty() &&
                    updates.constBegin().key().startsWith(QLatin1Char('/')))
                    return agent::ToolValidationResult{true, {}};
                return agent::ToolValidationResult{
                    false,
                    {toolName, QStringLiteral("arguments.updates"),
                     QStringLiteral(
                         "#/properties/updates/propertyNames/pattern"),
                     QStringLiteral("propertyNames"),
                     QStringLiteral(
                         "A dynamic property name must match pattern ^/.")}};
            },
            [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});

    QVERIFY(
        controller.start(QStringLiteral("Update one dynamic field"), {},
                         {ledgerTool(QStringLiteral("update_fields"), false)}));
    controller.receiveToken(QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.update_fields","arguments":{"updates":{"~1density~1fixedValue":1.5}}})"));
    controller.completeGeneration(false);

    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QCOMPARE(toolCallCount, 0);
    QCOMPARE(controller.activeRun()->stagnationRecoveries, 0);
    QCOMPARE(controller.activeRun()->validationRepairs, 1);
    QCOMPARE(controller.activeRun()->toolValidationAttempts, 1);
    QCOMPARE(controller.activeRun()->toolValidationFailures, 1);
    QVERIFY(generatedMessages.constLast().content.contains(
        QStringLiteral("propertyNames")));
    QVERIFY(generatedMessages.constLast().content.contains(
        QStringLiteral("instancePath and schemaPath only locate")));

    controller.receiveToken(QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.update_fields","arguments":{"updates":{"/density/fixedValue":1.5}}})"));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::ExecutingTool);
    QCOMPARE(toolCallCount, 1);
    QCOMPARE(controller.activeRun()->consecutiveValidationFailures, 0);
    controller.cancel();
}

void AgentControllerTest::repairsMissingOrderedPlanMetadata()
{
    auto toolCallCount = 0;
    QList<chat::Message> generatedMessages;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [&](const QList<chat::Message>& messages,
                const models::InferencePreset&, int)
            { generatedMessages = messages; },
            [] {},
            [&](const QString&, const QJsonObject&)
            {
                ++toolCallCount;
                return QStringLiteral("tool-request-%1").arg(toolCallCount);
            },
            [](const QString&) {}, acceptsToolArguments, [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});

    QVERIFY(controller.start(QStringLiteral("1. Inspect the workspace.\n"
                                            "2. Report the result."),
                             {},
                             {ledgerTool(QStringLiteral("inspect"), true)}));
    const QJsonArray plan{planStep(QStringLiteral("inspect"),
                                   QStringLiteral("Inspect the workspace"),
                                   true, {QStringLiteral("fake.inspect")}),
                          planStep(QStringLiteral("report"),
                                   QStringLiteral("Report the result"), false)};
    controller.receiveToken(orderedTaskPlanAction(plan));
    controller.completeGeneration(false);
    QVERIFY(controller.activeRun()->planningTask.has_value());
    QCOMPARE(controller.activeRun()->planningTask->status(),
             application::AgentTask::Status::Completed);
    QVERIFY(controller.activeRun()
                ->planningTask->runtimeSnapshot()
                .finishedAt.isValid());
    controller.receiveToken(QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.inspect","arguments":{}})"));
    controller.completeGeneration(false);

    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QCOMPARE(toolCallCount, 0);
    QCOMPARE(controller.activeRun()->stagnationRecoveries, 0);
    QCOMPARE(controller.activeRun()->orderedPlanRepairs, 1);
    QVERIFY(generatedMessages.constLast().content.contains(
        QStringLiteral("missing plan_step_id")));

    controller.receiveToken(QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.inspect","arguments":{},"plan_step_id":"inspect"})"));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QCOMPARE(toolCallCount, 0);
    QCOMPARE(controller.activeRun()->orderedPlanRepairs, 2);
    QVERIFY(generatedMessages.constLast().content.contains(
        QStringLiteral("missing completes_plan_step")));

    controller.receiveToken(QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.inspect","arguments":{},"plan_step_id":"inspect","completes_plan_step":false})"));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::ExecutingTool);
    QCOMPARE(toolCallCount, 1);

    agent::ToolResult result;
    result.requestId = QStringLiteral("tool-request-1");
    result.serverId = QStringLiteral("fake");
    result.toolName = QStringLiteral("inspect");
    result.structuredContent =
        QJsonObject{{QStringLiteral("ok"), true},
                    {QStringLiteral("workspace"), QStringLiteral("workspace")}};
    controller.receiveToolResult(result);
    QVERIFY(generatedMessages.constLast().content.contains(
        QStringLiteral("Task-plan step 'inspect' remains current")));

    controller.cancel();
}

void AgentControllerTest::blocksOrderedPlanWhenRequiredInputIsMissing()
{
    auto toolCallCount = 0;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [](const QList<chat::Message>&, const models::InferencePreset&,
               int) {},
            [] {},
            [&](const QString&, const QJsonObject&)
            {
                ++toolCallCount;
                return QStringLiteral("unexpected");
            },
            [](const QString&) {}, acceptsToolArguments, [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});
    QSignalSpy finalSpy(&controller,
                        &application::AgentController::finalAnswerReady);
    QSignalSpy finishedSpy(&controller,
                           &application::AgentController::runFinished);
    QSignalSpy metricsSpy(&controller,
                          &application::AgentController::metricsReady);

    QVERIFY(controller.start(
        QStringLiteral("1. Create the case.\n2. Report the result."), {},
        {ledgerTool(QStringLiteral("create"), false)}));
    const QJsonArray plan{
        planStep(QStringLiteral("create"), QStringLiteral("Create the case"),
                 true, {QStringLiteral("fake.create")}),
        planStep(QStringLiteral("report"), QStringLiteral("Report the result"),
                 false)};
    controller.receiveToken(orderedTaskPlanAction(plan));
    controller.completeGeneration(false);
    controller.receiveToken(QByteArrayLiteral(
        R"({"action":"blocked","reason":"missing_input","content":"Provide the parent directory for the case."})"));
    controller.completeGeneration(false);

    QCOMPARE(toolCallCount, 0);
    QCOMPARE(controller.state(), application::AgentRun::State::Blocked);
    QVERIFY(!controller.hasActiveRun());
    QCOMPARE(finalSpy.count(), 1);
    QCOMPARE(finalSpy.constFirst().at(1).toString(),
             QStringLiteral("Provide the parent directory for the case."));
    QCOMPARE(finishedSpy.count(), 1);
    QCOMPARE(
        finishedSpy.constFirst().at(1).value<application::AgentRun::State>(),
        application::AgentRun::State::Blocked);
    QCOMPARE(finishedSpy.constFirst().at(2).toString(),
             QStringLiteral("blocked_missing_input"));
    QCOMPARE(controller.conversationMessages().size(), 2);

    const auto steps = controller.activeRun()->completionSteps;
    QCOMPARE(steps.at(0).toObject().value(QStringLiteral("status")).toString(),
             QStringLiteral("blocked"));
    QCOMPARE(steps.at(1).toObject().value(QStringLiteral("status")).toString(),
             QString{});
    const auto progress = controller.progressSnapshot();
    QCOMPARE(progress.currentStepId, QString{});
    QCOMPARE(progress.steps.at(0).status,
             application::AgentProgressStepStatus::Blocked);
    QCOMPARE(progress.steps.at(1).status,
             application::AgentProgressStepStatus::Pending);
    QCOMPARE(metricsSpy.count(), 1);
    const auto metrics = metricsSpy.constFirst().at(1).toJsonObject();
    QCOMPARE(metrics.value(QStringLiteral("state")).toString(),
             QStringLiteral("blocked"));
    QCOMPARE(metrics.value(QStringLiteral("failureCategory")).toString(),
             QStringLiteral("blocked"));
}

void AgentControllerTest::
    successfulStructuredMutationAdvancesPlanWithoutReadBack()
{
    auto toolCallCount = 0;
    QList<chat::Message> generatedMessages;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [&](const QList<chat::Message>& messages,
                const models::InferencePreset&, int)
            { generatedMessages = messages; },
            [] {},
            [&](const QString&, const QJsonObject&)
            {
                ++toolCallCount;
                return QStringLiteral("tool-request-%1").arg(toolCallCount);
            },
            [](const QString&) {}, acceptsToolArguments, [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});

    QVERIFY(controller.start(
        QStringLiteral("1. Provision workspace2.\n2. Create its resource."), {},
        {ledgerTool(QStringLiteral("provision_workspace"), false),
         ledgerTool(QStringLiteral("add_object"), false)}));
    const QJsonArray plan{
        planStep(QStringLiteral("workspace"),
                 QStringLiteral("Provision workspace2"), true,
                 {QStringLiteral("fake.provision_workspace")}),
        planStep(QStringLiteral("resource"),
                 QStringLiteral("Create the resource"), true,
                 {QStringLiteral("fake.add_object")})};
    controller.receiveToken(orderedTaskPlanAction(plan));
    controller.completeGeneration(false);
    controller.receiveToken(QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.provision_workspace","arguments":{"workspace_name":"workspace2"},"plan_step_id":"workspace","completes_plan_step":true})"));
    controller.completeGeneration(false);
    QCOMPARE(toolCallCount, 1);

    agent::ToolResult created;
    created.requestId = QStringLiteral("tool-request-1");
    created.serverId = QStringLiteral("fake");
    created.toolName = QStringLiteral("provision_workspace");
    created.structuredContent = QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("workspace_name"), QStringLiteral("workspace2")},
        {QStringLiteral("workspace_path"),
         QStringLiteral("E:\\workspaces\\workspace2")},
        {QStringLiteral("result"), QJsonObject{}}};
    controller.receiveToolResult(created);

    QCOMPARE(controller.activeRun()
                 ->completionSteps.at(0)
                 .toObject()
                 .value(QStringLiteral("status"))
                 .toString(),
             QString{});
    QCOMPARE(controller.activeRun()->executionTasks.at(0).status(),
             application::ExecutionTask::Status::WaitingForModel);
    QVERIFY(generatedMessages.constLast().content.contains(
        QStringLiteral("review_plan_step")));
    for (qsizetype index = 1; index < generatedMessages.size(); ++index)
        QVERIFY2(generatedMessages.at(index - 1).role !=
                     generatedMessages.at(index).role,
                 "Agent inference messages must alternate roles.");
    controller.receiveToken(planStepReviewAction(
        QStringLiteral("workspace"), QStringLiteral("satisfied"), QJsonArray{1},
        QStringLiteral("workspace2 was created at the requested target.")));
    controller.completeGeneration(false);

    const auto steps = controller.activeRun()->completionSteps;
    QCOMPARE(steps.at(0).toObject().value(QStringLiteral("status")).toString(),
             QStringLiteral("satisfied"));
    QCOMPARE(steps.at(0).toObject().value(QStringLiteral("evidence")).toArray(),
             QJsonArray{1});
    QCOMPARE(steps.at(1).toObject().value(QStringLiteral("status")).toString(),
             QString{});
    QVERIFY(!controller.activeRun()->ledger.hasUnresolvedVerification());
    QCOMPARE(controller.progressSnapshot().currentStepId,
             QStringLiteral("resource"));
    QVERIFY(generatedMessages.constLast().content.contains(
        QStringLiteral("Create the resource")));
    QVERIFY(generatedMessages.constLast().content.contains(
        QStringLiteral("fake.add_object")));
}

void AgentControllerTest::
    keepsOrderedStepPendingWhenEvidenceMissesRequestedTarget()
{
    auto toolCallCount = 0;
    QList<chat::Message> generatedMessages;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [&](const QList<chat::Message>& messages,
                const models::InferencePreset&, int)
            { generatedMessages = messages; },
            [] {},
            [&](const QString&, const QJsonObject&)
            {
                ++toolCallCount;
                return QStringLiteral("tool-request-%1").arg(toolCallCount);
            },
            [](const QString&) {}, acceptsToolArguments, [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});

    QVERIFY(controller.start(
        QStringLiteral("1. Create case demo-mcp.\n"
                       "2. Create its fluid region."),
        {},
        {ledgerTool(QStringLiteral("create_case"), false),
         ledgerTool(QStringLiteral("create_fluid_region"), false),
         ledgerTool(QStringLiteral("inspect"), true)}));
    const QJsonArray plan{
        planStep(QStringLiteral("case"), QStringLiteral("Create case demo-mcp"),
                 true, {QStringLiteral("fake.create_case")}),
        planStep(QStringLiteral("fluid"),
                 QStringLiteral("Create the fluid region"), true,
                 {QStringLiteral("fake.create_fluid_region")})};
    controller.receiveToken(orderedTaskPlanAction(plan));
    controller.completeGeneration(false);
    controller.receiveToken(QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.create_case","arguments":{"case_name":"stl_case1"},"plan_step_id":"case","completes_plan_step":true})"));
    controller.completeGeneration(false);

    agent::ToolResult wrongCase;
    wrongCase.requestId = QStringLiteral("tool-request-1");
    wrongCase.serverId = QStringLiteral("fake");
    wrongCase.toolName = QStringLiteral("create_case");
    wrongCase.structuredContent =
        QJsonObject{{QStringLiteral("ok"), true},
                    {QStringLiteral("case_name"), QStringLiteral("stl_case1")},
                    {QStringLiteral("is_open"), true}};
    controller.receiveToolResult(wrongCase);
    QCOMPARE(controller.activeRun()->executionTasks.at(0).status(),
             application::ExecutionTask::Status::WaitingForModel);

    controller.receiveToken(planStepReviewAction(
        QStringLiteral("case"), QStringLiteral("pending"), QJsonArray{1},
        QStringLiteral(
            "Evidence created stl_case1, not the requested demo-mcp.")));
    controller.completeGeneration(false);
    QCOMPARE(controller.activeRun()
                 ->completionSteps.at(0)
                 .toObject()
                 .value(QStringLiteral("status"))
                 .toString(),
             QString{});
    QCOMPARE(controller.progressSnapshot().currentStepId,
             QStringLiteral("case"));

    controller.receiveToken(QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.inspect","arguments":{"case_name":"demo-mcp"},"plan_step_id":"case","completes_plan_step":false})"));
    controller.completeGeneration(false);
    QCOMPARE(toolCallCount, 2);
    QCOMPARE(controller.state(), application::AgentRun::State::ExecutingTool);
    agent::ToolResult inspection;
    inspection.requestId = QStringLiteral("tool-request-2");
    inspection.serverId = QStringLiteral("fake");
    inspection.toolName = QStringLiteral("inspect");
    inspection.structuredContent =
        QJsonObject{{QStringLiteral("ok"), true},
                    {QStringLiteral("case_name"), QStringLiteral("demo-mcp")},
                    {QStringLiteral("exists"), false}};
    controller.receiveToolResult(inspection);

    controller.receiveToken(QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.create_fluid_region","arguments":{},"plan_step_id":"case","completes_plan_step":true})"));
    controller.completeGeneration(false);
    QCOMPARE(toolCallCount, 2);
    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QVERIFY(generatedMessages.constLast().content.contains(
        QStringLiteral("not assigned to current task")));
    controller.cancel();
}

void AgentControllerTest::keepsSharedMutationWithinCurrentPlanStep()
{
    auto toolCallCount = 0;
    QJsonObject executedArguments;
    QList<chat::Message> generatedMessages;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [&](const QList<chat::Message>& messages,
                const models::InferencePreset&, int)
            { generatedMessages = messages; },
            [] {},
            [&](const QString&, const QJsonObject& arguments)
            {
                ++toolCallCount;
                executedArguments = arguments;
                return QStringLiteral("tool-request-%1").arg(toolCallCount);
            },
            [](const QString&) {}, acceptsToolArguments, [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});

    QVERIFY(controller.start(
        QStringLiteral("1. Import shaft.stl as a solid region.\n"
                       "2. Import inlet_shaft.stl as an inlet.\n"
                       "3. Import qyck_shaft.stl as a sampling window."),
        {}, {ledgerTool(QStringLiteral("import_stl"), false)}));
    const QJsonArray plan{
        planStep(QStringLiteral("solid"),
                 QStringLiteral("Import shaft.stl as object_type solidRegion"),
                 true, {QStringLiteral("fake.import_stl")}),
        planStep(QStringLiteral("inlet"),
                 QStringLiteral("Import inlet_shaft.stl as object_type inlet"),
                 true, {QStringLiteral("fake.import_stl")}),
        planStep(QStringLiteral("sample"),
                 QStringLiteral("Import qyck_shaft.stl as a sampling window"),
                 true, {QStringLiteral("fake.import_stl")})};
    controller.receiveToken(orderedTaskPlanAction(plan));
    controller.completeGeneration(false);

    controller.receiveToken(QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.import_stl","arguments":{"stl_file":"E:/stl_case1/inlet_shaft.stl","object_type":"inlet"},"plan_step_id":"solid","completes_plan_step":true})"));
    controller.completeGeneration(false);
    QCOMPARE(toolCallCount, 0);
    QVERIFY(generatedMessages.constLast().content.contains(
        QStringLiteral("review_tool_call")));
    QVERIFY(generatedMessages.constLast().content.contains(
        QStringLiteral("shaft.stl")));

    controller.receiveToken(toolCallReviewAction(
        QStringLiteral("reject"),
        QStringLiteral("The proposed call performs the inlet step.")));
    controller.completeGeneration(false);
    QCOMPARE(toolCallCount, 0);
    QCOMPARE(controller.progressSnapshot().currentStepId,
             QStringLiteral("solid"));
    QVERIFY(generatedMessages.constLast().content.contains(
        QStringLiteral("only this current step")));

    controller.receiveToken(QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.import_stl","arguments":{"stl_file":"E:/stl_case1/shaft.stl","object_type":"solidRegion"},"plan_step_id":"solid","completes_plan_step":true})"));
    controller.completeGeneration(false);
    QCOMPARE(toolCallCount, 0);
    controller.receiveToken(toolCallReviewAction(
        QStringLiteral("allow"),
        QStringLiteral("The call exactly imports the requested solid.")));
    controller.completeGeneration(false);
    QCOMPARE(toolCallCount, 1);
    QCOMPARE(executedArguments.value(QStringLiteral("object_type")).toString(),
             QStringLiteral("solidRegion"));
    QCOMPARE(executedArguments.value(QStringLiteral("stl_file")).toString(),
             QStringLiteral("E:/stl_case1/shaft.stl"));

    agent::ToolResult imported;
    imported.requestId = QStringLiteral("tool-request-1");
    imported.serverId = QStringLiteral("fake");
    imported.toolName = QStringLiteral("import_stl");
    imported.structuredContent = QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("object_type"), QStringLiteral("solidRegion")},
        {QStringLiteral("source_file"),
         QStringLiteral("E:/stl_case1/shaft.stl")},
        {QStringLiteral("object_uuid"), QStringLiteral("solid-1")}};
    controller.receiveToolResult(imported);
    QCOMPARE(controller.activeRun()->executionTasks.at(0).status(),
             application::ExecutionTask::Status::WaitingForModel);
    controller.receiveToken(planStepReviewAction(
        QStringLiteral("solid"), QStringLiteral("satisfied"), QJsonArray{1},
        QStringLiteral("The solid import matches the current step.")));
    controller.completeGeneration(false);

    QCOMPARE(controller.activeRun()->currentExecutionTaskIndex, 1);
    QCOMPARE(controller.progressSnapshot().currentStepId,
             QStringLiteral("inlet"));
    const auto& tasks = controller.activeRun()->executionTasks;
    QCOMPARE(tasks.at(0).status(),
             application::ExecutionTask::Status::Completed);
    QCOMPARE(tasks.at(1).status(),
             application::ExecutionTask::Status::WaitingForModel);
    QCOMPARE(tasks.at(2).status(), application::ExecutionTask::Status::Pending);
    QVERIFY(tasks.at(0).hasConversation());
    QVERIFY(tasks.at(1).hasConversation());
    QVERIFY(!tasks.at(2).hasConversation());
    const auto firstTaskRuntime = tasks.at(0).runtimeSnapshot();
    const auto secondTaskRuntime = tasks.at(1).runtimeSnapshot();
    const auto thirdTaskRuntime = tasks.at(2).runtimeSnapshot();
    QVERIFY(firstTaskRuntime.startedAt.isValid());
    QVERIFY(firstTaskRuntime.finishedAt.isValid());
    QVERIFY(firstTaskRuntime.elapsedMilliseconds >= 0);
    QVERIFY(secondTaskRuntime.startedAt.isValid());
    QVERIFY(!secondTaskRuntime.finishedAt.isValid());
    QVERIFY(!thirdTaskRuntime.startedAt.isValid());
    QCOMPARE(thirdTaskRuntime.elapsedMilliseconds, 0);
    QVERIFY(tasks.at(0).messages().size() > tasks.at(1).messages().size());
    QCOMPARE(tasks.at(1).messages().size(), 2);
    QVERIFY(generatedMessages.constLast().content.contains(
        QStringLiteral("<current_task>")));
    QVERIFY(generatedMessages.constLast().content.contains(
        QStringLiteral("inlet_shaft.stl")));
    QVERIFY(!generatedMessages.constLast().content.contains(
        QStringLiteral("qyck_shaft.stl")));
    QCOMPARE(controller.activeRun()
                 ->completionSteps.at(0)
                 .toObject()
                 .value(QStringLiteral("status"))
                 .toString(),
             QStringLiteral("satisfied"));
    controller.cancel();
}

void AgentControllerTest::advancesPlanOneStepPerTerminalToolResult()
{
    auto toolCallCount = 0;
    QList<chat::Message> generatedMessages;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [&](const QList<chat::Message>& messages,
                const models::InferencePreset&, int)
            { generatedMessages = messages; },
            [] {},
            [&](const QString&, const QJsonObject&)
            {
                ++toolCallCount;
                return QStringLiteral("tool-request-%1").arg(toolCallCount);
            },
            [](const QString&) {}, acceptsToolArguments, [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});

    QVERIFY(controller.start(
        QStringLiteral("1. Create the case.\n2. Report the result."), {},
        {ledgerTool(QStringLiteral("create"), false),
         ledgerTool(QStringLiteral("inspect"), true)}));
    const QJsonArray plan{
        planStep(
            QStringLiteral("create"), QStringLiteral("Create the case"), true,
            {QStringLiteral("fake.create"), QStringLiteral("fake.inspect")}),
        planStep(QStringLiteral("report"), QStringLiteral("Report the result"),
                 false)};
    controller.receiveToken(orderedTaskPlanAction(plan));
    controller.completeGeneration(false);
    controller.activeRun()->stagnationRecoveries = 1;
    controller.receiveToken(QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.inspect","arguments":{},"plan_step_id":"report","completes_plan_step":true})"));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QCOMPARE(toolCallCount, 0);
    QCOMPARE(controller.activeRun()->stagnationRecoveries, 1);
    QCOMPARE(controller.activeRun()->orderedPlanRepairs, 1);
    QCOMPARE(controller.activeRun()->duplicateToolActions, 0);
    QVERIFY(generatedMessages.constLast().content.contains(
        QStringLiteral("owns only 'create'")));
    QVERIFY(generatedMessages.constLast().content.contains(
        QStringLiteral("Create the case")));

    controller.receiveToken(QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.create","arguments":{"name":"case"},"plan_step_id":"create","completes_plan_step":false})"));
    controller.completeGeneration(false);
    QCOMPARE(toolCallCount, 1);
    agent::ToolResult intermediate;
    intermediate.requestId = QStringLiteral("tool-request-1");
    intermediate.serverId = QStringLiteral("fake");
    intermediate.toolName = QStringLiteral("create");
    intermediate.structuredContent =
        QJsonObject{{QStringLiteral("ok"), true},
                    {QStringLiteral("name"), QStringLiteral("case")},
                    {QStringLiteral("uuid"), QStringLiteral("case-1")}};
    controller.receiveToolResult(intermediate);
    QCOMPARE(controller.activeRun()
                 ->completionSteps.at(0)
                 .toObject()
                 .value(QStringLiteral("status"))
                 .toString(),
             QString{});
    QVERIFY(generatedMessages.constLast().content.contains(
        QStringLiteral("step 'create' remains current")));

    controller.receiveToken(QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.inspect","arguments":{"name_uuid":"case/case-1"},"plan_step_id":"create","completes_plan_step":true})"));
    controller.completeGeneration(false);

    agent::ToolResult result;
    result.requestId = QStringLiteral("tool-request-2");
    result.serverId = QStringLiteral("fake");
    result.toolName = QStringLiteral("inspect");
    result.structuredContent =
        QJsonObject{{QStringLiteral("ok"), true},
                    {QStringLiteral("name"), QStringLiteral("case")},
                    {QStringLiteral("uuid"), QStringLiteral("case-1")}};
    controller.receiveToolResult(result);
    QCOMPARE(controller.activeRun()->executionTasks.at(0).status(),
             application::ExecutionTask::Status::WaitingForModel);
    controller.receiveToken(planStepReviewAction(
        QStringLiteral("create"), QStringLiteral("satisfied"), QJsonArray({1}),
        QStringLiteral(
            "The earlier terminal mutation created the requested case; the "
            "later inspection resolved its verification.")));
    controller.completeGeneration(false);
    QCOMPARE(controller.activeRun()
                 ->completionSteps.at(0)
                 .toObject()
                 .value(QStringLiteral("status"))
                 .toString(),
             QStringLiteral("satisfied"));
    QCOMPARE(controller.activeRun()
                 ->completionSteps.at(0)
                 .toObject()
                 .value(QStringLiteral("evidence"))
                 .toArray(),
             QJsonArray({1}));
    QCOMPARE(controller.activeRun()
                 ->completionSteps.at(1)
                 .toObject()
                 .value(QStringLiteral("status"))
                 .toString(),
             QString{});
    const auto progress = controller.progressSnapshot();
    QCOMPARE(progress.currentStepId, QStringLiteral("report"));
    QCOMPARE(progress.steps.constFirst().status,
             application::AgentProgressStepStatus::Completed);
    QCOMPARE(progress.steps.constLast().status,
             application::AgentProgressStepStatus::Current);

    controller.receiveToken(QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.inspect","arguments":{},"plan_step_id":"report","completes_plan_step":true})"));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QCOMPARE(toolCallCount, 2);
    QVERIFY(generatedMessages.constLast().content.contains(
        QStringLiteral("does not require a tool")));

    controller.receiveToken(
        QByteArrayLiteral(R"({"action":"final","content":"Case created."})"));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QVERIFY(controller.activeRun()->summaryTask.has_value());
    QCOMPARE(controller.activeRun()->summaryTask->status(),
             application::AgentTask::Status::WaitingForModel);
    QVERIFY(generatedMessages.constLast().content.contains(
        QStringLiteral("verified every task")));

    controller.receiveToken(QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.inspect","arguments":{}})"));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QCOMPARE(toolCallCount, 2);
    QVERIFY(generatedMessages.constLast().content.contains(
        QStringLiteral("summary task rejected")));

    controller.receiveToken(
        QByteArrayLiteral(R"({"action":"final","content":"Case created."})"));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Completed);
    QCOMPARE(controller.activeRun()->currentExecutionTaskIndex, 2);
    QCOMPARE(controller.activeRun()->executionTasks.at(0).status(),
             application::ExecutionTask::Status::Completed);
    QCOMPARE(controller.activeRun()->executionTasks.at(1).status(),
             application::ExecutionTask::Status::Completed);
    QCOMPARE(controller.activeRun()->summaryTask->status(),
             application::AgentTask::Status::Completed);
    QVERIFY(controller.activeRun()
                ->summaryTask->runtimeSnapshot()
                .finishedAt.isValid());
    QCOMPARE(controller.progressSnapshot().currentStepId, QString{});
}

void AgentControllerTest::rejectsOutOfOrderCompletionReview()
{
    auto generationCount = 0;
    QList<chat::Message> generatedMessages;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [&](const QList<chat::Message>& messages,
                const models::InferencePreset&, int)
            {
                ++generationCount;
                generatedMessages = messages;
            },
            [] {}, [](const QString&, const QJsonObject&)
            { return QStringLiteral("unused"); }, [](const QString&) {},
            acceptsToolArguments, [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});
    QSignalSpy finalSpy(&controller,
                        &application::AgentController::finalAnswerReady);

    QVERIFY(controller.start(
        QStringLiteral("1. Create the case.\n2. Run the simulation."), {},
        {echoTool()}));
    const QJsonArray plan{planStep(QStringLiteral("create"),
                                   QStringLiteral("Create the case"), true),
                          planStep(QStringLiteral("simulate"),
                                   QStringLiteral("Run the simulation"), true)};
    controller.receiveToken(taskPlanAction(plan));
    controller.completeGeneration(false);
    controller.receiveToken(
        QByteArrayLiteral(R"({"action":"final","content":"Done"})"));
    controller.completeGeneration(false);
    const QJsonArray outOfOrder{
        reviewedStep(QStringLiteral("create"),
                     QStringLiteral("Create the case"), true,
                     QStringLiteral("pending")),
        reviewedStep(QStringLiteral("simulate"),
                     QStringLiteral("Run the simulation"), true,
                     QStringLiteral("satisfied"), QJsonArray{1})};
    controller.receiveToken(
        completionReviewAction(QStringLiteral("continue"), outOfOrder,
                               QStringLiteral("The second step is complete.")));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QCOMPARE(finalSpy.count(), 0);
    QVERIFY(generatedMessages.constLast().content.contains(
        QStringLiteral("preceding task-plan steps")));
    QVERIFY(generationCount >= 4);
}

void AgentControllerTest::rejectsRepeatedSuccessfulToolCall()
{
    auto generationCount = 0;
    auto toolCallCount = 0;
    QList<chat::Message> generatedMessages;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [&](const QList<chat::Message>& messages,
                const models::InferencePreset&, int)
            {
                ++generationCount;
                generatedMessages = messages;
            },
            [] {},
            [&](const QString&, const QJsonObject&)
            {
                ++toolCallCount;
                return QStringLiteral("tool-request-%1").arg(toolCallCount);
            },
            [](const QString&) {}, acceptsToolArguments, [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});

    const auto action = QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.echo","arguments":{"file_path":"E:/stl_case1/qyck_shaft.stl"}})");
    const auto equivalentPathAction = QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.echo","arguments":{"file_path":"E:\\stl_case1\\qyck_shaft.stl"}})");
    QVERIFY(controller.start(QStringLiteral("Do work"), {}, {echoTool()}));
    controller.receiveToken(action);
    controller.completeGeneration(false);
    QCOMPARE(toolCallCount, 1);

    agent::ToolResult result;
    result.requestId = QStringLiteral("tool-request-1");
    result.serverId = QStringLiteral("fake");
    result.toolName = QStringLiteral("echo");
    controller.receiveToolResult(result);
    QCOMPARE(generationCount, 2);

    controller.receiveToken(equivalentPathAction);
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QCOMPARE(toolCallCount, 1);
    QCOMPARE(generationCount, 3);
    QVERIFY(generatedMessages.constLast().content.contains(
        QStringLiteral("already completed successfully")));
}

void AgentControllerTest::limitsConsecutiveDiscoveryBreadth()
{
    auto toolCallCount = 0;
    QList<chat::Message> generatedMessages;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [&](const QList<chat::Message>& messages,
                const models::InferencePreset&, int)
            { generatedMessages = messages; },
            [] {},
            [&](const QString&, const QJsonObject&)
            {
                ++toolCallCount;
                return QStringLiteral("tool-request-%1").arg(toolCallCount);
            },
            [](const QString&) {}, acceptsToolArguments, [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});

    QVERIFY(controller.start(QStringLiteral("Inspect relevant case items."), {},
                             {ledgerTool(QStringLiteral("list_item"), true)}));
    for (auto index = 0; index < 4; ++index)
    {
        const auto action =
            QJsonDocument(
                QJsonObject{
                    {QStringLiteral("action"), QStringLiteral("call_tool")},
                    {QStringLiteral("tool"), QStringLiteral("fake.list_item")},
                    {QStringLiteral("arguments"),
                     QJsonObject{{QStringLiteral("item_type"),
                                  QStringLiteral("type-%1").arg(index)}}}})
                .toJson(QJsonDocument::Compact);
        controller.receiveToken(action);
        controller.completeGeneration(false);
        QCOMPARE(toolCallCount, index + 1);

        agent::ToolResult result;
        result.requestId = QStringLiteral("tool-request-%1").arg(index + 1);
        result.serverId = QStringLiteral("fake");
        result.toolName = QStringLiteral("list_item");
        result.structuredContent =
            QJsonObject{{QStringLiteral("ok"), true},
                        {QStringLiteral("result"), QJsonArray{}}};
        controller.receiveToolResult(result);
    }
    QCOMPARE(controller.activeRun()->consecutiveDiscoveryCalls, 4);

    controller.receiveToken(QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.list_item","arguments":{"item_type":"type-4"}})"));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QCOMPARE(toolCallCount, 4);
    QCOMPARE(controller.activeRun()->redundantDiscoveryCalls, 1);
    QVERIFY(generatedMessages.constLast().content.contains(
        QStringLiteral("consecutive read-only discovery")));

    controller.receiveToken(
        QByteArrayLiteral(R"({"action":"final","content":"Done."})"));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Completed);
}

void AgentControllerTest::preservesSummaryWhenCompletionReviewFormattingFails()
{
    auto toolCallCount = 0;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [](const QList<chat::Message>&, const models::InferencePreset&,
               int) {},
            [] {},
            [&](const QString&, const QJsonObject&)
            {
                ++toolCallCount;
                return QStringLiteral("tool-request-%1").arg(toolCallCount);
            },
            [](const QString&) {}, acceptsToolArguments, [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});
    QSignalSpy finalSpy(&controller,
                        &application::AgentController::finalAnswerReady);

    QVERIFY(controller.start(
        QStringLiteral("1. Import the STL.\n2. Summarize the result."), {},
        {echoTool()}));
    const QJsonArray plan{
        planStep(QStringLiteral("import"), QStringLiteral("Import the STL"),
                 true),
        planStep(QStringLiteral("summary"),
                 QStringLiteral("Summarize the result"), false)};
    controller.receiveToken(taskPlanAction(plan));
    controller.completeGeneration(false);
    controller.receiveToken(QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.echo","arguments":{"path":"case.stl"}})"));
    controller.completeGeneration(false);

    agent::ToolResult result;
    result.requestId = QStringLiteral("tool-request-1");
    result.serverId = QStringLiteral("fake");
    result.toolName = QStringLiteral("echo");
    controller.receiveToolResult(result);

    const auto final = QByteArrayLiteral(
        R"({"action":"final","content":"The STL was imported successfully."})");
    controller.receiveToken(final);
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);

    const auto malformedReview = QByteArrayLiteral(
        R"({"action":"review_completion","verdict":"","steps":[],"detail":""})");
    controller.receiveToken(malformedReview);
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QCOMPARE(finalSpy.count(), 0);

    controller.receiveToken(malformedReview);
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Completed);
    QCOMPARE(finalSpy.count(), 1);
    QCOMPARE(finalSpy.constFirst().at(1).toString(),
             QStringLiteral("The STL was imported successfully."));
    QCOMPARE(toolCallCount, 1);
}

void AgentControllerTest::allowsRepeatedPollingUntilTerminalStatus()
{
    auto generationCount = 0;
    auto toolCallCount = 0;
    QList<chat::Message> generatedMessages;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [&](const QList<chat::Message>& messages,
                const models::InferencePreset&, int)
            {
                ++generationCount;
                generatedMessages = messages;
            },
            [] {},
            [&](const QString&, const QJsonObject&)
            {
                ++toolCallCount;
                return QStringLiteral("tool-request-%1").arg(toolCallCount);
            },
            [](const QString&) {}, acceptsToolArguments, [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});

    const auto action = QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.echo","arguments":{}})");
    QVERIFY(controller.start(QStringLiteral("Wait for the operation"), {},
                             {echoTool()}));
    controller.receiveToken(action);
    controller.completeGeneration(false);
    QCOMPARE(toolCallCount, 1);

    agent::ToolResult firstResult;
    firstResult.requestId = QStringLiteral("tool-request-1");
    firstResult.serverId = QStringLiteral("fake");
    firstResult.toolName = QStringLiteral("echo");
    firstResult.structuredContent =
        QJsonObject{{QStringLiteral("ok"), true},
                    {QStringLiteral("result"),
                     QJsonObject{{QStringLiteral("isRunning"), true},
                                 {QStringLiteral("progress"), 33.0}}}};
    controller.receiveToolResult(firstResult);
    QCOMPARE(generationCount, 2);
    QCOMPARE(controller.activeRun()->ledger.jobs().size(), 1);
    QCOMPARE(controller.activeRun()->ledger.jobs().constFirst().status,
             QStringLiteral("running"));
    QVERIFY(!controller.activeRun()->ledger.jobs().constFirst().terminal);
    QVERIFY(generatedMessages.constLast().content.contains(
        QStringLiteral("still in progress")));

    controller.receiveToken(action);
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::ExecutingTool);
    QCOMPARE(toolCallCount, 1);
    QTRY_COMPARE_WITH_TIMEOUT(toolCallCount, 2, 2'000);

    agent::ToolResult secondResult;
    secondResult.requestId = QStringLiteral("tool-request-2");
    secondResult.serverId = QStringLiteral("fake");
    secondResult.toolName = QStringLiteral("echo");
    secondResult.result = {
        {QStringLiteral("structuredContent"),
         QJsonObject{{QStringLiteral("status"), QStringLiteral("in_progress")},
                     {QStringLiteral("progress"), 66.0}}}};
    controller.receiveToolResult(secondResult);

    controller.receiveToken(action);
    controller.completeGeneration(false);
    QCOMPARE(toolCallCount, 2);
    QTRY_COMPARE_WITH_TIMEOUT(toolCallCount, 3, 2'000);

    agent::ToolResult terminalResult;
    terminalResult.requestId = QStringLiteral("tool-request-3");
    terminalResult.serverId = QStringLiteral("fake");
    terminalResult.toolName = QStringLiteral("echo");
    terminalResult.structuredContent =
        QJsonObject{{QStringLiteral("result"),
                     QJsonObject{{QStringLiteral("isRunning"), false}}}};
    controller.receiveToolResult(terminalResult);
    QCOMPARE(controller.activeRun()->ledger.jobs().size(), 1);
    QCOMPARE(controller.activeRun()->ledger.jobs().constFirst().status,
             QStringLiteral("completed"));
    QVERIFY(controller.activeRun()->ledger.jobs().constFirst().terminal);

    controller.receiveToken(action);
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QCOMPARE(toolCallCount, 3);
    QVERIFY(generatedMessages.constLast().content.contains(
        QStringLiteral("already completed successfully")));
    controller.cancel();
}

void AgentControllerTest::classifiesStructuredLifecycleStates()
{
    agent::ToolResult failed;
    failed.requestId = QStringLiteral("request-1");
    failed.structuredContent =
        QJsonObject{{QStringLiteral("status"), QStringLiteral("failed")}};
    QCOMPARE(application::normalizedToolOutcome(failed),
             agent::ToolOutcome::ToolFailed);
    QCOMPARE(application::normalizedToolSideEffectState(failed),
             agent::ToolSideEffectState::KnownFailed);

    agent::ToolResult running;
    running.requestId = QStringLiteral("request-2");
    running.structuredContent = QJsonObject{
        {QStringLiteral("status"), QStringLiteral("running")},
        {QStringLiteral("previous"),
         QJsonObject{{QStringLiteral("status"), QStringLiteral("failed")}}}};
    QCOMPARE(application::normalizedToolOutcome(running),
             agent::ToolOutcome::InProgress);

    agent::ToolResult terminalFailure;
    terminalFailure.requestId = QStringLiteral("request-3");
    terminalFailure.structuredContent = QJsonObject{
        {QStringLiteral("state"), QStringLiteral("cancelled")},
        {QStringLiteral("worker"),
         QJsonObject{{QStringLiteral("status"), QStringLiteral("running")}}}};
    QCOMPARE(application::normalizedToolOutcome(terminalFailure),
             agent::ToolOutcome::ToolFailed);

    agent::ToolResult completed;
    completed.structuredContent =
        QJsonObject{{QStringLiteral("result"),
                     QJsonObject{{QStringLiteral("isRunning"), false}}}};
    QCOMPARE(application::normalizedToolOutcome(completed),
             agent::ToolOutcome::Succeeded);
}

void AgentControllerTest::cancelsPendingStatusPoll()
{
    auto toolCallCount = 0;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [](const QList<chat::Message>&, const models::InferencePreset&,
               int) {},
            [] {},
            [&](const QString&, const QJsonObject&)
            {
                ++toolCallCount;
                return QStringLiteral("tool-request-%1").arg(toolCallCount);
            },
            [](const QString&) {}, acceptsToolArguments, [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});

    const auto action = QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.echo","arguments":{}})");
    QVERIFY(controller.start(QStringLiteral("Wait for the operation"), {},
                             {echoTool()}));
    controller.receiveToken(action);
    controller.completeGeneration(false);

    agent::ToolResult runningResult;
    runningResult.requestId = QStringLiteral("tool-request-1");
    runningResult.serverId = QStringLiteral("fake");
    runningResult.toolName = QStringLiteral("echo");
    runningResult.structuredContent =
        QJsonObject{{QStringLiteral("isRunning"), true}};
    controller.receiveToolResult(runningResult);

    controller.receiveToken(action);
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::ExecutingTool);
    QCOMPARE(toolCallCount, 1);
    controller.cancel();
    QCOMPARE(controller.state(), application::AgentRun::State::Cancelled);
    QTest::qWait(1'100);
    QCOMPARE(toolCallCount, 1);
}

void AgentControllerTest::rejectsAlternatingCompletedToolCycle()
{
    auto generationCount = 0;
    auto toolCallCount = 0;
    QList<chat::Message> generatedMessages;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [&](const QList<chat::Message>& messages,
                const models::InferencePreset&, int)
            {
                ++generationCount;
                generatedMessages = messages;
            },
            [] {},
            [&](const QString&, const QJsonObject&)
            {
                ++toolCallCount;
                return QStringLiteral("tool-request-%1").arg(toolCallCount);
            },
            [](const QString&) {}, acceptsToolArguments, [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});

    const auto openAction = QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.echo","arguments":{"operation":"open"}})");
    const auto closeAction = QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.echo","arguments":{"operation":"close"}})");
    QVERIFY(controller.start(QStringLiteral("Keep working in the case"), {},
                             {echoTool()}));

    const auto completeTool =
        [&](const QByteArray& action, const QString& requestId, bool isError)
    {
        controller.receiveToken(action);
        controller.completeGeneration(false);
        QCOMPARE(controller.state(),
                 application::AgentRun::State::ExecutingTool);
        agent::ToolResult result;
        result.requestId = requestId;
        result.serverId = QStringLiteral("fake");
        result.toolName = QStringLiteral("echo");
        result.isError = isError;
        if (isError)
        {
            result.errorCode = QStringLiteral("operation_failed");
            result.errorMessage = QStringLiteral("Operation failed.");
        }
        controller.receiveToolResult(result);
        QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    };

    completeTool(openAction, QStringLiteral("tool-request-1"), false);
    completeTool(closeAction, QStringLiteral("tool-request-2"), true);
    completeTool(openAction, QStringLiteral("tool-request-3"), false);
    QCOMPARE(toolCallCount, 3);

    controller.receiveToken(closeAction);
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QCOMPARE(toolCallCount, 3);
    QCOMPARE(generationCount, 5);
    QVERIFY(generatedMessages.constLast().content.contains(
        QStringLiteral("repeated cycle of completed calls")));
    QVERIFY(generatedMessages.constLast().content.contains(
        QStringLiteral("toggle resources open and closed")));
}

void AgentControllerTest::reviewsPrematureFinalBeforeAnyToolCall()
{
    auto generationCount = 0;
    auto toolCallCount = 0;
    QList<chat::Message> generatedMessages;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [&](const QList<chat::Message>& messages,
                const models::InferencePreset&, int)
            {
                ++generationCount;
                generatedMessages = messages;
            },
            [] {},
            [&](const QString&, const QJsonObject&)
            {
                ++toolCallCount;
                return QStringLiteral("tool-request-1");
            },
            [](const QString&) {}, acceptsToolArguments, [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});
    QSignalSpy finalSpy(&controller,
                        &application::AgentController::finalAnswerReady);

    QVERIFY(controller.start(
        QStringLiteral("1. Create the case.\n2. Run the simulation."), {},
        {echoTool()}));
    const QJsonArray plan{planStep(QStringLiteral("create"),
                                   QStringLiteral("Create the case"), true),
                          planStep(QStringLiteral("simulate"),
                                   QStringLiteral("Run the simulation"), true)};
    controller.receiveToken(taskPlanAction(plan));
    controller.completeGeneration(false);
    controller.receiveToken(QByteArrayLiteral(
        R"({"action":"final","content":"I still need to inspect the files and create the case."})"));
    controller.completeGeneration(false);

    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QCOMPARE(generationCount, 3);
    QCOMPARE(toolCallCount, 0);
    QCOMPARE(finalSpy.count(), 0);
    QCOMPARE(controller.activeRun()->successfulToolResults, 0);
    QVERIFY(controller.activeRun()->awaitingCompletionReview);
    QVERIFY(!controller.activeRun()->pendingFinalCandidate.isEmpty());
    QCOMPARE(controller.activeRun()->completionSteps, plan);
    QVERIFY(generatedMessages.constLast().content.contains(
        QStringLiteral("Do not return another final action")));
    QVERIFY(generatedMessages.constLast().content.contains(
        QStringLiteral("successful terminal evidence")));

    controller.receiveToken(QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.echo","arguments":{}})"));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::ExecutingTool);
    QCOMPARE(toolCallCount, 1);

    agent::ToolResult result;
    result.requestId = QStringLiteral("tool-request-1");
    result.serverId = QStringLiteral("fake");
    result.toolName = QStringLiteral("echo");
    controller.receiveToolResult(result);

    controller.receiveToken(QByteArrayLiteral(
        R"({"action":"final","content":"The requested work is complete."})"));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);

    const QJsonArray completedSteps{
        reviewedStep(QStringLiteral("create"),
                     QStringLiteral("Create the case"), true,
                     QStringLiteral("satisfied"), QJsonArray{1}),
        reviewedStep(QStringLiteral("simulate"),
                     QStringLiteral("Run the simulation"), true,
                     QStringLiteral("satisfied"), QJsonArray{1})};
    controller.receiveToken(
        completionReviewAction(QStringLiteral("complete"), completedSteps,
                               QStringLiteral("All work is complete.")));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Completed);
    QCOMPARE(finalSpy.count(), 1);
}

void AgentControllerTest::reviewsCompletionAfterEachEvidenceRevision()
{
    auto generationCount = 0;
    auto toolCallCount = 0;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [&](const QList<chat::Message>&, const models::InferencePreset&,
                int) { ++generationCount; },
            [] {},
            [&](const QString&, const QJsonObject&)
            {
                ++toolCallCount;
                return QStringLiteral("tool-request-%1").arg(toolCallCount);
            },
            [](const QString&) {}, acceptsToolArguments, [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});
    QSignalSpy finalSpy(&controller,
                        &application::AgentController::finalAnswerReady);

    QVERIFY(controller.start(QStringLiteral("1. Do the first operation.\n"
                                            "2. Do the second operation."),
                             {}, {echoTool()}));
    const QJsonArray plan{
        planStep(QStringLiteral("first"),
                 QStringLiteral("Do the first operation"), true),
        planStep(QStringLiteral("second"),
                 QStringLiteral("Do the second operation"), true)};
    controller.receiveToken(taskPlanAction(plan));
    controller.completeGeneration(false);
    controller.receiveToken(QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.echo","arguments":{"value":1}})"));
    controller.completeGeneration(false);
    agent::ToolResult firstResult;
    firstResult.requestId = QStringLiteral("tool-request-1");
    firstResult.serverId = QStringLiteral("fake");
    firstResult.toolName = QStringLiteral("echo");
    controller.receiveToolResult(firstResult);

    controller.receiveToken(
        QByteArrayLiteral(R"({"action":"final","content":"Done"})"));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QCOMPARE(generationCount, 4);
    QCOMPARE(controller.activeRun()->lastReviewedEvidenceRevision, 1);

    controller.receiveToken(QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.echo","arguments":{"value":2}})"));
    controller.completeGeneration(false);
    agent::ToolResult secondResult;
    secondResult.requestId = QStringLiteral("tool-request-2");
    secondResult.serverId = QStringLiteral("fake");
    secondResult.toolName = QStringLiteral("echo");
    controller.receiveToolResult(secondResult);
    QCOMPARE(generationCount, 5);

    controller.receiveToken(
        QByteArrayLiteral(R"({"action":"final","content":"Done"})"));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QCOMPARE(generationCount, 6);
    QCOMPARE(controller.activeRun()->lastReviewedEvidenceRevision, 2);

    const QJsonArray completedSteps{
        reviewedStep(QStringLiteral("first"),
                     QStringLiteral("Do the first operation"), true,
                     QStringLiteral("satisfied"), QJsonArray{1}),
        reviewedStep(QStringLiteral("second"),
                     QStringLiteral("Do the second operation"), true,
                     QStringLiteral("satisfied"), QJsonArray{2})};
    controller.receiveToken(completionReviewAction(
        QStringLiteral("complete"), completedSteps,
        QStringLiteral("Both operations are complete.")));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Completed);
    QCOMPARE(generationCount, 6);
    QCOMPARE(finalSpy.count(), 1);
}

void AgentControllerTest::restoresRecordedPlanWhenCompletionReviewDrifts()
{
    auto generationCount = 0;
    QList<chat::Message> generatedMessages;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [&](const QList<chat::Message>& messages,
                const models::InferencePreset&, int)
            {
                ++generationCount;
                generatedMessages = messages;
            },
            [] {}, [](const QString&, const QJsonObject&)
            { return QStringLiteral("unused"); }, [](const QString&) {},
            acceptsToolArguments, [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});
    QSignalSpy finishedSpy(&controller,
                           &application::AgentController::runFinished);

    QVERIFY(controller.start(
        QStringLiteral("1. Create the case.\n2. Run the simulation."), {},
        {echoTool()}));
    const QJsonArray plan{planStep(QStringLiteral("step-1"),
                                   QStringLiteral("Create the case"), true),
                          planStep(QStringLiteral("step-2"),
                                   QStringLiteral("Run the simulation"), true)};
    controller.receiveToken(taskPlanAction(plan));
    controller.completeGeneration(false);

    const auto final = QByteArrayLiteral(
        R"({"action":"final","content":"Everything is complete."})");
    controller.receiveToken(final);
    controller.completeGeneration(false);
    controller.receiveToken(final);
    controller.completeGeneration(false);
    QCOMPARE(controller.activeRun()->completionReviewFailures, 1);

    const QJsonArray drifted{
        reviewedStep(QStringLiteral("other-step"),
                     QStringLiteral("An unrelated replacement step"), false,
                     QStringLiteral("satisfied"))};
    controller.receiveToken(completionReviewAction(
        QStringLiteral("complete"), drifted,
        QStringLiteral("The replacement checklist is complete.")));
    controller.completeGeneration(false);

    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QCOMPARE(finishedSpy.count(), 0);
    QCOMPARE(controller.activeRun()->completionReviewFailures, 1);
    QCOMPARE(controller.activeRun()->completionPlanDriftRepairs, 1);
    QCOMPARE(controller.activeRun()->completionSteps.size(), 2);
    QCOMPARE(controller.activeRun()
                 ->completionSteps.at(0)
                 .toObject()
                 .value(QStringLiteral("id"))
                 .toString(),
             QStringLiteral("step-1"));
    QVERIFY(generatedMessages.constLast().content.contains(
        QStringLiteral("<task_plan>")));
    QVERIFY(generatedMessages.constLast().content.contains(
        QStringLiteral("\"id\":\"step-1\"")));

    const QJsonArray corrected{
        reviewedStep(QStringLiteral("step-1"),
                     QStringLiteral("Create the case"), true,
                     QStringLiteral("pending")),
        reviewedStep(QStringLiteral("step-2"),
                     QStringLiteral("Run the simulation"), true,
                     QStringLiteral("pending"))};
    controller.receiveToken(
        completionReviewAction(QStringLiteral("continue"), corrected,
                               QStringLiteral("Create the case next.")));
    controller.completeGeneration(false);

    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QCOMPARE(finishedSpy.count(), 0);
    QCOMPARE(controller.activeRun()->completionReviewFailures, 0);
    QCOMPARE(controller.activeRun()->completionReviewSuccesses, 1);
    QCOMPARE(controller.activeRun()->completionSteps, corrected);
}

void AgentControllerTest::rejectsRepeatedFinalDuringCompletionReview()
{
    auto generationCount = 0;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [&](const QList<chat::Message>&, const models::InferencePreset&,
                int) { ++generationCount; },
            [] {}, [](const QString&, const QJsonObject&)
            { return QStringLiteral("unused"); }, [](const QString&) {},
            acceptsToolArguments, [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});
    QSignalSpy finalSpy(&controller,
                        &application::AgentController::finalAnswerReady);
    QSignalSpy finishedSpy(&controller,
                           &application::AgentController::runFinished);

    QVERIFY(controller.start(
        QStringLiteral("1. Create the case.\n2. Run the simulation."), {},
        {echoTool()}));
    const QJsonArray plan{planStep(QStringLiteral("create"),
                                   QStringLiteral("Create the case"), true),
                          planStep(QStringLiteral("simulate"),
                                   QStringLiteral("Run the simulation"), true)};
    controller.receiveToken(taskPlanAction(plan));
    controller.completeGeneration(false);

    const auto final = QByteArrayLiteral(
        R"({"action":"final","content":"Everything is complete."})");
    controller.receiveToken(final);
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QCOMPARE(generationCount, 3);

    controller.receiveToken(final);
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QCOMPARE(generationCount, 4);
    QCOMPARE(finalSpy.count(), 0);
    QVERIFY(controller.activeRun()->awaitingCompletionReview);

    controller.receiveToken(final);
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Failed);
    QCOMPARE(finalSpy.count(), 0);
    QCOMPARE(finishedSpy.constFirst().at(2).toString(),
             QStringLiteral("completion_unverified"));
}

void AgentControllerTest::completionReviewRejectsNonTerminalEvidence()
{
    auto generationCount = 0;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [&](const QList<chat::Message>&, const models::InferencePreset&,
                int) { ++generationCount; },
            [] {}, [](const QString&, const QJsonObject&)
            { return QStringLiteral("tool-request-1"); }, [](const QString&) {},
            acceptsToolArguments, [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});
    QSignalSpy finalSpy(&controller,
                        &application::AgentController::finalAnswerReady);

    QVERIFY(controller.start(
        QStringLiteral("1. Wait for completion.\n2. Report the result."), {},
        {echoTool()}));
    const QJsonArray plan{planStep(QStringLiteral("wait"),
                                   QStringLiteral("Wait for completion"), true),
                          planStep(QStringLiteral("report"),
                                   QStringLiteral("Report the result"), false)};
    controller.receiveToken(taskPlanAction(plan));
    controller.completeGeneration(false);
    controller.receiveToken(QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.echo","arguments":{}})"));
    controller.completeGeneration(false);

    agent::ToolResult runningResult;
    runningResult.requestId = QStringLiteral("tool-request-1");
    runningResult.serverId = QStringLiteral("fake");
    runningResult.toolName = QStringLiteral("echo");
    runningResult.structuredContent =
        QJsonObject{{QStringLiteral("isRunning"), true},
                    {QStringLiteral("progress"), 50.0}};
    controller.receiveToolResult(runningResult);

    controller.receiveToken(QByteArrayLiteral(
        R"({"action":"final","content":"The operation completed."})"));
    controller.completeGeneration(false);
    const QJsonArray invalidCompletedSteps{
        reviewedStep(QStringLiteral("wait"),
                     QStringLiteral("Wait for completion"), true,
                     QStringLiteral("satisfied"), QJsonArray{1}),
        reviewedStep(QStringLiteral("report"),
                     QStringLiteral("Report the result"), false,
                     QStringLiteral("satisfied"), QJsonArray{1})};
    controller.receiveToken(completionReviewAction(
        QStringLiteral("complete"), invalidCompletedSteps,
        QStringLiteral("The operation completed.")));
    controller.completeGeneration(false);

    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QCOMPARE(finalSpy.count(), 0);
    QCOMPARE(controller.activeRun()->completionReviewFailures, 1);
    QCOMPARE(generationCount, 5);

    const QJsonArray pendingSteps{
        reviewedStep(QStringLiteral("wait"),
                     QStringLiteral("Wait for completion"), true,
                     QStringLiteral("pending"), QJsonArray{1}),
        reviewedStep(QStringLiteral("report"),
                     QStringLiteral("Report the result"), false,
                     QStringLiteral("pending"), QJsonArray{})};
    controller.receiveToken(completionReviewAction(
        QStringLiteral("continue"), pendingSteps,
        QStringLiteral(
            "Poll the operation until it reaches a terminal state.")));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QCOMPARE(finalSpy.count(), 0);
    controller.cancel();
}

void AgentControllerTest::doesNotUsePreparedFinalWhenReviewStalls()
{
    auto generationCount = 0;
    auto toolCallCount = 0;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [&](const QList<chat::Message>&, const models::InferencePreset&,
                int) { ++generationCount; },
            [] {},
            [&](const QString&, const QJsonObject&)
            {
                ++toolCallCount;
                return QStringLiteral("tool-request-%1").arg(toolCallCount);
            },
            [](const QString&) {}, acceptsToolArguments, [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});
    QSignalSpy finalSpy(&controller,
                        &application::AgentController::finalAnswerReady);

    const auto repeatedAction = QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.echo","arguments":{"path":"controlData.json"}})");
    QVERIFY(controller.start(
        QStringLiteral("1. Read controlData.json.\n2. Report its state."), {},
        {echoTool()}));
    const QJsonArray plan{
        planStep(QStringLiteral("read"),
                 QStringLiteral("Read controlData.json"), true),
        planStep(QStringLiteral("report"), QStringLiteral("Report its state"),
                 false)};
    controller.receiveToken(taskPlanAction(plan));
    controller.completeGeneration(false);
    controller.receiveToken(repeatedAction);
    controller.completeGeneration(false);
    agent::ToolResult result;
    result.requestId = QStringLiteral("tool-request-1");
    result.serverId = QStringLiteral("fake");
    result.toolName = QStringLiteral("echo");
    controller.receiveToolResult(result);

    controller.receiveToken(
        QByteArrayLiteral(R"({"action":"final","content":"Prepared answer"})"));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QCOMPARE(generationCount, 4);

    controller.receiveToken(repeatedAction);
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QCOMPARE(toolCallCount, 1);
    QCOMPARE(generationCount, 5);
    QCOMPARE(finalSpy.count(), 0);

    controller.receiveToken(repeatedAction);
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Failed);
    QCOMPARE(toolCallCount, 1);
    QCOMPARE(finalSpy.count(), 0);
}

void AgentControllerTest::stagnationRecoveryIsIndependentFromActionRepair()
{
    auto generationCount = 0;
    auto toolCallCount = 0;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [&](const QList<chat::Message>&, const models::InferencePreset&,
                int) { ++generationCount; },
            [] {},
            [&](const QString&, const QJsonObject&)
            {
                ++toolCallCount;
                return QStringLiteral("tool-request-%1").arg(toolCallCount);
            },
            [](const QString&) {}, acceptsToolArguments, [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});

    const auto action = QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.echo","arguments":{"value":1}})");
    QVERIFY(
        controller.start(QStringLiteral("Read one value."), {}, {echoTool()}));
    controller.receiveToken(QByteArrayLiteral("invalid-json"));
    controller.completeGeneration(false);
    QCOMPARE(generationCount, 2);
    QCOMPARE(controller.activeRun()->repairAttempts, 1);

    controller.receiveToken(action);
    controller.completeGeneration(false);
    agent::ToolResult result;
    result.requestId = QStringLiteral("tool-request-1");
    result.serverId = QStringLiteral("fake");
    result.toolName = QStringLiteral("echo");
    controller.receiveToolResult(result);
    QCOMPARE(generationCount, 3);

    controller.receiveToken(action);
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QCOMPARE(controller.activeRun()->repairAttempts, 1);
    QCOMPARE(controller.activeRun()->stagnationRecoveries, 1);
    QCOMPARE(generationCount, 4);
    QCOMPARE(toolCallCount, 1);

    controller.receiveToken(
        QByteArrayLiteral(R"({"action":"final","content":"Done"})"));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Completed);
}

void AgentControllerTest::preservesStructuredScalarEvidence()
{
    agent::Action action;
    action.toolName = QStringLiteral("fake.scalar");
    agent::ToolResult result;
    result.result = {
        {QStringLiteral("content"),
         QJsonArray{QJsonObject{
             {QStringLiteral("type"), QStringLiteral("text")},
             {QStringLiteral("text"), QStringLiteral("accepted")}}}},
        {QStringLiteral("structuredContent"), QStringLiteral("accepted")}};

    const auto evidence =
        application::AgentContextCompactor::toolEvidence(1, action, result);
    QCOMPARE(evidence.value(QStringLiteral("result")),
             QJsonValue(QStringLiteral("accepted")));
}

void AgentControllerTest::compactsLongAgentContextAndPreservesEvidence()
{
    auto generationCount = 0;
    auto toolCallCount = 0;
    QList<chat::Message> generatedMessages;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [&](const QList<chat::Message>& messages,
                const models::InferencePreset&, int)
            {
                ++generationCount;
                generatedMessages = messages;
            },
            [] {},
            [&](const QString&, const QJsonObject&)
            {
                ++toolCallCount;
                return QStringLiteral("tool-request-%1").arg(toolCallCount);
            },
            [](const QString&) {}, acceptsToolArguments, [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});

    models::InferencePreset preset;
    preset.contextSize = 4'096;
    preset.maxOutputTokens = 512;
    const auto request = QStringLiteral(
        "1. Create case1 and preserve every real path and UUID.\n"
        "2. Create the shaft region.");
    QVERIFY(controller.start(request, preset, {echoTool()}));
    const QJsonArray plan{
        planStep(QStringLiteral("case"),
                 QStringLiteral("Create case1 and preserve its path and UUID"),
                 true),
        planStep(QStringLiteral("shaft"),
                 QStringLiteral("Create the shaft region"), true)};
    controller.receiveToken(taskPlanAction(plan));
    controller.completeGeneration(false);
    controller.receiveToken(QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.echo","arguments":{"case_path":"E:/stl_case1/case1"}})"));
    controller.completeGeneration(false,
                                  {{QStringLiteral("promptTokens"), 1'900}});

    const auto uuid = QStringLiteral("713b9921-7b45-45a5-aad3-0b8bc89fced6");
    const auto parentUuid =
        QStringLiteral("13a1ef20-1023-4a77-858b-60bb7f10f65d");
    const auto path = QStringLiteral("E:/stl_case1/case1");
    agent::ToolResult result;
    result.requestId = QStringLiteral("tool-request-1");
    result.serverId = QStringLiteral("fake");
    result.toolName = QStringLiteral("echo");
    result.result = {
        {QStringLiteral("structuredContent"),
         QJsonObject{{QStringLiteral("ok"), true},
                     {QStringLiteral("uuid"), uuid},
                     {QStringLiteral("parent_uuid"), parentUuid},
                     {QStringLiteral("case_path"), path},
                     {QStringLiteral("large_payload"),
                      QStringLiteral("mesh-data-").repeated(2'000)}}}};
    controller.receiveToolResult(result);

    QCOMPARE(generationCount, 3);
    QVERIFY(controller.activeRun().has_value());
    QCOMPARE(controller.activeRun()->contextCompactions, 1);
    QCOMPARE(generatedMessages.constFirst().role, chat::Role::System);
    QCOMPARE(generatedMessages.constLast().role, chat::Role::User);
    const auto compactedTask = generatedMessages.constLast().content;
    QVERIFY(compactedTask.contains(request));
    QVERIFY(compactedTask.contains(QStringLiteral("<agent_progress>")));
    QVERIFY(compactedTask.contains(QStringLiteral("qtllm-agent-progress-v3")));
    QVERIFY(compactedTask.contains(QStringLiteral("Create the shaft region")));
    QVERIFY(compactedTask.contains(uuid));
    QVERIFY(compactedTask.contains(parentUuid));
    QVERIFY(compactedTask.contains(path));
    QVERIFY(compactedTask.size() < 5'000);
    QVERIFY(
        !compactedTask.contains(QStringLiteral("mesh-data-").repeated(100)));

    controller.receiveToken(QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.echo","arguments":{"region_name":"shaft"}})"));
    controller.completeGeneration(false,
                                  {{QStringLiteral("promptTokens"), 2'200}});
    const auto secondUuid =
        QStringLiteral("af0af18a-6758-429f-b6cb-e161cc9d64d2");
    agent::ToolResult secondResult;
    secondResult.requestId = QStringLiteral("tool-request-2");
    secondResult.serverId = QStringLiteral("fake");
    secondResult.toolName = QStringLiteral("echo");
    secondResult.result = {
        {QStringLiteral("structuredContent"),
         QJsonObject{{QStringLiteral("ok"), true},
                     {QStringLiteral("uuid"), secondUuid},
                     {QStringLiteral("region_name"), QStringLiteral("shaft")},
                     {QStringLiteral("large_payload"),
                      QStringLiteral("region-data-").repeated(2'000)}}}};
    controller.receiveToolResult(secondResult);

    QCOMPARE(generationCount, 4);
    QCOMPARE(controller.activeRun()->contextCompactions, 2);
    const auto twiceCompactedTask = generatedMessages.constLast().content;
    QCOMPARE(twiceCompactedTask.count(QStringLiteral("<agent_progress>")), 1);
    QCOMPARE(twiceCompactedTask.count(request), 1);
    QVERIFY(twiceCompactedTask.contains(uuid));
    QVERIFY(twiceCompactedTask.contains(parentUuid));
    QVERIFY(twiceCompactedTask.contains(secondUuid));
    controller.cancel();
}

void AgentControllerTest::tracksRunScopedLedgerAndRequiresVerification()
{
    auto generationCount = 0;
    auto toolCallCount = 0;
    QList<chat::Message> generatedMessages;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [&](const QList<chat::Message>& messages,
                const models::InferencePreset&, int)
            {
                ++generationCount;
                generatedMessages = messages;
            },
            [] {},
            [&](const QString&, const QJsonObject&)
            {
                ++toolCallCount;
                return QStringLiteral("tool-request-%1").arg(toolCallCount);
            },
            [](const QString&) {}, acceptsToolArguments, [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});
    QSignalSpy finalSpy(&controller,
                        &application::AgentController::finalAnswerReady);

    const auto createAction = QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.create","arguments":{"name":"resource","parent_uuid":"parent-1"}})");
    const auto inspectAction = QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.inspect","arguments":{"name_uuid":"resource/resource-1"}})");
    auto createTool = ledgerTool(QStringLiteral("create"), false);
    createTool.outputSchema = {
        {QStringLiteral("type"), QStringLiteral("object")}};
    createTool.hasOutputSchema = true;
    QVERIFY(controller.start(
        QStringLiteral("Create the resource and report it."), {},
        {createTool, ledgerTool(QStringLiteral("inspect"), true)}));
    controller.receiveToken(createAction);
    controller.completeGeneration(false);

    agent::ToolResult createResult;
    createResult.requestId = QStringLiteral("tool-request-1");
    createResult.serverId = QStringLiteral("fake");
    createResult.toolName = QStringLiteral("create");
    createResult.structuredContent = QJsonObject{
        {QStringLiteral("created"),
         QJsonObject{
             {QStringLiteral("name"), QStringLiteral("resource")},
             {QStringLiteral("uuid"), QStringLiteral("resource-1")},
             {QStringLiteral("parent_uuid"), QStringLiteral("parent-1")}}}};
    controller.receiveToolResult(createResult);

    QVERIFY(controller.activeRun().has_value());
    const auto& ledger = controller.activeRun()->ledger;
    QCOMPARE(ledger.resources().size(), 1);
    QCOMPARE(ledger.resources().constFirst().stableId,
             QStringLiteral("resource-1"));
    QCOMPARE(ledger.resources().constFirst().parentId,
             QStringLiteral("parent-1"));
    QCOMPARE(ledger.verifications().size(), 1);
    QCOMPARE(ledger.verifications().constFirst().state,
             QStringLiteral("pending"));
    QVERIFY(generatedMessages.constLast().content.contains(
        QStringLiteral("<run_ledger>")));
    QVERIFY(generatedMessages.constLast().content.contains(
        QStringLiteral("parent-1")));

    controller.receiveToken(
        QByteArrayLiteral(R"({"action":"final","content":"Done"})"));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QCOMPARE(finalSpy.count(), 0);
    QVERIFY(generatedMessages.constLast().content.contains(
        QStringLiteral("read-back verification")));

    controller.receiveToken(inspectAction);
    controller.completeGeneration(false);
    agent::ToolResult inspectResult;
    inspectResult.requestId = QStringLiteral("tool-request-2");
    inspectResult.serverId = QStringLiteral("fake");
    inspectResult.toolName = QStringLiteral("inspect");
    inspectResult.structuredContent = QJsonObject{
        {QStringLiteral("name"), QStringLiteral("resource")},
        {QStringLiteral("uuid"), QStringLiteral("resource-1")},
        {QStringLiteral("parent_uuid"), QStringLiteral("parent-1")}};
    controller.receiveToolResult(inspectResult);
    QCOMPARE(controller.activeRun()->ledger.verifications().constFirst().state,
             QStringLiteral("verified"));

    controller.receiveToken(
        QByteArrayLiteral(R"({"action":"final","content":"Done"})"));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Completed);
    QCOMPARE(finalSpy.count(), 1);
}

void AgentControllerTest::requiresMatchingTargetForMutationSelfVerification()
{
    agent::Action action;
    action.type = agent::ActionType::CallTool;
    action.toolName = QStringLiteral("fake.create");
    action.arguments = {{QStringLiteral("name"), QStringLiteral("requested")}};

    agent::ToolResult mismatched;
    mismatched.serverId = QStringLiteral("fake");
    mismatched.toolName = QStringLiteral("create");
    mismatched.outputSchemaValidated = true;
    mismatched.structuredContent =
        QJsonObject{{QStringLiteral("ok"), true},
                    {QStringLiteral("name"), QStringLiteral("other")},
                    {QStringLiteral("uuid"), QStringLiteral("resource-1")}};

    application::AgentLedger mismatchedLedger;
    mismatchedLedger.recordToolResult(1, action, mismatched,
                                      application::ToolOperationKind::Mutation);
    QCOMPARE(mismatchedLedger.verifications().size(), 1);
    QCOMPARE(mismatchedLedger.verifications().constFirst().state,
             QStringLiteral("pending"));

    action.arguments = {
        {QStringLiteral("target_path"), QStringLiteral("E:/one/resource")}};
    agent::ToolResult sameBasename = mismatched;
    sameBasename.structuredContent = QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("target_path"), QStringLiteral("E:/two/resource")},
        {QStringLiteral("uuid"), QStringLiteral("resource-3")}};
    application::AgentLedger basenameLedger;
    basenameLedger.recordToolResult(1, action, sameBasename,
                                    application::ToolOperationKind::Mutation);
    QCOMPARE(basenameLedger.verifications().constFirst().state,
             QStringLiteral("pending"));

    action.arguments = {
        {QStringLiteral("name"), QStringLiteral("requested")},
        {QStringLiteral("parent"),
         QJsonObject{{QStringLiteral("uuid"), QStringLiteral("parent-1")},
                     {QStringLiteral("name"), QStringLiteral("container")}}}};
    agent::ToolResult matchingOnlyParent = mismatched;
    matchingOnlyParent.structuredContent = QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("name"), QStringLiteral("other")},
        {QStringLiteral("uuid"), QStringLiteral("resource-4")},
        {QStringLiteral("parent"),
         QJsonObject{{QStringLiteral("uuid"), QStringLiteral("parent-1")},
                     {QStringLiteral("name"), QStringLiteral("container")}}}};
    application::AgentLedger parentLedger;
    parentLedger.recordToolResult(1, action, matchingOnlyParent,
                                  application::ToolOperationKind::Mutation);
    QCOMPARE(parentLedger.verifications().constFirst().state,
             QStringLiteral("pending"));

    agent::ToolResult parentLocatorOnly = mismatched;
    parentLocatorOnly.structuredContent = QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("name"), QStringLiteral("requested")},
        {QStringLiteral("parent"),
         QJsonObject{{QStringLiteral("uuid"), QStringLiteral("parent-1")}}}};
    application::AgentLedger parentLocatorLedger;
    parentLocatorLedger.recordToolResult(
        1, action, parentLocatorOnly, application::ToolOperationKind::Mutation);
    QCOMPARE(parentLocatorLedger.verifications().constFirst().state,
             QStringLiteral("pending"));

    action.arguments = {{QStringLiteral("name"), QStringLiteral("requested")}};
    agent::ToolResult matching = mismatched;
    matching.structuredContent =
        QJsonObject{{QStringLiteral("ok"), true},
                    {QStringLiteral("name"), QStringLiteral("requested")},
                    {QStringLiteral("uuid"), QStringLiteral("resource-2")}};
    application::AgentLedger matchingLedger;
    matchingLedger.recordToolResult(1, action, matching,
                                    application::ToolOperationKind::Mutation);
    QCOMPARE(matchingLedger.verifications().size(), 1);
    QCOMPARE(matchingLedger.verifications().constFirst().state,
             QStringLiteral("verified"));
}

void AgentControllerTest::treatsDefaultModifyRiskAsMutation()
{
    auto toolCallCount = 0;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [](const QList<chat::Message>&, const models::InferencePreset&,
               int) {},
            [] {},
            [&](const QString&, const QJsonObject&)
            {
                ++toolCallCount;
                return QStringLiteral("tool-request-1");
            },
            [](const QString&) {}, acceptsToolArguments, [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; },
            [](const QString&)
            { return infrastructure::mcp::ToolRisk::ModifiesData; }});

    QVERIFY(controller.start(QStringLiteral("Apply the operation"), {},
                             {namedTool(QStringLiteral("apply"),
                                        QStringLiteral("Apply operation"))}));
    controller.receiveToken(QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.apply","arguments":{"name":"resource"}})"));
    controller.completeGeneration(false);
    QCOMPARE(toolCallCount, 1);

    agent::ToolResult result;
    result.requestId = QStringLiteral("tool-request-1");
    result.serverId = QStringLiteral("fake");
    result.toolName = QStringLiteral("apply");
    result.structuredContent =
        QJsonObject{{QStringLiteral("name"), QStringLiteral("resource")},
                    {QStringLiteral("uuid"), QStringLiteral("resource-1")}};
    controller.receiveToolResult(result);

    QCOMPARE(controller.activeRun()->ledger.verifications().size(), 1);
    QCOMPARE(controller.activeRun()->ledger.verifications().constFirst().state,
             QStringLiteral("pending"));
    controller.cancel();
}

void AgentControllerTest::rejectsCompletionWithUnverifiedMutation()
{
    auto generationCount = 0;
    auto toolCallCount = 0;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [&](const QList<chat::Message>&, const models::InferencePreset&,
                int) { ++generationCount; },
            [] {},
            [&](const QString&, const QJsonObject&)
            {
                ++toolCallCount;
                return QStringLiteral("tool-request-%1").arg(toolCallCount);
            },
            [](const QString&) {}, acceptsToolArguments, [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});
    QSignalSpy finalSpy(&controller,
                        &application::AgentController::finalAnswerReady);

    QVERIFY(controller.start(QStringLiteral("1. Create the resource.\n"
                                            "2. Report the resource."),
                             {},
                             {ledgerTool(QStringLiteral("create"), false),
                              ledgerTool(QStringLiteral("inspect"), true)}));
    const QJsonArray plan{
        planStep(QStringLiteral("create"),
                 QStringLiteral("Create the resource"), true),
        planStep(QStringLiteral("report"),
                 QStringLiteral("Report the resource"), false)};
    controller.receiveToken(taskPlanAction(plan));
    controller.completeGeneration(false);
    controller.receiveToken(QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.create","arguments":{"name":"resource"}})"));
    controller.completeGeneration(false);
    agent::ToolResult result;
    result.requestId = QStringLiteral("tool-request-1");
    result.serverId = QStringLiteral("fake");
    result.toolName = QStringLiteral("create");
    result.structuredContent =
        QJsonObject{{QStringLiteral("name"), QStringLiteral("resource")},
                    {QStringLiteral("uuid"), QStringLiteral("resource-1")}};
    controller.receiveToolResult(result);

    controller.receiveToken(
        QByteArrayLiteral(R"({"action":"final","content":"Done"})"));
    controller.completeGeneration(false);
    const QJsonArray incompleteReview{
        reviewedStep(QStringLiteral("create"),
                     QStringLiteral("Create the resource"), true,
                     QStringLiteral("satisfied"), QJsonArray{1}),
        reviewedStep(QStringLiteral("report"),
                     QStringLiteral("Report the resource"), false,
                     QStringLiteral("satisfied"), QJsonArray{1})};
    controller.receiveToken(
        completionReviewAction(QStringLiteral("complete"), incompleteReview,
                               QStringLiteral("The resource is complete.")));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QCOMPARE(finalSpy.count(), 0);

    controller.receiveToken(QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.inspect","arguments":{"name_uuid":"resource/resource-1"}})"));
    controller.completeGeneration(false);
    agent::ToolResult verification;
    verification.requestId = QStringLiteral("tool-request-2");
    verification.serverId = QStringLiteral("fake");
    verification.toolName = QStringLiteral("inspect");
    verification.structuredContent =
        QJsonObject{{QStringLiteral("name"), QStringLiteral("resource")},
                    {QStringLiteral("uuid"), QStringLiteral("resource-1")}};
    controller.receiveToolResult(verification);

    controller.receiveToken(
        QByteArrayLiteral(R"({"action":"final","content":"Done"})"));
    controller.completeGeneration(false);
    const QJsonArray completeReview{
        reviewedStep(QStringLiteral("create"),
                     QStringLiteral("Create the resource"), true,
                     QStringLiteral("satisfied"), QJsonArray{1, 2}),
        reviewedStep(QStringLiteral("report"),
                     QStringLiteral("Report the resource"), false,
                     QStringLiteral("satisfied"), QJsonArray{2})};
    controller.receiveToken(
        completionReviewAction(QStringLiteral("complete"), completeReview,
                               QStringLiteral("The resource is verified.")));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Completed);
    QCOMPARE(finalSpy.count(), 1);
}

void AgentControllerTest::hasNoToolCallCountLimit()
{
    auto toolCallCount = 0;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [](const QList<chat::Message>&, const models::InferencePreset&,
               int) {},
            [] {},
            [&](const QString&, const QJsonObject&)
            {
                ++toolCallCount;
                return QStringLiteral("tool-request-%1").arg(toolCallCount);
            },
            [](const QString&) {}, acceptsToolArguments, [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});
    QSignalSpy finishedSpy(&controller,
                           &application::AgentController::runFinished);
    QSignalSpy finalSpy(&controller,
                        &application::AgentController::finalAnswerReady);

    QVERIFY(controller.start(
        QStringLiteral("1. Perform all operations.\n2. Report completion."), {},
        {echoTool()}));
    const QJsonArray plan{
        planStep(QStringLiteral("operate"),
                 QStringLiteral("Perform all operations"), true),
        planStep(QStringLiteral("report"), QStringLiteral("Report completion"),
                 false)};
    controller.receiveToken(taskPlanAction(plan));
    controller.completeGeneration(false);
    for (auto index = 0; index < 20; ++index)
    {
        controller.receiveToken(
            QStringLiteral(
                R"({"action":"call_tool","tool":"fake.echo","arguments":{"value":%1}})")
                .arg(index)
                .toUtf8());
        controller.completeGeneration(false);
        QCOMPARE(controller.state(),
                 application::AgentRun::State::ExecutingTool);

        agent::ToolResult result;
        result.requestId = QStringLiteral("tool-request-%1").arg(index + 1);
        result.serverId = QStringLiteral("fake");
        result.toolName = QStringLiteral("echo");
        result.result = {{QStringLiteral("content"), index}};
        controller.receiveToolResult(result);
        QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    }
    QCOMPARE(toolCallCount, 20);

    controller.receiveToken(QByteArrayLiteral(
        R"({"action":"final","content":"All operations completed"})"));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QCOMPARE(finalSpy.count(), 0);

    QJsonArray allEvidence;
    for (auto sequence = 1; sequence <= 20; ++sequence)
        allEvidence.append(sequence);
    const QJsonArray completedSteps{
        reviewedStep(QStringLiteral("operate"),
                     QStringLiteral("Perform all operations"), true,
                     QStringLiteral("satisfied"), allEvidence),
        reviewedStep(QStringLiteral("report"),
                     QStringLiteral("Report completion"), false,
                     QStringLiteral("satisfied"), QJsonArray{20})};
    controller.receiveToken(
        completionReviewAction(QStringLiteral("complete"), completedSteps,
                               QStringLiteral("All operations completed.")));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Completed);
    QCOMPARE(toolCallCount, 20);
    QCOMPARE(finishedSpy.count(), 1);
    QCOMPARE(finishedSpy.constFirst().at(2).toString(), QString{});
    QCOMPARE(finalSpy.count(), 1);
}

void AgentControllerTest::longRunWarningDoesNotStopAgent()
{
    application::AgentController controller(
        application::AgentController::Dependencies{
            [](const QList<chat::Message>&, const models::InferencePreset&,
               int) {},
            [] {}, [](const QString&, const QJsonObject&)
            { return QStringLiteral("unused"); }, [](const QString&) {},
            acceptsToolArguments, [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});
    QSignalSpy eventSpy(&controller,
                        &application::AgentController::eventRecorded);
    QSignalSpy finishedSpy(&controller,
                           &application::AgentController::runFinished);

    QVERIFY(controller.start(QStringLiteral("Long-running task"), {},
                             {echoTool()}));
    QVERIFY(QMetaObject::invokeMethod(&controller, "notifyLongRunning",
                                      Qt::DirectConnection));
    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QVERIFY(controller.hasActiveRun());
    QCOMPARE(finishedSpy.count(), 0);
    const auto warning =
        qvariant_cast<agent::Event>(eventSpy.constLast().constFirst());
    QCOMPARE(warning.type, agent::EventType::Warning);
    QVERIFY(warning.message.contains(QStringLiteral("continue")));
    controller.cancel();
}
}  // namespace qtllm::tests

QTEST_GUILESS_MAIN(qtllm::tests::AgentControllerTest)

#include "AgentControllerTest.moc"
