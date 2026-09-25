#include "OrderedJson.hpp"

#include <QJsonArray>
#include <QJsonObject>

namespace bb::agent {

    OJson OJson::str(const QString &s) {
        OJson j;
        j.type = Type::String;
        j.text = s;
        return j;
    }

    OJson OJson::arr(std::vector<OJson> items) {
        OJson j;
        j.type  = Type::Array;
        j.array = std::move(items);
        return j;
    }

    OJson OJson::obj(std::vector<std::pair<QString, OJson>> members) {
        OJson j;
        j.type   = Type::Object;
        j.object = std::move(members);
        return j;
    }

    OJson *OJson::find(const QString &key) {
        for (auto &[k, v] : object)
            if (k == key)
                return &v;
        return nullptr;
    }

    const OJson *OJson::find(const QString &key) const {
        for (const auto &[k, v] : object)
            if (k == key)
                return &v;
        return nullptr;
    }

    OJson &OJson::setDefault(const QString &key, OJson init) {
        if (OJson *v = find(key))
            return *v;
        object.emplace_back(key, std::move(init));
        return object.back().second;
    }

    void OJson::remove(const QString &key) {
        for (auto it = object.begin(); it != object.end(); ++it)
            if (it->first == key) {
                object.erase(it);
                return;
            }
    }

    bool OJson::operator==(const OJson &o) const {
        if (type != o.type)
            return false;
        switch (type) {
            case Type::Null: return true;
            case Type::Bool: return boolean == o.boolean;
            case Type::Number:
            case Type::String: return text == o.text;
            case Type::Array: return array == o.array;
            case Type::Object: return object == o.object;
        }
        return false;
    }

    namespace {

        void dumpString(QByteArray &out, const QString &s) {
            out += '"';
            for (const char c : s.toUtf8()) {
                const auto u = static_cast<unsigned char>(c);
                switch (c) {
                    case '"': out += "\\\""; break;
                    case '\\': out += "\\\\"; break;
                    case '\n': out += "\\n"; break;
                    case '\r': out += "\\r"; break;
                    case '\t': out += "\\t"; break;
                    case '\b': out += "\\b"; break;
                    case '\f': out += "\\f"; break;
                    default:
                        if (u < 0x20)
                            out += QByteArray("\\u00") + QByteArray::number(u, 16).rightJustified(2, '0');
                        else
                            out += c;
                }
            }
            out += '"';
        }

        void dumpValue(QByteArray &out, const OJson &j, int depth, int indent) {
            const QByteArray pad(static_cast<qsizetype>((depth + 1) * indent), ' ');
            const QByteArray closePad(static_cast<qsizetype>(depth * indent), ' ');
            switch (j.type) {
                case OJson::Type::Null: out += "null"; return;
                case OJson::Type::Bool: out += j.boolean ? "true" : "false"; return;
                case OJson::Type::Number: out += j.text.toUtf8(); return;
                case OJson::Type::String: dumpString(out, j.text); return;
                case OJson::Type::Array:
                    if (j.array.empty()) {
                        out += "[]";
                        return;
                    }
                    out += "[\n";
                    for (size_t i = 0; i < j.array.size(); ++i) {
                        out += pad;
                        dumpValue(out, j.array[i], depth + 1, indent);
                        out += i + 1 < j.array.size() ? ",\n" : "\n";
                    }
                    out += closePad + ']';
                    return;
                case OJson::Type::Object:
                    if (j.object.empty()) {
                        out += "{}";
                        return;
                    }
                    out += "{\n";
                    for (size_t i = 0; i < j.object.size(); ++i) {
                        out += pad;
                        dumpString(out, j.object[i].first);
                        out += ": ";
                        dumpValue(out, j.object[i].second, depth + 1, indent);
                        out += i + 1 < j.object.size() ? ",\n" : "\n";
                    }
                    out += closePad + '}';
                    return;
            }
        }

        class Parser {
          public:
            explicit Parser(const QByteArray &b) : m_b(b) {}

