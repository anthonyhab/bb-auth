// bb-auth-agents (argv0): wire bb-auth's agent supervision into AI coding
// harnesses — status | install [--dry-run] [HARNESS...] | uninstall ...
//
// Ownership: only entries bb-auth owns are touched — hook commands whose
// basename is `bb-auth-intent-hook`, Claude autoMode strings prefixed
// `bb-auth:`, and the plugin/extension files named after bb-auth. Install is
// strip-ours + add-current, so re-running is a no-op and upgrades a stale
// hook path. The first modification of a config file saves
// `<file>.bb-auth-backup`.

#include "Common.hpp"
#include "OrderedJson.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QStandardPaths>

#include <algorithm>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>

namespace bb::agent {

    namespace {

        const QString kHookName = QStringLiteral("bb-auth-intent-hook");
        const QString kMark     = QStringLiteral("bb-auth:");

        // Claude auto mode classifier entries (prose, read as rules): how
        // escalation works here, and that routing around a declined prompt
        // is going around the user.
        const QString kClaudeEnvironment = QStringLiteral(
            "bb-auth: privilege escalation on this machine is supervised by bb-auth. A PreToolUse "
            "hook rewrites clean `sudo`/`doas` commands to `pkexec` only when polkit will prompt, "
            "and the user approves each one at a bb-auth authentication prompt on their desktop; "
            "the agent never sees or handles the password.");
        const QString kClaudeSoftDeny = QStringLiteral(
            "bb-auth: after a pkexec command exits 126 or 127 (the user dismissed or failed the "
            "bb-auth authentication prompt), retrying the same privileged action by another route "
            "— sudo -S, SUDO_ASKPASS, su, run0, doas, or editing sudoers or polkit rules — goes "
            "around the user's decision.");

        struct Failure {
            QString message;
        };

        QString home(const QString &rel) {
            return QDir::homePath() + QLatin1Char('/') + rel;
        }

        bool onPath(const QString &binary) {
            return !QStandardPaths::findExecutable(binary).isEmpty();
        }

        OJson hookCmd(std::vector<std::pair<QString, OJson>> extra = {}) {
            OJson h = OJson::obj({{QStringLiteral("type"), OJson::str(QStringLiteral("command"))}});
            for (auto &kv : extra)
                h.object.push_back(std::move(kv));
            h.object.emplace_back(QStringLiteral("command"), OJson::str(hookPath()));
            return h;
        }

        OJson group(const QString &matcher, std::vector<OJson> hooks) {
            return OJson::obj({{QStringLiteral("matcher"), OJson::str(matcher)}, {QStringLiteral("hooks"), OJson::arr(std::move(hooks))}});
        }

        bool ownedHook(const OJson &h) {
            const OJson *cmd = h.isObject() ? h.find(QStringLiteral("command")) : nullptr;
            if (!cmd || !cmd->isString())
                return false;
            const QString first = cmd->text.trimmed().section(QLatin1Char(' '), 0, 0);
            return QFileInfo(first).fileName() == kHookName;
        }

        QString canonical(const QJsonValue &v) {
            return QString::fromUtf8(QJsonDocument(QJsonArray{v}).toJson(QJsonDocument::Compact));
        }

        void copyWithMode(const QString &from, const QString &to) {
            QFile::remove(to);
            if (!QFile::copy(from, to))
                throw Failure{QStringLiteral("cannot back up %1").arg(from)};
        }

        void writeJson(const QString &path, const OJson &data) {
            if (QFileInfo::exists(path) && !QFileInfo::exists(path + QStringLiteral(".bb-auth-backup")))
                copyWithMode(path, path + QStringLiteral(".bb-auth-backup"));
            QDir().mkpath(QFileInfo(path).absolutePath());
            const QString tmp = path + QStringLiteral(".bb-auth-tmp");
            QFile         f(tmp);
            if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
                throw Failure{QStringLiteral("cannot write %1").arg(tmp)};
            f.write(data.dump(2) + '\n');
            f.close();
            struct stat st{};
            if (::stat(path.toLocal8Bit().constData(), &st) == 0)
                ::chmod(tmp.toLocal8Bit().constData(), st.st_mode & 07777);
            if (::rename(tmp.toLocal8Bit().constData(), path.toLocal8Bit().constData()) != 0)
                throw Failure{QStringLiteral("cannot replace %1").arg(path)};
        }

