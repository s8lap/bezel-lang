#include "sema/TypeResolution.hpp"
#include "sema/TypeResolution_impl.hpp"

#include <unordered_map>
#include <unordered_set>

#include "ast/Node.hpp"
#include "common/BuiltinRegistry.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeRef.hpp"

namespace bezel::sema {

// The Resolver worker class is declared in TypeResolution_impl.hpp; its
// method definitions live in the sibling TypeResolution_*.cpp translation
// units (TypeResolution_Names.cpp for type/name resolution,
// TypeResolution_Checks.cpp for the well-formedness checks). This file holds
// the public entry function plus the resolver's top-level driver.

// fix #5003 (§2.2 / §2.3, v0.307): is `n` the simple name of an §2.3
// always-available bare name — a name visible in every source file WITHOUT
// an explicit `import`? Per spec §2.3 (table), the always-available set is:
//   - Row 3: `Object`, `Type`
//   - Row 4: `IDisposable`, `IAsyncDisposable`
//   - Row 5: `Task`, `OwningTask` (Task family)
//   - Row 6: `Subscription`, `CancellationToken`, `IEvent`, `IAsyncEvent`,
//     `EventSource`, `AsyncEventSource`, `ISharedEvent`, `SharedEventSource`
//   - Row 7: every §31.4 standard exception type
//   - Row 8: System auto-imports — `Console`, `Math`, `Convert`,
//     `Environment`, `DateTime`, `TimeSpan`, `Guid`, `Random`,
//     `IComparable`, `IEquatable`, `ISerializer`, `IDeserializer`,
//     `ISerializable`, `IDeserializable`, `IntPtr`, `UIntPtr`, `CString`,
//     `WString`, `RawBytes`, plus stdlib §2's System-resident types
//     (`Boolean`, `BitConverter`, `Shared`, `Weak`, `Encoding`,
//     `ReadOnlyStringSpan`, `IFormattable`, `INumber`, `ISteppable`,
//     `IParsable`, the stdlib §2.19 static abstractions)
//   - Row 9: System.Text — `StringBuilder`, `Encoding`, `ReadOnlyStringSpan`
//   - Row 10: System.Collections.Generic — the whole of stdlib §4
//   - Row 11: System.Collections.Linq — `IGrouping`, `IOrderedEnumerable`
//
// `builtinTypeArity`'s table is BROADER than this (it also admits
// `Mutex`, `Lock`, `Semaphore`, `SemaphoreToken`, `Atomic`,
// `AtomicInteger`, `TaskCompletionSource`, `OwningTaskCompletionSource`,
// `TaskScope`, `CancellationTokenSource`, `Channel`, `ChannelSender`,
// `Domain`, `File`, `Directory`, `Path`, `FileStream`, `Stopwatch`,
// `Debug`, `BinarySerializer`, `BinaryDeserializer`, `JsonSerializer`,
// `JsonDeserializer`, `JsonOptions`, `Linq` — every name in stdlib §6
// through §9 that requires `import System.Threading` / `System.IO` /
// `System.Diagnostics` / `System.Serialization` per spec §2.3 L160). Use
// the explicit `kAlwaysAvailableNames` table below to enforce the right
// boundary; consult it BEFORE `builtinTypeArity`'s broader union.
//
// The spec §2.3 wording "every file sees these names" is what makes them
// "imported" for the purpose of §2.2's "unambiguously imported public
// bare name" rule — §2.7 rule 1 resolves imported bare names before
// aliases with no backtracking, so a same-named alias is unreachable in
// every chain-head position.
//
// Note: the §2.3 keyword-aliased primitives (`bool`, `int`, `string`, …)
// are NOT included here — they are unshadowable per §3.4, but the lexer
// rejects any `import Foo as bool;` at parse time (an alias would be a
// parse error long before reaching this check), so they need not be
// enumerated.
//
// Declared in TypeResolution.hpp (fix #5140) so ExprTypecheck.cpp's
// import-collision check can consult the SAME table.
bool isAlwaysAvailableBareName(std::string_view n) {
    return
        // Row 3 / 4 / 5 (core types, disposal interfaces, Task family).
        n == "Object" || n == "Type" ||
        n == "IDisposable" || n == "IAsyncDisposable" ||
        n == "Task" || n == "OwningTask" ||
        // Row 6 (events / subscription / cancellation).
        n == "Subscription" || n == "CancellationToken" ||
        n == "IEvent" || n == "IAsyncEvent" ||
        n == "EventSource" || n == "AsyncEventSource" ||
        n == "ISharedEvent" || n == "SharedEventSource" ||
        // Row 7 (§31.4 standard exceptions — also in builtinTypeArity).
        n == "Exception" || n == "ArgumentException" ||
        n == "ArgumentNullException" || n == "ArgumentOutOfRangeException" ||
        n == "ArithmeticException" || n == "OverflowException" ||
        n == "DivideByZeroException" || n == "InvalidOperationException" ||
        n == "DisposedAccessException" || n == "NotImplementedException" ||
        n == "NullReferenceException" || n == "IndexOutOfRangeException" ||
        n == "KeyNotFoundException" || n == "MatchFailureException" ||
        n == "OperationCanceledException" || n == "TypeInitializationException" ||
        n == "FormatException" || n == "StackOverflowException" ||
        n == "SerializationException" || n == "ContractViolationException" ||
        n == "ExceptionInfo" ||
        // Row 8 (System auto-imports).
        n == "Console" || n == "Math" || n == "Convert" || n == "Environment" ||
        n == "DateTime" || n == "TimeSpan" || n == "DateOnly" || n == "TimeOnly" ||
        n == "Guid" || n == "Random" ||
        n == "IComparable" || n == "IEquatable" ||
        n == "ISerializer" || n == "IDeserializer" ||
        n == "ISerializable" || n == "IDeserializable" ||
        n == "IFormattable" ||
        n == "IntPtr" || n == "UIntPtr" ||
        n == "CString" || n == "WString" || n == "RawBytes" ||
        n == "Boolean" || n == "BitConverter" ||
        n == "Shared" || n == "Weak" ||
        // Row 9 (System.Text).
        n == "StringBuilder" || n == "Encoding" || n == "ReadOnlyStringSpan" ||
        // Row 10 (System.Collections.Generic — the whole of stdlib §4).
        // Derived from kAlwaysAvailableBareNameCollections (BuiltinRegistry.cpp,
        // fix #5044) — the single authority for §2.3 Row 10 names.
        common::isAlwaysAvailableBareNameCollection(n) ||
        // Row 11 (System.Collections.Linq).
        n == "IGrouping" || n == "IOrderedEnumerable";
}

PrimitiveType primitiveFromName(std::string_view name) {
    if (name == "bool")    return PrimitiveType::Bool;
    if (name == "char")    return PrimitiveType::Char;
    if (name == "string")  return PrimitiveType::String;
    if (name == "byte")    return PrimitiveType::Byte;
    if (name == "sbyte")   return PrimitiveType::SByte;
    if (name == "short")   return PrimitiveType::Short;
    if (name == "ushort")  return PrimitiveType::UShort;
    if (name == "int")     return PrimitiveType::Int;
    if (name == "uint")    return PrimitiveType::UInt;
    if (name == "long")    return PrimitiveType::Long;
    if (name == "ulong")   return PrimitiveType::ULong;
    if (name == "float")   return PrimitiveType::Float;
    if (name == "double")  return PrimitiveType::Double;
    if (name == "decimal") return PrimitiveType::Decimal;
    if (name == "Object")  return PrimitiveType::Object;
    // §3.4, #884: PascalCase System type-name aliases.  Each maps to the same
    // PrimitiveType as its lowercase keyword counterpart, so `String`, `Int32`,
    // `Double`, etc. resolve identically to `string`, `int`, `double`, etc. in
    // type positions, explicit-cast positions, and static-member dispatch.
    if (name == "Boolean") return PrimitiveType::Bool;
    if (name == "Char")    return PrimitiveType::Char;
    if (name == "String")  return PrimitiveType::String;
    if (name == "Byte")    return PrimitiveType::Byte;
    if (name == "SByte")   return PrimitiveType::SByte;
    if (name == "Int16")   return PrimitiveType::Short;
    if (name == "UInt16")  return PrimitiveType::UShort;
    if (name == "Int32")   return PrimitiveType::Int;
    if (name == "UInt32")  return PrimitiveType::UInt;
    if (name == "Int64")   return PrimitiveType::Long;
    if (name == "UInt64")  return PrimitiveType::ULong;
    // fix #3833 (ruled #3812, v0.293): `Single` was an undocumented resolver
    // arm — §3.4's dual-form table pairs `float` only with `System.Float`, and
    // stdlib §9.2 declares no `Single` record type. Deleted; `float`'s sole
    // PascalCase alias is `Float`.
    if (name == "Float")   return PrimitiveType::Float;   // §3.4 / stdlib §9.2 (#1241)
    if (name == "Double")  return PrimitiveType::Double;
    if (name == "Decimal") return PrimitiveType::Decimal;
    return PrimitiveType::None;
}

// §7.9 / §22.4 (v0.181): see the declaration in TypeRef.hpp.
bool isNonNullableReferenceType(const TypeRef* t) {
    if (!t || t->nullable) return false;
    switch (t->kind) {
        case TypeRef::Kind::Primitive:
            return t->primitive == PrimitiveType::String ||
                   t->primitive == PrimitiveType::Object;
        case TypeRef::Kind::Function:
            return true;
        case TypeRef::Kind::Named:
            // A resolved class/interface has no default; a resolved record/enum
            // is a value type. A Named TypeRef with no declaration is an
            // imported/builtin reference type (a collection wrapper, `Task`, …)
            // or a bare generic parameter, both of which follow the
            // non-nullable-reference rule (§7.10).
            if (!t->declaration) return true;
            // fix #3462 (v0.315): builtinClassSym mints value types (Range, Ptr)
            // with kind=Class. Distinguish them by their empty moduleName (they
            // have no real module, per Symbol.hpp's moduleName comment).
            if (t->declaration->moduleName.empty()) return false;
            return t->declaration->kind == ast::TypeDeclKind::Class ||
                   t->declaration->kind == ast::TypeDeclKind::Interface;
        default:  // Unit, InlineArray, Error
            return false;
    }
}

void Resolver::run(const ast::SourceFile& src) {
    // §2.8.1 rule 4 (v0.188): "An `import` naming the enclosing file's own
    // module is rejected ('a module cannot import itself')." The enclosing
    // file's own module name is whatever this file's own `module Name;`
    // declares — computed once here so every `import` below can be compared
    // against it. A self-loop is not the legal cross-module SCC cycle rule 4
    // carves out; it is rejected unconditionally.
    std::string selfModuleName;
    if (src.module && src.module->name) {
        for (std::size_t i = 0; i < src.module->name->parts.size(); ++i) {
            if (i != 0) selfModuleName += '.';
            selfModuleName += std::string(src.module->name->parts[i]);
        }
    }

    // §2.15 / issue #631: build the set of imported namespace names so that
    // import-gated builtin types (BinarySerializer / BinaryDeserializer) can be
    // checked in resolveNamed. Every `import X;` contributes its dotted name.
    // §2.2 v0.164 / #1365: also collect import aliases for qualified-name
    // resolution in resolveNamed (§2.7).
    sourceImports_.clear();
    importAliases_.clear();
    ambiguousImportNames_.clear();
    // fix #5140 (§2.3, v0.309): per-file §2.3 imported-public-vs-always-
    // available collision set, populated by the import loop below.
    ambiguousAlwaysAvailableNames_.clear();
    ambiguousAlwaysAvailableNameImportingModule_.clear();
    importedByArity_.clear();
    for (const auto& imp : src.imports) {
        if (!imp || !imp->name) continue;
        std::string dotted;
        for (std::size_t i = 0; i < imp->name->parts.size(); ++i) {
            if (i != 0) dotted += '.';
            dotted += std::string(imp->name->parts[i]);
        }
        // §2.8.1 rule 4: reject an `import` naming this very file's own
        // module. Diagnose and skip — do not add the self-import to
        // sourceImports_/importAliases_, so a self-import behaves as absent
        // (not as a tautological self-visible import) for the rest of name
        // resolution, matching the "rejected" verdict rather than inventing
        // a fallback semantics (proposal 0034 §1: the compiler's prior
        // undefined behavior of silently exposing the same-named artifact's
        // publics is exactly the third, nowhere-specified meaning rule 4
        // forecloses).
        if (!selfModuleName.empty() && dotted == selfModuleName) {
            ctx_.diagnostics().report(
                Severity::Error, imp->span.begin,
                std::string("module `") + selfModuleName +
                "` cannot import itself (\xC2\xA7""2.8.1 rule 4)");
            continue;
        }
        sourceImports_.insert(dotted);
        // §2.2 v0.164: record the alias → module name mapping.
        if (!imp->alias.empty()) {
            importAliases_[imp->alias] = dotted;
        }
    }
    Scope moduleScope;
    // Populate module-scope: every top-level type / alias by name.
    // #2131 / §2.3 v0.226: a local top-level declaration of the same name as
    // an auto-imported prelude type SHADOWS the prelude (the prelude is
    // prepended into `unit->topDecls` by Driver::injectStdlibPrelude, but the
    // user's own decl sits AFTER it in iteration order; inserting it here
    // OVERWRITES the prelude's slot so resolveNamed sees only the user decl).
    // §2.2 reinforces the same rule for explicit `import` — locals win, imports
    // never shadow. A reader of the loop below who skips this comment would
    // read "child->name may collide with a prelude decl" as a bug; the pin
    // is the §2.3 paragraph "the other always-available names... may be
    // shadowed by local declarations under the normal name-resolution rules".
    if (ctx_.module()) {
        for (Symbol* child : ctx_.module()->children) {
            if (!child->name.empty()) {
                moduleScope.names[child->name] = child;
            }
        }
    }
    // §2.2 imports (issue #328 inc 3): make the public names of each imported
    // module visible in this file. For every `import X;` whose module X was
    // loaded into the context (a dependency artifact the driver passed), expose
    // X's exported top-level types by simple name in module scope, so a
    // reference to one resolves to the imported TypeSymbol. A local declaration
    // of the same name takes precedence (it was inserted above); imports never
    // shadow a local type (§2.2: imports make names *visible*, locals win).
    // §2.2 v0.164 collision rule (#1365): when two imported modules export the
    // same public name, that bare name is ambiguous — using it unqualified is a
    // compile error. Track each name's origin module; on collision, remove it
    // from scope and record it in ambiguousImportNames_ so resolveNamed can
    // diagnose a clear "ambiguous" error rather than "unresolved identifier".
    // Only engaged when the file has at least one `import` declaration — the
    // no-import path is byte-for-byte unchanged.
    //
    // fix #4690 (§2.2, v0.307): this used to read `ctx_.hasImports()`
    // (`!importedByModule_.empty()`), which is true only once some import
    // resolved to a LOADED dependency ARTIFACT (ModuleLoader's
    // registerImportedType). A file whose only imports are stdlib pseudo-
    // modules (`System.Threading`, `System.IO`, ...) never registers one —
    // those names resolve through the separate builtin-name tables in
    // TypeResolution_Names.cpp / ExprTypecheck_Calls.cpp, not through a
    // loaded artifact — so `hasImports()` was FALSE for such a file and this
    // entire block, including the alias/alias and alias/local-declaration
    // collision checks below (which need only `src.imports` and the local
    // scope, not a loaded artifact), silently never ran. Measured:
    // `import System.Threading as Th; public class Th ...` compiled clean
    // despite §2.2's "[an alias] may not collide with ... a type declared in
    // the file's module" (s2_2_0006 in the rule corpus). The
    // `importedNameOrigin` loop just below already no-ops per-import when
    // `ctx_.importedTypesOf(dotted)` is null (line ~196's `if (!exported)
    // continue;`), so widening the gate to "the file wrote an import" rather
    // than "an import loaded an artifact" changes behavior ONLY for the
    // alias-collision checks — which is exactly the gap to close — and stays
    // byte-for-byte unchanged for a file with no `import` at all.
    if (ctx_.hasImports() || !src.imports.empty()) {
        // Snapshot local module declarations already in scope: these shadow
        // imports and are never marked ambiguous (§2.2: local wins silently).
        std::unordered_set<std::string_view> localNames;
        for (const auto& [name, _] : moduleScope.names)
            localNames.insert(name);
        // Track which module first introduced each (name, arity) pair via
        // import. fix #5172 (§2.2, v0.310): the comparison key is the PAIR,
        // not the bare name — two imports exporting `Counter` (arity 0) and
        // `Counter<T>` (arity 1) are DISTINCT types (rule 1's collision fires
        // on identical (name, arity), not on identical name alone) and both
        // remain reachable; only same-(name, arity) duplicates are the §2.2
        // collision. The previous bare-name key reported this case as
        // ambiguous and lost the arity-selecting lookup the spec defines.
        std::unordered_map<std::pair<std::string, std::size_t>, std::string,
                           PairHash> importedNameOrigin;
        for (const auto& imp : src.imports) {
            if (!imp || !imp->name) continue;
            std::string dotted;
            for (std::size_t i = 0; i < imp->name->parts.size(); ++i) {
                if (i != 0) dotted += '.';
                dotted += std::string(imp->name->parts[i]);
            }
            // §2.8.1 rule 4: a self-import was already diagnosed above (and
            // excluded from sourceImports_). Also skip it here so a same-
            // named DEPENDENCY ARTIFACT — the issue #1730 second repro shape
            // (`module Dup; import Dup;` built against a `dup1.o` that also
            // declares `module Dup;`) — never gets its publics exposed into
            // this file's scope. Without this, the self-import diagnostic
            // above still fires, but the artifact's types remained visible
            // anyway: a third, nowhere-specified semantics (neither the
            // spec's literal no-op reading nor rejection) that proposal 0034
            // §1 calls out as the bug to close.
            if (!selfModuleName.empty() && dotted == selfModuleName) continue;
            const std::vector<TypeSymbol*>* exported =
                ctx_.importedTypesOf(dotted);
            if (!exported) continue;
            for (TypeSymbol* t : *exported) {
                if (t->name.empty()) continue;
                // §2.2: local declarations shadow imports — do not overwrite.
                if (localNames.count(std::string_view(t->name))) continue;
                // fix #5140 (§2.3, v0.309): a same-named public exported by an
                // explicitly imported module does not shadow the always-available
                // name but makes the bare name **ambiguous** in this file under
                // §2.2's collision rule (every row alike per spec §2.3). Do NOT
                // insert into `moduleScope.names` so the always-available
                // resolution path (TypeResolution_Names.cpp around line 2687)
                // remains the only `first` candidate; record the bare name as
                // ambiguous and the importing module for the diagnostic.
                // HOLD: `Counter` (§2.3 Row 10 / stdlib §4.14) is excluded here
                // pending the companion spec question on (identifier, generic
                // arity) keying — see issue #5140.
                if (isAlwaysAvailableBareName(t->name) &&
                    t->name != "Counter") {
                    ambiguousAlwaysAvailableNames_.insert(std::string(t->name));
                    ambiguousAlwaysAvailableNameImportingModule_.emplace(
                        std::string(t->name), dotted);
                    continue;
                }
                // fix #5172 (§2.2, v0.310): key the collision on
                // (name, arity). The arity is the imported type's own
                // generic-parameter count; user TypeSymbols carry it
                // directly on `genericParams`.
                std::size_t arity = t->genericParams.size();
                auto key = std::make_pair(std::string(t->name), arity);
                auto [originIt, inserted] =
                    importedNameOrigin.emplace(key, dotted);
                if (inserted) {
                    // First import to export this (name, arity) — add it to
                    // scope. A second (name, arity) from a different module
                    // is the §2.2 collision and is recorded below.
                    moduleScope.names.emplace(std::string_view(t->name), t);
                    // Track every imported type by (name, arity) so
                    // resolveNamed's arity-selecting lookup (§2.2 v0.310)
                    // can reach an arity-matching import even when a local
                    // declaration (same name, different arity) shadows the
                    // bare-name scope entry. The scope entry still wins for
                    // arity=0 (current-module shadowing); this side table
                    // is the arity>0 escape hatch.
                    importedByArity_[key] = t;
                } else if (originIt->second != dotted) {
                    // §2.2 collision: a second (different) import exports
                    // this (name, arity). Per §2.2 (v0.310) the bare name is
                    // ambiguous AT THIS ARITY; remove from scope and record
                    // the (name, arity) as ambiguous. A bare `Counter`
                    // (arity=0) reference may still resolve to an
                    // unambiguously-imported arity=0 from elsewhere; only
                    // the colliding (name, arity) pair is rejected.
                    ambiguousImportNames_.insert(key);
                    moduleScope.names.erase(std::string_view(t->name));
                }
                // else: same module listed twice (no-op).
            }
        }
        // §2.2 (v0.183, #1515 / #1489): an import alias may not collide with an
        // unambiguously imported public bare name. §2.7 rule 1 resolves
        // imported bare names before import aliases with no backtracking, so a
        // same-named alias would be unreachable in every chain-head position —
        // the collision is an error at the alias declaration rather than a
        // silently dead alias. A name is unambiguously imported iff some import
        // exported it (importedNameOrigin) and no second import made it
        // ambiguous (ambiguousImportNames_). A name shadowed by a local
        // declaration was never added to importedNameOrigin, so the alias/local
        // collision — a distinct entry in §2.2's list — is not diagnosed here.
        // §2.2 (v0.226, #2179): aliases are declarations, so each alias may
        // be bound only once in a file. Keep the first module for each alias so
        // a later declaration can name both conflicting modules.
        std::unordered_map<std::string, std::string> aliasOrigin;
        for (const auto& imp : src.imports) {
            if (!imp || !imp->name || imp->alias.empty()) continue;
            std::string dotted;
            for (std::size_t i = 0; i < imp->name->parts.size(); ++i) {
                if (i != 0) dotted += '.';
                dotted += std::string(imp->name->parts[i]);
            }
            auto [aliasIt, inserted] = aliasOrigin.emplace(imp->alias, dotted);
            if (!inserted) {
                ctx_.diagnostics().report(
                    Severity::Error, imp->span.begin,
                    std::string("import alias `") + imp->alias +
                    "` for module `" + dotted +
                    "` collides with the same alias already bound to module `" +
                    aliasIt->second + "`; choose a different alias (\xC2\xA7""2.2)");
                continue;
            }
            // §2.2 (v0.226, #2213): an import alias also occupies the
            // current module's declaration namespace. A local type wins over
            // an imported bare name for lookup, but the alias is still a
            // declaration-site collision rather than a silently shadowed
            // qualifier. `localNames` is the snapshot taken before imports,
            // so it covers all declarations already present in this module
            // scope when name resolution starts.
            if (localNames.count(imp->alias)) {
                ctx_.diagnostics().report(
                    Severity::Error, imp->span.begin,
                    std::string("import alias `") + imp->alias +
                    "` for module `" + dotted +
                    "` collides with type `" + imp->alias +
                    "` declared in the current module; choose a different "
                    "alias (\xC2\xA7""2.2)");
                continue;
            }
            // §2.2 (v0.183, #1515 / #1489): an import alias may not collide with an
            // unambiguously imported public bare name. §2.7 rule 1 resolves
            // imported bare names before import aliases with no backtracking, so a
            // same-named alias would be unreachable in every chain-head position —
            // the collision is an error at the alias declaration rather than a
            // silently dead alias. The `importedNameOrigin` key is (name, arity)
            // post-#5172 (§2.2 v0.310); an alias name without its own arity
            // collides with EVERY arity of an unambiguously-imported name, so we
            // scan the whole map for any (alias, *) that is unambiguous.
            {
                std::string aliasName = imp->alias;
                bool unambiguous = false;
                std::string originModule;
                for (const auto& kv : importedNameOrigin) {
                    if (kv.first.first != aliasName) continue;
                    if (!ambiguousImportNames_.count(kv.first)) {
                        unambiguous = true;
                        originModule = kv.second;
                        break;
                    }
                }
                if (unambiguous) {
                    ctx_.diagnostics().report(
                        Severity::Error, imp->span.begin,
                        std::string("import alias `") + imp->alias +
                        "` collides with the unambiguously imported public bare name `" +
                        imp->alias + "` (from module `" + originModule +
                        "`); \xC2\xA7""2.7 rule 1 resolves imported bare names before "
                        "aliases with no backtracking, so the alias would be "
                        "unreachable — choose a different alias (\xC2\xA7""2.2)");
                    continue;
                }
            }
            // fix #5003 (§2.2 / §2.3, v0.307): always-available bare name
            if (isAlwaysAvailableBareName(imp->alias)) {
                ctx_.diagnostics().report(
                    Severity::Error, imp->span.begin,
                    std::string("import alias `") + imp->alias +
                    "` collides with the always-available bare name `" +
                    imp->alias +
                    "` (spec \xC2\xA7""2.3); \xC2\xA7""2.7 rule 1 resolves "
                    "imported bare names before aliases with no backtracking, "
                    "so the alias would be unreachable — choose a different "
                    "alias (\xC2\xA7""2.2)");
                continue;
            }
            // fix #5003 (§2.2 / §2.3, v0.307): public of imported stdlib pseudo-module
            if (common::builtinClassModulePathOf(imp->alias) == dotted) {
                ctx_.diagnostics().report(
                    Severity::Error, imp->span.begin,
                    std::string("import alias `") + imp->alias +
                    "` collides with the public `" + imp->alias +
                    "` of the imported stdlib module `" + dotted +
                    "`; \xC2\xA7""2.7 rule 1 resolves imported bare names before "
                    "aliases with no backtracking, so the alias would be "
                    "unreachable — choose a different alias (\xC2\xA7""2.2)");
            }
        }
    }
    // per §32.13 clause 1 / §32.5 (v0.306): an `extern` signature's
    // admissibility depends on DERIVED properties of the types it names — is
    // this record C-compatible? is this `Ptr<T>`'s pointee in the FFI-pointee
    // set? — and those are computed from the record's own resolved field
    // types. This loop walks `topDecls` in DECLARATION ORDER, so an extern
    // block written above the record it names would see unresolved fields and
    // reject a declaration the spec admits. §32.1 puts extern declarations at
    // module level with no ordering rule, so resolve every other top-level
    // declaration first and the extern blocks afterwards. (Only signatures are
    // involved — no body is resolved here — so nothing else observes the
    // deferral.)
    for (const auto& decl : src.topDecls) {
        if (!decl) continue;
        if (dynamic_cast<const ast::ExternBlock*>(decl.get())) continue;
        resolveTopDecl(*decl, &moduleScope);
    }
    for (const auto& decl : src.topDecls) {
        if (!decl) continue;
        if (!dynamic_cast<const ast::ExternBlock*>(decl.get())) continue;
        resolveTopDecl(*decl, &moduleScope);
    }
    // fix #4047 (§27.8, v0.294): the forward pass above walks
    // `src.topDecls` in declaration order, so a chained alias-of-alias
    // whose OUTER alias is declared TEXTUALLY BEFORE the inner alias it
    // names leaves the outer alias's `aliasedType` in the "Named, no
    // declaration" silent-unresolved state — the alias-transparency arm
    // in resolveNamed (fix #578) only sees through to the underlying
    // type when the inner alias's `aliasedType` has already been
    // resolved by an earlier call. After the forward pass every alias's
    // `aliasedType` has been visited at least once, so a fixed-point
    // re-resolution sweep closes the gap. See the comment on
    // `Resolver::resolveAliasForwardReferences` for the full rationale.
    resolveAliasForwardReferences(src, &moduleScope);
    // §14.2: now that every instantiation site and every deferred
    // nested-generic obligation has been recorded, join them and re-check the
    // inner constraints with the outer parameter bound to each concrete
    // argument. This closes the soundness hole where a constraint on an inner
    // generic (`C<T> where T: new()`) was silently skipped when reached
    // through an outer generic (`Box<W>` ⇒ `C<W>`, then `Box<IFoo>`).
    checkDeferredNestedConstraints();
    // §11.8 / §14.2 (v0.176, #807): re-check type-bound failures deferred
    // during base-list resolution (the recurrence `class Meters:
    // INumber<Meters>` cannot see its own conformance until baseTypes are
    // fully populated). Runs after checkDeferredNestedConstraints so entries
    // that pass appends there are flushed too.
    recheckDeferredTypeBounds();
    // Slice S12: after every base-type reference is resolved,
    // walk each TypeSymbol's inheritance chain and report cycles.
    bool hadCycle = checkInheritanceCycles();
    // Slice S13: a concrete class must implement every inherited
    // abstract / interface member. Skipped when a cycle exists
    // (the chain walk wouldn't terminate cleanly).
    if (!hadCycle) checkAbstractImplementations();
    // §11.8 (v0.176, #807): a class/record conforming to an interface with
    // STATIC members must supply matching statics (exact signature).
    if (!hadCycle) checkStaticInterfaceConformance();
    // Slice S15: hiding forbidden / override must target a virtual.
    if (!hadCycle) checkHidingAndOverride();
    // Slice S14: duplicate non-overloadable member names within a
    // single type.
    checkDuplicateMembers();
    // §11.3 / §13.2 (#747 R42): two top-level types may not share a name.
    checkDuplicateTopLevelTypes();
    // Slice S16: a static class may contain only static members.
    checkStaticClassMembers();
    // §23.5 (#747): static indexers are not supported (any type).
    checkStaticIndexers();
    // §23.5 (v0.241, #2758): indexer parameters may not declare defaults.
    checkIndexerParamDefaults();
    // §23.4 / §1.6 (v0.334): an indexer's end clause may not carry an
    // identifier label.
    checkIndexerEndLabel();
    // §23.5 (v0.334): ref and out parameters are not allowed in indexer
    // parameter lists.
    checkIndexerRefOutParams();
    // §22.5 (v0.306, #4494): every parameter after the first defaulted
    // parameter must itself have a default.
    checkParamDefaultOrdering();
    // §25.2 (#746): comparison/equality operators must form a complete group.
    checkOperatorGroups();
    // §8.3 (#1193): a type overriding `Equals` must also override `GetHashCode`.
    checkEqualsHashCodePairing();
    // §25.2 (v0.158, #1338): a record/enum overloading `=`/`<>` (homogeneous
    // operands) must also declare `Equals(T)` + `override GetHashCode`.
    checkEqualityCoDeclaration();
    // §3.6 / §8.3 (v0.158, #1338): hash-container keys/elements must be
    // hashable (runs after all member types are resolved).
    checkHashContainerKeys();
    // stdlib §4.2 / §14.4 / §3.6 rule 2 (#2713): an owning collection whose
    // element slot is a bare generic parameter is re-checked against the type
    // each instantiation of the enclosing generic binds (runs after all member
    // types are resolved, so every instantiation has been recorded).
    checkOwningElementInstantiations();
    // §15.1.1 / §3.6 rule 2 (#4678): the `[N owned]T` inline-array sibling of
    // checkOwningElementInstantiations above — an owning inline array whose
    // element slot is a bare generic parameter is re-checked against the
    // type each instantiation of the enclosing generic binds (e.g.
    // `Pool<string>` at a field-type mention, per #4367's ruling that a
    // field-type mention is itself an instantiation site).
    checkOwningInlineArrayElementInstantiations();
    // §28.11 / §15.3 (#3775): every recorded element-category slot must not
    // be a move-only value type (an owning inline array or an owning-
    // classified choice) — runs after checkOwningElementInstantiations for
    // the same reason: every instantiation site has been recorded by now,
    // and a choice's classification needs every member type resolved.
    checkElementCategoryInstantiations();
    // §14.4 / §15.3 (#3794): OwningBuffer<T>/Span<T>/ReadOnlySpan<T> reject
    // the §15.3 move-only kinds (owning inline arrays, owning-classified
    // choices) per the §14.4 exception's own clause. Pass 1 (concrete slot)
    // runs inline at the annotation site in TypeResolution_Names.cpp; Pass 2
    // here re-walks bare generic parameters of the enclosing type against
    // each instantiation — same dependencies as checkElementCategory-
    // Instantiations above (every instantiation site must be recorded by
    // now, and a choice's classification needs every member type resolved).
    checkOwningBufferSpanMoveOnlyInstantiations();
    // §17.4 / §3.6 rule 2 (#2911, #2962): `x as T` over a TYPE-level parameter
    // is re-checked against the type each instantiation of the enclosing
    // generic binds (runs here for the same reason: every instantiation site
    // has been recorded by now).
    checkAsTargetInstantiations();
    // Slice S17: class-level modifier mutual-exclusions.
    checkClassModifierCombinations();
    // §11.7 (static class inheritance and interface implementation).
    // Walks the resolved base type list (`t->baseTypes`), so it must run
    // after type resolution populates it (it does — every preceding
    // type-resolution pass has finished by here).
    checkStaticClassInheritance();
    // fix #3100 (§19.6 / §11.7, v0.254): a `closed` class whose hierarchy
    // has a non-public concrete subclass makes the cross-module
    // exhaustiveness guarantee unsound (the leaf is invisible to importers
    // but can still be a runtime value of the closed type). Runs after
    // checkClassModifierCombinations because the descriptor needs the
    // whole module's class set collected (the descendants of a `closed`
    // root are not necessarily textually adjacent).
    checkClosedHierarchyLeafVisibility();
    // Slice S19: overloads must differ in signature.
    checkOverloadSignatures();
    // Slice S22: top-level types may only be public or internal.
    checkTopLevelVisibility();
    // Slice S23: record restrictions (§9.7).
    checkRecordRestrictions();
    // fix #4702 (§9.3/§9.9, v0.307): a combined-form body constructor
    // carrying a contract clause (see the declaration's comment for the
    // rule this does NOT yet implement, and why).
    checkRecordCombinedFormContractClauseConstructors();
    // #2208 (§35.1, v0.226): choice self-containment through value-typed
    // payload components (record↔choice by-value cycles, mutable mutual pairs
    // of choices, etc.). Walks the same value-containment graph as the
    // §9 record check; the cycle's choice-side diagnostic is reported here.
    checkChoiceSelfContainment();
    // §9.8 (issue #550): ISerializable/IDeserializable conformance eligibility.
    checkSerializationEligibility();
    // #594 (§28.2): user code may not declare Dispose() on a class/record.
    checkUserDeclaredDispose();
    // fix #3032 (§15.4.1, v0.262): public / owned C<T> return / both-forms
    // declaration-site diagnostics for a static `FromCollectionLiteral`
    // factory. Runs after every parameter / return type has been resolved
    // so the carrier identification and return-type shape checks can read
    // `ctx_.resolvedType(...)` (the same prerequisite every other post-
    // type-resolution check has).
    checkCollectionLiteralFactoryConjuncts();
    // Slice S28: accessor visibility (§13.6).
    checkAccessorVisibility();
    // Slice S30: const member type must be primitive or string.
    checkConstTypes();
    // Slice S32: enum member restrictions (§10.5).
    checkEnumRestrictions();
    // §35.2 (v0.288, #3837): choice methods' dispatch-modifier restrictions
    // (virtual/abstract always rejected; override admitted only on the
    // Equals/GetHashCode/ToString trio, mirroring enums).
    checkChoiceDispatchModifiers();
    // fix #3869 (§35.1 / §35.2, v0.295): choice member-KIND restrictions
    // (instance fields, constructors, destructors, block-form/storage-backed
    // properties, indexers, nested types, event members) — the admissible-
    // kind half checkChoiceDispatchModifiers's own comment deliberately left
    // out of scope, mirroring checkEnumRestrictions above.
    checkChoiceRestrictions();
    // fix #4115 (§22.16, v0.298): an `invariant` member on an enum or choice
    // body — invisible to both member-kind walks just above, since
    // `invariant` clauses are not a `Symbol` subclass at all.
    checkEnumChoiceInvariantMembers();
    // fix #4186 (§22.16, Appendix A.4 `InvariantDecl`, v0.298): the same gap
    // for a record body or a `static class` body — #4115's own scope
    // deliberately excluded these two shapes.
    checkRecordAndStaticClassInvariantMembers();
    // §14.2 (#746): contradictory generic constraints (e.g. class + value).
    checkGenericConstraintConsistency();
    // Slice S33: method body-presence rules (§12.5 / §11.2).
    checkMethodBodies();
    // fix #4026 (§12.5, v0.295): an abstract Auto-shorthand property's
    // initializer has no backing storage to write into and can never run —
    // reject it, mirroring checkMethodBodies' abstract-member-has-no-body
    // posture for the property/indexer analog of a "body" (the auto
    // initializer).
    checkAbstractPropertyInitializers();
    // fix #4110 (§12.5/§23.4, v0.298): an abstract NAMED property or
    // indexer given an explicit `=>`/block body (as opposed to
    // checkAbstractPropertyInitializers' Auto-shorthand-plus-initializer
    // shape) has no reachable call site either — the body is dead code.
    checkAbstractPropertyBodies();
    // Slice S34: static members may not be virtual (§12.1).
    checkStaticNotVirtual();
    // §11.5 (v0.307, #4719): out-of-order modifiers are rejected. Runs
    // after the dispatch-modifier checks so an out-of-order modifier
    // surfaces a single diagnostic rather than cascading false positives.
    checkMemberModifierOrder();
    // §9.2 (v0.297): a record member may not declare `virtual`, `abstract`,
    // or `sealed override`; `override` is rejected on any record member other
    // than `Equals` / `GetHashCode` / `ToString`. Records are implicitly
    // sealed and have no virtual-dispatch surface (§9.6), so the modifiers
    // would be inert; admitting them would mislead every reader of the
    // declaration. The three-name admission is the Object-slot fill §10.5
    // grants enums and §35.2 grants choices.
    checkRecordMemberModifiers();
    // §9.2 / fix #4643 (v0.298): `equality skip` on a static member is
    // an inert annotation (§9.2 line 1419, mirroring §9.8). The
    // grammar admits the clause on FieldDecl/PropertyDecl; sema rejects
    // it on the `static` modifier of either. Other reject positions
    // (computed property, method, enum constant, choice case) are caught
    // at parse time.
    checkEqualitySkipInert();
    // Slice S35: `async abstract` is a compile error (§12.6).
    checkAsyncModifiers();
    // §22.15 (v0.202, #2052): `pure` placement + signature restrictions.
    checkPureModifiers();
    // §22.15 (v0.202, #2052): `pure` dispatch matching (override + iface
    // implementation). Rule-1 checks run inline in checkHidingAndOverride
    // and checkAbstractImplementations (which ran earlier above); this
    // walker is a placeholder for future chain-wide audits.
    checkPureDispatch();
    // §23.2 (v0.214, #2088, proposal 0058): `readonly` is meaningful only on
    // auto-properties and interface properties — anywhere body-bearing it is
    // either redundant (shape expresses read-onlyness) or contradictory
    // (block form with a `set` accessor). The grammar admits `readonly?` on
    // the body-bearing productions as an over-admission; this pass performs
    // the §23.2 semantic rejection.
    checkReadonlyPropertyForms();
    // Slice S36: constructor chain-call placement (§12.8).
    checkConstructorChains();
    // Slice S37: static constructor takes no parameters (§22.7).
    checkStaticConstructorParams();
    // Slice S38: base/interface visibility (§13.7).
    checkBaseVisibility();
    // §13.3 (#956): a member's signature types must be at least as visible as
    // the member (private-type-in-non-private-member case).
    checkMemberSignatureVisibility();
    // §12.9 / issue #431: variance position enforcement on interface members.
    // `out T` must appear only in output positions; `in T` only in input.
    checkVariancePositions();
    // §12.9 / §11.8 (v0.181, issue #1559): variance markers (`in`/`out`) are
    // interface-only syntax — reject them on class/record/enum/method/alias
    // parameters, and on an interface's parameters when it declares a
    // static member.
    checkVarianceMarkerScope();
    // §22.12 rules 2 and 4 (v0.163, #1363): statics-and-domains checks.
    // Rule 2 errors mutable static class/interface fields; rule 4 warns
    // static readonly class/interface fields. Runs after type resolution
    // so field types are available.
    checkStaticFieldDomains();
    // §22.12 rule 3/4 (v0.262, #3272): pass 2 for the `static readonly`
    // bare-parameter classification. Walks the recorded instantiations of
    // each enclosing type whose body declared a `static readonly` field/
    // auto-property at a bare parameter (`T`, `T?`), and applies rule 3
    // (immutable content → clean), rule 4 (class-typed warning, suppressed
    // for the warning-suppression set), or the residual paragraph's per-
    // instantiation cases (interface, function, borrow-bearing record,
    // holds-borrows choice, owning choice → rejection, owning-inline-array
    // → default warning). The diagnostic lands at the instantiation site
    // per §3.6 rule 2. Runs here for the same reason as
    // checkOwningElementInstantiations / checkAsTargetInstantiations above:
    // every instantiation site has been recorded by now.
    checkStaticReadonlyBareTInstantiations();
    // §14.6 (v0.262, #3271): expanding in-SCC instantiation edges are a
    // compile error at the declaration. Runs after every instantiation
    // has been recorded (instantiations_ is fully populated by now) so
    // the SCC walk sees the whole graph. Placed LAST so the diagnostic
    // lands after every other declaration-site check has fired.
    checkExpandingInstantiationEdges();
    // §14.6 (v0.262, #3271), generic-MEMBER half: the same per-edge condition
    // on a generic member's SELF-edge (`M<T>`'s body spelling
    // `M<Pair<T, T>>(…)`), which the type-node graph above cannot see.
    // Immediately after its sibling so the two §14.6 verdicts are adjacent in
    // both the source and the diagnostic order.
    checkExpandingGenericMemberEdges();
}

// fix #4702 (§9.3 "Combined form" / §9.9 "Constructor-only records",
// v0.307): see the declaration comment (TypeResolution_impl.hpp).
//
// §9.3 states TWO body-constructor disqualifiers for a positional-or-
// combined-form record. Only the SECOND is implemented here:
//
//   (a) "The body may not declare a constructor with the same parameter
//       list as the auto-generated primary constructor." NOT IMPLEMENTED —
//       see issue #4702 for why: the rule-corpus's own hand-authored case
//       (s9_3_0002) and a pre-existing, apparently deliberate lit test
//       (sema/sema_serialization_550__record_combined_form_explicit_ctor_
//       match_accepted_spec_9_8_v0182.bzl, exercising §9.8's "Constructor
//       selection ... matched by name ... compared case-insensitively")
//       disagree on the SAME shape — a body constructor whose parameter
//       TYPES match the primary's but whose NAMES differ only in case
//       (`constructor(x: int, y: int)` against positional fields `X, Y`).
//       Reading "parameter list" as types-only (matching every other
//       signature-shape rule in this codebase — overload conflicts,
//       override matching) rejects the §9.8 test's Vec2, which the lit
//       suite has accepted since before this fix. Reading it as an exact
//       name match makes the rule nearly vacuous (records name positional
//       fields PascalCase by convention; body constructor parameters
//       essentially never collide case-for-case) and does not match the
//       corpus case's OWN lowercase-name repro. Both readings have real
//       evidence behind them; this is a genuine spec-vs-existing-test
//       conflict, not a case to silently edit either side of (CLAUDE.md
//       rule 1) — filed as #4702 rather than guessed at.
//   (b) "The body may not declare a constructor carrying a contract
//       clause" (§9.9's constructor-only trigger: "A record declaring
//       positional fields may not declare such a constructor: a compile
//       error at the record declaration"). IMPLEMENTED below — independent
//       of (a)'s shape question, and no existing test (corpus or lit)
//       conflicts with it (confirmed: no accepted test anywhere in the
//       tree pairs a positional-or-combined-form record with a body
//       constructor carrying `requires`/`ensures`).
//
// Reads the constructor's own `ast::ConstructorDecl` (via
// `ConstructorSymbol::decl`), which is null only for an IMPORTED symbol —
// an imported record's body constructors were already checked in the
// declaring module, so skipping them here is not a gap, it is the same
// once-per-declaration posture every other TypeResolution_Checks.cpp
// per-member walk in this file takes.
void Resolver::checkRecordCombinedFormContractClauseConstructors() {
    if (!ctx_.module()) return;
    for (Symbol* child : ctx_.module()->children) {
        auto* t = dynamic_cast<TypeSymbol*>(child);
        if (!t || t->kind != ast::TypeDeclKind::Record) continue;
        if (!recordIsPositional(t)) continue;

        for (Symbol* m : t->members) {
            auto* cs = dynamic_cast<ConstructorSymbol*>(m);
            if (!cs) continue;
            // SEAM-DEBT: #4714 the caller walks `ctx_.module()->children`, so
            // no imported TypeSymbol reaches this loop; and this is a
            // DECLARATION-site rule, so the null branch is the right answer
            // even if one did (an imported record's ctors were checked in its
            // own module). Re-derive from the sema::Symbol
            // (imported-symbol-audit-2026-08-13.md §4) if either premise moves.
            const auto* rawCtorDecl = cs->decl;
            const auto* cd =
                dynamic_cast<const ast::ConstructorDecl*>(rawCtorDecl);
            if (!cd) continue;  // imported — nothing local to check

            // §9.9 "on any constructor of the record, of any visibility, in
            // any recorded tier".
            if (!cd->contractClauses.empty()) {
                ctx_.diagnostics().report(
                    Severity::Error, m->declLoc,
                    std::string("record `") + std::string(t->name) +
                    "`'s body may not declare a constructor carrying a "
                    "contract clause (\xC2\xA7""22.16) — the auto-generated "
                    "primary constructor can carry none, so no record "
                    "declaring positional fields is constructor-only "
                    "(\xC2\xA7""9.9, \xC2\xA7""9.3)");
            }
            // fix #5100 (§9.3, v0.307) + fix-forward #5132 (§9.3 / §11.1 /
            // §22.6, v0.308): "The body may not declare a constructor with
            // the same parameter list as the auto-generated primary
            // constructor." The v0.308 ruling (per #4775's Q1/Q3/Q4 close)
            // is that "same parameter list" is the §11.3 clash sense — i.e.
            // §22.6's overload identity: arity, parameter types, `ref`/`out`
            // modifiers, and ordinary `owned` on a participating position;
            // parameter NAMES and DEFAULTS do not distinguish. So a record
            // body constructor that differs from the primary only by name
            // (e.g. `record Point(X: int, Y: int)` + `constructor(x: int, y:
            // int)`) or only by default presence is now also a clash.
            // The primary's parameters are the record's positional fields.
            // imported-symbol seam (#3152, audit §4): the primary's
            // parameters come off the positional FieldSymbols (`posParam`),
            // not a TypeDecl downcast.
            //
            // The body constructor's parameters are matched against the
            // PRIMARY's POSITIONAL components only — the combined form's
            // body fields (§9.3's "alongside the positional fields") are
            // NOT part of the primary's list, so they do not participate
            // in this comparison; a body constructor with extra or
            // differently-typed parameters relative to the positional set
            // is a different overload and is not rejected here (it is
            // itself admissible, and §9.8 step 2's name-binding rule is a
            // separate concern).
            const std::vector<const Symbol*> primary =
                recordPositionalFieldSymbols(t);
            if (primary.empty()) continue;
            if (cs->params.size() != primary.size() ||
                cd->params.size() != primary.size())
                continue;
            bool listsMatch = true;
            for (std::size_t i = 0; i < cs->params.size(); ++i) {
                auto* ps = dynamic_cast<ParameterSymbol*>(cs->params[i]);
                auto* pfs = dynamic_cast<const FieldSymbol*>(primary[i]);
                if (!ps || !pfs || !pfs->posParam) {
                    listsMatch = false;
                    break;
                }
                const ast::Param& pf = *pfs->posParam;
                // §22.6 overload identity: arity, parameter types,
                // `ref`/`out` modifiers, ordinary `owned`. Parameter NAMES
                // and DEFAULTS do not participate (§9.3 / §11.1 / §22.6,
                // v0.308).
                TypeRef* paramType =
                    ps->declType ? ctx_.resolvedType(*ps->declType) : nullptr;
                TypeRef* primaryType =
                    pf.type ? ctx_.resolvedType(*pf.type) : nullptr;
                if (!typeRefEqual(paramType, primaryType)) {
                    listsMatch = false;
                    break;
                }
                if (overloadModifier(ps->modifier) !=
                    overloadModifier(pf.modifier)) {
                    listsMatch = false;
                    break;
                }
            }
            if (listsMatch) {
                ctx_.diagnostics().report(
                    Severity::Error, cd->span.begin,
                    std::string("body constructor redeclares the primary ")
                        .append("constructor: it has the same parameter list ")
                        .append("as the auto-generated primary constructor ")
                        .append("(\xC2\xA7""9.3, v0.308)"));
            }
        }
    }
}

bool resolveTypes(const ast::SourceFile& src, SemaContext& ctx) {
    Resolver r(ctx);
    r.run(src);
    return !ctx.diagnostics().hasErrors();
}

// §9.8 / #2325: see the declaration comment (TypeResolution.hpp) and
// Resolver::checkClassSerializationEligibilityLate's own comment
// (TypeResolution_impl.hpp) for why this runs as a separate late pass.
bool checkClassSerializationEligibilityLate(const ast::SourceFile& src,
                                            SemaContext& ctx) {
    (void)src;
    Resolver r(ctx);
    r.checkClassSerializationEligibilityLate();
    return !ctx.diagnostics().hasErrors();
}

// §30.14 / §14.2 / #3235: see the declaration comment (TypeResolution.hpp)
// and Resolver::checkSendableConstraintsLate's own comment
// (TypeResolution_Checks.cpp) for why this runs as a separate late pass.
// Same pattern as `checkClassSerializationEligibilityLate`: a fresh
// Resolver wrapping the same `ctx` replays the deferred sites; the
// pending-sendable list is local to each Resolver instance, so this is
// safe to call from any `typecheckExpressions()` pipeline.
bool checkSendableConstraintsLate(const ast::SourceFile& src,
                                  SemaContext& ctx) {
    (void)src;
    Resolver r(ctx);
    r.checkSendableConstraintsLate();
    return !ctx.diagnostics().hasErrors();
}

// §14.2 / fix #3160: §14.2 constraint check at an instantiation site
// without an `ast::Type` node — the receiver of a static-member access
// (`Ferry<Bad>.Count()`). Same pattern as `checkSendableConstraintsLate`:
// a fresh Resolver wrapping `ctx` runs the check; deferred state is
// local to this Resolver and goes unused (already flushed by the
// resolveTypes-time Resolver before ExprTC calls this), but the
// common-shape case (concrete typeArgs) is handled. See the comment on
// `Resolver::checkGenericConstraintsAtUseSite` for the full rationale
// and the deferred-nested edge case.
//
// fix #4465: the deferred type-bound failures pushed by
// checkGenericConstraintsAtUseSite were NEVER reported — the temporary
// Resolver `r` is discarded without flushing. At ExprTC time all types
// are fully resolved, so flushing now correctly reports the constraint
// violation at the static-call site.
bool checkGenericConstraintsAtExpr(SemaContext& ctx,
                                   SourceLocation useSite,
                                   const std::vector<const ast::Type*>& argAsts,
                                   const std::vector<TypeRef*>& argRefs,
                                   TypeSymbol* tsym) {
    Resolver r(ctx);
    r.checkGenericConstraintsAtUseSite(useSite, argAsts, argRefs, tsym);
    r.flushDeferredTypeBoundFailures();
    return !ctx.diagnostics().hasErrors();
}

// §22.12 rule 4 residual + §9.7 static-field bullet (v0.265, #3273 /
// #3290): the §9.7 null-source value-site pass. Same pattern as
// `checkSendableConstraintsLate` and `checkClassSerializationEligibilityLate`:
// a fresh Resolver wrapping `ctx` walks every static readonly field /
// auto-property whose declared type is one of §9.7's three deferred
// kinds and verifies the initializer is the null-source form. Called
// from `typecheckExpressions()` (after the expression walk has named
// every positional argument) so the constructor-shape analysis has
// something to walk.
bool checkStaticReadonlyNullSource(SemaContext& ctx) {
    Resolver r(ctx);
    r.checkStaticReadonlyNullSource();
    return !ctx.diagnostics().hasErrors();
}

} // namespace bezel::sema
