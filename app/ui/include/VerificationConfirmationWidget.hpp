#pragma once

#include "AgentController.hpp"

#include <QWidget>

class QLabel;
class QPlainTextEdit;
class QPushButton;

namespace qtllm::ui
{
class VerificationConfirmationWidget final : public QWidget
{
    Q_OBJECT

   public:
    explicit VerificationConfirmationWidget(const QString& reason,
                                            QWidget* parent = nullptr);

    void markCancelled();

   signals:
    void decisionMade(qtllm::application::VerificationDecision decision,
                      const QString& userEvidence);

   private:
    void resolve(application::VerificationDecision decision);
    void setResolvedState(const QString& status, bool accepted);

    QLabel* statusLabel_ = nullptr;
    QLabel* reasonLabel_ = nullptr;
    QLabel* evidenceLabel_ = nullptr;
    QPlainTextEdit* evidenceInput_ = nullptr;
    QPushButton* submitEvidenceButton_ = nullptr;
    QPushButton* acceptButton_ = nullptr;
    QPushButton* stopButton_ = nullptr;
    bool resolved_ = false;
};
}  // namespace qtllm::ui