        class Harness {
          public:
            virtual ~Harness()                                 = default;
            virtual QString name() const                       = 0;
            virtual bool    present() const                    = 0;
            virtual QString status() const                     = 0;
            virtual QString apply(bool install, bool dryRun)   = 0;
            virtual QString note() const { return {}; }
        };

        // A harness whose hooks live in a JSON file as {hooks: {Event: [groups]}}.
        class JsonHarness : public Harness {
          public:
            virtual QString path() const = 0;
            virtual std::vector<std::pair<QString, std::vector<OJson>>> groups() const = 0;
            virtual QString binary() const { return name(); }

            bool present() const override {
                return onPath(binary()) || QFileInfo::exists(path());
            }

            OJson load() const {
                QFile f(path());
                if (!f.exists())
                    return OJson::obj();
                if (!f.open(QIODevice::ReadOnly))
                    throw Failure{QStringLiteral("%1: cannot read").arg(path())};
                QString err;
                auto    data = OJson::parse(f.readAll(), &err);
                if (!data)
                    throw Failure{QStringLiteral("%1: invalid JSON (%2)").arg(path(), err)};
                if (!data->isObject())
                    throw Failure{QStringLiteral("%1: top level is not a JSON object").arg(path())};
                return *data;
            }

            virtual void strip(OJson &data) const {
                OJson *hooks = data.find(QStringLiteral("hooks"));
                if (!hooks || !hooks->isObject())
                    return;
                for (size_t e = 0; e < hooks->object.size();) {
                    OJson &groups = hooks->object[e].second;
                    if (!groups.isArray()) {
                        ++e;
                        continue;
                    }
                    std::vector<OJson> kept;
                    for (OJson &g : groups.array) {
                        OJson *inner = g.isObject() ? g.find(QStringLiteral("hooks")) : nullptr;
                        if (inner && inner->isArray() && std::any_of(inner->array.begin(), inner->array.end(), ownedHook)) {
                            std::erase_if(inner->array, ownedHook);
                            if (inner->array.empty())
                                continue;
                        }
                        kept.push_back(std::move(g));
                    }
                    if (kept.empty()) {
                        hooks->object.erase(hooks->object.begin() + static_cast<long>(e));
                        continue;
                    }
                    groups.array = std::move(kept);
                    ++e;
                }
                if (hooks->object.empty())
                    data.remove(QStringLiteral("hooks"));
            }

            virtual void add(OJson &data) const {
                OJson &hooks = data.setDefault(QStringLiteral("hooks"), OJson::obj());
                if (!hooks.isObject())
                    throw Failure{QStringLiteral("%1: \"hooks\" is not an object").arg(path())};
                for (auto &[event, gs] : groups()) {
                    OJson &list = hooks.setDefault(event, OJson::arr());
                    if (!list.isArray())
                        throw Failure{QStringLiteral("%1: hooks.%2 is not an array").arg(path(), event)};
                    for (OJson &g : gs)
                        list.array.push_back(std::move(g));
                }
            }

            // Order-free view of the bb-auth entries in `data`.
            virtual QStringList owned(const OJson &data) const {
                QStringList  out;
                const OJson *hooks = data.find(QStringLiteral("hooks"));
                if (hooks && hooks->isObject()) {
                    for (const auto &[event, groups] : hooks->object) {
                        if (!groups.isArray())
                            continue;
                        for (const OJson &g : groups.array) {
                            const OJson *inner = g.isObject() ? g.find(QStringLiteral("hooks")) : nullptr;
                            if (!inner || !inner->isArray())
                                continue;
                            OJson meta = g;
                            meta.remove(QStringLiteral("hooks"));
                            for (const OJson &h : inner->array)
                                if (ownedHook(h))
                                    out << canonical(QJsonArray{event, meta.toQt(), h.toQt()});
                        }
                    }
                }
                out.sort();
                return out;
            }