            std::optional<OJson> document(QString *error) {
                skipWs();
                auto v = value(0);
                skipWs();
                if (v && m_pos != m_b.size())
                    fail("trailing data");
                if (!v || !m_err.isEmpty()) {
                    if (error)
                        *error = QStringLiteral("%1 at byte %2").arg(m_err.isEmpty() ? QStringLiteral("invalid JSON") : m_err).arg(m_pos);
                    return std::nullopt;
                }
                return v;
            }

          private:
            static constexpr int kMaxDepth = 256;
            const QByteArray    &m_b;
            qsizetype            m_pos = 0;
            QString              m_err;

            void fail(const char *msg) {
                if (m_err.isEmpty())
                    m_err = QString::fromLatin1(msg);
            }
            bool atEnd() const { return m_pos >= m_b.size(); }
            char peek() const { return atEnd() ? '\0' : m_b[m_pos]; }
            void skipWs() {
                while (!atEnd() && (peek() == ' ' || peek() == '\t' || peek() == '\n' || peek() == '\r'))
                    ++m_pos;
            }
            bool literal(const char *word) {
                const qsizetype n = static_cast<qsizetype>(qstrlen(word));
                if (m_b.mid(m_pos, n) != word)
                    return false;
                m_pos += n;
                return true;
            }

            std::optional<OJson> value(int depth) {
                if (depth > kMaxDepth) {
                    fail("nesting too deep");
                    return std::nullopt;
                }
                OJson j;
                switch (peek()) {
                    case '{': return objectValue(depth);
                    case '[': return arrayValue(depth);
                    case '"': {
                        auto s = stringValue();
                        if (!s)
                            return std::nullopt;
                        return OJson::str(*s);
                    }
                    case 't':
                    case 'f':
                        if (literal("true") || literal("false")) {
                            j.type    = OJson::Type::Bool;
                            j.boolean = m_b[m_pos - 1] == 'e' && m_b[m_pos - 4] == 't';
                            return j;
                        }
                        break;
                    case 'n':
                        if (literal("null"))
                            return j;
                        break;
                    default:
                        if (peek() == '-' || (peek() >= '0' && peek() <= '9'))
                            return numberValue();
                }
                fail("unexpected character");
                return std::nullopt;
            }

            std::optional<OJson> numberValue() {
                const qsizetype start = m_pos;
                const auto      digits = [&] {
                    const qsizetype s = m_pos;
                    while (!atEnd() && peek() >= '0' && peek() <= '9')
                        ++m_pos;
                    return m_pos > s;
                };
                if (peek() == '-')
                    ++m_pos;
                if (peek() == '0')
                    ++m_pos;
                else if (!digits()) {
                    fail("invalid number");
                    return std::nullopt;
                }
                if (peek() == '.') {
                    ++m_pos;
                    if (!digits()) {
                        fail("invalid number");
                        return std::nullopt;
                    }
                }
                if (peek() == 'e' || peek() == 'E') {
                    ++m_pos;
                    if (peek() == '+' || peek() == '-')
                        ++m_pos;
                    if (!digits()) {
                        fail("invalid number");
                        return std::nullopt;
                    }
                }
                OJson j;
                j.type = OJson::Type::Number;
                j.text = QString::fromLatin1(m_b.mid(start, m_pos - start));
                return j;
            }

            int hex4() {
                if (m_pos + 4 > m_b.size())
                    return -1;
                bool      ok = false;
                const int v  = m_b.mid(m_pos, 4).toInt(&ok, 16);
                if (!ok)
                    return -1;
                m_pos += 4;
                return v;
            }

