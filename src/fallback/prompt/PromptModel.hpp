#pragma once

#include <QString>

namespace bb::fallback::prompt {

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
        // Declared intent (display/audit only — never the decision). reason is the
        // agent-stated justification; intentMismatch flags a declared agent id that
        // disagrees with the OS-resolved identity.
        QString      reason;
        bool         intentMismatch     = false;
        // OS-resolved agent provenance — drives the explicit undeclared-reason
        // state so a missing reason reads as a signal, not an absence.
        bool         agentRequestor     = false;
        bool         passphrasePrompt   = false;
        bool         allowEmptyResponse = false;
    };

} // namespace bb::fallback::prompt