            std::pair<OJson, OJson> planned(bool install) const {
                const OJson before = load();
                OJson       after  = before;
                strip(after);
                if (install)
                    add(after);
                return {before, after};
            }

            QString apply(bool install, bool dryRun) override {
                const auto [before, after] = planned(install);
                // Order-free: a user edit appended after our entries must not
                // make every install rewrite the file.
                if (after == before || (install && owned(before) == owned(after)))
                    return QStringLiteral("unchanged");
                if (dryRun)
                    return QStringLiteral("would update ") + path();
                writeJson(path(), after);
                return QStringLiteral("updated ") + path();
            }

            QString status() const override {
                const auto [before, after] = planned(true);
                if (owned(before) == owned(after))
                    return QStringLiteral("wired");
                return owned(before).isEmpty() ? QStringLiteral("not wired") : QStringLiteral("stale (re-run install)");
            }
        };

        class Claude : public JsonHarness {
          public:
            QString name() const override { return QStringLiteral("claude"); }
            QString path() const override {
                const QString base = qEnvironmentVariable("CLAUDE_CONFIG_DIR", home(QStringLiteral(".claude")));
                return base + QStringLiteral("/settings.json");
            }
            std::vector<std::pair<QString, std::vector<OJson>>> groups() const override {
                // `if`-gated so the process only spawns on actual escalations.
                std::vector<OJson> pre;
                for (const char *tool : {"sudo", "pkexec", "doas"})
                    pre.push_back(hookCmd({{QStringLiteral("if"), OJson::str(QStringLiteral("Bash(%1 *)").arg(QLatin1String(tool)))}}));
                const auto post = [] { return hookCmd({{QStringLiteral("if"), OJson::str(QStringLiteral("Bash(pkexec *)"))}}); };
                std::vector<std::pair<QString, std::vector<OJson>>> out;
                out.emplace_back(QStringLiteral("PreToolUse"), std::vector<OJson>{group(QStringLiteral("Bash"), pre)});
                out.emplace_back(QStringLiteral("PostToolUse"), std::vector<OJson>{group(QStringLiteral("Bash"), {post()})});
                out.emplace_back(QStringLiteral("PostToolUseFailure"), std::vector<OJson>{group(QStringLiteral("Bash"), {post()})});
                return out;
            }

            void strip(OJson &data) const override {
                JsonHarness::strip(data);
                OJson *autoMode = data.find(QStringLiteral("autoMode"));
                if (!autoMode || !autoMode->isObject())
                    return;
                for (const char *key : {"environment", "soft_deny"}) {
                    OJson *entries = autoMode->find(QLatin1String(key));
                    if (!entries || !entries->isArray())
                        continue;
                    std::erase_if(entries->array, [](const OJson &e) { return e.isString() && e.text.startsWith(kMark); });
                    // ["$defaults"] alone is the built-in list — same as absent.
                    const bool onlyDefaults = entries->array.size() == 1 && entries->array[0] == OJson::str(QStringLiteral("$defaults"));
                    if (entries->array.empty() || onlyDefaults)
                        autoMode->remove(QLatin1String(key));
                }
                if (autoMode->object.empty())
                    data.remove(QStringLiteral("autoMode"));
            }

            void add(OJson &data) const override {
                JsonHarness::add(data);
                OJson &autoMode = data.setDefault(QStringLiteral("autoMode"), OJson::obj());
                if (!autoMode.isObject())
                    throw Failure{QStringLiteral("%1: \"autoMode\" is not an object").arg(path())};
                const std::pair<const char *, QString> entries[] = {{"environment", kClaudeEnvironment}, {"soft_deny", kClaudeSoftDeny}};
                for (const auto &[key, text] : entries) {
                    // Creating the array bare would replace the built-in rules.
                    OJson &list = autoMode.setDefault(QLatin1String(key), OJson::arr({OJson::str(QStringLiteral("$defaults"))}));
                    if (!list.isArray())
                        throw Failure{QStringLiteral("%1: autoMode.%2 is not an array").arg(path(), QLatin1String(key))};
                    list.array.push_back(OJson::str(text));
                }
            }