            std::optional<QString> stringValue() {
                ++m_pos; // opening quote
                QByteArray out;
                while (!atEnd()) {
                    const char c = m_b[m_pos++];
                    if (c == '"')
                        return QString::fromUtf8(out);
                    if (static_cast<unsigned char>(c) < 0x20) {
                        fail("control character in string");
                        return std::nullopt;
                    }
                    if (c != '\\') {
                        out += c;
                        continue;
                    }
                    const char e = atEnd() ? '\0' : m_b[m_pos++];
                    switch (e) {
                        case '"': out += '"'; break;
                        case '\\': out += '\\'; break;
                        case '/': out += '/'; break;
                        case 'b': out += '\b'; break;
                        case 'f': out += '\f'; break;
                        case 'n': out += '\n'; break;
                        case 'r': out += '\r'; break;
                        case 't': out += '\t'; break;
                        case 'u': {
                            int cp = hex4();
                            if (cp < 0) {
                                fail("invalid \\u escape");
                                return std::nullopt;
                            }
                            if (cp >= 0xD800 && cp <= 0xDBFF && m_b.mid(m_pos, 2) == "\\u") {
                                m_pos += 2;
                                const int lo = hex4();
                                if (lo < 0xDC00 || lo > 0xDFFF) {
                                    fail("invalid surrogate pair");
                                    return std::nullopt;
                                }
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            }
                            const char32_t ch = static_cast<char32_t>(cp);
                            out += QString::fromUcs4(&ch, 1).toUtf8();
                            break;
                        }
                        default: fail("invalid escape"); return std::nullopt;
                    }
                }
                fail("unterminated string");
                return std::nullopt;
            }

            std::optional<OJson> arrayValue(int depth) {
                ++m_pos;
                OJson j = OJson::arr();
                skipWs();
                if (peek() == ']') {
                    ++m_pos;
                    return j;
                }
                for (;;) {
                    skipWs();
                    auto v = value(depth + 1);
                    if (!v)
                        return std::nullopt;
                    j.array.push_back(std::move(*v));
                    skipWs();
                    if (peek() == ',') {
                        ++m_pos;
                        continue;
                    }
                    if (peek() == ']') {
                        ++m_pos;
                        return j;
                    }
                    fail("expected ',' or ']'");
                    return std::nullopt;
                }
            }

            std::optional<OJson> objectValue(int depth) {
                ++m_pos;
                OJson j = OJson::obj();
                skipWs();
                if (peek() == '}') {
                    ++m_pos;
                    return j;
                }
                for (;;) {
                    skipWs();
                    if (peek() != '"') {
                        fail("expected string key");
                        return std::nullopt;
                    }
                    auto key = stringValue();
                    if (!key)
                        return std::nullopt;
                    skipWs();
                    if (peek() != ':') {
                        fail("expected ':'");
                        return std::nullopt;
                    }
                    ++m_pos;
                    skipWs();
                    auto v = value(depth + 1);
                    if (!v)
                        return std::nullopt;
                    // Duplicate key: last value wins, first position kept.
                    if (OJson *existing = j.find(*key))
                        *existing = std::move(*v);
                    else
                        j.object.emplace_back(*key, std::move(*v));
                    skipWs();
                    if (peek() == ',') {
                        ++m_pos;
                        continue;
                    }
                    if (peek() == '}') {
                        ++m_pos;
                        return j;
                    }
                    fail("expected ',' or '}'");
                    return std::nullopt;
                }
            }
        };

    } // namespace

    QByteArray OJson::dump(int indent) const {
        QByteArray out;
        dumpValue(out, *this, 0, indent);
        return out;
    }

    QJsonValue OJson::toQt() const {
        switch (type) {
            case Type::Null: return QJsonValue::Null;
            case Type::Bool: return boolean;
            case Type::Number: return text.toDouble();
            case Type::String: return text;
            case Type::Array: {
                QJsonArray a;
                for (const OJson &v : array)
                    a.append(v.toQt());
                return a;
            }
            case Type::Object: {
                QJsonObject o;
                for (const auto &[k, v] : object)
                    o.insert(k, v.toQt());
                return o;
            }
        }
        return {};
    }

    std::optional<OJson> OJson::parse(const QByteArray &bytes, QString *error) {
        return Parser(bytes).document(error);
    }

} // namespace bb::agent
