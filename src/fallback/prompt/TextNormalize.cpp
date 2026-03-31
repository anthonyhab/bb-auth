#include "TextNormalize.hpp"

#include <QSet>

namespace bb::fallback::prompt {

    QString normalizeDetailText(const QString& text) {
        QString normalized = text;
        normalized.replace('\r', '\n');
        QStringList lines = normalized.split('\n');
        QStringList cleaned;
        cleaned.reserve(lines.size());
        for (const QString& line : lines) {
            const QString simplified = line.simplified();
            if (!simplified.isEmpty()) {
                cleaned << simplified;
            }
        }

        return cleaned.join("\n");
    }

    QString normalizeCompareText(const QString& text) {
        QString normalized = normalizeDetailText(text).toLower();
        normalized.replace('`', ' ');
        normalized.replace('"', ' ');
        normalized.replace(',', ' ');
        normalized.replace('.', ' ');
        normalized = normalized.simplified();
        return normalized;
    }

    bool textEquivalent(const QString& left, const QString& right) {
        const QString a = normalizeCompareText(left);
        const QString b = normalizeCompareText(right);
        if (a.isEmpty() || b.isEmpty()) {
            return false;
        }

        return a == b || a.startsWith(b) || b.startsWith(a);
    }

    QString firstMeaningfulLine(const QString& text) {
        const QString cleaned = normalizeDetailText(text);
        if (cleaned.isEmpty()) {
            return QString();
        }

        const int newline = cleaned.indexOf('\n');
        if (newline == -1) {
            return cleaned;
        }

        return cleaned.left(newline);
    }

    QString trimToLength(const QString& text, int maxChars) {
        if (text.size() <= maxChars) {
            return text;
        }

        return text.left(maxChars - 3).trimmed() + "...";
    }

    QString uniqueJoined(const QStringList& values) {
        QStringList   filtered;
        QSet<QString> seen;

        filtered.reserve(values.size());

        for (const QString& value : values) {
            const QString simplified = value.trimmed();
            if (simplified.isEmpty()) {
                continue;
            }

            const QString norm = normalizeCompareText(simplified);
            if (norm.isEmpty()) {
                filtered << simplified;
                continue;
            }

            if (!seen.contains(norm)) {
                filtered << simplified;
                seen.insert(norm);
            }
        }

        return filtered.join("\n");
    }

} // namespace bb::fallback::prompt