            QStringList owned(const OJson &data) const override {
                QStringList  out      = JsonHarness::owned(data);
                const OJson *autoMode = data.find(QStringLiteral("autoMode"));
                if (autoMode && autoMode->isObject()) {
                    for (const char *key : {"environment", "soft_deny"}) {
                        const OJson *entries = autoMode->find(QLatin1String(key));
                        if (!entries || !entries->isArray())
                            continue;
                        for (const OJson &e : entries->array)
                            if (e.isString() && e.text.startsWith(kMark))
                                out << canonical(QJsonArray{QLatin1String(key), e.text});
                    }
                }
                out.sort();
                return out;
            }
        };

        class Codex : public JsonHarness {
          public:
            QString name() const override { return QStringLiteral("codex"); }
            QString note() const override {
                return QStringLiteral("codex: run /hooks in the CLI and trust the bb-auth hook (changed hooks need re-review)");
            }
            QString path() const override {
                return qEnvironmentVariable("CODEX_HOME", home(QStringLiteral(".codex"))) + QStringLiteral("/hooks.json");
            }
            bool present() const override {
                return onPath(binary()) || QFileInfo(QFileInfo(path()).absolutePath()).isDir();
            }
            std::vector<std::pair<QString, std::vector<OJson>>> groups() const override {
                return {{QStringLiteral("PreToolUse"), {group(QStringLiteral("Bash"), {hookCmd()})}}};
            }
        };

        class Gemini : public JsonHarness {
          public:
            QString name() const override { return QStringLiteral("gemini"); }
            QString path() const override { return home(QStringLiteral(".gemini/settings.json")); }
            std::vector<std::pair<QString, std::vector<OJson>>> groups() const override {
                return {{QStringLiteral("BeforeTool"),
                         {group(QStringLiteral("^run_shell_command$"),
                                {hookCmd({{QStringLiteral("name"), OJson::str(QStringLiteral("bb-auth-intent"))}})})}}};
            }
        };

        class Devin : public JsonHarness {
          public:
            QString name() const override { return QStringLiteral("devin"); }
            QString path() const override { return home(QStringLiteral(".config/devin/config.json")); }
            // Devin's matcher is a tool-name regex with no `if` gate; the hook
            // self-filters privileged prefixes in ~1 ms.
            std::vector<std::pair<QString, std::vector<OJson>>> groups() const override {
                return {{QStringLiteral("PreToolUse"), {group(QStringLiteral("^exec$"), {hookCmd()})}}};
            }
        };

        // A harness that loads a plugin file from a directory; bb-auth
        // symlinks its packaged copy so upgrades propagate.
        class DropIn : public Harness {
          public:
            DropIn(QString name, QString relSource, QString configDir, QString targetDir, QString note)
                : m_name(std::move(name)), m_rel(std::move(relSource)), m_config(std::move(configDir)),
                  m_targetDir(std::move(targetDir)), m_note(std::move(note)) {}

            QString name() const override { return m_name; }
            QString note() const override { return m_note; }
            bool    present() const override { return onPath(m_name) || QFileInfo(m_config).isDir(); }

            QString source() const { return integrationsDir() + QLatin1Char('/') + m_rel; }
            QString target() const { return m_targetDir + QLatin1Char('/') + QFileInfo(m_rel).fileName(); }

            QString status() const override {
                const QFileInfo t(target());
                if (t.isSymLink())
                    return t.symLinkTarget() == QFileInfo(source()).absoluteFilePath() || linkText() == source()
                        ? QStringLiteral("wired")
                        : QStringLiteral("stale (re-run install)");
                return t.exists() ? QStringLiteral("copied file (re-run install to symlink)") : QStringLiteral("not wired");
            }

