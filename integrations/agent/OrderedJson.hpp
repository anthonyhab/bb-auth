#pragma once

// Order-preserving JSON for editing user-owned config files. QJsonObject
// sorts keys, which would reshuffle a user's settings.json on every edit;
// this keeps document order and serializes in the conventional 2-space form
// (identical to Python's json.dumps(indent=2, ensure_ascii=False)), so a file
// round-trips byte-for-byte. Strict RFC 8259 parsing, depth-bounded; numbers
// keep their source lexeme.

#include <QByteArray>
#include <QJsonValue>
#include <QString>

#include <optional>
#include <utility>
#include <vector>

namespace bb::agent {

    struct OJson {
        enum class Type { Null, Bool, Number, String, Array, Object };

        Type                                   type = Type::Null;
        bool                                   boolean = false;
        QString                                text; // string value, or number lexeme
        std::vector<OJson>                     array;
        std::vector<std::pair<QString, OJson>> object;

        static OJson str(const QString &s);
        static OJson arr(std::vector<OJson> items = {});
        static OJson obj(std::vector<std::pair<QString, OJson>> members = {});

        bool isObject() const { return type == Type::Object; }
        bool isArray() const { return type == Type::Array; }
        bool isString() const { return type == Type::String; }

        OJson       *find(const QString &key);
        const OJson *find(const QString &key) const;
        // Existing member, or a new one appended with `init`.
        OJson &setDefault(const QString &key, OJson init);
        void   remove(const QString &key);

        bool operator==(const OJson &o) const;
        bool operator!=(const OJson &o) const { return !(*this == o); }

        QByteArray dump(int indent = 0) const;
        QJsonValue toQt() const; // for canonical, order-free comparisons

        static std::optional<OJson> parse(const QByteArray &bytes, QString *error);
    };

} // namespace bb::agent
