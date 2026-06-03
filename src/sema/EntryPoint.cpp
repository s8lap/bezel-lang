#include "sema/EntryPoint.hpp"

#include <string>
#include <vector>

#include "sema/Symbol.hpp"

namespace bezel::sema {

namespace {

bool hasModifier(const MethodSymbol& ms, std::string_view m) {
    for (auto mod : ms.modifiers) {
        if (mod == m) return true;
    }
    return false;
}

// A single-identifier Named type matching `name`, with no generic args and
// (by default) not nullable. Used to recognize the `int` return type and the
// `Task` return type.
bool isPlainNamed(const ast::Type* t, std::string_view name) {
    if (!t || t->kind != ast::TypeKind::Named || t->nullable) return false;
    if (!t->name || t->name->parts.size() != 1) return false;
    if (t->name->parts.front() != name) return false;
    return t->genericArgs.empty();
}

// `Array<string>` — the only admitted parameter type for `Main` (§33.2 /
// §33.4: `args` is typed `Array<string>`). One generic argument, the plain
// non-nullable `string` primitive.
bool isArrayOfString(const ast::Type* t) {
    if (!t || t->kind != ast::TypeKind::Named || t->nullable) return false;
    if (!t->name || t->name->parts.size() != 1) return false;
    if (t->name->parts.front() != "Array") return false;
    if (t->genericArgs.size() != 1) return false;
    return isPlainNamed(t->genericArgs.front().get(), "string");
}

// `Task` or `Task<int>` — the admitted async return types (§33.2).
bool isTaskOrTaskInt(const ast::Type* t) {
    if (!t || t->kind != ast::TypeKind::Named || t->nullable) return false;
    if (!t->name || t->name->parts.size() != 1) return false;
    if (t->name->parts.front() != "Task") return false;
    if (t->genericArgs.empty()) return true;        // `Task`
    if (t->genericArgs.size() != 1) return false;
    return isPlainNamed(t->genericArgs.front().get(), "int");  // `Task<int>`
}

// per §33.2: does `ms` match one of the eight accepted entry-point
// signatures? Requires public + static, a parameter list that is either
// empty or a single `Array<string>`, and a return type that matches the
// sync set (none / `int`) or, when `async`, the async set (`Task` /
// `Task<int>`). A method named `Main` that fails any of these is an ordinary
// method (§33.2) and is not an entry point.
bool isQualifyingMain(const MethodSymbol& ms) {
    if (ms.name != "Main") return false;
    if (ms.visibility != ast::Visibility::Public) return false;  // §33.2
    if (!hasModifier(ms, "static")) return false;                // §33.2
    if (ms.isOperator) return false;
    if (!ms.genericParams.empty()) return false;  // Main is not generic

    auto* md = dynamic_cast<const ast::MethodDecl*>(ms.decl);
    if (!md) return false;  // an entry point is a MethodDecl, never an operator

    // §33.2: parameters are either none or a single `Array<string>`.
    if (md->params.size() > 1) return false;
    if (md->params.size() == 1) {
        const ast::Param& p = md->params.front();
        if (!p.modifier.empty()) return false;  // no ref/out/owned on `args`
        if (!isArrayOfString(p.type.get())) return false;
    }

    const ast::Type* ret = md->returnType.get();
    const bool isAsync = hasModifier(ms, "async");
    if (isAsync) {
        // §33.2: async Main returns `Task` or `Task<int>`.
        return isTaskOrTaskInt(ret);
    }
    // §33.2: sync Main returns nothing (unit) or `int`.
    if (ret == nullptr) return true;          // `Main()` / `Main(args)`
    return isPlainNamed(ret, "int");          // `Main(): int`
}

}  // namespace

bool checkEntryPoint(const ast::SourceFile& /*src*/, SemaContext& ctx) {
    // per §33.5: collect every qualifying entry point across all classes.
    std::vector<const MethodSymbol*> candidates;
    for (const auto& sym : ctx.symbols()) {
        auto* ms = dynamic_cast<const MethodSymbol*>(sym.get());
        if (!ms) continue;
        if (isQualifyingMain(*ms)) candidates.push_back(ms);
    }

    if (candidates.empty()) {
        // per §33.5 / §33.1: an executable must have exactly one entry point.
        // (A library module has no entry point and is built as a shared
        // library — §33.6 — but this check is only invoked for executable
        // builds, so reaching here means an executable with no `Main`.)
        ctx.diagnostics().report(
            Severity::Error, SourceLocation{},
            "program has no entry point: no `public static method Main` with "
            "an accepted signature was found (§33.1, §33.2)");
        return false;
    }

    if (candidates.size() > 1) {
        // per §33.5: more than one qualifying `Main` is ill-formed (whether
        // across classes or within a single class). No `--main-class`
        // disambiguation surface exists in this driver, so we list every
        // qualifying candidate. The error is anchored at the first candidate;
        // notes point at each.
        std::string msg =
            "program has more than one entry point; §33.5 requires exactly "
            "one qualifying `Main`. Candidates:";
        for (const MethodSymbol* c : candidates) {
            std::string cls =
                c->parent ? std::string(c->parent->name) : std::string("<global>");
            msg += "\n  " + cls + ".Main";
        }
        const MethodSymbol* first = candidates.front();
        ctx.diagnostics().report(Severity::Error, first->declLoc, msg);
        for (const MethodSymbol* c : candidates) {
            std::string cls =
                c->parent ? std::string(c->parent->name) : std::string("<global>");
            ctx.diagnostics().report(Severity::Note, c->declLoc,
                                     "candidate entry point: " + cls + ".Main");
        }
        return false;
    }

    return true;
}

}  // namespace bezel::sema