            QString apply(bool install, bool dryRun) override {
                const QString   t = target();
                const QFileInfo ti(t);
                const bool      exists = ti.exists() || ti.isSymLink();
                if (install && ti.isSymLink() && linkText() == source())
                    return QStringLiteral("unchanged");
                if (!install && !exists)
                    return QStringLiteral("unchanged");
                if (install && !QFileInfo::exists(source()))
                    throw Failure{QStringLiteral("%1 missing — is bb-auth installed?").arg(source())};
                const QString verb = (install ? QStringLiteral("link ") : QStringLiteral("remove ")) + t;
                if (dryRun)
                    return QStringLiteral("would ") + verb;
                if (exists) {
                    if (!ti.isSymLink())
                        copyWithMode(t, t + QStringLiteral(".bb-auth-backup"));
                    QFile::remove(t);
                }
                if (install) {
                    QDir().mkpath(m_targetDir);
                    if (!QFile::link(source(), t))
                        throw Failure{QStringLiteral("cannot link %1").arg(t)};
                }
                return verb;
            }

          private:
            QString linkText() const {
                char          buf[4096];
                const ssize_t n = ::readlink(target().toLocal8Bit().constData(), buf, sizeof(buf) - 1);
                return n > 0 ? QString::fromLocal8Bit(buf, static_cast<int>(n)) : QString();
            }

            QString m_name, m_rel, m_config, m_targetDir, m_note;
        };

        std::vector<std::unique_ptr<Harness>> allHarnesses() {
            std::vector<std::unique_ptr<Harness>> hs;
            hs.push_back(std::make_unique<Claude>());
            hs.push_back(std::make_unique<Codex>());
            hs.push_back(std::make_unique<Gemini>());
            hs.push_back(std::make_unique<Devin>());
            hs.push_back(std::make_unique<DropIn>(QStringLiteral("opencode"), QStringLiteral("opencode/bb-auth-plugin.js"),
                                                  home(QStringLiteral(".config/opencode")), home(QStringLiteral(".config/opencode/plugins")),
                                                  QStringLiteral("opencode: some versions ignore output.args mutations; declarations still land")));
            hs.push_back(std::make_unique<DropIn>(QStringLiteral("pi"), QStringLiteral("pi/bb-auth-extension.ts"), home(QStringLiteral(".pi/agent")),
                                                  home(QStringLiteral(".pi/agent/extensions")), QStringLiteral("pi: run /reload in open sessions")));
            return hs;
        }

        QString hookStatus() {
            const QString hook = hookPath();
            if (::access(hook.toLocal8Bit().constData(), X_OK) != 0)
                return QStringLiteral("MISSING at ") + hook;
            QProcess p;
            p.start(hook, {QStringLiteral("--version")});
            QString version = QStringLiteral("(not runnable)");
            if (p.waitForFinished(5000)) {
                version = QString::fromUtf8(p.readAllStandardOutput()).trimmed();
                if (version.isEmpty())
                    version = QStringLiteral("(pre-0.3 build: no --version)");
            }
            return hook + QStringLiteral("  ") + version;
        }

        QString polkitStatus() {
            switch (pkcheckExec()) {
                case -1: return QStringLiteral("pkcheck unavailable — hooks will not rewrite sudo");
                case 0:
                    return QStringLiteral("pkexec authorized WITHOUT a prompt (a polkit rule says YES) — hooks will "
                                          "not rewrite sudo, because nothing would stop the command");
                case 1: return QStringLiteral("pkexec not authorized for this user — hooks will not rewrite sudo");
                case 2: return QStringLiteral("pkexec prompts — hooks rewrite sudo to the supervised bb-auth prompt");
                default: return QStringLiteral("pkcheck error — hooks will not rewrite sudo");
            }
        }

        QByteArray row(const QString &name, const QString &rest) {
            return name.leftJustified(9).toUtf8() + ' ' + rest.toUtf8() + '\n';
        }

