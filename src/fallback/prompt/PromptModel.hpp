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
        bool         passphrasePrompt   = false;
        bool         allowEmptyResponse = false;
    };

} // namespace bb::fallback::prompt