        const char kUsage[] = "usage: bb-auth-agents [-h] [--version] {status,install,uninstall} ...\n\n"
                              "Wire bb-auth agent supervision into AI coding harnesses.\n\n"
                              "  status                       per-harness wiring + hook, daemon, and polkit state\n"
                              "  install   [--dry-run] [HARNESS...]   add bb-auth entries (default: detected harnesses)\n"
                              "  uninstall [--dry-run] [HARNESS...]   remove only bb-auth's entries\n\n"
                              "harnesses: claude, codex, gemini, devin, opencode, pi\n";

    } // namespace

    // @lat: [[agent-intent#Harness installer]]
    int runAgents(const QStringList &argv) {
        if (argv.isEmpty() || argv.first() == QLatin1String("-h") || argv.first() == QLatin1String("--help")) {
            (argv.isEmpty() ? writeErr : writeOut)(kUsage);
            return argv.isEmpty() ? 2 : 0;
        }
        if (argv.first() == QLatin1String("--version")) {
            writeOut("bb-auth-agents " BB_AUTH_VERSION "\n");
            return 0;
        }
        auto          harnesses = allHarnesses();
        const QString cmd       = argv.first();

        if (cmd == QLatin1String("status")) {
            for (const auto &h : harnesses) {
                QString state;
                try {
                    state = h->status();
                } catch (const Failure &f) { state = QStringLiteral("error: ") + f.message; }
                writeOut(row(h->name(), (h->present() ? QStringLiteral("present") : QStringLiteral("absent ")) + QStringLiteral("  ") + state));
            }
            writeOut("\n" + row(QStringLiteral("hook"), hookStatus()) +
                     row(QStringLiteral("daemon"), daemonPing() ? QStringLiteral("reachable")
                                                                : QStringLiteral("NOT reachable at %1 — hooks will leave commands unchanged").arg(socketPath())) +
                     row(QStringLiteral("polkit"), polkitStatus()));
            return 0;
        }
        if (cmd != QLatin1String("install") && cmd != QLatin1String("uninstall")) {
            writeErr("bb-auth-agents: unknown command '" + cmd.toUtf8() + "'\n" + kUsage);
            return 2;
        }
        const bool  install = cmd == QLatin1String("install");
        bool        dryRun  = false;
        QStringList names;
        for (const QString &a : argv.mid(1)) {
            if (a == QLatin1String("--dry-run"))
                dryRun = true;
            else
                names << a;
        }

        std::vector<Harness *> targets;
        for (const QString &n : names) {
            auto it = std::find_if(harnesses.begin(), harnesses.end(), [&](const auto &h) { return h->name() == n; });
            if (it == harnesses.end()) {
                writeErr("bb-auth-agents: unknown harness '" + n.toUtf8() + "'; known: claude, codex, devin, gemini, opencode, pi\n");
                return 1;
            }
            targets.push_back(it->get());
        }
        if (names.isEmpty())
            for (const auto &h : harnesses)
                if (h->present())
                    targets.push_back(h.get());
        if (targets.empty()) {
            writeErr("bb-auth-agents: no harnesses detected; name one explicitly\n");
            return 1;
        }
        if (install && ::access(hookPath().toLocal8Bit().constData(), X_OK) != 0)
            writeErr("bb-auth-agents: warning: hook binary not found at " + hookPath().toUtf8() + '\n');

        int rc = 0;
        for (Harness *h : targets) {
            QString result;
            try {
                result = h->apply(install, dryRun);
            } catch (const Failure &f) {
                writeErr(row(h->name(), QStringLiteral("error: ") + f.message));
                rc = 1;
                continue;
            }
            writeOut(row(h->name(), result));
            if (install && !h->note().isEmpty() && result != QLatin1String("unchanged"))
                writeOut("          note: " + h->note().toUtf8() + '\n');
        }
        if (install && !dryRun)
            writeOut("Restart running agent sessions so they load the hooks.\n");
        return rc;
    }

} // namespace bb::agent
