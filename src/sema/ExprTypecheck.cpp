#include "sema/ExprTypecheck.hpp"
#include "sema/ExprTypecheck_impl.hpp"

#include <cerrno>
#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast/Node.hpp"
#include "sema/ConstFold.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeRef.hpp"

namespace bezel::sema {

// §5.1 (#738): does the compile-time integer constant `v` fit the range of a
// narrower integer target type? Used to gate implicit literal/constant
// narrowing (an out-of-range constant is a §5.1 error, not silent truncation).
// Returns true for any type that is not one of the gated narrower targets.
static bool fitsInIntType(long long v, PrimitiveType t) {
    switch (t) {
        case PrimitiveType::SByte:  return v >= -128 && v <= 127;
        case PrimitiveType::Byte:   return v >= 0 && v <= 255;
        case PrimitiveType::Short:  return v >= -32768 && v <= 32767;
        case PrimitiveType::UShort: return v >= 0 && v <= 65535;
        case PrimitiveType::UInt:   return v >= 0 && v <= 4294967295LL;
        case PrimitiveType::ULong:  return v >= 0;  // any non-neg ll fits ulong
        default:                    return true;
    }
}

// §5.1 / §19 (#1153): the closed representable range [lo, hi] of an integer
// scrutinee type that is NARROWER than the i64 constant-fold width, so a
// `case` constant / range-bound literal that falls outside it can be rejected
// instead of being silently truncated to the scrutinee width by codegen's
// litAtWidth(). Returns false for any type the gate does not apply to: Long /
// ULong never truncate at i64 (and a >64-bit literal does not fold, so it is
// already rejected by the literal-typing path), and non-integer primitives have
// no integer range. Mirrors fitsInIntType (the var-decl / assignment gate).
static bool caseIntScrutineeRange(PrimitiveType t, long long& lo, long long& hi) {
    switch (t) {
        case PrimitiveType::SByte:  lo = -128;          hi = 127;          return true;
        case PrimitiveType::Byte:   lo = 0;             hi = 255;          return true;
        case PrimitiveType::Short:  lo = -32768;        hi = 32767;        return true;
        case PrimitiveType::UShort: lo = 0;             hi = 65535;        return true;
        case PrimitiveType::Int:    lo = -2147483648LL; hi = 2147483647LL; return true;
        case PrimitiveType::UInt:   lo = 0;             hi = 4294967295LL; return true;
        default:                    return false;
    }
}

// The ExprTC class is declared in ExprTypecheck_impl.hpp; its method
// definitions live across this and the sibling ExprTypecheck_*.cpp
// translation units. This file holds the public entry point, the worker's
// driver (run / visitTopDecl / visitMember / visitStmt), and the small
// shared helpers.

void ExprTC::run(const ast::SourceFile& src) {
    moduleScope_ = std::make_unique<Scope>();
    partialModulePaths_.clear();
    // §2.2 v0.164 / #1365: collect import aliases + sourceImports for
    // alias/module-prefix resolution in resolveMemberAccess (§2.7).
    importAliases_.clear();
    sourceImports_.clear();
    ambiguousImportNames_.clear();
    for (const auto& imp : src.imports) {
        if (!imp || !imp->name) continue;
        std::string dotted;
        for (std::size_t i = 0; i < imp->name->parts.size(); ++i) {
            if (i != 0) dotted += '.';
            dotted += std::string(imp->name->parts[i]);
        }
        sourceImports_.insert(dotted);
        if (!imp->alias.empty())
            importAliases_[imp->alias] = dotted;
    }
    if (ctx_.module()) {
        for (Symbol* child : ctx_.module()->children) {
            if (!child->name.empty()) {
                moduleScope_->names[child->name] = child;
            }
            // §32: an extern block's methods are module-scoped names so
            // that calls to them resolve like ordinary functions.
            if (auto* xb = dynamic_cast<ExternBlockSymbol*>(child)) {
                for (Symbol* d : xb->decls) {
                    if (d && !d->name.empty())
                        moduleScope_->names[d->name] = d;
                }
            }
        }
    }
    // §2.2 imports (issue #328 inc 3): expose each imported module's exported
    // top-level types by simple name, so a bare identifier naming an imported
    // type (e.g. `Calc` in `Calc.Add(...)`) resolves to the imported TypeSymbol
    // and the static-member-access path can find its method. Mirrors the
    // type-resolution pass's import gating (a local declaration wins). Engaged
    // only when dependencies were loaded — the single-module path is unchanged.
    // §2.2 v0.164 collision rule (#1365): same as TypeResolution — track which
    // module first introduced each bare name; on collision, remove from scope
    // and record as ambiguous.
    if (ctx_.hasImports()) {
        std::unordered_set<std::string_view> localNames;
        for (const auto& [name, _sym] : moduleScope_->names)
            localNames.insert(name);
        std::unordered_map<std::string, std::string> importedNameOrigin;
        for (const auto& imp : src.imports) {
            if (!imp || !imp->name) continue;
            std::string dotted;
            for (std::size_t i = 0; i < imp->name->parts.size(); ++i) {
                if (i != 0) dotted += '.';
                dotted += std::string(imp->name->parts[i]);
            }
            if (const std::vector<TypeSymbol*>* exported =
                    ctx_.importedTypesOf(dotted)) {
                for (TypeSymbol* t : *exported) {
                    if (t->name.empty()) continue;
                    if (localNames.count(std::string_view(t->name))) continue;
                    auto [originIt, inserted] =
                        importedNameOrigin.emplace(std::string(t->name), dotted);
                    if (inserted) {
                        moduleScope_->names.emplace(std::string_view(t->name), t);
                    } else if (originIt->second != dotted) {
                        ambiguousImportNames_.insert(std::string(t->name));
                        moduleScope_->names.erase(std::string_view(t->name));
                    }
                }
            }
        }
    }
    for (const auto& decl : src.topDecls) {
        if (!decl) continue;
        visitTopDecl(*decl);
    }
}

// Return (or lazily create) a synthetic TypeSymbol for a built-in class.
// §2.5 / §2.6 / §6.1 / §28 (issue #434): value-type builtins (DateTime,
// TimeSpan, Guid, CancellationToken) carry isValueBuiltin=true so the
// ownership checker treats them as non-owned (no §28.3 disposal obligation).
// All other builtins (CancellationTokenSource, Encoding, …) keep the default
// isValueBuiltin=false and remain owned/reference-typed.
//
// issue #922: delegates to SemaContext::builtinClassSym so the TypeResolution
// annotation pass and the ExprTypecheck pass share the SAME TypeSymbol pointer
// for each builtin name, enabling pointer-equality guards (e.g. isRangeType,
// the Range index-check in ExprTypecheck_Exprs.cpp).
TypeSymbol* ExprTC::builtinClassSym(std::string_view name) {
    // Use the shared SemaContext cache; keep the per-ExprTC builtinClassCache_
    // as a fast forwarding layer (both caches are pointer-stable).
    auto* ts = ctx_.builtinClassSym(name);
    builtinClassCache_[std::string(name)] = ts;
    return ts;
}

// §32.7 / stdlib §10: a value-builtin (IntPtr/UIntPtr) resolved by NAME. The
// TypeRef carries astNode (single-part name) with declaration=null, exactly the
// shape produced by parsing a `: IntPtr` annotation, so codegen's layout lowers
// it to a pointer-sized integer (i64 host). See LlvmBackend_Layout.cpp's FFI
// value-type branch. Mirrors the `rb.Pointer` → IntPtr synthesis (fix #985).
TypeRef* ExprTC::namedValueBuiltinRef(std::string_view name) {
    auto* tr = ctx_.makeTypeRef();
    tr->kind = TypeRef::Kind::Named;
    auto* ann = new ast::Type();
    ann->kind = ast::TypeKind::Named;
    auto* nm = new ast::QualifiedName();
    nm->parts.push_back(std::string(name));
    ann->name.reset(nm);
    tr->astNode = ann;
    return tr;
}

// Slice S29: report when a bare identifier inside a field
// initializer resolves to a non-static member of the current type
// (an implicit-`this` reference, forbidden by §22.8). Static
// members, class constants, enum constants, locals, parameters,
// types, and free names are all permitted.
void ExprTC::checkNotInstanceMember(const ast::IdentifierExpr& id, Symbol* sym) {
    auto* mem = dynamic_cast<MemberSymbol*>(sym);
    if (!mem) return;
    // ConstMemberSymbol is implicitly static (a class const).
    if (dynamic_cast<ConstMemberSymbol*>(sym)) return;
    // EnumConstantSymbol: enum constants are implicitly static compile-time
    // values — they are not instance members. A static readonly field
    // initializer on an enum may freely reference the enum's declared
    // constants (§10 / §22.13 "enum constants are compile-time values and
    // are available before any static readonly initializer runs").
    if (dynamic_cast<EnumConstantSymbol*>(sym)) return;
    bool isStatic = false;
    for (auto mod : mem->modifiers) {
        if (mod == "static") { isStatic = true; break; }
    }
    if (isStatic) return;
    // Flag instance members of the type we're initializing OR any ancestor:
    // an INHERITED instance member is equally a forbidden implicit-`this`
    // reference (§22.8, #931) — `this` is not fully constructed when a field
    // initializer runs. isSubtype is reflexive, so this covers both the
    // same-type case and the inherited case; an unrelated type's member is
    // still skipped.
    if (currentType_ && mem->parent && mem->parent != currentType_ &&
        !isSubtype(currentType_, mem->parent))
        return;
    // per §22.8 / §22.10: a primary-constructor parameter (a FieldSymbol
    // whose `posParam` is non-null AND whose parent type is a CLASS, not a
    // record) is explicitly permitted in field/property initializers. The
    // parameter's value is guaranteed available before field initializers
    // run (§22.8 step 0). A record's positional fields are genuine instance
    // members and remain forbidden. Bug #478. Scoped to the CURRENT type: an
    // INHERITED base primary-ctor param is not a parameter of this class's
    // initializer scope and is reached only through `this`, so it stays
    // forbidden (#931).
    if (auto* fs = dynamic_cast<FieldSymbol*>(sym)) {
        if (fs->posParam && mem->parent == currentType_ &&
            mem->parent->kind == ast::TypeDeclKind::Class)
            return;
    }
    ctx_.diagnostics().report(
        Severity::Error, id.span.begin,
        std::string("a field initializer may not reference instance "
                    "member `") +
        std::string(id.name) + "` (§22.8)");
}

// Slice DR1 (§21.7): does control definitely leave this statement
// list abruptly (return / throw / non-terminating loop) before
// reaching its end? Conservatively returns true only when certain,
// so a value-method check that flags `!bodyAlwaysExits` never
// false-positives on a body that does always exit (modulo the
// noreturn-call case, which mainstream compilers also flag).
bool ExprTC::bodyAlwaysExits(
    const std::vector<std::unique_ptr<ast::Stmt>>& body) {
    for (const auto& s : body) {
        if (s && stmtAlwaysExits(*s)) return true;
    }
    return false;
}

// §29.3: check if a statement list contains any yield return/break
// (making it an iterator method body that is exempt from DR1).
bool ExprTC::stmtsContainYield(
    const std::vector<std::unique_ptr<ast::Stmt>>& body) {
    for (const auto& s : body) {
        if (!s) continue;
        if (dynamic_cast<const ast::YieldStmt*>(s.get())) return true;
        if (auto* fs = dynamic_cast<const ast::ForStmt*>(s.get()))
            if (stmtsContainYield(fs->body)) return true;
        if (auto* ws = dynamic_cast<const ast::WhileStmt*>(s.get()))
            if (stmtsContainYield(ws->body)) return true;
        if (auto* ifs = dynamic_cast<const ast::IfStmt*>(s.get())) {
            if (stmtsContainYield(ifs->ifBranch.body)) return true;
            for (const auto& el : ifs->elifBranches)
                if (stmtsContainYield(el.body)) return true;
            if (ifs->hasElse && stmtsContainYield(ifs->elseBody)) return true;
        }
        if (auto* db = dynamic_cast<const ast::DoEndBlock*>(s.get()))
            if (stmtsContainYield(db->body)) return true;
        // §29.6 / §29.7 (#623): yield inside a try body (not catch or finally,
        // which §29.6 forbids) makes the enclosing method an iterator; recurse
        // into the try body so the DR1 "missing return on every path" check
        // doesn't misfire on iterator methods whose yield is wrapped in try.
        if (auto* trys = dynamic_cast<const ast::TryStmt*>(s.get()))
            if (stmtsContainYield(trys->body)) return true;
        // #1107/#16 (§29.3 / §21.7): a `yield return` inside a `case`-statement
        // arm makes the enclosing method an iterator too — recurse into each
        // block-form arm. Without this an iterator whose entire body is a
        // yielding `case` was misclassified as a normal value-returning method
        // and wrongly rejected "does not return a value on every path".
        if (auto* cs = dynamic_cast<const ast::CaseStmt*>(s.get()))
            for (const auto& arm : cs->arms)
                if (stmtsContainYield(arm.stmts)) return true;
    }
    return false;
}

bool ExprTC::stmtAlwaysExits(const ast::Stmt& s) {
    if (dynamic_cast<const ast::ReturnStmt*>(&s)) return true;
    if (dynamic_cast<const ast::ThrowStmt*>(&s)) return true;
    // §31.2.2 / §21.7: an EXPRESSION-form catch arm `catch E => throw …` (bare
    // rethrow `throw;` or `throw expr`) is parsed as an ExpressionStmt wrapping
    // a ThrowExpr (Parser_Stmts.cpp expression-leading branch), NOT a ThrowStmt.
    // A throw-expression always diverges, so the statement always exits. Without
    // this, `try return x; catch E => throw; end;` (and `=> throw new X()`) was
    // wrongly flagged "does not return a value on every path" — while the block
    // form `catch E => do throw …; end` (a ThrowStmt) was correctly accepted.
    if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(&s))
        return es->expr &&
               dynamic_cast<const ast::ThrowExpr*>(es->expr.get()) != nullptr;
    if (auto* ifs = dynamic_cast<const ast::IfStmt*>(&s)) {
        if (!ifs->hasElse) return false;  // false edge falls through
        if (!bodyAlwaysExits(ifs->ifBranch.body)) return false;
        for (const auto& el : ifs->elifBranches) {
            if (!bodyAlwaysExits(el.body)) return false;
        }
        return bodyAlwaysExits(ifs->elseBody);
    }
    if (auto* ws = dynamic_cast<const ast::WhileStmt*>(&s)) {
        // `while true do … end` without a reachable break never
        // completes normally. We conservatively treat a literal-true
        // condition as non-terminating (a break would let it fall
        // through, which would only make us UNDER-report — never a
        // false positive).
        auto* lit =
            dynamic_cast<const ast::BoolLiteralExpr*>(ws->condition.get());
        return lit && lit->value;
    }
    if (auto* dob = dynamic_cast<const ast::DoEndBlock*>(&s)) {
        return bodyAlwaysExits(dob->body);
    }
    if (auto* cs = dynamic_cast<const ast::CaseStmt*>(&s)) {
        // §19.6: a non-exhaustive case STATEMENT falls through on no
        // match. We can only be sure it exits when there is an
        // unguarded `_` arm (full cover) AND every arm body exits.
        bool hasWildcard = false;
        for (const auto& arm : cs->arms) {
            if (!armBodyAlwaysExits(arm)) return false;
            if (!arm.guard &&
                dynamic_cast<const ast::DiscardPattern*>(
                    arm.pattern.get())) {
                hasWildcard = true;
            }
        }
        return hasWildcard;
    }
    if (auto* ts = dynamic_cast<const ast::TryStmt*>(&s)) {
        // A finally that always exits dominates. Otherwise the try
        // body and every catch arm must always exit.
        if (ts->hasFinally && bodyAlwaysExits(ts->finallyBody)) {
            return true;
        }
        if (!bodyAlwaysExits(ts->body)) return false;
        for (const auto& arm : ts->catches) {
            if (!arm.body || !stmtAlwaysExits(*arm.body)) return false;
        }
        return true;
    }
    return false;  // loops that may not run, plain stmts, etc.
}

bool ExprTC::armBodyAlwaysExits(const ast::CaseArm& arm) {
    if (arm.bodyKind == ast::CaseArmBodyKind::Block) {
        return bodyAlwaysExits(arm.stmts);
    }
    // Expression-bodied arm in statement position discards its value
    // and falls through; treat as non-exiting.
    return false;
}

namespace {

// §19.3: an unguarded `_` (DiscardPattern) or unconstrained `var x`
// (BindingPattern with no type annotation) matches any value — a
// catch-all that makes the `case` trivially exhaustive (§19.6).
bool isUnguardedCatchAll(const ast::CaseArm& arm) {
    if (arm.guard) return false;  // a guard can fail at runtime
    const ast::Pattern* p = arm.pattern.get();
    if (dynamic_cast<const ast::DiscardPattern*>(p)) return true;
    if (auto* bp = dynamic_cast<const ast::BindingPattern*>(p))
        return bp->type == nullptr;  // `var x` (untyped) is total; `var x: T` is a type test
    return false;
}

// §19.6: collect the last name part of every enum-constant pattern an
// unguarded arm covers. Enum patterns appear as a NamedPattern (`E.Red`
// or bare `Red`) or as a ConstantPattern holding one or more constant
// expressions (each a MemberAccessExpr `E.Red`, an IdentifierExpr `Red`,
// or a QualifiedName-bearing form).
void collectEnumPatternNames(const ast::CaseArm& arm,
                             std::unordered_set<std::string_view>& covered) {
    if (arm.guard) return;  // a guarded arm cannot be relied on for coverage
    // §19.3 (v0.183, #1584): an AltPattern's constant/named members contribute
    // their enum-constant coverage the same as a ConstantPattern's; its range
    // and relational members do not (§19.6 counts declared constants, not
    // extents). Recurse into members, then fall through for the leaf kinds.
    std::function<void(const ast::Pattern*)> collect =
        [&](const ast::Pattern* p) {
        if (auto* np = dynamic_cast<const ast::NamedPattern*>(p)) {
            if (np->name && !np->name->parts.empty())
                covered.insert(np->name->parts.back());
        } else if (auto* cp = dynamic_cast<const ast::ConstantPattern*>(p)) {
            for (const auto& c : cp->constants) {
                if (!c) continue;
                if (auto* ma =
                        dynamic_cast<const ast::MemberAccessExpr*>(c.get()))
                    covered.insert(ma->member);
                else if (auto* id =
                             dynamic_cast<const ast::IdentifierExpr*>(c.get()))
                    covered.insert(id->name);
            }
        } else if (auto* ap = dynamic_cast<const ast::AltPattern*>(p)) {
            for (const auto& m : ap->members) collect(m.get());
        }
    };
    collect(arm.pattern.get());
}

// §10.2 (issue #1577): resolve every declared enum constant's underlying
// value, honoring an explicit `:= N` initializer and auto-increment
// continuation from the previous value — the same algorithm as the
// underlying-type range check in TypeResolution_Checks.cpp's
// checkEnumRestrictions, recomputed here because EnumConstantSymbol::value
// (Symbol.hpp) is populated only for an IMPORTED enum (the cross-module
// carrier); a LOCAL enum's values are always re-derived from its AST. A
// value that fails to fold (a non-constant initializer — a separate,
// already-diagnosed concern) falls back to the auto-increment default so
// later siblings still continue correctly.
std::unordered_map<std::string_view, long long> computeLocalEnumConstantValues(
        const ast::TypeDecl& td) {
    std::unordered_map<std::string_view, long long> values;
    long long running = 0;
    for (const auto& ec : td.enumConstants) {
        long long val = running;
        if (ec.value) {
            auto resolve = [&](std::string_view name, int)
                    -> std::optional<long long> {
                auto it = values.find(name);
                if (it != values.end()) return it->second;
                return std::nullopt;
            };
            if (auto folded = foldIntConst(*ec.value, resolve)) val = *folded;
        }
        values[ec.name] = val;
        running = val + 1;
    }
    return values;
}

// §10.2 (issue #1577): the enum-constant-name -> underlying-value map for
// `sub`. Prefers the AST (local enum, full fidelity including auto-increment
// continuation); falls back to the per-symbol `value` field for an imported
// enum, which has no ast::TypeDecl (mirrors the fallback in the exhaustiveness
// check just below).
std::unordered_map<std::string_view, long long> enumConstantValues(
        const TypeSymbol& sub) {
    if (auto* td = dynamic_cast<const ast::TypeDecl*>(sub.decl))
        return computeLocalEnumConstantValues(*td);
    std::unordered_map<std::string_view, long long> values;
    for (const Symbol* m : sub.members)
        if (auto* ec = dynamic_cast<const EnumConstantSymbol*>(m))
            values[ec->name] = ec->value;
    return values;
}

// §10.2 (issue #1577): the last name part + source location of one
// enum-constant reference inside a `case` pattern — either a bare
// NamedPattern member or one entry of a ConstantPattern's `|` alternative
// list (mirrors collectEnumPatternNames's two pattern shapes above).
struct EnumPatternRef {
    std::string_view name;
    SourceLocation loc;
};
std::vector<EnumPatternRef> enumPatternRefs(const ast::CaseArm& arm) {
    std::vector<EnumPatternRef> refs;
    // §19.3 (v0.183, #1584): recurse into an AltPattern's constant/named members
    // (its range/relational members carry no enum constant).
    std::function<void(const ast::Pattern*)> collect =
        [&](const ast::Pattern* p) {
        if (auto* np = dynamic_cast<const ast::NamedPattern*>(p)) {
            if (np->name && !np->name->parts.empty())
                refs.push_back({np->name->parts.back(), np->span.begin});
        } else if (auto* cp = dynamic_cast<const ast::ConstantPattern*>(p)) {
            for (const auto& c : cp->constants) {
                if (!c) continue;
                if (auto* ma =
                        dynamic_cast<const ast::MemberAccessExpr*>(c.get()))
                    refs.push_back({ma->member, c->span.begin});
                else if (auto* id =
                             dynamic_cast<const ast::IdentifierExpr*>(c.get()))
                    refs.push_back({id->name, c->span.begin});
            }
        } else if (auto* ap = dynamic_cast<const ast::AltPattern*>(p)) {
            for (const auto& m : ap->members) collect(m.get());
        }
    };
    collect(arm.pattern.get());
    return refs;
}

// §19.6: does any unguarded arm explicitly match `null` (NullPattern)?
bool armsCoverNull(const ast::CaseExpr& ce) {
    for (const auto& arm : ce.arms) {
        if (arm.guard) continue;
        if (dynamic_cast<const ast::NullPattern*>(arm.pattern.get()))
            return true;
    }
    return false;
}

// §11.7 / §19.6: is `cls` a closed abstract base — one whose complete set
// of leaf types is statically knowable? Two forms qualify:
//   * `sealed abstract`: no subclass outside the declaring module can
//     exist, so the closed set holds across module boundaries (§11.7).
//   * a same-module (non-imported) `abstract` class whose every direct
//     subclass is itself `sealed`/closed (recursively forms a closed set,
//     §19.6). An imported plain-abstract base may have unseen subclasses,
//     so it is treated as open.
bool hasModifier(const TypeSymbol* t, std::string_view m) {
    for (auto mod : t->modifiers) if (mod == m) return true;
    return false;
}

} // namespace

// §19.6: emit a non-exhaustiveness WARNING for an expression-position
// `case` whose arms the compiler cannot prove cover every possible value.
// The spec lists the scrutinee shapes for which exhaustiveness IS provable
// — enum (all constants), bool (both literals), a closed sealed-abstract
// class hierarchy (all leaves), and the nullable variant of those (the
// above plus an explicit `null` arm). An unguarded `_` or unconstrained
// `var x` arm is a catch-all and is always exhaustive. Every other shape
// (an open class hierarchy, int/string/range scrutinees, the scrutinee-
// less boolean-condition form, partial coverage) cannot be proven
// exhaustive, so the warning fires unless a catch-all is present (§19.6).
//
// This is a WARNING, not an error: §19.6 states an unmatched value raises
// MatchFailureException at runtime and "the compiler warns when it cannot
// prove exhaustiveness statically." The diagnostic does not fail
// compilation (DiagnosticEngine::hasErrors counts only errors).
void ExprTC::checkCaseExhaustiveness(const ast::CaseExpr& ce, TypeRef* scrutTy) {
    // §5.1 / §19 (#1153): range-check integer pattern literals against the
    // scrutinee FIRST — this is the editable hook the CaseExpr handler reaches
    // for every expression-position `case` (it runs even when a `_` catch-all
    // makes the exhaustiveness check below return early).
    checkCasePatternConstantRanges(ce.arms, scrutTy);
    // An unguarded catch-all (`_` / untyped `var x`) covers everything.
    for (const auto& arm : ce.arms) {
        if (isUnguardedCatchAll(arm)) return;
    }

    auto warn = [&](const std::string& detail) {
        ctx_.diagnostics().report(
            Severity::Warning, ce.span.begin,
            "non-exhaustive `case`: " + detail +
            " (add a `_` arm or cover the remaining value(s)) (§19.6)");
    };

    // A nullable scrutinee additionally requires an explicit `null` arm
    // for exhaustiveness (§19.6: "the rules above plus an explicit `null`
    // pattern"). If the value can be null but no arm matches null, the
    // case is non-exhaustive regardless of the non-null coverage below.
    bool nullableUncovered =
        scrutTy && scrutTy->nullable && !armsCoverNull(ce);

    // §19.3 / §19.5 / §19.6 (#1472): a bare `..` RangePattern (no lo/hi/step,
    // grammar A.7) is vacuously satisfied by every value in the value domain,
    // so an unguarded `..` arm covers all NON-NULL values — but a range pattern
    // never matches null (§19.5: null is matched only by `null`/untyped `var
    // x`/`_`). For a non-nullable scrutinee this is a total catch-all
    // (exhaustive); for a nullable scrutinee the non-null domain is covered but
    // `null` still needs its own arm (§19.6 nullable rule). Mirrors the
    // typed-binding-covers-the-full-type logic below. Without this a valid
    // total `..` case drew a spurious non-exhaustiveness warning.
    for (const auto& arm : ce.arms) {
        if (arm.guard) continue;
        auto* rp = dynamic_cast<const ast::RangePattern*>(arm.pattern.get());
        if (!rp) continue;
        auto* re = dynamic_cast<const ast::RangeExpr*>(rp->rangeExpr.get());
        if (re && !re->lo && !re->hi && !re->step) {
            if (!nullableUncovered) return;  // total over the value domain
            warn("the nullable scrutinee's non-null values are covered by a "
                 "`..` range pattern, but `null` is not");
            return;
        }
    }

    // §19.5 / §19.6 (#784): an unguarded typed-binding arm `var n: T` matches
    // ALL non-null values (§19.5 — "a typed binding pattern `var n: T` matches
    // only non-null values"). When the scrutinee is assignable to T (T is the
    // scrutinee's own static type or a supertype of it), such an arm covers
    // every non-null value of the scrutinee, so the case is exhaustive once
    // null is also covered: a NON-nullable scrutinee is fully covered by the
    // binding alone; a nullable scrutinee additionally needs a `null` arm.
    // Without this, `case x null => 0, var n: int => n end` (x: int?) drew a
    // spurious non-exhaustiveness warning even though it covers every value
    // (sibling of the bool? exhaustiveness fix, daa83f4).
    if (scrutTy) {
        for (const auto& arm : ce.arms) {
            if (arm.guard) continue;
            auto* bp =
                dynamic_cast<const ast::BindingPattern*>(arm.pattern.get());
            if (!bp || !bp->type) continue;
            const TypeRef* bt = ctx_.resolvedType(*bp->type);
            if (!bt || bt->kind != scrutTy->kind) continue;
            bool coversNonNull = false;
            if (bt->kind == TypeRef::Kind::Primitive)
                coversNonNull = bt->primitive == scrutTy->primitive;
            else if (bt->kind == TypeRef::Kind::Named && bt->declaration &&
                     scrutTy->declaration)
                coversNonNull = bt->declaration == scrutTy->declaration ||
                                isSubtype(scrutTy->declaration, bt->declaration);
            if (!coversNonNull) continue;
            if (!scrutTy->nullable || armsCoverNull(ce)) return;  // exhaustive
            // Non-null fully covered, but null is not (§19.6 nullable rule).
            warn("the nullable scrutinee's non-null values are covered by a "
                 "typed binding, but `null` is not");
            return;
        }
    }

    // --- Bool scrutinee (§19.6): both `true` and `false` must be covered. A
    // nullable `bool?` is exhaustive when, per the nullable rule (§19.6:
    // "the rules above plus an explicit `null` pattern"), `true`, `false`, AND
    // `null` are all covered. fix #747 R43: the nullable case was previously
    // excluded here (`!scrutTy->nullable`), so a fully-covered `bool?` fell
    // through to the generic "cannot prove" path and drew a spurious warning.
    if (scrutTy &&
        scrutTy->kind == TypeRef::Kind::Primitive &&
        scrutTy->primitive == PrimitiveType::Bool) {
        bool t = false, f = false;
        for (const auto& arm : ce.arms) {
            if (arm.guard) continue;
            auto scan = [&](const ast::Expr* e) {
                if (auto* b = dynamic_cast<const ast::BoolLiteralExpr*>(e)) {
                    if (b->value) t = true; else f = true;
                }
            };
            if (auto* cp =
                    dynamic_cast<const ast::ConstantPattern*>(arm.pattern.get()))
                for (const auto& c : cp->constants) if (c) scan(c.get());
        }
        if (!(t && f))
            warn("bool scrutinee does not cover both `true` and `false`");
        else if (nullableUncovered)
            warn("nullable `bool?` scrutinee has no `null` arm");
        return;
    }

    // --- Enum / closed-hierarchy scrutinee both resolve to a Named type.
    TypeSymbol* sub = (scrutTy && scrutTy->kind == TypeRef::Kind::Named)
                          ? scrutTy->declaration
                          : nullptr;

    if (sub && sub->kind == ast::TypeDeclKind::Enum) {
        // §19.6: every declared enum constant must have a matching pattern.
        std::unordered_set<std::string_view> covered;
        for (const auto& arm : ce.arms) collectEnumPatternNames(arm, covered);
        // §10.2 (issue #1577): "exhaustiveness is over the set of distinct
        // declared values, so an arm naming either alias covers both." Map
        // covered NAMES to their underlying values so a covered alias also
        // covers its same-valued siblings, instead of requiring every
        // declared NAME to appear.
        const std::unordered_map<std::string_view, long long> values =
            enumConstantValues(*sub);
        std::unordered_set<long long> coveredValues;
        for (auto n : covered) {
            auto it = values.find(n);
            if (it != values.end()) coveredValues.insert(it->second);
        }
        std::vector<std::string_view> missing;
        std::unordered_set<long long> reportedMissingValues;
        auto considerName = [&](std::string_view name) {
            if (covered.count(name)) return;
            auto it = values.find(name);
            if (it != values.end()) {
                if (coveredValues.count(it->second)) return;  // an alias covers it
                if (!reportedMissingValues.insert(it->second).second)
                    return;  // this value already reported via an earlier alias
            }
            missing.push_back(name);
        };
        if (auto* td = dynamic_cast<const ast::TypeDecl*>(sub->decl)) {
            for (const auto& ec : td->enumConstants) considerName(ec.name);
        } else {
            // No AST decl (e.g. imported enum): fall back to symbol members.
            for (const Symbol* m : sub->members)
                if (dynamic_cast<const EnumConstantSymbol*>(m))
                    considerName(m->name);
        }
        if (!missing.empty() || nullableUncovered) {
            std::string detail = "enum `" + std::string(sub->name) + "`";
            if (!missing.empty()) {
                detail += " is missing constant(s):";
                for (auto n : missing) detail += " " + std::string(n);
            }
            if (nullableUncovered)
                detail += (missing.empty() ? " nullable value not covered"
                                           : " (and a `null` arm)");
            warn(detail);
        }
        return;
    }

    if (sub && sub->kind == ast::TypeDeclKind::Class) {
        // §11.7 / §19.6: the scrutinee's hierarchy is closed only when the
        // base is `sealed abstract`, or a same-module abstract whose every
        // subclass is recursively sealed/closed. Otherwise the set of leaf
        // types is open and exhaustiveness cannot be proven.
        bool isAbstract = hasModifier(sub, "abstract");
        bool isSealed = hasModifier(sub, "sealed");

        // Gather every transitive subclass of `sub` visible in this
        // compilation. A leaf to cover is a concrete (non-abstract)
        // subclass; abstract intermediates are matched only through their
        // concrete descendants. `descendants` keeps the full set (including
        // abstract intermediates) so a pattern naming an intermediate by
        // name still resolves (subtype matching, §19.3).
        std::vector<TypeSymbol*> leaves;
        std::vector<TypeSymbol*> descendants;
        bool sawOpenLeaf = false;  // a non-sealed subclass leaves the set open
        for (const auto& s : ctx_.symbols()) {
            auto* ts = dynamic_cast<TypeSymbol*>(s.get());
            if (!ts || ts == sub || ts->kind != ast::TypeDeclKind::Class)
                continue;
            // Is `sub` an ancestor of `ts`?
            bool isDescendant = false;
            std::vector<TypeSymbol*> work = ts->baseTypes;
            std::unordered_set<TypeSymbol*> seen;
            while (!work.empty()) {
                TypeSymbol* b = work.back();
                work.pop_back();
                if (!b || !seen.insert(b).second) continue;
                if (b == sub) { isDescendant = true; break; }
                for (TypeSymbol* bb : b->baseTypes) work.push_back(bb);
            }
            if (!isDescendant) continue;
            descendants.push_back(ts);
            if (!hasModifier(ts, "abstract"))
                leaves.push_back(ts);  // concrete leaf to be covered
            if (!hasModifier(ts, "sealed") && !hasModifier(ts, "abstract"))
                sawOpenLeaf = true;    // a non-sealed concrete subclass
        }

        // The hierarchy is provably closed when the base is `sealed
        // abstract` (cross-module guarantee, §11.7), OR a same-module
        // abstract base with no non-sealed concrete leaf (recursively
        // closed, §19.6). A non-imported plain-abstract base whose leaves
        // are all sealed still forms a closed set within the module.
        bool closed =
            (isAbstract && isSealed) ||
            (isAbstract && !ctx_.isImported(sub) && !sawOpenLeaf);
        if (!closed) {
            warn("class hierarchy rooted at `" + std::string(sub->name) +
                 "` is open");
            return;
        }

        // Collect the leaf types covered by an unguarded type-test arm.
        std::unordered_set<const TypeSymbol*> coveredLeaves;
        for (const auto& arm : ce.arms) {
            if (arm.guard) continue;
            const ast::Pattern* p = arm.pattern.get();
            TypeSymbol* matchTy = nullptr;
            // A type-test arm names the matched class either as a bare
            // NamedPattern (`Circle => …`) or as a typed binding
            // (`var c: Circle => …`). Resolve the named class by its last
            // name part against the closed hierarchy's leaf set — the
            // BindingPattern's `bp->type` annotation is NOT resolved by
            // TypeResolution inside case arms (no `resolvedType` entry), so
            // we match by name, consistent with the NamedPattern path.
            std::string_view typeName;
            if (auto* np = dynamic_cast<const ast::NamedPattern*>(p)) {
                if (np->name && !np->name->parts.empty())
                    typeName = np->name->parts.back();
            } else if (auto* bp =
                           dynamic_cast<const ast::BindingPattern*>(p)) {
                if (bp->type && bp->type->name &&
                    !bp->type->name->parts.empty())
                    typeName = bp->type->name->parts.back();
            }
            // Resolve against the full descendant set (a leaf, or an
            // abstract intermediate that subtype-covers its own leaves), or
            // against `sub` itself (`var x: Shape` covers everything).
            if (!typeName.empty()) {
                if (typeName == sub->name) matchTy = sub;
                for (auto* d : descendants)
                    if (d->name == typeName) matchTy = d;
            }
            if (matchTy) {
                // A pattern naming an ancestor also covers all its leaf
                // descendants (subtype matching, §19.3).
                for (auto* lf : leaves) {
                    if (lf == matchTy) { coveredLeaves.insert(lf); continue; }
                    std::vector<TypeSymbol*> work = lf->baseTypes;
                    std::unordered_set<TypeSymbol*> seen;
                    while (!work.empty()) {
                        TypeSymbol* b = work.back();
                        work.pop_back();
                        if (!b || !seen.insert(b).second) continue;
                        if (b == matchTy) { coveredLeaves.insert(lf); break; }
                        for (TypeSymbol* bb : b->baseTypes)
                            work.push_back(bb);
                    }
                }
            }
        }
        std::vector<std::string_view> missing;
        for (auto* lf : leaves)
            if (!coveredLeaves.count(lf)) missing.push_back(lf->name);
        if (!missing.empty() || nullableUncovered) {
            std::string detail =
                "closed hierarchy `" + std::string(sub->name) + "`";
            if (!missing.empty()) {
                detail += " is missing leaf type(s):";
                for (auto n : missing) detail += " " + std::string(n);
            }
            if (nullableUncovered)
                detail += (missing.empty() ? " nullable value not covered"
                                           : " (and a `null` arm)");
            warn(detail);
        }
        return;
    }

    // --- The scrutinee-less boolean-condition form (§19.1) has no value
    // set to enumerate and no `_` (we already returned on a catch-all), so
    // it cannot be proven exhaustive (§19.6).
    if (!ce.scrutinee) {
        warn("scrutinee-less `case` has no catch-all `_` arm");
        return;
    }

    // A scrutinee whose type failed to resolve (Error sentinel) or was not
    // inferred is suppressed here: a non-exhaustiveness warning layered on
    // top of an upstream type error would be a confusing cascade (the Error
    // kind exists precisely to suppress such follow-on diagnostics —
    // TypeRef.hpp). The real diagnostic is reported by the resolution pass.
    if (!scrutTy || scrutTy->kind == TypeRef::Kind::Error) return;

    // --- Every other scrutinee shape (int / string / range / float /
    // open class) cannot be proven exhaustive (§19.6): there is no finite,
    // statically-knowable value set to enumerate. Without a catch-all arm
    // (handled above) the case is non-exhaustive.
    warn("scrutinee type cannot be proven exhaustive");
}

// Slice S21: does a callable with this return-type ascription
// produce a value? No ascription → no (unit). An ascription that
// resolves to the unit type → no. Anything else → yes.
bool ExprTC::returnsValue(const ast::Type* rt) {
    if (!rt) return false;
    TypeRef* r = ctx_.resolvedType(*rt);
    if (r && r->kind == TypeRef::Kind::Unit) return false;
    return true;
}

// async-readiness 3b (§30.1): map an `async` method's declared return type to
// the return-FORM context and the result-assignability type used inside its
// body. The task wrapper is peeled: `Task` is a void-method context (no
// result), `Task<T>` / `OwningTask<T>` are value-method contexts whose result
// type is the element T. Anything else (no return type, a non-async type, or
// an async-iterator interface) is left as a void context — the dedicated async
// return-type diagnostic (§30.1, in TypeResolution) already names a non-async
// return type, and an async-iterator method is exempt from the value-return
// check via the `isIteratorMethod` path.
// issue #777 / §30.6: a Named `Task<…>` / `OwningTask<…>` shape — the async task
// wrapper. An async lambda/method body must produce the unwrapped element T, not
// a Task value; a Task-shaped body where T is NOT itself Task-shaped is a
// forgotten `await`.
bool ExprTC::isTaskShapedRef(const TypeRef* t) const {
    return t && t->kind == TypeRef::Kind::Named && !t->nullable &&
           t->astNode && t->astNode->name &&
           t->astNode->name->parts.size() == 1 &&
           (t->headName() == "Task" ||
            t->headName() == "OwningTask");
}

// stdlib §5 (#755): a BARE lazy GroupBy result — `IEnumerable<IGrouping<TKey,T>>`
// is modelled by sema's makeLazyEnumerable(IGrouping<…>): a Named TypeRef with NO
// astNode whose sole generic argument is the synthetic "IGrouping" element. The
// lazy IGrouping enumerator dangles once stored in a variable (the #755 [8]
// crash). A MATERIALIZED result (`.ToArray()` → `Array<IGrouping>`, astNode
// "Array") or a terminal scalar (`.Count()`/`.Any()`) is a different, working
// type and is deliberately NOT matched, so existing GroupBy chaining keeps
// compiling. The element-position IGrouping itself (a for-var binding `g`) is the
// inner element, not this outer lazy enumerable, so it is not matched either.
static bool isBareLazyGroupingType(const TypeRef* t) {
    if (!t || t->kind != TypeRef::Kind::Named || t->astNode != nullptr ||
        t->genericArgs.empty())
        return false;
    const TypeRef* elem = t->genericArgs[0];
    return elem && elem->astNode && elem->astNode->name &&
           !elem->astNode->name->parts.empty() &&
           elem->headName() == "IGrouping";
}

void ExprTC::asyncReturnCtxAndType(const ast::Type* rt, ReturnCtx& outCtx,
                                   TypeRef*& outType) {
    outCtx = ReturnCtx::VoidMethod;
    outType = nullptr;
    if (!rt) return;
    TypeRef* r = ctx_.resolvedType(*rt);
    if (!r || r->kind != TypeRef::Kind::Named || !r->astNode ||
        !r->astNode->name || r->astNode->name->parts.size() != 1) {
        return;
    }
    std::string_view head = r->headName();
    if (head == "Task" && r->genericArgs.empty()) {
        // `Task` — no result value; void-method form rules.
        return;
    }
    if ((head == "Task" || head == "OwningTask") && !r->genericArgs.empty()) {
        // `Task<T>` / `OwningTask<T>` — value-method form rules; the body
        // returns the element type T.
        outCtx = ReturnCtx::ValueMethod;
        outType = r->genericArgs.front();
    }
}

// Slice S21 (§21.7) return-statement form rules:
//  - constructor / destructor: `return expression;` is an error.
//  - value-returning method: bare `return;` is an error.
//  - unit method: `return expression;` is an error.
// The "every exit path returns" coverage rule needs flow analysis
// and is left to a definite-return slice.
void ExprTC::checkReturnForm(const ast::ReturnStmt& rs) {
    // §29.5 (#721, #747): a method that uses `yield` is an iterator and may not
    // use plain `return` AT ALL — neither `return;` nor `return expr;`. Use
    // `yield break;` to end iteration. Checked up front because the per-context
    // §21.7 rules below otherwise (a) misreport a sync iterator's `return;` as
    // "must provide a value", and (b) MISS a sync iterator's `return expr;`
    // entirely — a sync iterator returns IEnumerable<T> (a ValueMethod ctx that
    // accepts a value), so the value-carrying return slipped through to codegen
    // and emitted malformed IR (#747 R42). inIteratorMethod_ is cleared inside
    // nested lambda bodies (ExprTypecheck_Exprs.cpp), so a lambda's own `return`
    // here is unaffected.
    if (inIteratorMethod_) {
        ctx_.diagnostics().report(
            Severity::Error, rs.span.begin,
            "an iterator method cannot use `return` — use `yield break;` to "
            "end iteration (§29.5)");
        return;
    }
    switch (returnCtx_) {
        case ReturnCtx::None:
            return;
        case ReturnCtx::CtorDtor:
            if (rs.value) {
                ctx_.diagnostics().report(
                    Severity::Error, rs.span.begin,
                    "a constructor or destructor may not return a "
                    "value; use `return;` (§21.7)");
            }
            return;
        case ReturnCtx::ValueMethod:
            if (!rs.value) {
                ctx_.diagnostics().report(
                    Severity::Error, rs.span.begin,
                    "this callable has a return type — `return;` "
                    "must provide a value (§21.7)");
            }
            return;
        case ReturnCtx::VoidMethod:
            // Iterator returns are rejected up front (§29.5); here only the
            // §21.7 "no return type — `return` may not carry a value" rule
            // remains for ordinary void methods.
            if (rs.value) {
                ctx_.diagnostics().report(
                    Severity::Error, rs.span.begin,
                    "this callable has no return type — `return` "
                    "may not carry a value (§21.7)");
            }
            return;
    }
}

// Slice T1: §5.1 numeric/char implicit-widening table. Returns
// true iff `from` implicitly widens to `to` (both primitives,
// distinct). Identity is handled by the caller.
bool ExprTC::primitiveWidens(PrimitiveType from, PrimitiveType to) {
    using P = PrimitiveType;
    auto isAny = [&](std::initializer_list<P> set) {
        for (P p : set) if (p == to) return true;
        return false;
    };
    switch (from) {
        case P::SByte:
            return isAny({P::Short, P::Int, P::Long, P::Float,
                          P::Double, P::Decimal});
        case P::Byte:
            return isAny({P::Short, P::UShort, P::Int, P::UInt,
                          P::Long, P::ULong, P::Float, P::Double,
                          P::Decimal});
        case P::Short:
            return isAny({P::Int, P::Long, P::Float, P::Double,
                          P::Decimal});
        case P::UShort:
            return isAny({P::Int, P::UInt, P::Long, P::ULong,
                          P::Float, P::Double, P::Decimal});
        case P::Int:
            return isAny({P::Long, P::Float, P::Double, P::Decimal});
        case P::UInt:
            return isAny({P::Long, P::ULong, P::Float, P::Double,
                          P::Decimal});
        case P::Long:
            return isAny({P::Float, P::Double, P::Decimal});
        case P::ULong:
            return isAny({P::Float, P::Double, P::Decimal});
        case P::Float:
            return isAny({P::Double});
        case P::Char:
            return isAny({P::Int, P::UInt, P::Long, P::ULong,
                          P::Float, P::Double, P::Decimal});
        default:
            return false;
    }
}

// §12.9 (issue #1180): forward declarations for variance-check helpers.
// All are defined after builtinInterfaceAncestorsOf (the §26.5 hierarchy
// section), which they depend on.
static std::string_view namedTypeSimpleName(const TypeRef*);
static bool variantGenericAssignable(const TypeRef*, const TypeRef*,
                                      bool (*)(TypeSymbol*, TypeSymbol*));

// Slice T4: is `from` a subtype of `to` — i.e. does `to` appear in
// `from`'s transitive base-type chain (base class or implemented
// interface)? BFS, cycle-guarded.
bool ExprTC::isSubtype(TypeSymbol* from, TypeSymbol* to) {
    std::vector<TypeSymbol*> frontier{from};
    std::unordered_set<TypeSymbol*> seen{from};
    std::size_t i = 0;
    while (i < frontier.size()) {
        TypeSymbol* t = frontier[i++];
        if (t == to) return true;
        for (TypeSymbol* b : t->baseTypes) {
            if (b && seen.insert(b).second) frontier.push_back(b);
        }
    }
    return false;
}

// Slices T1/T4: assignability check (§5.1, §7, §12 subtyping).
// Handles two cases and conservatively skips everything else
// (mixed primitive/named, generics, null, Object target) so it
// never false-positives while the type lattice is incomplete:
//   - primitive → primitive: identity or §5.1 widening.
//   - named → named (non-generic): identity or subclass/interface.
// A non-nullable target accepting a nullable source is rejected.
void ExprTC::checkAssignable(TypeRef* from, TypeRef* to,
                             SourceLocation loc, const std::string& what,
                             const ast::Expr* fromExpr) {
    if (!from || !to) return;
    // §7.4 / §12.7 (#914): a value-type (primitive) nullable simple variable
    // proved non-null by null-flow narrowing (`if x is null then return; end`,
    // `is not null`, else-of-`is null`, or initialization with a non-null value)
    // is non-nullable in value position — so it is assignable to / returnable as
    // a non-nullable target. Treat the narrowed `from` as non-nullable for this
    // check WITHOUT rewriting its exprType (the declared {flag,value} layout is
    // preserved, so null-comparisons / `is` tests on the variable are unaffected
    // — that is the soundness boundary). Codegen unwraps the tagged-optional slot
    // at the non-nullable-target coercion sites (local-init / assignment-store /
    // return). Restricted to a VALUE-TYPE nullable identifier in nullNarrowed_:
    // a primitive, an ENUM, or a RECORD — all laid out as the {flag, value}
    // tagged-optional struct that `unwrapNarrowedNullable` extracts — OR a
    // class / interface reference (#1056), whose nullable form is just a bare
    // pointer: `unwrapNarrowedNullable` is a no-op for it (no struct), and a
    // narrowed (non-null) pointer assigns directly to a non-nullable reference
    // slot. (For a class reference the assignment still needs `move` — that
    // ownership rule is orthogonal and enforced separately.)
    if (fromExpr && from->nullable && !to->nullable &&
        (from->kind == TypeRef::Kind::Primitive ||
         (from->kind == TypeRef::Kind::Named && from->declaration))) {
        // Peel a `move x` wrapper (#1056): a class/interface reference assignment
        // transfers ownership via `move`, so `fromExpr` is a UnaryExpr whose
        // operand is the narrowed identifier (value-type copies have the bare
        // identifier directly).
        const ast::Expr* narrowExpr = fromExpr;
        if (auto* ue = dynamic_cast<const ast::UnaryExpr*>(fromExpr))
            if (ue->op == "move" && ue->operand)
                narrowExpr = ue->operand.get();
        if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(narrowExpr)) {
            if (Symbol* s = ctx_.exprSymbol(*id))
                if (nullNarrowed_.count(s)) {
                    if (from->kind == TypeRef::Kind::Primitive) {
                        from = ctx_.primitiveTypeRef(from->primitive,
                                                     /*nullable=*/false);
                    } else {
                        TypeRef* nn = ctx_.makeTypeRef();
                        *nn = *from;
                        nn->nullable = false;
                        from = nn;
                    }
                }
        }
    }
    // `Object` (the root) accepts anything — never flag.
    if (to->kind == TypeRef::Kind::Primitive &&
        to->primitive == PrimitiveType::Object) {
        return;
    }
    auto reportNullable = [&] {
        ctx_.diagnostics().report(
            Severity::Error, loc,
            std::string("the ") + what +
            " is nullable but the target type is non-nullable (§7)");
    };
    auto reportNotConvertible = [&] {
        ctx_.diagnostics().report(
            Severity::Error, loc,
            std::string("the ") + what + " has a type that is not "
            "implicitly convertible to the target type (§5.1)");
    };
    // §5.4 / §10 (#924): an enum is NOT implicitly convertible to or from its
    // underlying integer type — both directions require an explicit cast
    // (`int(e)` / `E(n)`). The mixed Named(enum)↔Primitive case falls through
    // both the Primitive-Primitive and Named-Named branches below, so without
    // this gate `var n: int := e` and `var e: E := n` were silently accepted
    // and miscompiled. Enum→Object boxing stays accepted via the `Object`
    // early-return above; explicit casts go through the cast-expression path.
    {
        bool fromEnum = from->kind == TypeRef::Kind::Named &&
                        from->declaration &&
                        from->declaration->kind == ast::TypeDeclKind::Enum;
        bool toEnum = to->kind == TypeRef::Kind::Named && to->declaration &&
                      to->declaration->kind == ast::TypeDeclKind::Enum;
        // `Object` is the root type, not the enum's underlying integer type:
        // enum→Object is boxing (an upcast) and Object→enum is unbox/downcast,
        // both handled elsewhere. Only the underlying-integer (and other scalar)
        // primitives are the conversion §5.4/§10 forbids without an explicit
        // cast, so exclude Object here. (A `case` arm unifying enum constants
        // with a `null` arm yields `Object?`, which must still reach an enum?
        // target — see conformance spec_0025.)
        bool fromIntPrim = from->kind == TypeRef::Kind::Primitive &&
                           from->primitive != PrimitiveType::Object;
        bool toIntPrim = to->kind == TypeRef::Kind::Primitive &&
                         to->primitive != PrimitiveType::Object;
        if ((fromEnum && toIntPrim) || (toEnum && fromIntPrim)) {
            reportNotConvertible();
            return;
        }
    }
    if (from->kind == TypeRef::Kind::Primitive &&
        to->kind == TypeRef::Kind::Primitive) {
        // The `null` literal is typed `Object?`. A non-nullable Object source is
        // implicitly convertible here (an unbox), and a nullable one is fine when
        // the TARGET is nullable (`var x: int? := null`); but a nullable Object
        // source into a NON-nullable target is not — without this gate
        // `var x: int := null` slipped past the early-return before the nullable
        // check below and emitted malformed IR (§5.1 / §7).
        if (from->primitive == PrimitiveType::Object &&
            (!from->nullable || to->nullable))
            return;
        if (from->nullable && !to->nullable) { reportNullable(); return; }
        if (from->primitive == to->primitive) return;
        if (primitiveWidens(from->primitive, to->primitive)) return;
        reportNotConvertible();
        return;
    }
    if (from->kind == TypeRef::Kind::Named &&
        to->kind == TypeRef::Kind::Named &&
        from->declaration && to->declaration) {
        // §12.9 (issue #1180): for SAME-declaration generics, check variance.
        // Cross-declaration generics (from->declaration != to->declaration)
        // fall through to the isSubtype check below (conservative — no per-arg
        // check; full cross-type user-declared variance is a residual).
        if (!from->genericArgs.empty() || !to->genericArgs.empty()) {
            if (from->declaration == to->declaration &&
                !from->genericArgs.empty() &&
                from->genericArgs.size() == to->genericArgs.size()) {
                // Same-declaration: enforce declared variance per §12.9.
                if (!variantGenericAssignable(from, to, isSubtype)) {
                    reportNotConvertible();
                    return;
                }
                if (from->nullable && !to->nullable) { reportNullable(); return; }
                return;
            }
            // Cross-declaration or arity mismatch: conservative skip.
            return;
        }
        if (from->nullable && !to->nullable) { reportNullable(); return; }
        if (from->declaration == to->declaration) return;
        if (isSubtype(from->declaration, to->declaration)) return;
        reportNotConvertible();
        return;
    }
    // §4.16 / §8.6 (#970): a span type (ArraySpan<T>/ReadOnlyArraySpan<T>/
    // Span<T>/ReadOnlySpan<T>) is implicitly coercible to IReadOnlySpan<T>
    // for call-bounded parameter passing. The span is boxed into a heap-managed
    // {vtable*, rc, span_bytes} object at the call site; the callee receives a
    // pointer and dispatches through the IReadOnlySpan itable.
    if (from->kind == TypeRef::Kind::Named && !from->declaration &&
        to->kind   == TypeRef::Kind::Named &&  to->declaration &&
        to->declaration->kind == ast::TypeDeclKind::Interface &&
        to->declaration->name == "IReadOnlySpan") {
        std::string_view fromN = namedTypeSimpleName(from);
        if (fromN == "ArraySpan" || fromN == "ReadOnlyArraySpan" ||
            fromN == "Span"      || fromN == "ReadOnlySpan") {
            if (from->nullable && !to->nullable) { reportNullable(); return; }
            return;  // accepted — codegen boxes the span at the call site
        }
    }
    // §4.14/§4.15 (#1245): a ReadOnly span is NOT assignable to its writable
    // counterpart — ReadOnlyArraySpan<T>→ArraySpan<T> would allow writes
    // through a read-only borrow (§4.15), and ReadOnlySpan<T>→Span<T> would
    // allow disposal/mutation through a read-only owning view (§4.14).
    // Both families are null-declaration Named builtins; namedTypeSimpleName
    // recovers the name from either an annotation-resolved ref (astNode set by
    // TypeResolution) or a synthesised ref (astNode built by ExprTC_Calls for
    // AsSpan / AsReadOnlySpan return types).
    if (from->kind == TypeRef::Kind::Named && !from->declaration &&
        to->kind   == TypeRef::Kind::Named && !to->declaration) {
        std::string_view fromN = namedTypeSimpleName(from);
        std::string_view toN   = namedTypeSimpleName(to);
        if ((fromN == "ReadOnlyArraySpan" && toN == "ArraySpan") ||
            (fromN == "ReadOnlySpan"      && toN == "Span")) {
            reportNotConvertible();
            return;
        }
    }
    // §12.9 (issue #1180): variance check for builtin (null-declaration) Named
    // generic types. The Named-Named branch above requires both sides to have
    // non-null declarations; builtins like Array<T>, IEnumerable<T>, IList<T>
    // have null declarations and fall through to here. When `to` is a builtin
    // generic interface, verify that `from`'s matching ancestor's type args
    // satisfy `to`'s declared variance (§12.9 / Appendix C invariant).
    if (from->kind == TypeRef::Kind::Named &&
        to->kind == TypeRef::Kind::Named &&
        !to->genericArgs.empty() && !to->declaration) {
        if (!variantGenericAssignable(from, to, isSubtype)) {
            reportNotConvertible();
            return;
        }
    }
    // §27.6 (#756): a target-typed PROPERTY REFERENCE. `var f: () -> T := b.P`
    // where P is a property whose getter returns T — a property read is a value
    // by default, but when the target is a zero-arg function returning the
    // property's type, re-type the source expression to that function type so it
    // is the getter-as-function value (≡ `() => b.P`) and codegen emits a
    // property thunk. (Methods are already unconditionally function-typed; a
    // property read needs this target-directed promotion.)
    if (to->kind == TypeRef::Kind::Function &&
        from->kind != TypeRef::Kind::Function && fromExpr &&
        to->funcParams.empty() && to->funcReturn) {
        if (auto* ps = dynamic_cast<PropertySymbol*>(ctx_.exprSymbol(*fromExpr))) {
            auto sameRetTy = [](const TypeRef* a, const TypeRef* b) {
                if (!a || !b || a->kind != b->kind || a->nullable != b->nullable)
                    return false;
                if (a->kind == TypeRef::Kind::Primitive)
                    return a->primitive == b->primitive;
                if (a->kind == TypeRef::Kind::Named)
                    return a->declaration == b->declaration;
                return false;
            };
            if (!ps->isIndexer && sameRetTy(to->funcReturn, from)) {
                ctx_.setExprType(const_cast<ast::Expr*>(fromExpr), to);
                return;  // accepted as a property getter reference
            }
        }
    }
    // §27.6 / §5.1 (issue #436): a bare method reference has a Function type.
    // A Function value is not implicitly convertible to a non-Function type
    // (Primitive or Named). Report a type mismatch so the user sees a clean
    // §5.1 error instead of producing malformed IR.  Function-to-Function
    // compatibility (e.g. `var f: () -> int := obj.Method`) is left to a
    // future function-type signature-matching slice; for now it passes silently
    // (the existing codegen path handles it correctly).
    if (from->kind == TypeRef::Kind::Function &&
        to->kind != TypeRef::Kind::Function) {
        reportNotConvertible();
        return;
    }
    // §27.2 / §28.14 (v0.175): `scoped` participates in function-signature
    // IDENTITY like ref/out — `(scoped () -> Task) -> ()` and
    // `(() -> Task) -> ()` are distinct types with no conversion either way.
    // Deliberately NARROW: general function-to-function signature matching is
    // still the deferred slice above, so this fires ONLY when the two sides
    // disagree on a `scoped` position (same arity), leaving every previously-
    // accepted function assignment unchanged.
    if (from->kind == TypeRef::Kind::Function &&
        to->kind == TypeRef::Kind::Function &&
        from->funcParams.size() == to->funcParams.size()) {
        for (std::size_t i = 0; i < from->funcParams.size(); ++i) {
            const bool fromScoped = from->funcParams[i].modifier == "scoped";
            const bool toScoped = to->funcParams[i].modifier == "scoped";
            if (fromScoped != toScoped) {
                ctx_.diagnostics().report(
                    Severity::Error, loc,
                    std::string("the ") + what +
                    " has a function type whose parameter " +
                    std::to_string(i + 1) +
                    (fromScoped ? " is `scoped` but the target's is not"
                                : " is not `scoped` but the target's is") +
                    " — `scoped` is part of a function signature's identity; "
                    "there is no conversion between the two (§27.2, §28.14 "
                    "v0.175)");
                return;
            }
        }
    }
    // A nullable source assigned to a non-nullable target whose shapes did not
    // match either branch above — notably the `null` literal (typed `Object?`, a
    // Primitive) assigned to a non-nullable NAMED reference target
    // (`var x: Box := null`). The Named-Named branch requires both sides Named,
    // so this fell through unhandled and a null stored into a non-nullable
    // reference faulted on first use. Reject it (§7 / §5.1). The Primitive→
    // Primitive and Named→Named nullable cases are already handled above; the
    // generic-deferral path returned before reaching here, so generics are
    // unaffected.
    if (from->nullable && !to->nullable) { reportNullable(); return; }
    // §5.1 / §4.6 (#1178): a mixed pair of a CONCRETE user-declared class /
    // record / interface and a CONCRETE primitive (incl. `string`, which is a
    // Primitive in this type model) matches neither the Primitive-Primitive nor
    // the Named-Named branch above, so it reached the fall-through and was
    // silently accepted (`var s: string := d`, `var d: Dog := 5`,
    // `var d: Dog := "hi"`). There is no implicit conversion between a class
    // reference and a primitive (§4.6 forbids implicit to-string), so reject it.
    // Stay surgical: only fire when the Named side is a CONCRETE user type
    // (declaration != null — a generic type-parameter and a builtin both have a
    // null declaration and must keep flowing through; the monomorphizer / later
    // slices handle those) and the Primitive side is NOT `Object` (the `null`
    // literal is typed `Object?`, and Object boxing/unboxing is handled
    // elsewhere). Enum↔primitive is already rejected by the §5.4/§10 gate above.
    {
        bool fromConcreteClass = from->kind == TypeRef::Kind::Named &&
                                 from->declaration &&
                                 from->declaration->kind !=
                                     ast::TypeDeclKind::Enum;
        bool toConcreteClass = to->kind == TypeRef::Kind::Named &&
                               to->declaration &&
                               to->declaration->kind != ast::TypeDeclKind::Enum;
        bool fromConcretePrim = from->kind == TypeRef::Kind::Primitive &&
                                from->primitive != PrimitiveType::Object;
        bool toConcretePrim = to->kind == TypeRef::Kind::Primitive &&
                              to->primitive != PrimitiveType::Object;
        if ((fromConcreteClass && toConcretePrim) ||
            (toConcreteClass && fromConcretePrim)) {
            reportNotConvertible();
            return;
        }
    }
    // Mixed / generic-parameter / null-literal cases: skip.
}

// Slice S18: a condition must be `bool` (§21.2 if, §21.3 while,
// §20 if-expression). Only flags when the condition has a resolved
// primitive type that is not bool — conditions whose type isn't yet
// inferred (unresolved identifiers, un-typed calls) are left alone
// so this pass never produces false positives on incomplete typing.
void ExprTC::checkConditionIsBool(const ast::Expr& cond, std::string_view ctx) {
    TypeRef* t = ctx_.exprType(cond);
    if (!t) return;
    // A `bool?` is NOT a valid condition: §21.2 requires the condition to be
    // `bool`, and §7.11 (no three-valued logic) makes a nullable bool in a
    // boolean position a compile error. Without this guard a `bool?` condition
    // (kind=Primitive, primitive=Bool, nullable=true) slipped past the bool
    // check below and reached codegen, which branched on the whole `{i8,i1}`
    // nullable struct and emitted IR the verifier rejected (R39).
    if (t->kind == TypeRef::Kind::Primitive &&
        t->primitive == PrimitiveType::Bool && t->nullable) {
        ctx_.diagnostics().report(
            Severity::Error, cond.span.begin,
            std::string("the ") + std::string(ctx) +
            " condition must be `bool`, not `bool?` — a nullable bool cannot be "
            "used in a boolean position (§21.2, §7.11); test it explicitly "
            "(e.g. `x = true`) or coalesce it (`x ?? false`)");
        return;
    }
    if (t->kind != TypeRef::Kind::Primitive) return;
    if (t->primitive == PrimitiveType::Bool) return;
    ctx_.diagnostics().report(
        Severity::Error, cond.span.begin,
        std::string("the ") + std::string(ctx) +
        " condition must be `bool` (§21.2)");
}

// §20: type the `if` expression with the COMMON type of its branches.
// The common type is the one branch's type that the other widens (§5.1)
// or subtypes (§12) into; a bottom-typed (`throw`/diverging) branch — its
// type left null — imposes no constraint and the result is the other
// branch's type. When both branches are typed but neither converts to the
// other, no common type exists and a clean §20 error is reported. The
// reporting is conservative (mirroring checkAssignable) so the pass never
// false-positives while the type lattice is incomplete.
//
// This is also what guards the `throw E if c else x` shape — which parses
// as `throw (E if c else x)` per the grammar (ThrowExpr's operand is a
// full Expression; §17.12, Appendix A) — where the conditional's arms are
// an exception value (Named) and a plain value (e.g. a primitive). They
// have no common type, so codegen never reaches the ill-typed PHI that
// crashed it; sema rejects the program first.
// §20: the pairwise common type of two typed branches/arms — the one the
// other widens (§5.1) or subtypes (§12) into. Returns null when neither
// converts to the other (no common type exists). Both inputs must be
// non-null (callers handle the bottom-typed `null` case before getting here:
// a bottom-typed arm imposes no constraint). An Error-kind input yields the
// other operand so a prior diagnostic doesn't cascade. `Object` is the
// universal common supertype. Conservative (mirrors checkAssignable /
// convertsInto) so it never false-positives while the type lattice is
// incomplete.
TypeRef* ExprTC::commonTypeOf(TypeRef* a, TypeRef* b) {
    if (!a) return b;
    if (!b) return a;
    if (a->kind == TypeRef::Kind::Error) return b;
    if (b->kind == TypeRef::Kind::Error) return a;
    auto isObject = [](TypeRef* t) {
        return t->kind == TypeRef::Kind::Primitive &&
               t->primitive == PrimitiveType::Object;
    };
    if (isObject(a)) return a;
    if (isObject(b)) return b;
    // `from` widens/subtypes into `to` — so `to` is a candidate common type.
    auto convertsInto = [&](TypeRef* from, TypeRef* to) -> bool {
        // §7 / §20 (#747 R42): nullability is part of the conversion. A
        // non-nullable `T` widens into `T?`, but a nullable `T?` does NOT
        // implicitly convert into non-nullable `T` (that would silently drop the
        // null possibility). Without this guard `commonTypeOf(int, int?)`
        // returned the NON-nullable `int` (because `convertsInto(int?, int)`
        // ignored the flag), so a `case` whose untyped `var n` arm binds the
        // whole `int?` value (§19.5) was mis-typed `int`; codegen then built a
        // PHI mixing the `{i8,T}` nullable struct with a bare `T` and aborted
        // the compiler. The correct common type is `int?`.
        if (from->nullable && !to->nullable) return false;
        if (from->kind != to->kind) return false;
        if (from->kind == TypeRef::Kind::Primitive) {
            if (from->primitive == to->primitive) return true;
            return primitiveWidens(from->primitive, to->primitive);
        }
        if (from->kind == TypeRef::Kind::Named &&
            from->declaration && to->declaration) {
            // Defer generics to the variance slice (conservative skip).
            if (!from->genericArgs.empty() || !to->genericArgs.empty())
                return true;
            if (from->declaration == to->declaration) return true;
            return isSubtype(from->declaration, to->declaration);
        }
        // Other kinds (InlineArray/Function/Unit): conservatively treat
        // same-kind as compatible to avoid false positives.
        return true;
    };
    // The common type is whichever operand the other converts into. Prefer
    // `a` on a tie (equal types / mutual conversion).
    if (convertsInto(b, a)) return a;
    if (convertsInto(a, b)) return b;
    return nullptr;  // no common type
}

// §31.2.1: nearest common base (NCB) of the types listed in a multi-type
// catch-arm binding (`var e: T1, T2, ...`). The binding's static type is the
// most specific type that is a base of ALL listed types.
//
// Two-step approach, per §31.2.1:
//
// Step 1 — commonTypeOf fold: handles user-defined hierarchies where all
//   listed types have TypeSymbol base chains. `commonTypeOf` walks the
//   TypeSymbol base-types list (§12 inheritance) and returns the common type
//   if one branch is a subtype of the other.
//
// Step 2 — AST base-type walk: handles the common case where the shared base
//   is a built-in (e.g. `Exception`). Built-in exception types have no user
//   TypeSymbol (their TypeRef carries `declaration == nullptr`), so they are
//   absent from TypeSymbol::baseTypes even when declared as base types in the
//   user source. We therefore also walk each type's AST TypeDecl::baseTypes
//   list (which records ALL declared bases, including built-ins) and collect
//   base-type names. The NCB is the first name (BFS from the first type's
//   ancestor chain) that appears in ALL listed types' ancestor name-sets.
TypeRef* ExprTC::catchArmNcb(
    const std::vector<std::unique_ptr<ast::Type>>& types) {
    if (types.empty() || !types[0]) return nullptr;
    if (types.size() == 1) return ctx_.resolvedType(*types[0]);

    // --- Step 1: fold commonTypeOf ----------------------------------------
    TypeRef* running = ctx_.resolvedType(*types[0]);
    for (std::size_t i = 1; i < types.size(); ++i) {
        TypeRef* ti = types[i] ? ctx_.resolvedType(*types[i]) : nullptr;
        if (!running || !ti) { running = nullptr; break; }
        running = commonTypeOf(running, ti);
    }
    if (running) return running;

    // --- Step 2: AST base-type walk (handles built-in common bases) --------
    //
    // Helper: extract the simple name of a Named TypeRef, for comparing
    // built-in exception TypeRefs that carry no TypeSymbol declaration.
    auto namedRefName = [](const TypeRef* ref) -> std::string_view {
        if (!ref || ref->kind != TypeRef::Kind::Named) return {};
        if (ref->declaration) return ref->declaration->name;
        if (!ref->importedName.empty()) return ref->importedName;
        if (ref->astNode && ref->astNode->name &&
                !ref->astNode->name->parts.empty())
            return ref->astNode->name->parts.back();
        return {};
    };

    // Helper: collect all ancestor TypeRefs of `ty` in BFS order, walking
    // both the TypeSymbol base-type chain AND the AST TypeDecl's base-type
    // list (which captures built-in bases missing from the TypeSymbol chain).
    // Stops at built-in TypeRefs (declaration==nullptr), which have no AST
    // TypeDecl to walk further.
    auto collectAncestors =
        [&](TypeRef* ty) -> std::vector<std::pair<std::string_view, TypeRef*>> {
        std::vector<std::pair<std::string_view, TypeRef*>> result;
        std::unordered_set<std::string_view> seen;
        std::vector<TypeRef*> queue{ty};
        while (!queue.empty()) {
            TypeRef* cur = queue.back();
            queue.pop_back();
            if (!cur || cur->kind != TypeRef::Kind::Named) continue;
            std::string_view curName = namedRefName(cur);
            if (!curName.empty() && seen.insert(curName).second)
                result.emplace_back(curName, cur);
            if (cur->declaration) {
                // Visit TypeSymbol base-type refs (user-declared bases with
                // TypeSymbols — these are already resolved and transitively walk).
                for (TypeRef* b : cur->declaration->baseTypeRefs)
                    queue.push_back(b);
                // Visit AST-declared bases (includes built-in bases that have no
                // TypeSymbol and thus are absent from baseTypeRefs).
                const auto* astDecl =
                    dynamic_cast<const ast::TypeDecl*>(cur->declaration->decl);
                if (astDecl) {
                    for (const auto& baseAst : astDecl->baseTypes) {
                        if (baseAst) queue.push_back(ctx_.resolvedType(*baseAst));
                    }
                }
            }
        }
        return result;
    };

    // Helper: does `ty` have an ancestor with the given name?
    auto hasAncestorNamed =
        [&](TypeRef* ty, std::string_view ancestorName) -> bool {
        // Direct name match (the type IS the ancestor).
        if (namedRefName(ty) == ancestorName) return true;
        // BFS ancestor search.
        auto ancestors = collectAncestors(ty);
        for (const auto& [name, _] : ancestors)
            if (name == ancestorName) return true;
        return false;
    };

    // Compute the ancestor list of the first type. For each ancestor (in
    // BFS order = most-specific first), check whether ALL other listed types
    // also have that ancestor. The first match is the NCB.
    TypeRef* first = ctx_.resolvedType(*types[0]);
    auto firstAncestors = collectAncestors(first);
    for (const auto& [name, candidateRef] : firstAncestors) {
        if (name.empty()) continue;
        bool allMatch = true;
        for (std::size_t i = 1; i < types.size(); ++i) {
            TypeRef* ti = types[i] ? ctx_.resolvedType(*types[i]) : nullptr;
            if (!hasAncestorNamed(ti, name)) { allMatch = false; break; }
        }
        if (allMatch) return candidateRef;
    }

    return nullptr;  // No common base found.
}

void ExprTC::typeConditional(const ast::ConditionalExpr& cnd,
                             const ast::Expr& e) {
    TypeRef* tt = cnd.thenValue ? ctx_.exprType(*cnd.thenValue) : nullptr;
    TypeRef* et = cnd.elseValue ? ctx_.exprType(*cnd.elseValue) : nullptr;
    // §20 / §7.4 (#1089): a bare `null` branch does not impose a type — it makes
    // the RESULT nullable around the OTHER branch's type. `T if c else null`
    // (and its mirror) is `T?`. A `null` literal carries no type of its own, so
    // without this it fell to the bottom-typed-branch rule below and the result
    // was typed as the non-null branch's type `T` (non-nullable). An inferred
    // `var x := T if c else null` was then non-nullable, so x got null-narrowed
    // (§7.4) and `x ?? d` lowered to a bare value-read (ignoring the null tag) —
    // returning T's zero on the null path instead of `d`.
    bool thenNull = cnd.thenValue &&
        dynamic_cast<const ast::NullLiteralExpr*>(cnd.thenValue.get());
    bool elseNull = cnd.elseValue &&
        dynamic_cast<const ast::NullLiteralExpr*>(cnd.elseValue.get());
    auto nullableOf = [&](TypeRef* ty) -> TypeRef* {
        if (!ty || ty->nullable) return ty;
        TypeRef* nn = ctx_.makeTypeRef(*ty);
        nn->nullable = true;
        return nn;
    };
    if (elseNull && !thenNull && tt) {
        ctx_.setExprType(&e, nullableOf(tt));
        return;
    }
    if (thenNull && !elseNull && et) {
        ctx_.setExprType(&e, nullableOf(et));
        return;
    }
    // A bottom-typed branch leaves its type null: the result is the other
    // branch's type, with no compatibility constraint. This is the valid
    // `x if c else throw E` idiom (and its mirror).
    if (!tt || !et) {
        if (tt) ctx_.setExprType(&e, tt);
        else if (et) ctx_.setExprType(&e, et);
        return;
    }
    // Both branches typed: take the §20 common type (the one the other
    // widens/subtypes into). commonTypeOf handles Error-kind and Object.
    if (TypeRef* common = commonTypeOf(tt, et)) {
        ctx_.setExprType(&e, common);
        return;
    }
    // No common type: report and adopt the then-branch type so member
    // access on the (already-erroneous) result still resolves downstream.
    ctx_.setExprType(&e, tt);
    ctx_.diagnostics().report(
        Severity::Error, cnd.span.begin,
        "the two branches of an `if` expression have no common type: "
        "neither branch's type is implicitly convertible to the other "
        "(§20)");
}

// §19 + §20: type a `case` EXPRESSION with the COMMON type of its
// (non-diverging) arm bodies. A diverging arm — a `throw` (§17.12) or
// other bottom-typed body — leaves its body's exprType null and imposes
// no type constraint, exactly like the bottom-typed branch of an `if`
// expression. The result is the running common type of the value-producing
// arms (each folded through commonTypeOf, identical to typeConditional's
// pairwise rule). When two value arms have no common type, a clean §20
// error is reported here rather than letting codegen build an ill-typed
// result PHI and crash. When EVERY arm diverges, the case yields no value
// and no type is recorded.
void ExprTC::typeCase(const ast::CaseExpr& cse, const ast::Expr& e) {
    TypeRef* common = nullptr;     // running common type of value arms
    bool haveValueArm = false;     // at least one non-diverging arm seen
    bool reported = false;         // emit the §20 error at most once
    for (const auto& arm : cse.arms) {
        // Only expression-bodied arms produce a value (statement / `do…end`
        // arms are statement-position only, §19.2). A bottom-typed arm body
        // (e.g. `throw E`) has a null exprType and imposes no constraint.
        if (!arm.exprBody) continue;
        TypeRef* at = ctx_.exprType(*arm.exprBody);
        if (!at) continue;  // diverging (bottom) arm — no constraint
        if (!haveValueArm) {
            common = at;
            haveValueArm = true;
            continue;
        }
        TypeRef* merged = commonTypeOf(common, at);
        if (!merged) {
            // No common type between this arm and the arms so far. Report
            // once and keep the running type so later arms still merge.
            if (!reported) {
                ctx_.diagnostics().report(
                    Severity::Error, cse.span.begin,
                    "the arms of a `case` expression have no common type: "
                    "an arm's type is not implicitly convertible to that of "
                    "the other arms (§20)");
                reported = true;
            }
            continue;  // leave `common` unchanged
        }
        common = merged;
    }
    // Record the common type (when there was a value-producing arm) so
    // member access on the case result (`(case … end).M`) resolves. When
    // every arm diverges, no type is recorded — the case produces no value.
    if (haveValueArm && common) ctx_.setExprType(&e, common);
}

// Slice S7: diagnose assignment to a readonly field outside the
// declaring class's constructor. Tracks only the `this.<field>`
// target shape; `other.f := X` would need cross-object readonly
// tracking that we don't have yet (and rarely matters in practice).
void ExprTC::checkReadonlyAssignment(const ast::AssignmentStmt& as) {
    if (!as.target) return;
    // §10 (#747 R42): a `const` member is a compile-time constant and is never
    // assignable — in any method OR constructor, through any receiver
    // (`this.K`, `c.K`, `Type.K`, or a bare `K`). Resolve the target's member
    // symbol directly so every receiver shape is covered. Without this the
    // assignment was silently accepted and dropped at runtime.
    if (dynamic_cast<const ConstMemberSymbol*>(ctx_.exprSymbol(*as.target))) {
        std::string nm;
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(
                as.target.get()))
            nm = std::string(ma->member);
        else if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                     as.target.get()))
            nm = std::string(id->name);
        ctx_.diagnostics().report(
            Severity::Error, as.span.begin,
            "cannot assign to const member `" + nm +
            "` — a const is a compile-time constant");
        return;
    }
    if (inConstructor_) return;
    // §22.8 (#747 R42): a readonly field may be assigned only in a field
    // initializer or a constructor body; assignment anywhere else is a compile
    // error. Resolve the target's field symbol directly so EVERY receiver shape
    // is covered — `this.f`, `c.f`, `Type.f`, and a bare `f`. The previous check
    // matched only `this.f`, so an instance- or type-qualified readonly write
    // (e.g. `Config.MaxSize := 50` in a normal method) slipped through and was
    // silently dropped at runtime.
    auto* fs = dynamic_cast<FieldSymbol*>(ctx_.exprSymbol(*as.target));
    if (!fs) return;
    // Check the readonly modifier on the field's MemberSymbol.
    for (auto mod : fs->modifiers) {
        if (mod == "readonly") {
            ctx_.diagnostics().report(
                Severity::Error, as.span.begin,
                std::string("cannot assign to readonly field `") +
                std::string(fs->name) +
                "` outside the declaring class's constructor "
                "(§22.8)");
            return;
        }
    }
}

// Slice S5: member lookup that walks the base-type chain.
// BFS over the inheritance graph (no cycle detection — the spec
// forbids cycles; sema's class-graph validation slice will catch
// them). First match wins; ambiguity resolution between sibling
// interfaces is a separate slice.
Symbol* ExprTC::lookupMember(TypeSymbol* type, std::string_view name) {
    std::vector<TypeSymbol*> frontier{type};
    std::size_t i = 0;
    while (i < frontier.size()) {
        TypeSymbol* t = frontier[i++];
        for (Symbol* m : t->members) {
            if (m && m->name == name) return m;
        }
        for (TypeSymbol* b : t->baseTypes) {
            if (b) frontier.push_back(b);
        }
    }
    return nullptr;
}

// §26.5 (#714): extract the simple name from a Named TypeRef — works for
// both user-declared types (via declaration->name) and null-declaration
// builtin types (via astNode->name->parts or importedName).
static std::string_view namedTypeSimpleName(const TypeRef* ref) {
    if (!ref || ref->kind != TypeRef::Kind::Named) return {};
    if (ref->declaration) return ref->declaration->name;
    if (!ref->importedName.empty()) return ref->importedName;
    if (ref->astNode && ref->astNode->name &&
        !ref->astNode->name->parts.empty())
        return ref->astNode->name->parts.back();
    return {};
}

// §26.5 (#714): does an extension declared on `target` apply to a receiver
// of type `recv`? This slice matches EXACTLY (the receiver's own compile-time
// type), ignoring nullability on both sides (§26.2 forbids `extension int?`,
// so an extension on `int` applies to an `int`/`int?` receiver alike).
// Base-class, interface, and generic-extension matching (§26.5 step 2/3) is
// handled by extensionTargetMatchesReceiverOrBase; this function handles the
// EXACT match, including for null-declaration builtin Named types (comparing
// by name rather than declaration pointer so `extension IEnumerable<T>` can
// match a synthesised `IEnumerable<int>` ancestor without a TypeDecl).
static bool extensionTargetMatchesReceiver(
    const TypeRef* target, const TypeRef* recv,
    const std::unordered_set<std::string_view>& freeVars) {
    if (!target || !recv) return false;
    if (target->kind != recv->kind) return false;
    switch (target->kind) {
        case TypeRef::Kind::Primitive:
            return target->primitive == recv->primitive;
        case TypeRef::Kind::Named: {
            // Two user-declared types: compare by declaration pointer (the
            // existing path; generic-arg matching with freeVars below).
            // One or both are null-declaration builtins: compare by name.
            // A null-declaration + non-null-declaration pair is never equal
            // (a builtin cannot match a user type and vice versa).
            bool targetIsBuiltin = (target->declaration == nullptr);
            bool recvIsBuiltin   = (recv->declaration   == nullptr);
            if (targetIsBuiltin != recvIsBuiltin)
                return false;   // builtin vs user-declared → never match
            if (!targetIsBuiltin) {
                // Both user-declared: pointer equality.
                if (target->declaration != recv->declaration) return false;
            } else {
                // Both builtins (null declaration): compare by simple name.
                std::string_view tname = namedTypeSimpleName(target);
                std::string_view rname = namedTypeSimpleName(recv);
                if (tname.empty() || rname.empty() || tname != rname)
                    return false;
            }
            // §26.5 (#773): generic-extension matching. The declarations agree
            // (same generic type); now match each type argument. A target arg
            // that is one of the extension's FREE TYPE VARIABLES (the `T` in
            // `extension Bag<T> where T: class`, resolved to a Named ref with no
            // declaration whose name is in `freeVars`) unifies with the
            // receiver's corresponding arg — so `extension Bag<T>` applies to
            // any `Bag<X>`. A CONCRETE target arg must match the receiver's arg
            // exactly (recursively), so `extension Box<int>` is never applied to
            // a `Box<string>` (the former-deferral safety is preserved).
            if (target->genericArgs.size() != recv->genericArgs.size())
                return false;
            for (std::size_t i = 0; i < target->genericArgs.size(); ++i) {
                const TypeRef* ta = target->genericArgs[i];
                const TypeRef* ra = recv->genericArgs[i];
                if (!ta || !ra) return false;
                bool taFree =
                    ta->kind == TypeRef::Kind::Named &&
                    ta->declaration == nullptr && ta->astNode &&
                    ta->astNode->name && !ta->astNode->name->parts.empty() &&
                    freeVars.count(ta->astNode->name->parts.back());
                if (taFree) continue;  // free var unifies with the receiver arg
                if (!extensionTargetMatchesReceiver(ta, ra, freeVars))
                    return false;  // concrete arg must match exactly
            }
            return true;
        }
        default:
            return false;  // function/array/unit types are not extensible
    }
}

// §26.5 (#714): for a null-declaration builtin receiver type (e.g. `Array<int>`
// or `OwningArray<int>`), return its transitive interface ancestors as synthesised
// null-declaration TypeRefs that carry the same generic args as the receiver.
// This lets extensionTargetMatchesReceiverOrBase BFS the conceptual interface
// hierarchy without requiring real TypeDecl nodes.
//
// The hierarchy (per stdlib §4.4/§4.5):
//   Array<T>      : IList<T>
//   OwningArray<T> : IOwningList<T>
//   Stack<T>      : IReadOnlyCollection<T> (conservative; full chain when needed)
//   IList<T>      : IReadOnlyList<T>, ICollection<T>
//   IOwningList<T> : IReadOnlyList<T>, IOwningCollection<T>
//   IReadOnlyList<T>       : IReadOnlyCollection<T>
//   ICollection<T>         : IReadOnlyCollection<T>
//   IOwningCollection<T>    : IReadOnlyCollection<T>
//   IReadOnlyCollection<T> : IEnumerable<T>
//   IEnumerable<T>         : (root)
//
// Only arity-1 types are covered here; types without standard covariant element
// chains (e.g. Dictionary<K,V>) are not in scope for this helper.
static std::vector<const TypeRef*> builtinInterfaceAncestors(
    std::string_view name, const std::vector<TypeRef*>& genericArgs,
    std::vector<std::unique_ptr<TypeRef>>& owned) {
    // Helper to create a synthesised null-declaration Named TypeRef.
    // ifaceName MUST be a string-literal (static lifetime) so the
    // string_view importedName field remains valid indefinitely.
    auto makeBuiltin = [&](std::string_view ifaceName,
                           std::vector<TypeRef*> args) -> TypeRef* {
        auto ref = std::make_unique<TypeRef>();
        ref->kind = TypeRef::Kind::Named;
        ref->declaration = nullptr;
        ref->importedName = ifaceName;  // caller supplies a string literal
        ref->genericArgs = std::move(args);
        TypeRef* raw = ref.get();
        owned.push_back(std::move(ref));
        return raw;
    };
    // Only handle arity-1 types with a meaningful interface chain.
    if (genericArgs.size() != 1) return {};
    TypeRef* T = genericArgs[0];
    if (!T) return {};
    // Build the transitive ancestor list in breadth-first order (topmost
    // interfaces first so the BFS in extensionTargetMatchesReceiverOrBase
    // encounters them in the most-specific → least-specific direction).
    std::vector<const TypeRef*> ancestors;
    // Tier 1 parents of the concrete collection types.
    if (name == "Array")
        ancestors.push_back(makeBuiltin("IList", {T}));
    else if (name == "OwningArray")
        ancestors.push_back(makeBuiltin("IOwningList", {T}));
    else if (name == "Stack" || name == "OwningStack" ||
             name == "Queue" || name == "OwningQueue" ||
             name == "Set"   || name == "OwningSet" ||
             name == "LinkedList" || name == "OwningLinkedList")
        ancestors.push_back(makeBuiltin("IReadOnlyCollection", {T}));
    else if (name == "IList")
        ancestors.push_back(makeBuiltin("IReadOnlyList", {T})),
        ancestors.push_back(makeBuiltin("ICollection", {T}));
    else if (name == "IOwningList")
        ancestors.push_back(makeBuiltin("IReadOnlyList", {T})),
        ancestors.push_back(makeBuiltin("IOwningCollection", {T}));
    else if (name == "ICollection")
        ancestors.push_back(makeBuiltin("IReadOnlyCollection", {T}));
    else if (name == "IOwningCollection")
        ancestors.push_back(makeBuiltin("IReadOnlyCollection", {T}));
    else if (name == "IReadOnlyList")
        ancestors.push_back(makeBuiltin("IReadOnlyCollection", {T}));
    else if (name == "IReadOnlyCollection")
        ancestors.push_back(makeBuiltin("IEnumerable", {T}));
    // For a name we don't recognise at this level, return empty (no hierarchy
    // known) — this is conservative: no false-positive extension matches.
    if (ancestors.empty()) return {};
    // Recursively expand each ancestor's own ancestors so the returned list is
    // the full transitive closure.
    std::vector<const TypeRef*> result;
    std::vector<std::unique_ptr<TypeRef>> scratch;
    std::vector<const TypeRef*> frontier(ancestors.begin(), ancestors.end());
    std::size_t fi = 0;
    while (fi < frontier.size()) {
        const TypeRef* anc = frontier[fi++];
        result.push_back(anc);
        if (anc->kind == TypeRef::Kind::Named && !anc->importedName.empty()) {
            std::vector<std::unique_ptr<TypeRef>> childOwned;
            auto children = builtinInterfaceAncestors(
                anc->importedName, anc->genericArgs, childOwned);
            for (auto& u : childOwned) owned.push_back(std::move(u));
            for (const TypeRef* c : children) frontier.push_back(c);
        }
    }
    return result;
}

// §26.5 (#714): convenience wrapper — same as above but derives name and args
// from the receiver TypeRef directly (must be a null-decl Named builtin).
static std::vector<const TypeRef*> builtinInterfaceAncestorsOf(
    const TypeRef* recv,
    std::vector<std::unique_ptr<TypeRef>>& owned) {
    if (!recv || recv->kind != TypeRef::Kind::Named || recv->declaration)
        return {};
    std::string_view name = namedTypeSimpleName(recv);
    if (name.empty()) return {};
    return builtinInterfaceAncestors(name,
        const_cast<std::vector<TypeRef*>&>(recv->genericArgs), owned);
}

// ---------------------------------------------------------------------------
// §12.9 (issue #1180) — generic variance helpers
// ---------------------------------------------------------------------------

// Variance annotation of the i-th type parameter of a known BUILTIN generic
// interface (those with null TypeSymbol declarations).  Returns "out"
// (covariant), "in" (contravariant), or "" (invariant).
// Per §12.9 and the language-primitives table in §2.1:
//   covariant (out T): IEnumerable, IEnumerator, IAsync*, IReadOnlyCollection,
//                      IReadOnlyList, IEvent, IAsyncEvent.
//   invariant: IList, ICollection, IOwningList, IOwningCollection, Array,
//              OwningArray, Stack, Queue, Set, LinkedList, etc.
static std::string_view builtinParamVariance(std::string_view name,
                                              std::size_t /*i*/) {
    if (name == "IEnumerable" || name == "IEnumerator" ||
        name == "IAsyncEnumerable" || name == "IAsyncEnumerator" ||
        name == "IReadOnlyCollection" || name == "IReadOnlyList" ||
        name == "IEvent" || name == "IAsyncEvent")
        return "out";
    return "";  // invariant
}

// §12.9 / L1592: is a TypeRef a VALUE type that cannot participate in a
// covariant/contravariant generic conversion?
// Value types in Bezel: numeric primitives, bool, char (PrimitiveType except
// Object and String), enums, and records.  String and Object are reference types.
static bool isValueTypeArg(const TypeRef* t) {
    if (!t) return false;
    if (t->kind == TypeRef::Kind::Primitive)
        return t->primitive != PrimitiveType::Object &&
               t->primitive != PrimitiveType::String;
    if (t->kind == TypeRef::Kind::Named && t->declaration)
        return t->declaration->kind == ast::TypeDeclKind::Enum ||
               t->declaration->kind == ast::TypeDeclKind::Record;
    return false;
}

// §12.9: shallow base-type equality for a single generic argument.
// Two TypeRefs share the "same base type" when both are the same primitive,
// both are Named user-declared with the same TypeSymbol pointer, or both are
// Named builtins with the same importedName (arity not recursively checked;
// deeper nesting is left to the recursive variance walk if ever implemented).
// Nullability is intentionally ignored — variance operates on the base type.
static bool typeArgSameBase(const TypeRef* a, const TypeRef* b) {
    if (!a || !b) return a == b;
    if (a->kind != b->kind) return false;
    if (a->kind == TypeRef::Kind::Primitive) return a->primitive == b->primitive;
    if (a->kind == TypeRef::Kind::Named) {
        if (a->declaration && b->declaration)
            return a->declaration == b->declaration;
        if (!a->declaration && !b->declaration) {
            std::string_view an = namedTypeSimpleName(a);
            std::string_view bn = namedTypeSimpleName(b);
            return !an.empty() && an == bn;
        }
    }
    return false;
}

// §12.9: for a COVARIANT position, is `fromArg` a reference-type subtype of
// `toArg`?  Exact match is always a subtype; PrimitiveType::Object is the
// universal root; otherwise both must be Named with non-null declarations and
// `isSubtypeFn(from, to)` must hold.
static bool typeArgRefSubtype(const TypeRef* fromArg, const TypeRef* toArg,
                               bool (*isSubtypeFn)(TypeSymbol*, TypeSymbol*)) {
    if (!fromArg || !toArg) return false;
    if (typeArgSameBase(fromArg, toArg)) return true;
    // PrimitiveType::Object is the universal root type.
    if (toArg->kind == TypeRef::Kind::Primitive &&
        toArg->primitive == PrimitiveType::Object)
        return true;
    // Both Named with user declarations: walk the subtype chain.
    if (fromArg->kind == TypeRef::Kind::Named &&
        toArg->kind == TypeRef::Kind::Named &&
        fromArg->declaration && toArg->declaration)
        return isSubtypeFn(fromArg->declaration, toArg->declaration);
    return false;
}

// §12.9 (issue #1180): check whether the generic assignment `from → to` is
// sound under §12.9 variance rules.  Returns true (compatible) or false
// (unsound — caller should report §5.1 error).
//
// Two cases handled:
//   A) Same base type (same TypeSymbol, or same importedName for builtins):
//      compare args pairwise under the declared/hardcoded variance.
//   B) Different base types where `from` is a builtin subtype of `to`:
//      walk builtinInterfaceAncestorsOf to find the matching ancestor, then
//      apply case A on that pair.
//      User-declared cross-type generics are conservatively accepted
//      (residual — deferred to the full §12.9 milestone).
static bool variantGenericAssignable(
    const TypeRef* from, const TypeRef* to,
    bool (*isSubtypeFn)(TypeSymbol*, TypeSymbol*)) {
    if (!from || !to) return true;
    if (from->kind != TypeRef::Kind::Named ||
        to->kind != TypeRef::Kind::Named) return true;
    if (to->genericArgs.empty()) return true;  // non-generic target → accept

    std::string_view toName = namedTypeSimpleName(to);

    // Find `candidate`: the instantiation of `to`'s base that `from` produces.
    const TypeRef* candidate = nullptr;
    std::vector<std::unique_ptr<TypeRef>> owned;

    if (!to->declaration) {
        // `to` is a builtin generic interface (null declaration).
        if (!from->declaration && !from->genericArgs.empty()) {
            // `from` is also a builtin.
            std::string_view fromName = namedTypeSimpleName(from);
            if (fromName == toName) {
                candidate = from;   // same builtin — compare args directly
            } else {
                // Walk the builtin interface hierarchy to find the matching ancestor.
                auto ancs = builtinInterfaceAncestorsOf(from, owned);
                for (const TypeRef* anc : ancs) {
                    if (namedTypeSimpleName(anc) == toName) {
                        candidate = anc;
                        break;
                    }
                }
            }
        }
        // `from` has a user declaration: user-declared → builtin is a residual
        // (would need baseTypeRefs walk with substitution).  Accept conservatively.
    } else {
        // `to` is a user-declared generic type (has declaration).
        if (from->declaration == to->declaration) {
            candidate = from;  // same declaration — compare args directly
        }
        // Cross-declaration user generics: conservatively accept (residual).
    }

    if (!candidate) return true;  // no matching ancestor → conservative accept
    if (candidate->genericArgs.size() != to->genericArgs.size()) return true;

    for (std::size_t i = 0; i < to->genericArgs.size(); ++i) {
        const TypeRef* fromArg = candidate->genericArgs[i];
        const TypeRef* toArg = to->genericArgs[i];
        if (!fromArg || !toArg) continue;  // conservative skip on unresolved

        // §14.1 (issue #1180): a Named TypeRef with null declaration and no
        // genericArgs is a free type variable (generic method/type parameter
        // like A, B, T). Free variables accept any binding without variance
        // restriction — the only constraint is the concrete substitution, which
        // is enforced at the call site, not here.
        {
            bool toArgFree = toArg->kind == TypeRef::Kind::Named &&
                             !toArg->declaration &&
                             toArg->genericArgs.empty() &&
                             !namedTypeSimpleName(toArg).empty();
            bool fromArgFree = fromArg->kind == TypeRef::Kind::Named &&
                               !fromArg->declaration &&
                               fromArg->genericArgs.empty() &&
                               !namedTypeSimpleName(fromArg).empty();
            if (toArgFree || fromArgFree) continue;
        }

        // Determine variance of `to`'s i-th parameter.
        std::string_view variance;
        if (!to->declaration) {
            // Builtin: use the hardcoded variance table.
            variance = builtinParamVariance(toName, i);
        } else {
            // User-declared: read from the GenericParam annotations.
            if (auto* td = dynamic_cast<const ast::TypeDecl*>(
                    to->declaration->decl)) {
                if (i < td->genericParams.size())
                    variance = td->genericParams[i].variance;
            }
            // §12.9 L1590: classes and records are always invariant.
            if (to->declaration->kind == ast::TypeDeclKind::Class ||
                to->declaration->kind == ast::TypeDeclKind::Record)
                variance = "";
        }

        bool ok;
        if (variance == "out") {
            // Covariant: exact match is always OK; otherwise both args must be
            // reference types and fromArg must be a subtype of toArg.
            // §12.9 L1592: value-typed T → variant assignment is forbidden even
            // for covariant interfaces; only exact-match is permitted.
            if (typeArgSameBase(fromArg, toArg)) {
                ok = true;
            } else if (isValueTypeArg(fromArg) || isValueTypeArg(toArg)) {
                ok = false;  // per §12.9 L1592
            } else {
                ok = typeArgRefSubtype(fromArg, toArg, isSubtypeFn);
            }
        } else if (variance == "in") {
            // Contravariant: exact match OK; otherwise toArg must be a
            // reference-type subtype of fromArg.
            if (typeArgSameBase(fromArg, toArg)) {
                ok = true;
            } else if (isValueTypeArg(fromArg) || isValueTypeArg(toArg)) {
                ok = false;
            } else {
                ok = typeArgRefSubtype(toArg, fromArg, isSubtypeFn);
            }
        } else {
            // Invariant (or unknown variance): exact base match required.
            ok = typeArgSameBase(fromArg, toArg);
        }

        if (!ok) return false;
    }
    return true;
}

// §26.5 (#714/#927): find the ancestor of `recv` (or `recv` itself) that
// exactly matches `target` under freeVars unification, or return nullptr.
// Walks user-declared receivers via their TypeDecl::baseTypeRefs chain (§26.5
// step 2) and builtin receivers via the hardcoded stdlib interface hierarchy
// (Array<T>→IList<T>→…→IEnumerable<T>). The returned ancestor's genericArgs
// carry the concrete type bindings (e.g. IEnumerable<int> when T=int).
static const TypeRef* extensionFindMatchingAncestor(
    const TypeRef* target, const TypeRef* recv,
    const std::unordered_set<std::string_view>& freeVars,
    std::vector<std::unique_ptr<TypeRef>>& owned) {
    if (!target || !recv) return nullptr;
    if (extensionTargetMatchesReceiver(target, recv, freeVars)) return recv;
    // §26.2/§26.5 (#1308): `Object` (a Primitive TypeRef) is the implicit base
    // of every class and interface — spec §2 "every type derives from Object",
    // §12 "if no base class is specified, the implicit base is Object". A
    // receiver's `baseTypeRefs` chain does NOT materialise this implicit Object
    // base, so the walk below never reaches Object for a class/interface
    // receiver whose declared bases are empty (or don't transitively list
    // Object). Treat Object as a matching (least-specific, §26.5.1 rule 1)
    // ancestor of any reference-typed receiver so `extension Object` methods
    // resolve. Value-typed receivers (record/enum/primitive) would need boxing
    // to Object before the call and are out of scope here — they fall through
    // to the existing diagnostic.
    if (target->kind == TypeRef::Kind::Primitive &&
        target->primitive == PrimitiveType::Object &&
        recv->kind == TypeRef::Kind::Named && recv->declaration &&
        (recv->declaration->kind == ast::TypeDeclKind::Class ||
         recv->declaration->kind == ast::TypeDeclKind::Interface))
        return target;
    // Walk user-declared receiver's base type chain.
    if (recv->kind == TypeRef::Kind::Named && recv->declaration) {
        std::vector<const TypeRef*> frontier;
        for (const TypeRef* b : recv->declaration->baseTypeRefs)
            if (b) frontier.push_back(b);
        std::size_t i = 0;
        while (i < frontier.size()) {
            const TypeRef* anc = frontier[i++];
            if (!anc) continue;
            if (extensionTargetMatchesReceiver(target, anc, freeVars))
                return anc;
            if (anc->kind == TypeRef::Kind::Named && anc->declaration)
                for (const TypeRef* b : anc->declaration->baseTypeRefs)
                    if (b) frontier.push_back(b);
        }
    }
    // Walk builtin (null-declaration) receiver's conceptual interface chain.
    if (recv->kind == TypeRef::Kind::Named && !recv->declaration) {
        auto builtinAncs = builtinInterfaceAncestorsOf(recv, owned);
        for (const TypeRef* anc : builtinAncs)
            if (extensionTargetMatchesReceiver(target, anc, freeVars))
                return anc;
    }
    return nullptr;
}

// §26.5 (#714): given the extension's `target` TypeRef (e.g. `IEnumerable<T>`)
// and the matched ancestor of the receiver (e.g. `IEnumerable<int>`), extract
// the concrete binding for each free var by positional alignment.
// Example: target = IEnumerable<T (freeVar)>, ancestor = IEnumerable<int>
//   → binding: [("T", TypeRef{int})]
static void collectFreeVarBinding(
    const TypeRef* target, const TypeRef* matchedAncestor,
    const std::unordered_set<std::string_view>& freeVars,
    std::vector<std::pair<std::string, TypeRef*>>& binding) {
    if (!target || !matchedAncestor) return;
    if (target->genericArgs.size() != matchedAncestor->genericArgs.size())
        return;
    for (std::size_t i = 0; i < target->genericArgs.size(); ++i) {
        const TypeRef* ta = target->genericArgs[i];
        TypeRef* ra = matchedAncestor->genericArgs[i];
        if (!ta || !ra) continue;
        // Is ta a free var? (null-decl Named whose name is in freeVars)
        if (ta->kind == TypeRef::Kind::Named && !ta->declaration) {
            std::string_view vname;
            if (ta->astNode && ta->astNode->name &&
                !ta->astNode->name->parts.empty())
                vname = ta->astNode->name->parts.back();
            else if (!ta->importedName.empty())
                vname = ta->importedName;
            if (!vname.empty() && freeVars.count(vname)) {
                binding.emplace_back(std::string(vname), ra);
                continue;
            }
        }
        // ta is a concrete arg: recurse to pick up nested free vars.
        collectFreeVarBinding(ta, ra, freeVars, binding);
    }
}

// §26.5 (#773): does an ast::Type name (transitively through generic args /
// array element / function signature) reference one of `freeVars`?
static bool typeNamesFreeVar(
    const ast::Type* t, const std::unordered_set<std::string_view>& freeVars) {
    if (!t) return false;
    if (t->name && !t->name->parts.empty() &&
        freeVars.count(t->name->parts.back()))
        return true;
    for (const auto& ga : t->genericArgs)
        if (typeNamesFreeVar(ga.get(), freeVars)) return true;
    if (typeNamesFreeVar(t->arrayElement.get(), freeVars)) return true;
    if (typeNamesFreeVar(t->funcReturn.get(), freeVars)) return true;
    for (const auto& p : t->funcParams)
        if (typeNamesFreeVar(p.type.get(), freeVars)) return true;
    return false;
}

// §26.5 (#773): true when an extension method's parameter or return types
// reference a free type variable of the extension — the case whose codegen
// substitution is deferred for user-declared extension targets (see
// lookupExtensionMember). For builtin-target extensions (§26.5 #714), the
// binding-and-codegen path is now implemented, so this check is not applied.
static bool extSignatureUsesFreeVar(
    const ast::MethodDecl* md,
    const std::unordered_set<std::string_view>& freeVars) {
    if (typeNamesFreeVar(md->returnType.get(), freeVars)) return true;
    for (const auto& p : md->params)
        if (typeNamesFreeVar(p.type.get(), freeVars)) return true;
    return false;
}

Symbol* ExprTC::lookupExtensionMember(
    const TypeRef* receiver,
    std::string_view name,
    std::vector<std::pair<std::string, TypeRef*>>* outBinding) {
    if (!receiver || !ctx_.module()) return nullptr;
    for (Symbol* child : ctx_.module()->children) {
        auto* ext = dynamic_cast<ExtensionSymbol*>(child);
        if (!ext) continue;
        auto* decl = dynamic_cast<const ast::ExtensionDecl*>(ext->decl);
        if (!decl || !decl->targetType) continue;
        const TypeRef* target = ctx_.resolvedType(*decl->targetType);
        // §26.5 (#773/#714): the extension's free type variables are EITHER
        // named in a `where T: …` constraint (`extension Bag<T> where T: class`)
        // OR introduced implicitly as the generic-arg placeholder name in the
        // target type (`extension IEnumerable<T>` without a where clause). In
        // the latter case `genericConstraints` is empty but the target TypeRef's
        // generic args include a null-declaration Named ref whose name is the
        // free var — e.g. `IEnumerable<T>` has genericArgs[0] = {Named, null, "T"}.
        // Collect both sources so a plain `extension IEnumerable<T>` is treated
        // identically to `extension IEnumerable<T> where T: class` for matching.
        std::unordered_set<std::string_view> freeVars;
        for (const auto& gc : decl->genericConstraints)
            freeVars.insert(gc.typeParam);
        // §26.4/§26.5 (#714/#1276): for an extension target without a `where`
        // clause (e.g. `IEnumerable<T>` or a user-declared `IBox<T>`), the
        // generic args are type params scoped by TypeResolution (null-decl
        // Named refs with empty genericArgs, resolveTopDecl's extScope
        // binding) that aren't already in freeVars via genericConstraints.
        // Collect them so the extension matches correctly. Per §26.4, "each
        // bare single-segment non-nullable identifier introduces a fresh
        // block-level type parameter — always" — this holds for BOTH
        // builtin (null-declaration) and user-declared (non-null-declaration)
        // target types; TypeResolution already resolves such an identifier
        // to a null-decl placeholder Named ref unconditionally (it does not
        // special-case user-declared targets — TypeResolution_Names.cpp
        // resolveTopDecl), so the free-var scan below must not special-case
        // them either. Previously gated to `!target->declaration` only
        // (builtin targets), which left a plain `extension SomeUserGeneric<T>`
        // (no `where` clause) unmatched against every receiver — #1276's
        // generic-interface extension via an interface-typed receiver is one
        // instance (silently unresolved -> codegen emitted no dispatch and
        // the call returned garbage); the same gap also FALSE_REJECTed the
        // identical shape called directly on a class receiver.
        if (target && target->kind == TypeRef::Kind::Named) {
            std::function<void(const TypeRef*)> scanFreeVars =
                [&](const TypeRef* tr) {
                if (!tr) return;
                if (tr->kind == TypeRef::Kind::Named && !tr->declaration) {
                    std::string_view vname;
                    if (tr->astNode && tr->astNode->name &&
                        !tr->astNode->name->parts.empty())
                        vname = tr->astNode->name->parts.back();
                    else if (!tr->importedName.empty())
                        vname = tr->importedName;
                    // A bare single-part null-decl name with no genericArgs is
                    // a free type variable (builtins have non-empty genericArgs).
                    if (!vname.empty() && tr->genericArgs.empty())
                        freeVars.insert(vname);
                }
                for (const TypeRef* ga : tr->genericArgs) scanFreeVars(ga);
            };
            scanFreeVars(target);
        }
        // §26.5 (#714): find the ancestor of `receiver` that matches `target`
        // (could be receiver itself if exact match). We need the ancestor both
        // for the bool match AND for extracting the concrete T-binding.
        std::vector<std::unique_ptr<TypeRef>> ownedAncs;
        const TypeRef* matchedAnc = extensionFindMatchingAncestor(
            target, receiver, freeVars, ownedAncs);
        if (!matchedAnc) continue;
        // Determine whether the extension target is a builtin (null-decl) type.
        // For builtin-target extensions (e.g. `extension IEnumerable<T>`), the
        // full binding-and-codegen path is implemented (#714). For user-declared
        // extension targets (e.g. `extension Bag<T> where T: class`), methods
        // whose SIGNATURES use the free type variable are still deferred (#773):
        // skip them so the lookup falls through to a clean diagnostic rather than
        // generating type-mismatched IR.
        bool targetIsBuiltin =
            target && target->kind == TypeRef::Kind::Named && !target->declaration;
        for (Symbol* m : ext->members) {
            if (!m || m->name != name) continue;
            if (!freeVars.empty() && !targetIsBuiltin) {
                // User-declared generic extension (#773): skip methods whose
                // signatures reference a free type variable — that case is
                // deferred. Methods that do NOT use the free var in their
                // signatures (e.g. `IsEmpty(): bool` on a `Bag<T>`) are fine.
                if (auto* ms = dynamic_cast<MethodSymbol*>(m)) {
                    if (auto* md =
                            dynamic_cast<const ast::MethodDecl*>(ms->decl)) {
                        if (extSignatureUsesFreeVar(md, freeVars)) continue;
                    }
                }
            }
            // §26.5 (#714/#773): when the extension has free type variables,
            // compute the binding (T → concrete TypeRef) from the matched
            // ancestor's generic args. This binding lets typeCall substitute
            // the return type and codegen emit a specialized body.
            std::vector<std::pair<std::string, TypeRef*>> binding;
            if (!freeVars.empty()) {
                // Collect T→concrete mapping positionally from matched ancestor.
                collectFreeVarBinding(target, matchedAnc, freeVars, binding);
            }
            if (outBinding) *outBinding = binding;
            return m;
        }
    }
    return nullptr;
}

// §26.5 step 1 (v0.183): see header. Re-runs the same extension search
// lookupExtensionMember uses — a same-named extension member is name-blocked
// (never consulted) whenever an instance member of that name exists, which
// is exactly the situation callers of this helper are already in (they call
// it only after a name-blocked instance call has failed), so the search
// itself is unconditional: no `member == nullptr` guard is needed here.
std::string ExprTC::extensionNameBlockedNote(const TypeRef* receiver,
                                             std::string_view name) {
    if (!receiver) return std::string();
    if (!lookupExtensionMember(receiver, name, nullptr)) return std::string();
    return "; note: an extension member named `" + std::string(name) +
          "` also matches this receiver but was not considered — an "
          "instance member of the same name name-blocks extension lookup "
          "regardless of applicability or accessibility (§26.5)";
}

// §11.8 (v0.176, #807): is `name` a declared generic type parameter of the
// enclosing method (checked first — more specific) or the enclosing type?
bool ExprTC::isInScopeTypeParam(std::string_view name) const {
    if (currentMethodDecl_) {
        for (const auto& gp : currentMethodDecl_->genericParams)
            if (gp.name == name) return true;
    }
    if (currentType_ && currentType_->decl) {
        if (auto* td = dynamic_cast<const ast::TypeDecl*>(currentType_->decl)) {
            for (const auto& gp : td->genericParams)
                if (gp.name == name) return true;
        }
    }
    return false;
}

// §11.8: the cached marker GenericParamSymbol for a type-parameter receiver.
// Consumers key off the SYMBOL KIND (dynamic_cast<GenericParamSymbol*>) and
// the name; the marker needs no scope identity.
GenericParamSymbol* ExprTC::typeParamMarker(std::string_view name) {
    auto it = typeParamMarkers_.find(name);
    if (it != typeParamMarkers_.end()) return it->second;
    auto* gps = ctx_.makeSymbol<GenericParamSymbol>();
    gps->name = name;
    typeParamMarkers_[name] = gps;
    return gps;
}

// §11.8: walk the `type`-kind constraint bounds declared for `paramName` —
// the enclosing method's `where` clauses first (more specific), then the
// enclosing type's — calling `fn` with each resolved bound until it accepts.
bool ExprTC::forEachConstraintBound(
    std::string_view paramName, const std::function<bool(TypeRef*)>& fn) {
    auto tryList = [&](const std::vector<ast::GenericParam>& gps,
                       const std::vector<ast::GenericConstraint>& gcs) -> bool {
        bool isParam = false;
        for (const auto& gp : gps)
            if (gp.name == paramName) { isParam = true; break; }
        if (!isParam) return false;
        for (const auto& gc : gcs) {
            if (gc.typeParam != paramName) continue;
            for (const auto& item : gc.items) {
                if (item.kind != "type" || !item.typeArg) continue;
                TypeRef* ctr = ctx_.resolvedType(*item.typeArg);
                if (!ctr || ctr->kind != TypeRef::Kind::Named ||
                    !ctr->declaration)
                    continue;
                if (fn(ctr)) return true;
            }
        }
        return false;
    };
    if (currentMethodDecl_ && !currentMethodDecl_->genericParams.empty()) {
        if (tryList(currentMethodDecl_->genericParams,
                    currentMethodDecl_->genericConstraints))
            return true;
    }
    if (currentType_ && currentType_->decl) {
        if (auto* td = dynamic_cast<const ast::TypeDecl*>(currentType_->decl)) {
            if (!td->genericParams.empty() &&
                tryList(td->genericParams, td->genericConstraints))
                return true;
        }
    }
    return false;
}

// §11.8 (v0.176, #807): find a static-interface OPERATOR member for a binary
// op on `paramName`-typed operands. The interface member must carry the op's
// token with two parameters of the interface's own recurrence parameter (the
// §2.19 shape `static operator +(a: T, b: T): T`).
//
// §25.2 (v0.181, #1498) complete-group entitlement. A constraint interface
// that declares ANY ONE of the comparison group `<`,`>`,`<=`,`>=` entitles the
// generic body to ALL FOUR — the stdlib `INumber<T>`/`ISteppable<T>` declare
// `<` alone, yet `a > b`, `a <= b`, `a >= b` are all admissible on `T`-typed
// operands (§25.2: "a constraint interface that declares any one of them …
// entitles the generic body to all four"). The same holds for the `=`/`<>`
// pair. So when `op` is a member of a complete group and no exact-token member
// exists on the constraint, ANY sibling operator serves as the witness: it
// shares the (T,T):bool signature that drives result typing, and each use is
// bound at instantiation to the conforming type's OWN declaration of `op`
// (codegen resolves the concrete member by the ACTUAL operator token, never by
// synthesizing `a > b` from `b < a`). An exact-token member is preferred when
// present.
MethodSymbol* ExprTC::findConstraintStaticOperator(std::string_view paramName,
                                                   std::string_view op,
                                                   TypeRef** outBound) {
    auto isRel = [](std::string_view s) {
        return s == "<" || s == ">" || s == "<=" || s == ">=";
    };
    auto isEq = [](std::string_view s) { return s == "=" || s == "<>"; };
    auto sameGroup = [&](std::string_view a, std::string_view b) {
        return (isRel(a) && isRel(b)) || (isEq(a) && isEq(b));
    };
    MethodSymbol* found = nullptr;          // exact-token witness (preferred)
    MethodSymbol* groupWitness = nullptr;   // complete-group sibling witness
    TypeRef* groupBound = nullptr;
    forEachConstraintBound(paramName, [&](TypeRef* ctr) -> bool {
        TypeSymbol* iface = ctr->declaration;
        if (iface->kind != ast::TypeDeclKind::Interface) return false;
        // The interface's own type-parameter names (`T` in `INumber<T>`).
        std::unordered_set<std::string_view> ifaceParams;
        for (Symbol* gp : iface->genericParams)
            if (gp) ifaceParams.insert(gp->name);
        for (Symbol* m : iface->members) {
            auto* ms = dynamic_cast<MethodSymbol*>(m);
            if (!ms || !ms->isOperator) continue;
            bool exact = ms->operatorToken == op;
            bool group = !exact && sameGroup(op, ms->operatorToken);
            if (!exact && !group) continue;
            if (ms->params.size() != 2) continue;
            // Both parameters must be the recurrence parameter itself so the
            // substituted signature is (paramName, paramName).
            bool bothRecur = true;
            for (Symbol* ps : ms->params) {
                auto* par = dynamic_cast<ParameterSymbol*>(ps);
                TypeRef* pt = (par && par->declType)
                                  ? ctx_.resolvedType(*par->declType)
                                  : nullptr;
                if (!pt || pt->kind != TypeRef::Kind::Named ||
                    pt->declaration || pt->nullable || !pt->astNode ||
                    !pt->astNode->name ||
                    pt->astNode->name->parts.size() != 1 ||
                    !ifaceParams.count(pt->headName())) {
                    bothRecur = false;
                    break;
                }
            }
            if (!bothRecur) continue;
            if (exact) {
                found = ms;
                if (outBound) *outBound = ctr;
                return true;  // exact token wins — stop scanning
            }
            // Remember the first group sibling; keep scanning for an exact hit.
            if (!groupWitness) { groupWitness = ms; groupBound = ctr; }
        }
        return false;
    });
    if (found) return found;
    if (groupWitness && outBound) *outBound = groupBound;
    return groupWitness;
}

Symbol* ExprTC::lookupName(std::string_view name) {
    for (Scope* s = currentScope_; s; s = s->parent) {
        auto it = s->names.find(name);
        if (it != s->names.end()) {
            // §22.5 (issue #1160): a default-value expression may read only
            // parameters declared EARLIER in the same list. While checking
            // such a default, `forbiddenForwardParams_` holds this-and-later
            // params; a reference to one is recorded so `runMethodBody`
            // reports the §22.5 error. The symbol is still returned (so the
            // expression types as that param's type and no cascading
            // "unresolved identifier" follows) — the recorded error fails the
            // compile.
            if (!forbiddenForwardParams_.empty() &&
                forbiddenForwardParams_.count(it->second))
                forwardParamRef_ = std::string(name);
            return it->second;
        }
    }
    // An unqualified member name resolves as an implicit-`this`
    // reference — search the enclosing type AND its base types
    // (§12 inheritance), mirroring lookupMember's transitive walk so
    // an inherited field/method is reachable without `this.`/`base.`.
    if (currentType_) {
        if (Symbol* m = lookupMember(currentType_, name)) return m;
    }
    return nullptr;
}

void ExprTC::checkThrowableType(const ast::Expr& value) {
    TypeRef* vt = ctx_.exprType(value);
    if (!vt) return;  // unresolved type — don't false-positive
    // §31.1 (v0.182): the throw operand must be an instance of a CLASS type —
    // any class, deliberately NOT restricted to `Exception`-derived (the #771
    // ruling: a thrown class is caught by its own type or a base). A Named type
    // whose declaration is a RECORD or ENUM is a value type and not throwable;
    // an INTERFACE-typed operand is likewise rejected (its referent may be a
    // boxed value, which is not throwable — narrow to the class first). A
    // Named type with a null declaration (an imported builtin exception) is
    // conservatively treated as throwable. `Object` (Primitive::Object),
    // `string` (Primitive::String) and the other primitives, an inline array,
    // a function value, and unit are not Named-class and not throwable.
    // // per §31.1, §17.12
    bool throwable = vt->kind == TypeRef::Kind::Named &&
                     !(vt->declaration &&
                       (vt->declaration->kind == ast::TypeDeclKind::Record ||
                        vt->declaration->kind == ast::TypeDeclKind::Enum ||
                        vt->declaration->kind == ast::TypeDeclKind::Interface));
    if (!throwable)
        ctx_.diagnostics().report(
            Severity::Error, value.span.begin,
            "the thrown value must be an exception — an instance of a class "
            "type (\xc2\xa7""31.1): a primitive, `string`, `Object`, an "
            "interface, a record, an enum, an inline array, or a function "
            "value cannot be thrown (an `Object`- or interface-typed value "
            "must be narrowed to its class type first)");
}

void ExprTC::validateCaseArms(const std::vector<ast::CaseArm>& arms) {
    for (std::size_t i = 0; i < arms.size(); ++i) {
        const ast::Pattern* p = arms[i].pattern.get();
        if (!p) continue;
        // §16.4: `step` is not legal in a `case` range pattern — patterns test
        // membership and a stride has no meaning. Reject cleanly; codegen has no
        // lowering for a stepped range pattern and otherwise emits malformed IR
        // (an unterminated case.end block, issue #738).
        auto checkRangePattern = [&](const ast::RangePattern* rp) {
            auto* re = dynamic_cast<const ast::RangeExpr*>(rp->rangeExpr.get());
            if (!re) return;
            if (re->step)
                ctx_.diagnostics().report(
                    Severity::Error, rp->span.begin,
                    "`step` is not legal in a `case` range pattern — a "
                    "pattern tests membership and a stride has no meaning "
                    "(§16.4)");
            // §16.6 (v0.162, fix #1340): a stepless range pattern with
            // constant integer bounds that are reversed (lo > hi for a
            // closed `lo..hi`, or lo >= hi for a half-open `lo..<hi`)
            // can NEVER match — the pattern's extent is empty in the
            // default ascending direction.  Warn at the pattern site so
            // the author sees it immediately ("reversed bounds" mirrors
            // the Rust / Python convention for this diagnostic).
            if (!re->step && re->lo && re->hi) {
                auto loV = foldIntConstExprTC(*re->lo);
                auto hiV = foldIntConstExprTC(*re->hi);
                if (loV && hiV) {
                    bool neverMatches =
                        re->inclusive ? (*loV > *hiV) : (*loV >= *hiV);
                    if (neverMatches)
                        ctx_.diagnostics().report(
                            Severity::Warning, rp->span.begin,
                            "this pattern never matches \xe2\x80\x94 "
                            "bounds are reversed (§16.6)");
                }
            }
        };
        if (auto* rp = dynamic_cast<const ast::RangePattern*>(p)) {
            checkRangePattern(rp);
        } else if (auto* ap = dynamic_cast<const ast::AltPattern*>(p)) {
            // §19.3 (v0.183, #1584): the same step/reversed-bounds checks apply
            // to a range member inside an alternative list.
            for (const auto& m : ap->members)
                if (auto* rp = dynamic_cast<const ast::RangePattern*>(m.get()))
                    checkRangePattern(rp);
        }
        // §19.3 / §19.6: an unguarded `_` is a catch-all and must be the last
        // arm; an earlier one makes every following arm unreachable. (A guarded
        // `_ when g` is conditional, so it may appear earlier.)
        if (!arms[i].guard &&
            dynamic_cast<const ast::DiscardPattern*>(p) &&
            i + 1 < arms.size())
            ctx_.diagnostics().report(
                Severity::Error, p->span.begin,
                "the `_` discard pattern must be the last arm of a `case` — "
                "arms after an unguarded `_` are unreachable (§19.6)");
        // §19.3 (v0.167, #1371): a `|` alternative list is constants-only —
        // reject a type member (a binding member is caught at parse).
        checkAltMembersConstant(p);
    }
}

// §19.3 (v0.167, #1371): recurse through a pattern and, for every `|`
// alternatives ConstantPattern (two or more members), reject a member that
// names a TYPE. Per-slot alternatives are reached through DestructuringPattern
// slots. A single-member ConstantPattern is an ordinary single constant (or a
// scrutinee-less bool condition) and is not an alternatives list.
void ExprTC::checkAltMembersConstant(const ast::Pattern* p) {
    if (!p) return;
    auto reportTypeMember = [&](SourceLocation loc) {
        ctx_.diagnostics().report(
            Severity::Error, loc,
            "a type may not appear in a `|` pattern alternative list "
            "\xe2\x80\x94 every member must be binding-free (a constant, a range, "
            "or a relational pattern); grouped type tests are written as "
            "separate arms (\xc2\xa7""19.3)");
    };
    if (auto* cp = dynamic_cast<const ast::ConstantPattern*>(p)) {
        if (cp->constants.size() < 2) return;
        for (const auto& c : cp->constants)
            if (c && altMemberNamesType(c.get())) reportTypeMember(c->span.begin);
        return;
    }
    // §19.3 (v0.183, #1584): an AltPattern is a binding-free alternative list
    // with a range/relational member. Its ranges and relationals bind nothing
    // and are always admissible; a constant-shaped member (the parser folds a
    // NamedPattern / `null` into a single-constant ConstantPattern) that
    // resolves to a TYPE is the same compile error — a type alters
    // exhaustiveness/binding analysis, which is what the constants-only
    // reasoning targeted (rationale §3.8).
    if (auto* ap = dynamic_cast<const ast::AltPattern*>(p)) {
        for (const auto& m : ap->members)
            if (auto* mcp = dynamic_cast<const ast::ConstantPattern*>(m.get()))
                for (const auto& c : mcp->constants)
                    if (c && altMemberNamesType(c.get()))
                        reportTypeMember(c->span.begin);
        return;
    }
    if (auto* dp = dynamic_cast<const ast::DestructuringPattern*>(p))
        for (const auto& sp : dp->subPatterns)
            if (sp.pattern) checkAltMembersConstant(sp.pattern.get());
}

// §19.3 / §A.7: does one alternative-member expression denote a TYPE (a class,
// interface, record, enum, or nested type) rather than a constant? A literal or
// other non-name expression is never a type. For a bare name, a value/const
// binding shadows a same-named type (so a resolved non-type symbol wins). For a
// qualified name, `Color.Red` is an enum constant (its final segment is not a
// nested type) while `Outer.Inner` is a nested type.
bool ExprTC::altMemberNamesType(const ast::Expr* c) {
    // Peel a MemberAccess chain into dotted parts; the innermost must be a
    // plain IdentifierExpr for this to be a qualified name.
    std::vector<std::string_view> parts;
    const ast::Expr* cur = c;
    while (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(cur)) {
        parts.push_back(ma->member);
        cur = ma->target.get();
    }
    auto* id = dynamic_cast<const ast::IdentifierExpr*>(cur);
    if (!id) return false;
    parts.push_back(id->name);
    for (std::size_t lo = 0, hi = parts.size(); lo + 1 < hi; ++lo, --hi)
        std::swap(parts[lo], parts[hi - 1]);
    return altPartsNameType(parts);
}

// §19.3 / §A.7: the shared dotted-name resolution behind altMemberNamesType,
// operating on an already-split QualifiedName (so a NamedPattern alternative
// member reaches it directly, #1584).
bool ExprTC::altPartsNameType(const std::vector<std::string_view>& parts) {
    if (parts.empty()) return false;
    // Resolve the head. A resolved non-type symbol (local/param/const/enum
    // constant) shadows a same-named type and means "not a type".
    TypeSymbol* ts = nullptr;
    if (Symbol* head = lookupName(parts[0])) {
        if (auto* t = dynamic_cast<TypeSymbol*>(head)) ts = t;
        else if (auto* nt = dynamic_cast<NestedTypeSymbol*>(head)) ts = nt->type;
        else return false;  // resolves to a value/const → not a type
    } else {
        // Not in scope: fall back to a top-level TypeSymbol of that name.
        for (const auto& sym : ctx_.symbols())
            if (auto* t = dynamic_cast<TypeSymbol*>(sym.get()))
                if (t->name == parts[0]) { ts = t; break; }
    }
    if (!ts) return false;
    if (parts.size() == 1) return true;  // bare type name (`Circle`)
    // Qualified: only a name whose FINAL segment is itself a nested type is a
    // type; a trailing enum constant / static const (`Color.Red`) is not.
    for (std::size_t i = 1; i < parts.size(); ++i) {
        NestedTypeSymbol* found = nullptr;
        for (Symbol* m : ts->members)
            if (m->name == parts[i]) {
                found = dynamic_cast<NestedTypeSymbol*>(m);
                break;
            }
        if (!found) return false;  // final segment is a member, not a type
        ts = found->type;
    }
    return ts != nullptr;
}

// §5.1 / §19 (#1153): a `case` constant or range-bound integer literal that
// does not fit the scrutinee's integer type was silently truncated to the
// scrutinee width by codegen's litAtWidth() — so `case x (x: byte) 256 => …`
// matched x == 0 (256 → i8 0), and a range bound `0..5000000000` over an `int`
// truncated to a 32-bit value, both producing wrong matches. Sema never
// range-checked the pattern literal. Reject it here with the same §5.1
// narrowing diagnostic the language gives for `var b: byte := 256` (which
// rejects the out-of-range constant rather than truncating it). Only an
// integer scrutinee narrower than the i64 fold width is gated (Long/ULong
// never truncate; see caseIntScrutineeRange). A non-constant operand, a
// non-integer constant, or a >64-bit literal does not fold here and is left to
// the existing literal-typing / §19 category checks.
void ExprTC::checkCasePatternConstantRanges(
    const std::vector<ast::CaseArm>& arms, TypeRef* scrutTy) {
    // §19.3/§A.7 (#1205): a `case` pattern constant must be a literal, `null`,
    // an enum constant, or a named `const` — a structurally non-constant
    // expression (e.g. a `new …` constructor call) is not a valid pattern
    // constant under §A.7 and would otherwise reach codegen's emitEquals /
    // CreateICmpEQ, which asserts on aggregate-struct operands (LLVM ICE).
    // Reject it here, before any codegen path is reached. // per §19.3/§A.7 (#1205)
    auto rejectNonConstantPatternExpr = [&](const ast::Expr* c) {
        if (!c) return;
        if (dynamic_cast<const ast::NewExpr*>(c))
            ctx_.diagnostics().report(
                Severity::Error, c->span.begin,
                "a `case` pattern constant must be a literal, `null`, an enum "
                "constant, or a named `const` \xe2\x80\x94 a `new \xe2\x80\xa6` "
                "expression is not a valid pattern constant (\xc2\xa7""A.7/\xc2\xa7""19.3)");
    };
    for (const auto& arm : arms) {
        const ast::Pattern* p = arm.pattern.get();
        if (!p) continue;
        if (auto* cp = dynamic_cast<const ast::ConstantPattern*>(p))
            for (const auto& c : cp->constants)
                rejectNonConstantPatternExpr(c.get());
    }
    // §10.2 (issue #1577): "under duplicate constant values, in a `case` over
    // the enum, two arms — or two `|` alternatives — naming same-valued
    // constants are a compile error, the later one being unreachable."
    // Declaration-level aliasing itself stays legal and undiagnosed (§10.2);
    // only a `case` naming the same value twice is rejected here, since the
    // second reference can never be reached (or, within one `|` list, adds
    // nothing — it is the exact same membership test repeated).
    if (scrutTy && scrutTy->kind == TypeRef::Kind::Named && scrutTy->declaration &&
            scrutTy->declaration->kind == ast::TypeDeclKind::Enum) {
        const std::unordered_map<std::string_view, long long> values =
            enumConstantValues(*scrutTy->declaration);
        // value -> name of the constant that first claimed it from an
        // UNGUARDED arm/alternative; only an unguarded claim makes a later
        // reference to the same value truly unreachable (a guarded arm may
        // fall through to its guard failing, so it does not "consume" the
        // value for arms that follow it).
        std::unordered_map<long long, std::string_view> unguardedClaim;
        for (const auto& arm : arms) {
            std::unordered_map<long long, std::string_view> seenThisArm;
            for (const auto& ref : enumPatternRefs(arm)) {
                auto vit = values.find(ref.name);
                if (vit == values.end()) continue;  // not this enum's own member
                long long v = vit->second;
                auto sit = seenThisArm.find(v);
                if (sit != seenThisArm.end()) {
                    ctx_.diagnostics().report(
                        Severity::Error, ref.loc,
                        "`" + std::string(ref.name) + "` denotes the same "
                        "underlying value as `" + std::string(sit->second) +
                        "` in this `|` alternative list — duplicate enum "
                        "constant values are the same value, not distinct "
                        "alternatives (§10.2)");
                    continue;
                }
                auto uit = unguardedClaim.find(v);
                if (uit != unguardedClaim.end()) {
                    ctx_.diagnostics().report(
                        Severity::Error, ref.loc,
                        "this `case` arm is unreachable: `" +
                        std::string(ref.name) + "` denotes the same "
                        "underlying value as `" + std::string(uit->second) +
                        "`, already matched by an earlier unguarded arm "
                        "(§10.2)");
                    continue;
                }
                seenThisArm.emplace(v, ref.name);
            }
            if (!arm.guard) {
                for (const auto& [v, name] : seenThisArm)
                    unguardedClaim.emplace(v, name);
            }
        }
    }
    // §19.3 (#1399): a Range<T> scrutinee is a struct value, not a scalar. A
    // RangePattern, RelOpPattern, or non-null ConstantPattern matched against a
    // Range scrutinee would reach codegen's ICmp/FCmp with mismatched operand
    // types and abort the backend (SIGABRT in ICmpInst::AssertOK). Reject here
    // with a clean type-mismatch diagnostic. `_`, `var x`, and type-test
    // patterns remain valid and unaffected. per §19.3 (#1399)
    if (scrutTy && scrutTy->kind == TypeRef::Kind::Named &&
            scrutTy->declaration == builtinClassSym("Range")) {
        for (const auto& arm : arms) {
            const ast::Pattern* p = arm.pattern.get();
            if (!p) continue;
            if (dynamic_cast<const ast::RangePattern*>(p) ||
                    dynamic_cast<const ast::RelOpPattern*>(p)) {
                ctx_.diagnostics().report(
                    Severity::Error, p->span.begin,
                    "range or relational pattern is not compatible with a "
                    "Range-typed scrutinee \xe2\x80\x94 a Range value is a "
                    "struct, not a scalar; use `_` or `var x` to match "
                    "(\xc2\xa7""19.3)");
            } else if (auto* cp =
                           dynamic_cast<const ast::ConstantPattern*>(p)) {
                for (const auto& c : cp->constants) {
                    if (!c) continue;
                    // null is the nullable-scrutinee arm and is handled by
                    // the nullable-unwrap path; only non-null constants would
                    // reach ICmp and crash the backend.
                    if (!dynamic_cast<const ast::NullLiteralExpr*>(c.get()))
                        ctx_.diagnostics().report(
                            Severity::Error, c->span.begin,
                            "constant pattern is not compatible with a "
                            "Range-typed scrutinee \xe2\x80\x94 a Range value "
                            "is a struct, not a scalar; use `_` or `var x` "
                            "to match (\xc2\xa7""19.3)");
                }
            } else if (dynamic_cast<const ast::AltPattern*>(p)) {
                // §19.3 (v0.183, #1584): an alternative list always carries a
                // range/relational member for a Range-typed scrutinee — the
                // same struct-vs-scalar mismatch that would crash the backend.
                ctx_.diagnostics().report(
                    Severity::Error, p->span.begin,
                    "range or relational pattern is not compatible with a "
                    "Range-typed scrutinee \xe2\x80\x94 a Range value is a "
                    "struct, not a scalar; use `_` or `var x` to match "
                    "(\xc2\xa7""19.3)");
            }
        }
        return;
    }
    if (!scrutTy || scrutTy->kind != TypeRef::Kind::Primitive) return;
    long long lo = 0, hi = 0;
    if (!caseIntScrutineeRange(scrutTy->primitive, lo, hi)) return;
    auto checkConst = [&](const ast::Expr* c) {
        if (!c) return;
        std::optional<long long> v = foldIntConstExprTC(*c);
        if (!v) return;  // non-constant / non-integer: not gated here
        if (*v < lo || *v > hi)
            ctx_.diagnostics().report(
                Severity::Error, c->span.begin,
                "the `case` pattern constant `" + std::to_string(*v) +
                "` has a type that is not implicitly convertible to the "
                "scrutinee type (§5.1) — the value does not fit the "
                "scrutinee's integer range and would otherwise be silently "
                "truncated to its width");
    };
    std::function<void(const ast::Pattern*)> checkPatternConsts =
        [&](const ast::Pattern* p) {
        if (!p) return;
        if (auto* cp = dynamic_cast<const ast::ConstantPattern*>(p)) {
            for (const auto& c : cp->constants) checkConst(c.get());
        } else if (auto* rp = dynamic_cast<const ast::RangePattern*>(p)) {
            if (auto* re =
                    dynamic_cast<const ast::RangeExpr*>(rp->rangeExpr.get())) {
                checkConst(re->lo.get());
                checkConst(re->hi.get());
            }
        } else if (auto* relp = dynamic_cast<const ast::RelOpPattern*>(p)) {
            // #1156: a relational pattern (`> EXPR`, `<= EXPR`, …) compares the
            // scrutinee against a constant operand. If that operand does not fit
            // the scrutinee's integer type, codegen's litAtWidth() silently
            // truncates it to the scrutinee width — e.g. `case x (x: byte) > 300`
            // compiles to `x > 44` (300 → i8 44), a wrong match. Gate the operand
            // with the same §5.1 narrowing diagnostic as the constant/range bounds.
            checkConst(relp->expr.get());
        } else if (auto* ap = dynamic_cast<const ast::AltPattern*>(p)) {
            // §19.3 (v0.183, #1584): the same §5.1 narrowing gate applies to
            // every member of a range/relational alternative list.
            for (const auto& m : ap->members) checkPatternConsts(m.get());
        }
    };
    for (const auto& arm : arms) checkPatternConsts(arm.pattern.get());
}

std::optional<long long> ExprTC::foldIntConstExprTC(const ast::Expr& e) {
    std::function<std::optional<long long>(std::string_view, int)> resolve =
        [&](std::string_view name, int d) -> std::optional<long long> {
        Symbol* s = lookupName(name);
        if (!s) return std::nullopt;
        const ast::Expr* val = nullptr;
        if (auto* lc = dynamic_cast<const ast::LocalConstDecl*>(s->decl))
            val = lc->value.get();
        else if (auto* cd = dynamic_cast<const ast::ConstDecl*>(s->decl))
            val = cd->value.get();
        if (!val) return std::nullopt;
        return foldIntConst(*val, resolve, d + 1);
    };
    return foldIntConst(e, resolve, 0);
}

void ExprTC::foldLocalConstArraySize(TypeRef* declType) {
    if (!declType || declType->kind != TypeRef::Kind::InlineArray) return;
    if (declType->arraySizeValue >= 0) return;  // already folded (literal/member)
    if (!declType->arraySize) return;
    // Resolve a bare identifier to an in-scope `const`'s int value. lookupName
    // walks the lexical scope chain (local consts) and the enclosing type
    // (member consts); either decl form carries the initializer expression.
    std::function<std::optional<long long>(std::string_view, int)> resolve =
        [&](std::string_view name, int d) -> std::optional<long long> {
        Symbol* s = lookupName(name);
        if (!s) return std::nullopt;
        const ast::Expr* val = nullptr;
        if (auto* lc = dynamic_cast<const ast::LocalConstDecl*>(s->decl))
            val = lc->value.get();
        else if (auto* cd = dynamic_cast<const ast::ConstDecl*>(s->decl))
            val = cd->value.get();
        if (!val) return std::nullopt;
        return foldIntConst(*val, resolve, d + 1);
    };
    if (auto v = foldIntConst(*declType->arraySize, resolve, 0)) {
        // §15.1 (#953): N must be greater than zero. A local-`const`-sized
        // `[N]T` with N <= 0 reaches here (member/literal sizes are caught at
        // type resolution). Report once, using the size expression's location.
        if (*v <= 0) {
            ctx_.diagnostics().report(
                Severity::Error, declType->arraySize->span.begin,
                "an inline-array size must be greater than zero — "
                "`[0]T` is not a valid type (§15.1)");
        } else {
            declType->arraySizeValue = *v;
        }
    }
}

// §12.7: a single `<simple var> is T` / `is not T` type-test contributes a
// narrowing to one branch. `x is T` narrows the true branch; `x is not T`
// narrows the else branch (the spec: in the true branch of `is not T` the
// value could be many non-T types, so no narrowing). Returns true iff an entry
// was written into narrowings_.
bool ExprTC::narrowFromTypeTest(const ast::Expr* cond, bool forTrueBranch) {
    auto* te = dynamic_cast<const ast::TypeTestExpr*>(cond);
    if (!te) return false;
    // Only the type-class tests `is` / `is not` narrow here. (The
    // nullability tests `is null` / `is not null` change nullability, not the
    // type-class declaration; they are deferred — see collectNarrowings.)
    bool narrowsThis;
    if (te->op == "is") narrowsThis = forTrueBranch;       // true branch
    else if (te->op == "is not") narrowsThis = !forTrueBranch;  // else branch
    else return false;                                     // `as` etc.
    if (!narrowsThis) return false;
    // The tested expression must be a SIMPLE VARIABLE (§12.7). We narrow
    // locals and parameters — the unambiguous "simple variable" case. (A bare
    // field is a `this.f` access whose narrowing would need cross-object
    // invalidation tracking we don't have; deferred — see collectNarrowings.)
    auto* id = dynamic_cast<const ast::IdentifierExpr*>(te->operand.get());
    if (!id || id->name == "this" || id->name == "base") return false;
    Symbol* sym = lookupName(id->name);
    if (!sym) return false;
    if (!dynamic_cast<LocalSymbol*>(sym) &&
        !dynamic_cast<ParameterSymbol*>(sym))
        return false;
    // The narrowed type is the test type. It must resolve to a Named type with
    // a declaration so member lookup can run against it. The narrowed form is
    // non-nullable (a successful `is T` means the value is a non-null T).
    if (!te->testType) return false;
    TypeRef* tt = ctx_.resolvedType(*te->testType);
    if (!tt || tt->kind != TypeRef::Kind::Named || !tt->declaration)
        return false;
    TypeRef* narrowed = tt;
    if (tt->nullable) {
        narrowed = ctx_.makeTypeRef(*tt);
        narrowed->nullable = false;
    }
    narrowings_[sym] = narrowed;
    return true;
}

// The resolved declared TypeRef of a local or parameter, or null for any other
// symbol kind (issue #418). Locals carry an already-resolved declType; params
// carry an ast::Type that must be looked up (or a context-supplied
// resolvedDeclType for inferred lambda params).
TypeRef* ExprTC::declaredTypeOf(Symbol* sym) {
    if (auto* loc = dynamic_cast<LocalSymbol*>(sym)) {
        return loc->declType;
    }
    if (auto* par = dynamic_cast<ParameterSymbol*>(sym)) {
        if (par->declType) return ctx_.resolvedType(*par->declType);
        return par->resolvedDeclType;
    }
    return nullptr;
}

// §7.4 (issue #418): a simple variable is known non-null when null-narrowing
// has proved it so (`nullNarrowed_`), when a §12.7 type-narrowing is in effect
// (those always narrow to a non-nullable form), or when its declared type is
// itself non-nullable.
bool ExprTC::isKnownNonNull(Symbol* sym) {
    if (!sym) return false;
    if (nullNarrowed_.count(sym)) return true;
    if (auto it = narrowings_.find(sym); it != narrowings_.end())
        return it->second && !it->second->nullable;
    TypeRef* dt = declaredTypeOf(sym);
    return dt && !dt->nullable;
}

// §7.4 (issue #418): refresh `nullNarrowed_` for `sym := value`. A non-null
// value proves the variable non-null from here; a nullable value (including a
// bare `null` literal, typed nullable) clears any prior knowledge.
void ExprTC::narrowOnAssignment(Symbol* sym, const TypeRef* valueType) {
    if (!sym) return;
    if (!dynamic_cast<LocalSymbol*>(sym) &&
        !dynamic_cast<ParameterSymbol*>(sym))
        return;
    if (valueType && !valueType->nullable)
        nullNarrowed_.insert(sym);
    else
        nullNarrowed_.erase(sym);
}

// §7.4 / §8.4 (issue #418): a `<simple var> <> null` / `= null` test proves a
// nullable simple variable non-null in one branch by recording it in
// `nullNarrowed_`. The parser desugars `x is not null` to `x <> null` and
// `x is null` to `x = null` (Parser_Exprs §17.4), so handling the two binary
// operators covers all four spec spellings. `<> null` narrows the TRUE branch;
// `= null` narrows the ELSE branch (where the value is known non-null). Returns
// true iff a narrowing was recorded. Unlike §12.7 type-narrowing this does NOT
// rewrite the variable's exprType (codegen relies on the declared nullable type
// to locate the tagged-optional payload).
bool ExprTC::narrowFromNullTest(const ast::Expr* cond, bool forTrueBranch) {
    auto* be = dynamic_cast<const ast::BinaryExpr*>(cond);
    if (!be) return false;
    bool narrowsThis;
    if (be->op == "<>") narrowsThis = forTrueBranch;       // true branch
    else if (be->op == "=") narrowsThis = !forTrueBranch;  // else branch
    else return false;
    if (!narrowsThis) return false;
    // One operand must be the null literal and the other a simple variable
    // (§7.4: narrowing applies to local variables only, never fields). Accept
    // `x <> null` and `null <> x`.
    const ast::Expr* varSide = nullptr;
    if (dynamic_cast<const ast::NullLiteralExpr*>(be->right.get()))
        varSide = be->left.get();
    else if (dynamic_cast<const ast::NullLiteralExpr*>(be->left.get()))
        varSide = be->right.get();
    if (!varSide) return false;
    auto* id = dynamic_cast<const ast::IdentifierExpr*>(varSide);
    if (!id || id->name == "this" || id->name == "base") return false;
    Symbol* sym = lookupName(id->name);
    if (!sym) return false;
    if (!dynamic_cast<LocalSymbol*>(sym) &&
        !dynamic_cast<ParameterSymbol*>(sym))
        return false;
    nullNarrowed_.insert(sym);
    return true;
}

// §22.4 (v0.183, #1585): caller-side narrowing for an out-nullability
// postcondition. When the condition is a call to a method that declares
// `when true` / `when false` on an `out`/`out owned` parameter, the out
// recipient is known non-null in the branch where the call's `bool` result
// equals the stated constant. Out arguments are positional (§22.5) and an out
// parameter can never follow an optional parameter (§22.5), so arg[j] maps to
// param[j] for every out parameter. The call has already been visited, so
// `ce->callee` resolves to the chosen overload and each out argument's `value`
// expression carries its recipient symbol.
bool ExprTC::narrowFromOutPostcond(const ast::Expr* cond, bool forTrueBranch) {
    auto* ce = dynamic_cast<const ast::CallExpr*>(cond);
    if (!ce || !ce->callee) return false;
    auto* ms = dynamic_cast<MethodSymbol*>(ctx_.exprSymbol(*ce->callee));
    if (!ms) return false;
    bool any = false;
    for (std::size_t j = 0;
         j < ms->params.size() && j < ce->args.size(); ++j) {
        auto* ps = dynamic_cast<ParameterSymbol*>(ms->params[j]);
        if (!ps || ps->outNullPostcond == ast::OutNullWhen::None) continue;
        const bool whenTrue =
            ps->outNullPostcond == ast::OutNullWhen::WhenTrue;
        if (whenTrue != forTrueBranch) continue;
        const auto& a = ce->args[j];
        // Only an out-form argument produces a narrowable recipient. (Guards
        // against a positional misalignment in an already-erroring program.)
        if (!(a.modifier == "out" || a.modifier == "out var" ||
              a.modifier == "out using var" || a.modifier == "out owned var"))
            continue;
        if (!a.value) continue;
        Symbol* recip = ctx_.exprSymbol(*a.value);
        if (!recip) continue;
        if (!dynamic_cast<LocalSymbol*>(recip) &&
            !dynamic_cast<ParameterSymbol*>(recip))
            continue;
        nullNarrowed_.insert(recip);
        any = true;
    }
    return any;
}

// §22.4 (v0.183, #1585): callee-side postcondition check at a return point.
void ExprTC::checkOutPostcondAtReturn(const ast::Expr* returnValue,
                                      SourceLocation loc) {
    if (currentOutPostconds_.empty() || !returnValue) return;
    // A literal `true`/`false` return value fixes which constant this path
    // produces; any other expression may produce either.
    const auto* lit = dynamic_cast<const ast::BoolLiteralExpr*>(returnValue);
    for (const auto& [psym, whenTrue] : currentOutPostconds_) {
        // A path that provably returns the OTHER constant carries no obligation
        // (it typically assigns `null`).
        if (lit && lit->value != whenTrue) continue;
        // Apply the §7 facts that hold when the returned expression equals the
        // stated constant, then require the parameter provably non-null. Save
        // and restore the flow state so this probe does not leak.
        auto savedNull = nullNarrowed_;
        auto savedNarrow = narrowings_;
        collectNarrowings(returnValue, /*forTrueBranch=*/whenTrue);
        const bool ok = isKnownNonNull(psym);
        nullNarrowed_ = std::move(savedNull);
        narrowings_ = std::move(savedNarrow);
        if (!ok) {
            const std::string cst = whenTrue ? "true" : "false";
            ctx_.diagnostics().report(
                Severity::Error, loc,
                "the `out` parameter `" + std::string(psym->name) +
                    "` must be provably non-null on every path that returns `" +
                    cst + "` — its `when " + cst +
                    "` nullability postcondition is not satisfied here (§22.4)");
        }
    }
}

// §12.7: collect every flow-narrowing an `if`/`elif` condition establishes for
// one branch into narrowings_. A single type test narrows per
// narrowFromTypeTest; a single null test narrows per narrowFromNullTest (§7.4 /
// §8.4); `and`-joined tests (`x is T and y is S`, `a <> null and b <> null`)
// contribute all their narrowings to the combined true branch. The caller has
// already visited the condition (so operand symbols and test types are
// resolved) and is responsible for save/restore of narrowings_ around the
// branch body.
void ExprTC::collectNarrowings(const ast::Expr* cond, bool forTrueBranch) {
    if (!cond) return;
    if (narrowFromTypeTest(cond, forTrueBranch)) return;
    if (narrowFromNullTest(cond, forTrueBranch)) return;
    // §22.4 (v0.183, #1585): a `Try`-style call with an out-nullability
    // postcondition narrows its out recipient on the branch matching the
    // stated constant.
    if (narrowFromOutPostcond(cond, forTrueBranch)) return;
    // `and`-joined tests: both sides narrow throughout the combined true
    // branch (§12.7). This does not extend to `or` (where neither side is
    // guaranteed) nor to the else branch of an `and` (De Morgan would make it
    // an `or` of negations — no single narrowing holds); those are deferred.
    if (auto* be = dynamic_cast<const ast::BinaryExpr*>(cond)) {
        if (be->op == "and" && forTrueBranch) {
            collectNarrowings(be->left.get(), /*forTrueBranch=*/true);
            collectNarrowings(be->right.get(), /*forTrueBranch=*/true);
        }
    }
}

void ExprTC::visitTopDecl(const ast::TopDecl& d) {
    if (auto* td = dynamic_cast<const ast::TypeDecl*>(&d)) {
        TypeSymbol* tsym = nullptr;
        if (ctx_.module()) {
            for (Symbol* child : ctx_.module()->children) {
                if (auto* ts = dynamic_cast<TypeSymbol*>(child)) {
                    if (ts->decl == td) { tsym = ts; break; }
                }
            }
        }
        TypeSymbol* save = currentType_;
        currentType_ = tsym;
        // §10.2/§10.3 (#740): an enum member value is a "compile-time constant
        // of the underlying type", so an unsuffixed integer literal that
        // exceeds `int` must widen to the enum's underlying type (long/ulong/
        // uint) — exactly as a typed local initializer does (issue #184) —
        // rather than being rejected for want of an `L`/`U` suffix. The smaller
        // underlying types (byte/short/…) already fit in `int`, so they need no
        // hint; their range is checked separately.
        PrimitiveType savedEnumHint = currentLiteralWideningHint_;
        if (tsym && tsym->enumUnderlyingType &&
            tsym->enumUnderlyingType->kind == TypeRef::Kind::Primitive) {
            using P = PrimitiveType;
            switch (tsym->enumUnderlyingType->primitive) {
                case P::Long: case P::ULong: case P::UInt:
                    currentLiteralWideningHint_ =
                        tsym->enumUnderlyingType->primitive;
                    break;
                default: break;
            }
        }
        for (const auto& ec : td->enumConstants) {
            if (ec.value) visitExpr(*ec.value);
        }
        currentLiteralWideningHint_ = savedEnumHint;
        for (const auto& m : td->members) {
            if (m) visitMember(*m);
        }
        // §11.1 / #873: visit primaryBaseCtorArgs so that identifier
        // expressions like `v` in `class D(v: int): Base(v)` get their
        // exprSymbol attached.  These expressions live in the same scope as
        // field initializers (primary-ctor params are reachable via
        // currentType_->members as FieldSymbols with posParam != null).
        // Without this pass, lowerExpr returns nullptr for those identifiers
        // because ctx_.exprSymbol() is null → base fields stay zero.
        if (td->kind == ast::TypeDeclKind::Class &&
            td->hasPositionalFields &&
            !td->primaryBaseCtorArgs.empty()) {
            bool saveIFI = inFieldInitializer_;
            inFieldInitializer_ = true;
            Scope* saveScope = currentScope_;
            if (!currentScope_) currentScope_ = moduleScope_.get();
            for (const auto& arg : td->primaryBaseCtorArgs) {
                if (arg.value) visitExpr(*arg.value);
            }
            currentScope_ = saveScope;
            inFieldInitializer_ = saveIFI;
        }
        currentType_ = save;
    } else if (auto* ext = dynamic_cast<const ast::ExtensionDecl*>(&d)) {
        // §26.1: inside an extension block, `this` is the receiver. For an
        // extension on a user class/record/enum, point currentType_ at the
        // target's TypeSymbol so the body's `this.member` and bare-member
        // references resolve against the extended type (exactly as a real
        // method body does). For a primitive/builtin extension the target has
        // no user TypeSymbol; currentType_ stays null (a primitive has no
        // member set — `this` is used as a value, e.g. `this * 2`).
        TypeSymbol* targetSym = nullptr;
        if (ext->targetType) {
            if (auto* rt = ctx_.resolvedType(*ext->targetType))
                if (rt->kind == TypeRef::Kind::Named)
                    targetSym = rt->declaration;
        }
        // Find the ExtensionSymbol for this decl so paramSymbolsOf can locate
        // the extension methods' parameters (they live on the ExtensionSymbol,
        // not on the extended type).
        ExtensionSymbol* extSym = nullptr;
        if (ctx_.module()) {
            for (Symbol* child : ctx_.module()->children) {
                if (auto* es = dynamic_cast<ExtensionSymbol*>(child)) {
                    if (es->decl == ext) { extSym = es; break; }
                }
            }
        }
        TypeSymbol* save = currentType_;
        ExtensionSymbol* saveExt = currentExtension_;
        currentType_ = targetSym;
        currentExtension_ = extSym;
        for (const auto& m : ext->members) {
            if (!m) continue;
            // §26.3 (#936): an extension block may not declare an operator
            // overload. Without this gate the declaration was silently accepted
            // and then ignored at call sites (the built-in operator was used),
            // leaving an invalid declaration with no diagnostic.
            if (auto* od = dynamic_cast<const ast::OperatorDecl*>(m.get())) {
                ctx_.diagnostics().report(
                    Severity::Error, od->span.begin,
                    "an extension block may not declare an operator overload "
                    "`operator " + std::string(od->opToken) + "` (§26.3)");
                continue;
            }
            // §26.3 (v0.183, #1581): an extension block may not declare a
            // `static` member. An extension block adds receiver-directed
            // members only; statics resolve against the extended type's own
            // declarations (§22.12), and admitting extension statics would
            // need its own resolution/merging/ambiguity story for no
            // recorded need. Write a module-level method or a static class
            // instead.
            {
                const std::vector<std::string_view>* mods = nullptr;
                SourceLocation loc{};
                if (auto* md = dynamic_cast<const ast::MethodDecl*>(m.get())) {
                    mods = &md->modifiers; loc = md->span.begin;
                } else if (auto* pd =
                               dynamic_cast<const ast::PropertyDecl*>(m.get())) {
                    mods = &pd->modifiers; loc = pd->span.begin;
                }
                bool isStaticMember = false;
                if (mods) {
                    for (auto mod : *mods) {
                        if (mod == "static") { isStaticMember = true; break; }
                    }
                }
                if (isStaticMember) {
                    ctx_.diagnostics().report(
                        Severity::Error, loc,
                        "extension blocks may not declare `static` members; "
                        "statics resolve against the extended type's own "
                        "declarations — write a module-level method or a "
                        "static class instead (§26.3)");
                    continue;
                }
            }
            // §26.3 (#1346, v0.160): extension members must be `public` or
            // `internal`; `protected` and `private` are compile errors — an
            // extension block has no declaring class and no inheritance, so
            // §13.1's definitions for those levels do not apply.
            {
                ast::Visibility vis = ast::Visibility::None;
                SourceLocation loc{};
                if (auto* md = dynamic_cast<const ast::MethodDecl*>(m.get())) {
                    vis = md->visibility; loc = md->span.begin;
                } else if (auto* pd =
                               dynamic_cast<const ast::PropertyDecl*>(m.get())) {
                    vis = pd->visibility; loc = pd->span.begin;
                }
                if (vis == ast::Visibility::Protected ||
                    vis == ast::Visibility::Private) {
                    ctx_.diagnostics().report(
                        Severity::Error, loc,
                        "extension members must be `public` or `internal`; "
                        "`" + std::string(vis == ast::Visibility::Protected
                                          ? "protected" : "private") +
                        "` is not permitted — an extension block has no "
                        "declaring class (§26.3)");
                }
            }
            // §26.3 (#1346, v0.160): extension properties are expression-bodied
            // only (`property Name: Type => expr;`); the block-accessor and
            // auto-property forms are not admitted in extension blocks.
            if (auto* pd = dynamic_cast<const ast::PropertyDecl*>(m.get())) {
                if (pd->bodyKind != ast::PropertyBodyKind::Expression) {
                    std::string_view formName =
                        (pd->bodyKind == ast::PropertyBodyKind::Block)
                        ? "block-accessor" : "auto-property";
                    ctx_.diagnostics().report(
                        Severity::Error, pd->span.begin,
                        "extension properties must be expression-bodied "
                        "(`property Name: Type => expr;`); the " +
                        std::string(formName) +
                        " form is not admitted in extension blocks (§26.3)");
                    continue;
                }
            }
            visitMember(*m);
        }
        currentType_ = save;
        currentExtension_ = saveExt;
    }
}

// Param-symbol lookup for the in-progress method/ctor on currentType_.
const std::vector<Symbol*>* ExprTC::paramSymbolsOf(const ast::Member& m) const {
    // §26.1: inside an extension block, the method's params live on the
    // ExtensionSymbol, not on currentType_ (the extended type). Search there
    // first when type-checking an extension body.
    if (currentExtension_) {
        for (Symbol* child : currentExtension_->members) {
            if (!child || child->decl != &m) continue;
            if (auto* ms = dynamic_cast<MethodSymbol*>(child))
                return &ms->params;
        }
    }
    if (!currentType_) return nullptr;
    for (Symbol* child : currentType_->members) {
        if (!child->decl) continue;
        if (child->decl == &m) {
            if (auto* ms = dynamic_cast<MethodSymbol*>(child))
                return &ms->params;
            if (auto* cs = dynamic_cast<ConstructorSymbol*>(child))
                return &cs->params;
        }
    }
    return nullptr;
}

namespace {

// §7.4 (v0.182, #1566): syntactic scan collecting the names of enclosing-scope
// locals/parameters that are mutated — assigned, or passed `ref`/`out` — inside
// any lambda in a method body. The spec's mutably-captured rule is deliberately
// conservative: any lambda in the body counts, whether or not it escapes, so no
// escape or reachability analysis is needed. A lambda's own parameters shadow an
// outer name for the whole lambda body, so they are excluded (a stack of active
// lambda-parameter names). Lambda-local `var`s are NOT excluded — over-recording
// is the sound direction (it can only invalidate a narrowing the spec would also
// drop) and precise block-scope tracking of lambda locals is unnecessary here.
struct MutablyCapturedScan {
    explicit MutablyCapturedScan(std::unordered_set<std::string_view>& o)
        : out(o) {}
    std::unordered_set<std::string_view>& out;
    int lambdaDepth = 0;
    std::vector<std::string_view> shadowed;  // active lambda-parameter names

    bool isShadowed(std::string_view n) const {
        for (auto s : shadowed)
            if (s == n) return true;
        return false;
    }
    // §27.4 (v0.183, #1408): a destructuring lambda parameter has no single
    // `p.name` — its slots are the shadowing names instead (`(Entry(k, v)) =>
    // ...` shadows an outer `k`/`v` for the lambda body exactly as a plain
    // named parameter would). Recurse into nested LambdaDestructure slots.
    void collectDestructureNames(const ast::DestructuringPattern* dp) {
        if (!dp) return;
        for (const auto& sp : dp->subPatterns) {
            if (!sp.nakedBinding.empty()) {
                shadowed.push_back(sp.nakedBinding);
            } else if (auto* nested = dynamic_cast<
                           const ast::DestructuringPattern*>(sp.pattern.get())) {
                collectDestructureNames(nested);
            }
        }
    }
    // Record a bare-identifier mutation target when inside a lambda and the name
    // is not one of the enclosing lambdas' parameters.
    void recordTarget(const ast::Expr* target) {
        if (lambdaDepth == 0 || !target) return;
        if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(target)) {
            if (id->name.empty() || id->name == "_" || id->name == "this" ||
                id->name == "base")
                return;
            if (!isShadowed(id->name)) out.insert(id->name);
        }
    }
    void argList(const std::vector<ast::CallArgument>& as) {
        for (const auto& a : as) {
            // `ref` / `out` / `out owned` mutate an EXISTING variable → capture.
            // The `out var` / `out using var` / `out owned var` forms declare a
            // FRESH local (not a capture of an outer var), so they don't record.
            if (a.modifier == "ref" || a.modifier == "out" ||
                a.modifier == "out owned")
                recordTarget(a.value.get());
            if (a.value) expr(a.value.get());
        }
    }
    void body(const std::vector<std::unique_ptr<ast::Stmt>>& b) {
        for (const auto& s : b) stmt(s.get());
    }
    void expr(const ast::Expr* e);
    void stmt(const ast::Stmt* s);
};

void MutablyCapturedScan::expr(const ast::Expr* e) {
    if (!e) return;
    if (auto* lam = dynamic_cast<const ast::LambdaExpr*>(e)) {
        std::size_t mark = shadowed.size();
        for (const auto& p : lam->params) {
            if (!p.name.empty()) shadowed.push_back(p.name);
            else if (p.destructure) collectDestructureNames(p.destructure.get());
        }
        ++lambdaDepth;
        if (lam->exprBody) expr(lam->exprBody.get());
        body(lam->stmts);
        --lambdaDepth;
        shadowed.resize(mark);
        return;
    }
    if (auto* be = dynamic_cast<const ast::BinaryExpr*>(e)) {
        expr(be->left.get());
        expr(be->right.get());
    } else if (auto* ue = dynamic_cast<const ast::UnaryExpr*>(e)) {
        expr(ue->operand.get());
    } else if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(e)) {
        expr(ma->target.get());
    } else if (auto* nm = dynamic_cast<const ast::NullCondMemberExpr*>(e)) {
        expr(nm->target.get());
    } else if (auto* ce = dynamic_cast<const ast::CallExpr*>(e)) {
        expr(ce->callee.get());
        argList(ce->args);
    } else if (auto* nc = dynamic_cast<const ast::NullCondCallExpr*>(e)) {
        expr(nc->callee.get());
        argList(nc->args);
    } else if (auto* ix = dynamic_cast<const ast::IndexExpr*>(e)) {
        expr(ix->target.get());
        argList(ix->indices);
    } else if (auto* pb = dynamic_cast<const ast::PostfixBangExpr*>(e)) {
        expr(pb->operand.get());
    } else if (auto* ne = dynamic_cast<const ast::NewExpr*>(e)) {
        argList(ne->args);
    } else if (auto* tt = dynamic_cast<const ast::TypeTestExpr*>(e)) {
        expr(tt->operand.get());
    } else if (auto* co = dynamic_cast<const ast::ConditionalExpr*>(e)) {
        expr(co->thenValue.get());
        expr(co->condition.get());
        expr(co->elseValue.get());
    } else if (auto* th = dynamic_cast<const ast::ThrowExpr*>(e)) {
        expr(th->operand.get());
    } else if (auto* rg = dynamic_cast<const ast::RangeExpr*>(e)) {
        expr(rg->lo.get());
        expr(rg->hi.get());
        expr(rg->step.get());
    } else if (auto* we = dynamic_cast<const ast::WithExpr*>(e)) {
        expr(we->source.get());
        for (const auto& ia : we->assigns) expr(ia.value.get());
    } else if (auto* cl = dynamic_cast<const ast::CollectionLiteralExpr*>(e)) {
        for (const auto& k : cl->keys) expr(k.get());
        for (const auto& v : cl->elements) expr(v.get());
    } else if (auto* is = dynamic_cast<const ast::InterpolatedStringExpr*>(e)) {
        for (const auto& part : is->parts)
            if (part.expr) expr(part.expr.get());
    } else if (auto* cx = dynamic_cast<const ast::CaseExpr*>(e)) {
        expr(cx->scrutinee.get());
        for (const auto& arm : cx->arms) {
            expr(arm.guard.get());
            if (arm.exprBody) expr(arm.exprBody.get());
            body(arm.stmts);
        }
    }
    // Leaf / type-only exprs (identifiers, literals, typeof/nameof/default,
    // unit) hold no sub-expression that can contain a lambda; nothing to do.
}

void MutablyCapturedScan::stmt(const ast::Stmt* s) {
    if (!s) return;
    if (auto* ld = dynamic_cast<const ast::LocalDecl*>(s)) {
        expr(ld->initializer.get());
    } else if (auto* lc = dynamic_cast<const ast::LocalConstDecl*>(s)) {
        expr(lc->value.get());
    } else if (auto* rs = dynamic_cast<const ast::ReturnStmt*>(s)) {
        expr(rs->value.get());
    } else if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(s)) {
        expr(es->expr.get());
    } else if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(s)) {
        recordTarget(as->target.get());  // `x := …` inside a lambda captures x
        expr(as->target.get());
        expr(as->value.get());
    } else if (auto* ts = dynamic_cast<const ast::ThrowStmt*>(s)) {
        expr(ts->value.get());
    } else if (auto* ifs = dynamic_cast<const ast::IfStmt*>(s)) {
        expr(ifs->ifBranch.condition.get());
        body(ifs->ifBranch.body);
        for (const auto& el : ifs->elifBranches) {
            expr(el.condition.get());
            body(el.body);
        }
        body(ifs->elseBody);
    } else if (auto* ws = dynamic_cast<const ast::WhileStmt*>(s)) {
        expr(ws->condition.get());
        body(ws->body);
    } else if (auto* fs = dynamic_cast<const ast::ForStmt*>(s)) {
        expr(fs->iterable.get());
        expr(fs->withExpr.get());
        body(fs->body);
    } else if (auto* de = dynamic_cast<const ast::DoEndBlock*>(s)) {
        body(de->body);
    } else if (auto* inc = dynamic_cast<const ast::IncrementStmt*>(s)) {
        recordTarget(inc->target.get());  // `x++` / `x--` inside a lambda mutates x
        expr(inc->target.get());
    } else if (auto* ds = dynamic_cast<const ast::DiscardStmt*>(s)) {
        expr(ds->value.get());
    } else if (auto* ys = dynamic_cast<const ast::YieldStmt*>(s)) {
        expr(ys->value.get());
    } else if (auto* trys = dynamic_cast<const ast::TryStmt*>(s)) {
        body(trys->body);
        for (const auto& c : trys->catches) {
            expr(c.guard.get());
            stmt(c.body.get());
        }
        body(trys->finallyBody);
    } else if (auto* cs = dynamic_cast<const ast::CaseStmt*>(s)) {
        expr(cs->scrutinee.get());
        for (const auto& arm : cs->arms) {
            expr(arm.guard.get());
            if (arm.exprBody) expr(arm.exprBody.get());
            body(arm.stmts);
        }
    }
    // BreakStmt / ContinueStmt carry no sub-nodes.
}

}  // namespace

// §7.4 (v0.182, #1566): recompute the mutably-captured-name set for a body.
void ExprTC::computeMutablyCaptured(
    const std::vector<std::unique_ptr<ast::Stmt>>* stmts,
    const ast::Expr* exprBody) {
    mutablyCapturedNames_.clear();
    MutablyCapturedScan scan{mutablyCapturedNames_};
    if (exprBody) scan.expr(exprBody);
    if (stmts) scan.body(*stmts);
}

// §7.4 (v0.182, #1566): drop the null-narrowing (and any §12.7 type-narrowing)
// of every currently-narrowed local whose name is mutably captured. The narrowed
// symbol is a local/parameter; a same-named write inside a lambda of this body
// means a closure invoked by the control-transfer operation just executed could
// have nulled it (the field paragraph's "a closure invoked in between"). No-op
// when nothing is captured or nothing is narrowed.
void ExprTC::invalidateCapturedNarrowings() {
    if (mutablyCapturedNames_.empty()) return;
    for (auto it = nullNarrowed_.begin(); it != nullNarrowed_.end();) {
        Symbol* s = *it;
        if (s && mutablyCapturedNames_.count(s->name))
            it = nullNarrowed_.erase(it);
        else
            ++it;
    }
    for (auto it = narrowings_.begin(); it != narrowings_.end();) {
        Symbol* s = it->first;
        if (s && mutablyCapturedNames_.count(s->name))
            it = narrowings_.erase(it);
        else
            ++it;
    }
}

// Build a method scope (params on top of module scope), then visit
// body. Locals are registered into this scope as visitStmt walks
// LocalDecls.
void ExprTC::runMethodBody(
    const std::vector<ast::Param>& params,
    const std::vector<std::unique_ptr<ast::Stmt>>* stmts,
    const ast::Expr* exprBody,
    const std::vector<Symbol*>* paramSyms) {
    Scope methodScope;
    methodScope.parent = moduleScope_.get();
    if (paramSyms) {
        for (std::size_t i = 0;
             i < params.size() && i < paramSyms->size(); ++i) {
            if ((*paramSyms)[i] && !params[i].name.empty()) {
                methodScope.names[params[i].name] = (*paramSyms)[i];
            }
        }
    }
    Scope* save = currentScope_;
    currentScope_ = &methodScope;
    // §12.7: flow narrowings are method-local — start each body with none.
    // (The if-statement walk save/restores the map per controlled block, so
    // this is normally already empty; clearing makes the boundary explicit and
    // robust to symbol reuse across methods.)
    narrowings_.clear();
    // §7.4 (issue #418): nullability narrowing is likewise method-local.
    nullNarrowed_.clear();
    // §7.4 (v0.182, #1566): precompute which locals/params are mutably captured
    // by a lambda anywhere in THIS body, so a control-transfer operation inside a
    // controlled block can invalidate their narrowing (invalidateCapturedNarrowings).
    computeMutablyCaptured(stmts, exprBody);
    // §22.4 (v0.183, #1585): collect the body's out-nullability postcondition
    // params so each `return E;` (and an expression body) can be checked. Only
    // a bool-returning method carries these (the admittance check in
    // TypeResolution rejects the clause elsewhere). Saved/restored so a nested
    // callable body does not inherit this body's obligations.
    auto savedPostconds = std::move(currentOutPostconds_);
    currentOutPostconds_.clear();
    const bool retIsBool =
        currentReturnType_ &&
        currentReturnType_->kind == TypeRef::Kind::Primitive &&
        currentReturnType_->primitive == PrimitiveType::Bool;
    if (retIsBool && paramSyms) {
        for (std::size_t i = 0;
             i < params.size() && i < paramSyms->size(); ++i) {
            if (params[i].outNullPostcond == ast::OutNullWhen::None) continue;
            // Only a genuine out parameter carries the obligation; a stray
            // clause on a non-out parameter is already an admittance error
            // (TypeResolution), so skip it here rather than cascade.
            if (params[i].modifier != "out" && params[i].modifier != "out owned")
                continue;
            if (auto* ps = dynamic_cast<ParameterSymbol*>((*paramSyms)[i]))
                currentOutPostconds_.emplace_back(
                    ps, params[i].outNullPostcond == ast::OutNullWhen::WhenTrue);
        }
    }
    // §22.5 (issue #1160): a default-value expression may read parameters
    // appearing EARLIER in the same parameter list; a reference to the
    // parameter itself or to a LATER one is a forward reference that would read
    // an uninitialized slot, and is rejected. While checking parameter i's
    // default, mark params i..n-1 as forbidden (lookupName records the hit in
    // forwardParamRef_), then report at the default expression.
    for (std::size_t i = 0; i < params.size(); ++i) {
        if (!params[i].defaultValue) continue;
        forbiddenForwardParams_.clear();
        if (paramSyms) {
            for (std::size_t j = i;
                 j < params.size() && j < paramSyms->size(); ++j) {
                if ((*paramSyms)[j])
                    forbiddenForwardParams_.insert((*paramSyms)[j]);
            }
        }
        forwardParamRef_.clear();
        visitExpr(*params[i].defaultValue);
        if (!forwardParamRef_.empty()) {
            ctx_.diagnostics().report(
                Severity::Error, params[i].defaultValue->span.begin,
                std::string("a default-value expression may not reference "
                            "parameter `") +
                    forwardParamRef_ +
                    "` — a default may read only parameters declared earlier "
                    "in the same list, not the parameter itself or a later one "
                    "(§22.5)");
        }
    }
    forbiddenForwardParams_.clear();
    if (exprBody) {
        visitExpr(*exprBody);
        // §22.4 (v0.183, #1585): the expression body is the returned value; it
        // must satisfy any out-nullability postcondition. (Realistically an
        // out-param method has a statement body — an expression body cannot
        // assign the out param — so this is a conservative belt-and-braces.)
        checkOutPostcondAtReturn(exprBody, exprBody->span.begin);
    }
    if (stmts) {
        for (const auto& s : *stmts) {
            if (s) visitStmt(*s);
        }
    }
    currentScope_ = save;
    // §22.4 (v0.183, #1585): restore the enclosing body's postcondition set.
    currentOutPostconds_ = std::move(savedPostconds);
}

void ExprTC::visitMember(const ast::Member& m) {
    if (auto* fd = dynamic_cast<const ast::FieldDecl*>(&m)) {
        if (fd->initializer) {
            // Slice S29 (§22.8): a field initializer may not observe
            // instance state.
            bool save = inFieldInitializer_;
            inFieldInitializer_ = true;
            // fix #582 / §22.13: a static field initializer may reference
            // other classes by name (e.g. `B := OtherClass.Constant + 1`).
            // moduleScope_ holds all module-level type names; make it reachable
            // via currentScope_ so lookupName("OtherClass") finds the
            // TypeSymbol.  Without this, currentScope_ is null (field
            // initializers have no enclosing method scope), and the
            // cross-class qualifier resolves to null → lowerExpr returns null
            // → the runtime store is skipped → field stays zero.
            Scope* saveScope = currentScope_;
            if (!currentScope_) currentScope_ = moduleScope_.get();
            // per §4.2 (v0.159/v0.182): a typed field initializer is an
            // admitting typed context — an unsuffixed literal exceeding
            // `int`'s range but fitting the field's declared wider integer
            // type (long/ulong/uint) widens to it, mirroring the typed-local
            // hint (#184) instead of being rejected for want of a suffix.
            PrimitiveType savedFieldHint = currentLiteralWideningHint_;
            if (fd->type) {
                if (auto* frt = ctx_.resolvedType(*fd->type)) {
                    if (frt->kind == TypeRef::Kind::Primitive) {
                        using P = PrimitiveType;
                        switch (frt->primitive) {
                            case P::Long: case P::ULong: case P::UInt:
                                currentLiteralWideningHint_ = frt->primitive;
                                break;
                            default: break;
                        }
                    }
                }
            }
            visitExpr(*fd->initializer);
            currentLiteralWideningHint_ = savedFieldHint;
            // §17.7 (#987, mirrors the LocalDecl fix #979): a typeless
            // `new(args)` shorthand field initializer takes its construction
            // target from the field's declared type. Record it as the NewExpr's
            // exprType (codegen's lowerNew and the ownership pass read it) and
            // validate the constructor arguments — without this the shorthand
            // new was never typed, codegen skipped construction, and the field
            // stayed null (SIGSEGV on first access).
            if (fd->type) {
                if (auto* shNe = dynamic_cast<const ast::NewExpr*>(
                        fd->initializer.get())) {
                    if (!shNe->type) {
                        if (auto* fty = ctx_.resolvedType(*fd->type)) {
                            ctx_.setExprType(fd->initializer.get(), fty);
                            if (fty->kind == TypeRef::Kind::Named &&
                                fty->declaration &&
                                fty->declaration->kind ==
                                    ast::TypeDeclKind::Class)
                                checkNewExprArgs(*shNe, fty->declaration);
                        }
                    }
                }
            }
            currentScope_ = saveScope;
            inFieldInitializer_ = save;
            // §22.13: a static readonly field may not forward-reference a later
            // static readonly field of the same type.
            checkStaticReadonlyForwardRef(fd);
        }
    } else if (auto* md = dynamic_cast<const ast::MethodDecl*>(&m)) {
        const std::vector<std::unique_ptr<ast::Stmt>>* stmts =
            (md->bodyKind == ast::MethodBodyKind::Block) ? &md->stmts
                                                         : nullptr;
        const ast::Expr* expr =
            (md->bodyKind == ast::MethodBodyKind::Expression)
                ? md->exprBody.get() : nullptr;
        // Slice S6: enter static context if this method is static.
        bool isStatic = false;
        for (auto mod : md->modifiers) {
            if (mod == "static") { isStatic = true; break; }
        }
        bool save = inStaticMember_;
        inStaticMember_ = isStatic;
        // issue #484 / §14.2: expose the in-progress MethodDecl so that
        // resolveMemberAccess can walk its generic constraints when the
        // receiver is a type parameter (e.g. `x: T` where `where T: ISome`).
        const ast::MethodDecl* saveMD = currentMethodDecl_;
        currentMethodDecl_ = md;
        ReturnCtx saveRet = returnCtx_;
        TypeRef* saveRetTy = currentReturnType_;
        // async-readiness 3b (§30.2): an `async` method body is an async
        // context, where `await` is valid.
        bool saveAsync = inAsyncContext_;
        inAsyncContext_ = false;
        for (auto mod : md->modifiers) {
            if (mod == "async") { inAsyncContext_ = true; break; }
        }
        // async-readiness 3b (§30.1): an async method's return-FORM rules and
        // the type a `return EXPR;` is checked against are driven by the
        // task's *result* type, not the task wrapper itself:
        //   async ...: Task          -> behaves like a void method — bare
        //                               `return;` is allowed, and falling off
        //                               the end is `return;`; `return EXPR;`
        //                               is rejected (the task has no result).
        //   async ...: Task<T> /
        //              OwningTask<T>  -> behaves like a value method — must
        //                               `return EXPR;` on every path, with
        //                               EXPR assignable to the element type T.
        //   async ...: IAsyncEnumerable<T> / ... (async iterator) -> an
        //                               iterator method (it uses `yield`);
        //                               exempt from the value-return check
        //                               via `isIteratorMethod` below, exactly
        //                               like a synchronous iterator (§29.3).
        if (inAsyncContext_) {
            asyncReturnCtxAndType(md->returnType.get(), returnCtx_,
                                  currentReturnType_);
        } else {
            returnCtx_ = returnsValue(md->returnType.get())
                ? ReturnCtx::ValueMethod : ReturnCtx::VoidMethod;
            currentReturnType_ = md->returnType
                ? ctx_.resolvedType(*md->returnType) : nullptr;
        }
        // §29.3/§29.5 (#721): a method using `yield` is an iterator. Set the
        // flag BEFORE visiting the body so checkReturnForm can reject a plain
        // `return;` (the async iterator case slips past §21.7). Restore after.
        bool isIteratorMethod = stmts && stmtsContainYield(*stmts);
        bool saveIter = inIteratorMethod_;
        inIteratorMethod_ = isIteratorMethod;
        runMethodBody(md->params, stmts, expr, paramSymbolsOf(m));
        inIteratorMethod_ = saveIter;
        // Slice DR1 (§21.7): a value-returning method with a block
        // body must return (or otherwise abruptly exit) on every
        // path. Abstract / expression bodies are exempt.
        // §29.3: iterator methods (containing yield return/break) are
        // exempt — the compiler synthesizes the return mechanism.
        if (stmts && returnCtx_ == ReturnCtx::ValueMethod &&
            !isIteratorMethod &&
            !bodyAlwaysExits(*stmts)) {
            ctx_.diagnostics().report(
                Severity::Error, md->span.begin,
                std::string("method `") + std::string(md->name) +
                "` has a return type but does not return a value on "
                "every path (§21.7)");
        }
        currentReturnType_ = saveRetTy;
        returnCtx_ = saveRet;
        inAsyncContext_ = saveAsync;
        inStaticMember_ = save;
        currentMethodDecl_ = saveMD;  // issue #484 / §14.2
    } else if (auto* od = dynamic_cast<const ast::OperatorDecl*>(&m)) {
        const std::vector<std::unique_ptr<ast::Stmt>>* stmts =
            (od->bodyKind == ast::MethodBodyKind::Block) ? &od->stmts
                                                         : nullptr;
        const ast::Expr* expr =
            (od->bodyKind == ast::MethodBodyKind::Expression)
                ? od->exprBody.get() : nullptr;
        ReturnCtx saveRet = returnCtx_;
        TypeRef* saveRetTy = currentReturnType_;
        returnCtx_ = returnsValue(od->returnType.get())
            ? ReturnCtx::ValueMethod : ReturnCtx::VoidMethod;
        currentReturnType_ = od->returnType
            ? ctx_.resolvedType(*od->returnType) : nullptr;
        runMethodBody(od->params, stmts, expr, paramSymbolsOf(m));
        currentReturnType_ = saveRetTy;
        returnCtx_ = saveRet;
    } else if (auto* cd = dynamic_cast<const ast::ConstructorDecl*>(&m)) {
        bool saveStatic = inStaticMember_;
        bool saveCtor = inConstructor_;
        ReturnCtx saveRet = returnCtx_;
        // Slice S6: static constructor body has no `this`.
        inStaticMember_ = cd->isStatic;
        // Slice S7: readonly fields are assignable inside ctors.
        inConstructor_ = true;
        returnCtx_ = ReturnCtx::CtorDtor;
        runMethodBody(cd->params, &cd->body, nullptr,
                      paramSymbolsOf(m));
        returnCtx_ = saveRet;
        inStaticMember_ = saveStatic;
        inConstructor_ = saveCtor;
    } else if (auto* dd = dynamic_cast<const ast::DestructorDecl*>(&m)) {
        std::vector<ast::Param> none;
        ReturnCtx saveRet = returnCtx_;
        returnCtx_ = ReturnCtx::CtorDtor;
        runMethodBody(none, &dd->body, nullptr, nullptr);
        returnCtx_ = saveRet;
    } else if (auto* pd = dynamic_cast<const ast::PropertyDecl*>(&m)) {
        std::vector<ast::Param> none;
        // §22.10: for an indexer property, find the PropertySymbol's
        // indexerParams so they're visible inside accessor bodies.
        const std::vector<Symbol*>* idxSymsPtr = nullptr;
        std::vector<Symbol*> idxSyms;
        if (pd->isIndexer && currentType_) {
            for (Symbol* mem : currentType_->members) {
                if (auto* ps = dynamic_cast<PropertySymbol*>(mem)) {
                    if (ps->isIndexer && ps->decl == pd) {
                        idxSyms = ps->indexerParams;
                        idxSymsPtr = &idxSyms;
                        break;
                    }
                }
            }
        }
        // Use pd->indexerParams as the param reference (move-only but
        // we only need its .name field in runMethodBody).
        const auto& pdParams = pd->isIndexer ? pd->indexerParams : none;
        if (pd->exprBody)
            runMethodBody(pdParams, nullptr, pd->exprBody.get(),
                          idxSymsPtr);
        if (pd->autoInitializer) {
            // §22.8 applies to auto-property initializers too.
            bool save = inFieldInitializer_;
            inFieldInitializer_ = true;
            // #1088 (mirrors the #582 / §22.13 fix for FIELD initializers): an
            // auto-property initializer may reference other classes by name
            // (`property P: int := C.Default();`, a static method/field). It has
            // no enclosing method scope, so without moduleScope_ as
            // currentScope_ the class qualifier resolves to null → no exprSymbol
            // → codegen lowerExpr returns null → the init store is skipped → the
            // backing field stays zero. (A foldable constant initializer needs
            // no name resolution, which is why it worked.)
            Scope* saveScope = currentScope_;
            if (!currentScope_) currentScope_ = moduleScope_.get();
            // per §4.2 (v0.159/v0.182, #1590): an auto-property default
            // initializer initializes the compiler-generated backing field
            // (§23.1) at the same construction-time step §22.8 governs for
            // an explicit field initializer — §11.1 and the design
            // rationale's cyclic-serialization rule both speak of "field and
            // property initializers" together as one construction-time
            // concept. It is therefore an admitting typed context: an
            // unsuffixed literal exceeding `int`'s range but fitting the
            // property's declared wider integer type (long/ulong/uint)
            // widens to it, mirroring the FieldDecl fix (#1576) instead of
            // being rejected for want of a suffix.
            PrimitiveType savedPropHint = currentLiteralWideningHint_;
            if (pd->type) {
                if (auto* prt = ctx_.resolvedType(*pd->type)) {
                    if (prt->kind == TypeRef::Kind::Primitive) {
                        using P = PrimitiveType;
                        switch (prt->primitive) {
                            case P::Long: case P::ULong: case P::UInt:
                                currentLiteralWideningHint_ = prt->primitive;
                                break;
                            default: break;
                        }
                    }
                }
            }
            visitExpr(*pd->autoInitializer);
            currentLiteralWideningHint_ = savedPropHint;
            currentScope_ = saveScope;
            inFieldInitializer_ = save;
        }
        for (const auto& a : pd->accessors) {
            // §22.9: a setter accessor has an implicit `value` parameter
            // (renamable via a SetterParamSpec) of the property's type.
            // Its decl anchors on the PropertyDecl so codegen can map it
            // to the assigned value. For indexers, include indexer params.
            if (a.isSet) {
                std::string_view vname = a.setterParamName.empty()
                    ? std::string_view("value")
                    : a.setterParamName;
                auto* vsym = ctx_.makeSymbol<LocalSymbol>();
                vsym->name = vname;
                vsym->decl = pd;
                if (pd->type) vsym->declType = ctx_.resolvedType(*pd->type);
                // Build extended symbol list: indexer params + value.
                std::vector<Symbol*> ssyms;
                for (auto* s : idxSyms) ssyms.push_back(s);
                ssyms.push_back(vsym);
                // Build synthetic param list for the combined set.
                // runMethodBody just reads param[i].name to populate scope.
                std::vector<ast::Param> sp;
                for (const auto& p : pd->indexerParams) {
                    ast::Param dummy; dummy.name = p.name;
                    sp.push_back(std::move(dummy));
                }
                ast::Param vp; vp.name = vname;
                sp.push_back(std::move(vp));
                if (a.exprBody)
                    runMethodBody(sp, nullptr, a.exprBody.get(), &ssyms);
                if (!a.stmts.empty())
                    runMethodBody(sp, &a.stmts, nullptr, &ssyms);
                continue;
            }
            // Getter: include indexer params.
            if (a.exprBody)
                runMethodBody(pdParams, nullptr, a.exprBody.get(),
                              idxSymsPtr);
            if (!a.stmts.empty())
                runMethodBody(pdParams, &a.stmts, nullptr, idxSymsPtr);
        }
    } else if (auto* cd = dynamic_cast<const ast::ConstDecl*>(&m)) {
        if (cd->value) {
            // §22.12 / §A.8 (sweep #7): a const initializer may reference another
            // const or enum constant via a QUALIFIED name (`K.V`, `Color.Red`).
            // Resolving that class/enum qualifier needs moduleScope_ reachable as
            // currentScope_ (a const member has no enclosing method scope) —
            // without it `K` resolves to null, `K.V` has no exprSymbol, and
            // isConstInitExpr below falsely rejects it. Mirrors #582/#1088.
            Scope* saveScope = currentScope_;
            if (!currentScope_) currentScope_ = moduleScope_.get();
            visitExpr(*cd->value);
            currentScope_ = saveScope;
            // §22.12 (#747 R42): a `const` member's initializer must be a
            // compile-time-constant expression. Without this check an
            // initializer that read a (non-const) static field or called a
            // method was silently accepted and treated like a static-field
            // initializer (`const K := _runtime * 2;` compiled and ran).
            if (!isConstInitExpr(*cd->value)) {
                ctx_.diagnostics().report(
                    Severity::Error, cd->value->span.begin,
                    std::string("const `") + std::string(cd->name) +
                    "` must be initialized with a compile-time-constant "
                    "expression — a literal, an operation on constants, or a "
                    "reference to another const or enum constant (§22.12)");
            }
        }
    } else if (auto* nt = dynamic_cast<const ast::NestedTypeMember*>(&m)) {
        if (nt->decl) {
            TypeSymbol* nested = nullptr;
            if (currentType_) {
                for (Symbol* mem : currentType_->members) {
                    if (auto* nts =
                            dynamic_cast<NestedTypeSymbol*>(mem)) {
                        if (nts->type &&
                            nts->type->decl == nt->decl.get()) {
                            nested = nts->type; break;
                        }
                    }
                }
            }
            TypeSymbol* save = currentType_;
            currentType_ = nested;
            for (const auto& innerM : nt->decl->members) {
                if (innerM) visitMember(*innerM);
            }
            currentType_ = save;
        }
    }
}

bool ExprTC::isConstInitExpr(const ast::Expr& e) {
    // §22.12 / §A.8: literals are constants — except interpolated strings,
    // which are not compile-time constants in general.
    if (dynamic_cast<const ast::IntLiteralExpr*>(&e) ||
        dynamic_cast<const ast::FloatLiteralExpr*>(&e) ||
        dynamic_cast<const ast::StringLiteralExpr*>(&e) ||
        dynamic_cast<const ast::CharLiteralExpr*>(&e) ||
        dynamic_cast<const ast::BoolLiteralExpr*>(&e) ||
        dynamic_cast<const ast::NullLiteralExpr*>(&e))
        return true;
    if (dynamic_cast<const ast::InterpolatedStringExpr*>(&e))
        return false;
    // Operations over constants are constant.
    if (auto* u = dynamic_cast<const ast::UnaryExpr*>(&e))
        return u->operand && isConstInitExpr(*u->operand);
    if (auto* b = dynamic_cast<const ast::BinaryExpr*>(&e))
        return b->left && b->right && isConstInitExpr(*b->left) &&
               isConstInitExpr(*b->right);
    if (auto* c = dynamic_cast<const ast::ConditionalExpr*>(&e))
        return c->condition && c->thenValue && c->elseValue &&
               isConstInitExpr(*c->condition) && isConstInitExpr(*c->thenValue) &&
               isConstInitExpr(*c->elseValue);
    // A named reference is constant iff it resolves to another const member or
    // an enum constant. A (non-const) field, property, or method reference is
    // not — that is the runtime-dependent case this check rejects.
    if (dynamic_cast<const ast::IdentifierExpr*>(&e) ||
        dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        Symbol* sym = ctx_.exprSymbol(e);
        return dynamic_cast<ConstMemberSymbol*>(sym) != nullptr ||
               dynamic_cast<EnumConstantSymbol*>(sym) != nullptr;
    }
    // A `nameof(...)` expression is a compile-time-constant string.
    if (dynamic_cast<const ast::NameOfExpr*>(&e)) return true;
    // Conservative: anything else — a method call, `new`, indexing, a primitive
    // conversion (`int(x)`), or a reference to a non-const field/property — is
    // treated as not constant. This matches §A.8's `Constant` form (literals +
    // named const/enum references) plus operations over constants; admitting
    // constant-folded conversions in a `const` initializer is a possible future
    // widening, but rejecting them never accepts a non-constant value.
    return false;
}

void ExprTC::checkStaticReadonlyForwardRef(const ast::FieldDecl* fd) {
    if (!fd || !fd->initializer || !currentType_) return;
    auto hasMod = [](const std::vector<std::string_view>& mods,
                     std::string_view m) {
        for (auto x : mods) if (x == m) return true;
        return false;
    };
    if (!hasMod(fd->modifiers, "static") || !hasMod(fd->modifiers, "readonly"))
        return;
    // Declaration-order index of a symbol in the current type, or -1.
    auto indexOf = [&](const Symbol* s) -> int {
        int i = 0;
        for (const Symbol* m : currentType_->members) {
            if (m == s) return i;
            ++i;
        }
        return -1;
    };
    int iF = -1;
    for (std::size_t i = 0; i < currentType_->members.size(); ++i)
        if (auto* fs = dynamic_cast<const FieldSymbol*>(currentType_->members[i]))
            if (fs->decl == fd) { iF = static_cast<int>(i); break; }
    if (iF < 0) return;
    // A reference is offending iff it resolves to a static-readonly field of the
    // same type declared after F.
    auto laterSR = [&](Symbol* g) -> const FieldSymbol* {
        auto* fs = dynamic_cast<FieldSymbol*>(g);
        if (!fs) return nullptr;
        if (!hasMod(fs->modifiers, "static") ||
            !hasMod(fs->modifiers, "readonly"))
            return nullptr;
        return indexOf(fs) > iF ? fs : nullptr;
    };
    std::function<void(const ast::Expr*)> walk = [&](const ast::Expr* e) {
        if (!e) return;
        if (dynamic_cast<const ast::IdentifierExpr*>(e) ||
            dynamic_cast<const ast::MemberAccessExpr*>(e)) {
            if (auto* g = laterSR(ctx_.exprSymbol(*e)))
                ctx_.diagnostics().report(
                    Severity::Error, e->span.begin,
                    std::string("static readonly `") + std::string(fd->name) +
                    "` reads static readonly `" + std::string(g->name) +
                    "`, which is declared later in the same type — a forward "
                    "reference between static readonly fields is not allowed; "
                    "reorder the declarations or use a static constructor "
                    "(§22.13)");
        }
        if (auto* b = dynamic_cast<const ast::BinaryExpr*>(e)) {
            walk(b->left.get()); walk(b->right.get());
        } else if (auto* u = dynamic_cast<const ast::UnaryExpr*>(e)) {
            walk(u->operand.get());
        } else if (auto* c = dynamic_cast<const ast::ConditionalExpr*>(e)) {
            walk(c->condition.get()); walk(c->thenValue.get());
            walk(c->elseValue.get());
        } else if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(e)) {
            walk(ma->target.get());
        } else if (auto* call = dynamic_cast<const ast::CallExpr*>(e)) {
            walk(call->callee.get());
            for (const auto& a : call->args) walk(a.value.get());
        } else if (auto* ix = dynamic_cast<const ast::IndexExpr*>(e)) {
            walk(ix->target.get());
            for (const auto& a : ix->indices) walk(a.value.get());
        } else if (auto* ne = dynamic_cast<const ast::NewExpr*>(e)) {
            for (const auto& a : ne->args) walk(a.value.get());
        }
    };
    walk(fd->initializer.get());
}

void ExprTC::visitStmt(const ast::Stmt& s) {
    if (auto* ld = dynamic_cast<const ast::LocalDecl*>(&s)) {
        // Initializer evaluates BEFORE the local comes into scope.
        // §5 / issue #184: if the variable has an explicit wider integer
        // type, pass it as a hint so unsuffixed int literals can be
        // widened instead of rejected.
        PrimitiveType savedHint = currentLiteralWideningHint_;
        if (ld->type && ld->initializer) {
            if (auto* rt = ctx_.resolvedType(*ld->type)) {
                if (rt->kind == TypeRef::Kind::Primitive) {
                    using P = PrimitiveType;
                    switch (rt->primitive) {
                        case P::Long: case P::ULong:
                        case P::UInt:
                            currentLiteralWideningHint_ = rt->primitive;
                            break;
                        default: break;
                    }
                }
            }
        }
        // issue #777 / §30.6: when a typed local's declared type is a function
        // type `(…) -> Task<T>` / `(…) -> OwningTask<T>` and the initializer is an
        // `async` lambda, thread the peeled element T to the lambda visitor so it
        // can reject a forgotten `await` in the body. Restored after the visit.
        TypeRef* savedLamRet = pendingLambdaReturnType_;
        std::vector<TypeRef*> savedLamParams = pendingLambdaParamTypes_;
        if (ld->type && ld->initializer) {
            if (dynamic_cast<const ast::LambdaExpr*>(ld->initializer.get())) {
                if (auto* dt = ctx_.resolvedType(*ld->type)) {
                    if (dt->kind == TypeRef::Kind::Function && dt->funcReturn &&
                        isTaskShapedRef(dt->funcReturn) &&
                        !dt->funcReturn->genericArgs.empty())
                        pendingLambdaReturnType_ =
                            dt->funcReturn->genericArgs.front();
                    // fix #829: thread the target's positional parameter types so
                    // an untyped lambda param `(a, b) => …` is given its inferred
                    // type (the body's operator/member resolution then sees the
                    // real type instead of falling back to primitives).
                    if (dt->kind == TypeRef::Kind::Function &&
                        !dt->funcParams.empty()) {
                        pendingLambdaParamTypes_.clear();
                        for (const auto& fp : dt->funcParams)
                            pendingLambdaParamTypes_.push_back(fp.type);
                    }
                }
            }
        }
        if (ld->initializer) visitExpr(*ld->initializer);
        pendingLambdaReturnType_ = savedLamRet;
        pendingLambdaParamTypes_ = savedLamParams;
        currentLiteralWideningHint_ = savedHint;
        // stdlib §5 (#755): reject STORING a bare lazy GroupBy result in a local —
        // the lazy IGrouping<TKey,T> enumerator dangles after the store and
        // SIGSEGVs on iteration (#755 [8]). The IGrouping value model is not yet
        // implemented, so a GroupBy(...) result may currently only be consumed
        // INLINE (a direct `for var g in <seq>.GroupBy(...)`, or a terminal
        // `.ToArray()/.Count()/.Any()`), never bound to a variable. This converts
        // a clean-compile-then-crash into a clean diagnostic (decline-with-reasons
        // until the value model lands).
        if (ld->initializer) {
            if (const TypeRef* it = ctx_.exprType(*ld->initializer))
                if (isBareLazyGroupingType(it))
                    ctx_.diagnostics().report(
                        Severity::Error, ld->initializer->span.begin,
                        "a GroupBy(...) result cannot be stored in a variable yet: "
                        "the IGrouping<TKey, T> value model is not implemented "
                        "(#755). Consume it inline — iterate "
                        "`for var g in <seq>.GroupBy(...)`, or use a terminal "
                        "operator such as `.ToArray()`, `.Count()`, or `.Any()`");
        }
        if (currentScope_ && !ld->name.empty()) {
            auto* lsym = ctx_.makeSymbol<LocalSymbol>();
            lsym->name = ld->name;
            lsym->decl = ld;
            lsym->isUsing = ld->isUsing;
            lsym->isAwaitUsing = ld->isAwaitUsing;
            // Slice 3g: type inference. Explicit `: Type` wins;
            // otherwise infer from the initializer's exprType.
            // Either may be null (plain `var x;` with no init).
            if (ld->type) {
                lsym->declType = ctx_.resolvedType(*ld->type);
                // §15.1 (#734): fold a local-`const` inline-array size now that
                // the const is in scope (member/module consts were folded at
                // type resolution; locals were not yet bound there).
                foldLocalConstArraySize(lsym->declType);
                // §17.7 (#979): a typeless `new(args)` shorthand initializer
                // takes its construction target from the declared type. The
                // NewExpr typing in visitExpr only runs when `new Type` is
                // explicit, so for the shorthand the node was left untyped,
                // codegen's lowerNew skipped construction, and the uninitialized
                // local was dereferenced (SIGSEGV). Record the declared type as
                // the NewExpr's exprType (lowerNew reads it as the construction
                // target) and validate the constructor arguments against the
                // target's constructor exactly as the explicit path does.
                if (lsym->declType && ld->initializer) {
                    if (auto* shNe = dynamic_cast<const ast::NewExpr*>(
                            ld->initializer.get())) {
                        if (!shNe->type) {
                            ctx_.setExprType(ld->initializer.get(),
                                             lsym->declType);
                            if (lsym->declType->kind == TypeRef::Kind::Named &&
                                lsym->declType->declaration &&
                                lsym->declType->declaration->kind ==
                                    ast::TypeDeclKind::Class)
                                checkNewExprArgs(
                                    *shNe, lsym->declType->declaration);
                        }
                    }
                }
                // §15.4 / §15.1 (sweep #11): an inline-array literal initializer
                // must have EXACTLY N elements — "The list must contain exactly
                // N elements; mismatch is a compile error" (§15.4). Too few left
                // uninitialized slots (garbage reads); too many overran the slot
                // storage at codegen (SIGSEGV). Both compiled silently before
                // this gate. Checked once N is folded (>= 0).
                if (ld->initializer && lsym->declType &&
                    lsym->declType->kind == TypeRef::Kind::InlineArray &&
                    lsym->declType->arraySizeValue >= 0) {
                    if (auto* cl = dynamic_cast<const ast::CollectionLiteralExpr*>(
                            ld->initializer.get())) {
                        if (!cl->isDict) {
                            long long n = lsym->declType->arraySizeValue;
                            long long got =
                                static_cast<long long>(cl->elements.size());
                            if (got != n) {
                                ctx_.diagnostics().report(
                                    Severity::Error,
                                    ld->initializer->span.begin,
                                    "inline-array initializer has " +
                                    std::to_string(got) + " element(s) but the "
                                    "type requires exactly " +
                                    std::to_string(n) +
                                    " (`[" + std::to_string(n) +
                                    "]…`) (§15.4)");
                            }
                        }
                    }
                }
                // per §14.1/§14.2 + §A.7 (issue #391): return-target generic
                // type-argument inference. When the initializer is a call to a
                // generic method whose type parameter appears only in the
                // return type (`Create<T>(): T`), the typed local declaration
                // supplies the target that solves T. This re-types the call to
                // the substituted return type before the assignability check.
                if (auto* initCall = dynamic_cast<const ast::CallExpr*>(
                        ld->initializer.get())) {
                    if (lsym->declType)
                        inferReturnTargetTypeArgs(*initCall, lsym->declType,
                                                  /*outNeedsInfer=*/nullptr);
                }
                // Slice T1: when an explicit ascription and an
                // initializer are both present and both primitive,
                // check primitive assignability (§5.1).
                // §5.1 special: an integer CONSTANT EXPRESSION assigns to any
                // integer type that can represent its value — re-type it as the
                // target type so the assignability check passes. #738: gate on
                // the initializer actually being a compile-time int constant
                // (foldIntConst: a literal, arithmetic on literals, or a `const`
                // reference); a non-constant int value (a variable / call result)
                // does NOT implicitly narrow and falls through to checkAssignable,
                // which reports the §5.1 error (`var y: sbyte := someIntVar`).
                if (ld->initializer) {
                    TypeRef* fromTy = ctx_.exprType(*ld->initializer);
                    if (fromTy && lsym->declType &&
                        fromTy->kind == TypeRef::Kind::Primitive &&
                        lsym->declType->kind == TypeRef::Kind::Primitive &&
                        fromTy->primitive == PrimitiveType::Int) {
                        PrimitiveType tgt = lsym->declType->primitive;
                        bool narrowerIntTarget =
                            tgt == PrimitiveType::Byte ||
                            tgt == PrimitiveType::SByte ||
                            tgt == PrimitiveType::Short ||
                            tgt == PrimitiveType::UShort ||
                            tgt == PrimitiveType::UInt ||
                            tgt == PrimitiveType::ULong;
                        if (narrowerIntTarget) {
                            auto cv = foldIntConstExprTC(*ld->initializer);
                            // #738 / §5.1: a compile-time int constant re-types to
                            // the narrower target only when its value FITS — an
                            // out-of-range constant (e.g. `sbyte := 300`) falls
                            // through to checkAssignable, which reports the §5.1
                            // error (no silent truncation). A non-constant value
                            // never folds and is likewise rejected.
                            if (cv && fitsInIntType(*cv, tgt)) {
                                ctx_.setExprType(ld->initializer.get(),
                                                 lsym->declType);
                                fromTy = lsym->declType;
                            }
                        }
                    }
                    checkAssignable(
                        fromTy,
                        lsym->declType, ld->initializer->span.begin,
                        std::string("initializer for `") +
                        std::string(ld->name) + "`",
                        ld->initializer.get());  // §27.6 #756 property-ref
                }
            } else if (ld->initializer) {
                // per §15.4: a collection literal has no standalone type; a
                // `var x := [...]` with no type annotation gives no target type
                // and is a compile error. Must be checked BEFORE visitExpr so
                // we diagnose on the LocalDecl's position (and return early to
                // avoid cascaded inference errors).
                if (dynamic_cast<const ast::CollectionLiteralExpr*>(
                        ld->initializer.get())) {
                    ctx_.diagnostics().report(
                        Severity::Error, ld->initializer->span.begin,
                        "collection literal has no determinable target type — "
                        "add a type annotation (`var " + std::string(ld->name) +
                        ": <CollectionType> := [...]`) (§15.4)");
                    // Still visit sub-expressions so inner errors are found.
                    visitExpr(*ld->initializer);
                    currentScope_->names[ld->name] = lsym;
                    return;  // early-return: lsym->declType intentionally null
                }
                // §7.1 (#1084): a bare `null` literal carries no type of its own,
                // so `var x := null` with no annotation gives the local no
                // determinable type. Codegen later lowers a use of `x` (e.g.
                // `x ?? 99`) with mismatched LLVM types and aborts on a PHI
                // assertion (a compiler crash). Reject at the declaration,
                // directing the user to a nullable type annotation. Mirrors the
                // untyped collection-literal reject above.
                if (dynamic_cast<const ast::NullLiteralExpr*>(
                        ld->initializer.get())) {
                    ctx_.diagnostics().report(
                        Severity::Error, ld->initializer->span.begin,
                        "cannot infer a type for `" + std::string(ld->name) +
                        "` from a bare `null` — `null` has no type of its own; "
                        "add a nullable type annotation (`var " +
                        std::string(ld->name) + ": <T>? := null`) (§7.1)");
                    currentScope_->names[ld->name] = lsym;
                    return;  // early-return: lsym->declType intentionally null
                }
                lsym->declType = ctx_.exprType(*ld->initializer);
                // per §14.1 + §A.7 (issue #391): a `var x := Generic.Create();`
                // without a type annotation gives the call no target, so a
                // return-only type parameter cannot be solved. Per §15.4 /
                // §17.7 the spec withholds target inference from `var` without
                // annotation; diagnose rather than silently leaving T unsolved.
                if (auto* initCall = dynamic_cast<const ast::CallExpr*>(
                        ld->initializer.get())) {
                    reportNoTargetInferenceIfNeeded(*initCall);
                }
            }
            currentScope_->names[ld->name] = lsym;
            // §7.4 (issue #418): a nullable-declared local initialized with a
            // non-null value is known non-null until reassigned (parallel to
            // the assignment-narrowing in AssignmentStmt above).
            if (ld->initializer && lsym->declType && lsym->declType->nullable) {
                if (TypeRef* initTy = ctx_.exprType(*ld->initializer))
                    if (!initTy->nullable) nullNarrowed_.insert(lsym);
            }
        }
    } else if (auto* lc =
                   dynamic_cast<const ast::LocalConstDecl*>(&s)) {
        if (lc->value) visitExpr(*lc->value);
        // Slice S31 (§21.1 / §22.12): a local const's explicit type
        // must be a primitive value type or string.
        if (lc->type) {
            if (auto* rt = ctx_.resolvedType(*lc->type)) {
                bool ok = rt->kind == TypeRef::Kind::Primitive &&
                          rt->primitive != PrimitiveType::Object;
                if (!ok) {
                    ctx_.diagnostics().report(
                        Severity::Error, lc->span.begin,
                        std::string("const `") + std::string(lc->name) +
                        "` must have a primitive value type or "
                        "`string` (§22.12)");
                }
            }
        }
        if (currentScope_ && !lc->name.empty()) {
            auto* lsym = ctx_.makeSymbol<LocalSymbol>();
            lsym->name = lc->name;
            lsym->decl = lc;
            lsym->isConst = true;
            currentScope_->names[lc->name] = lsym;
        }
    } else if (auto* rs = dynamic_cast<const ast::ReturnStmt*>(&s)) {
        // per §4.2 (v0.182): `return` in a method with a declared wider
        // integer return type (long/ulong/uint) is an admitting typed
        // context — an unsuffixed literal exceeding `int`'s range but
        // fitting the return type widens to it, mirroring the typed-local
        // hint (#184) instead of being rejected for want of a suffix. This
        // context joined the admitting-context list in v0.182 alongside the
        // assignment-target case below.
        PrimitiveType savedRetHint = currentLiteralWideningHint_;
        if (returnCtx_ == ReturnCtx::ValueMethod && currentReturnType_ &&
            currentReturnType_->kind == TypeRef::Kind::Primitive) {
            using P = PrimitiveType;
            switch (currentReturnType_->primitive) {
                case P::Long: case P::ULong: case P::UInt:
                    currentLiteralWideningHint_ = currentReturnType_->primitive;
                    break;
                default: break;
            }
        }
        if (rs->value) visitExpr(*rs->value);
        currentLiteralWideningHint_ = savedRetHint;
        // §22.4 (v0.183, #1585): a `return E;` that may produce a postcondition's
        // stated constant must leave the out parameter provably non-null.
        checkOutPostcondAtReturn(rs->value.get(), rs->span.begin);
        // per §31.2: `return` inside a `finally` block is a compile error.
        // Appendix A: "`return` does not appear in `finally` blocks."
        if (finallyDepth_ > 0) {
            ctx_.diagnostics().report(
                Severity::Error, rs->span.begin,
                "`return` inside a `finally` block is a compile error (§31.2)");
            return;
        }
        checkReturnForm(*rs);
        // per §14.1/§14.2 + §A.7 (issue #391): return-target generic
        // type-argument inference. `return Generic.Create();` in a method with
        // a declared return type supplies that return type as the target that
        // solves a return-only type parameter (§15.4 / §17.7 recognize the
        // enclosing return type as a target when the method has one).
        if (rs->value && returnCtx_ == ReturnCtx::ValueMethod &&
            currentReturnType_) {
            if (auto* retCall = dynamic_cast<const ast::CallExpr*>(
                    rs->value.get())) {
                inferReturnTargetTypeArgs(*retCall, currentReturnType_,
                                          /*outNeedsInfer=*/nullptr);
            }
        }
        // Slice T3: a returned value must be primitive-assignable to
        // the callable's return type.
        if (rs->value && returnCtx_ == ReturnCtx::ValueMethod) {
            checkAssignable(
                ctx_.exprType(*rs->value), currentReturnType_,
                rs->value->span.begin, "returned value",
                rs->value.get());  // §27.6 #756 property-ref
        }
        // issue #777 / §30.6: forgotten `await` in an async METHOD body. For an
        // async method `(): Task<T>`, currentReturnType_ is the peeled element T
        // and inAsyncContext_ is set. `return <Task<U>>` (a bare un-awaited
        // producer call) is silently accepted by checkAssignable (Task has no
        // declaration, so the §5.1 path falls through) and then codegen stores
        // the 8-byte task pointer into the element-sized result slot → memory
        // corruption / SIGSEGV. Reject it with a clear diagnostic, unless the
        // element T is itself Task-shaped (a legitimate `Task<Task<U>>` result).
        if (rs->value && returnCtx_ == ReturnCtx::ValueMethod &&
            inAsyncContext_ && currentReturnType_ &&
            !isTaskShapedRef(currentReturnType_) &&
            isTaskShapedRef(ctx_.exprType(*rs->value))) {
            ctx_.diagnostics().report(
                Severity::Error, rs->value->span.begin,
                "an `async` method's `return` must produce the element type of "
                "its `Task<T>`/`OwningTask<T>` return, not a `Task` value; add "
                "`await` (e.g. `return await Producer();`) (§30.6)");
        }
    } else if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(&s)) {
        if (es->expr) visitExpr(*es->expr);
        // per §14.1 + §A.7 (issue #391): a bare expression statement discards
        // the result, so a return-only type parameter has no target. Diagnose.
        if (es->expr) {
            if (auto* exCall =
                    dynamic_cast<const ast::CallExpr*>(es->expr.get()))
                reportNoTargetInferenceIfNeeded(*exCall);
        }
        // §21.9 (#743): a bare expression statement that DISCARDS a freshly
        // OWNED value requiring disposal silently abandons the disposal
        // obligation — a leak. For a non-trivial-dispose class (a destructor
        // anywhere in its chain) or a move-only owning array, this is a hard
        // error. GATE on the value being owned-PRODUCED here — `new T()` or a
        // call whose method returns `owned T` — so the ubiquitous borrowed-
        // return statement `obj.Foo();` is never false-rejected (the caller
        // does not own a borrowed result). Mirrors the §17.13 discard check.
        if (es->expr) {
            bool ownedProduced = false;
            if (dynamic_cast<const ast::NewExpr*>(es->expr.get())) {
                ownedProduced = true;
            } else if (auto* ce =
                           dynamic_cast<const ast::CallExpr*>(es->expr.get())) {
                if (ce->callee)
                    if (auto* sym = ctx_.exprSymbol(*ce->callee))
                        if (auto* ms = dynamic_cast<MethodSymbol*>(sym))
                            if (auto* md = dynamic_cast<const ast::MethodDecl*>(
                                    ms->decl))
                                ownedProduced = md->returnOwned;
                // §21.9 (#832): a builtin owning-collection DRAIN — Pop /
                // Dequeue / PopFirst / PopLast / PopAt / PopValue on an Owned*
                // collection — returns `owned T` but has no user MethodDecl, so
                // returnOwned is unavailable. Detect it by the member name plus
                // the receiver being an Owned* collection (Get/Peek BORROW the
                // element and are excluded). Without this, a bare drain statement
                // silently leaked the popped owned element.
                if (!ownedProduced && ce->callee) {
                    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(
                            ce->callee.get())) {
                        std::string_view mm = ma->member;
                        bool drain = mm == "Pop" || mm == "Dequeue" ||
                                     mm == "PopFirst" || mm == "PopLast" ||
                                     mm == "PopAt" || mm == "PopValue";
                        if (drain && ma->target) {
                            if (auto* rt = ctx_.exprType(*ma->target))
                                if (rt->kind == TypeRef::Kind::Named &&
                                    rt->astNode && rt->astNode->name &&
                                    !rt->astNode->name->parts.empty()) {
                                    std::string_view head =
                                        rt->headName();
                                    ownedProduced =
                                        head == "OwningArray" ||
                                        head == "OwningStack" ||
                                        head == "OwningQueue" ||
                                        head == "OwningLinkedList" ||
                                        head == "OwningDictionary";
                                }
                        }
                    }
                }
            }
            // §21.9 / §2.11 (#949): an `<event>.Subscribe(...)` statement drops
            // the owned Subscription it returns. Its result is untyped, so the
            // ownedProduced/type-based path above misses it — recognize it
            // structurally and reject directly.
            bool subDrop = isEventSubscribeDrop(es->expr.get());
            if (subDrop) ownedProduced = true;
            if (ownedProduced) {
                const TypeRef* t = ctx_.exprType(*es->expr);
                bool reject = subDrop;
                if (t && t->kind == TypeRef::Kind::InlineArray && t->arrayOwned) {
                    reject = true;
                } else if (t && !t->nullable &&
                           t->kind == TypeRef::Kind::Named && t->declaration &&
                           t->declaration->kind == ast::TypeDeclKind::Class &&
                           !t->declaration->isValueBuiltin) {
                    for (TypeSymbol* cur = t->declaration; cur && !reject;) {
                        for (Symbol* m : cur->members)
                            if (dynamic_cast<DestructorSymbol*>(m)) {
                                reject = true;
                                break;
                            }
                        TypeSymbol* nextBase = nullptr;
                        for (TypeSymbol* b : cur->baseTypes)
                            if (b && b->kind == ast::TypeDeclKind::Class) {
                                nextBase = b;
                                break;
                            }
                        cur = nextBase;
                    }
                }
                if (reject) {
                    ctx_.diagnostics().report(
                        Severity::Error, es->expr->span.begin,
                        "this expression produces an owned value that requires "
                        "disposal (a non-trivial-dispose class or a move-only "
                        "owning array); discarding it as a statement leaks the "
                        "resource — bind it (`using var _ := expression;`), call "
                        "`.Dispose()`, or transfer it (`move`/`return`) (§21.9)");
                }
            }
        }
        // §21.9 (#1497): a bare `Task`/`Task<T>`/`OwningTask<T>` expression
        // statement — e.g. `Work();` where `Work(): Task` — silently drops the
        // task: no diagnostic at all, not even the generic §21.9 discard
        // WARNING, because the "unowned, non-() result" warning path never
        // matched Task in the first place. §21.9 says the warning is replaced
        // by a hard error for Task, the same as the non-trivial-dispose class
        // arm just above; that class arm is gated on `ownedProduced`, which a
        // plain (non-`owned`) async call never sets, so Task fell through
        // entirely. Checked directly on the statement's type so it fires
        // regardless of how the Task was produced (call, await of a
        // Task<Task<T>>, etc.). Remedy order follows Appendix C.3 (spec:8532)
        // / §17.13's `_ := taskExpr;` hard error below (#1014): `await` or
        // `Task.FireAndForget` lead, since discarding/disposing a task
        // abandons a running operation — `using var` is the fallback, not the
        // first-offered fix, unlike the class arm above.
        if (es->expr) {
            const TypeRef* taskStmtTy = ctx_.exprType(*es->expr);
            // A `move <task>;` statement is an explicit ownership transfer that
            // DISCHARGES an owned Task binding (e.g. discharging an `owned t:
            // Task<T>` parameter after `await`), governed by §28.6/§28.8 — not a
            // silent drop. §21.9 targets the case where a Task result vanishes
            // with no acknowledgement (a bare producing call, an identifier
            // read, a task-typed property access); a `move` is the programmer
            // acknowledging the transfer, so it is excluded here.
            auto* moveExpr = dynamic_cast<const ast::UnaryExpr*>(es->expr.get());
            bool isMove = moveExpr && moveExpr->op == "move";
            if (isTaskShapedRef(taskStmtTy) && !isMove) {
                ctx_.diagnostics().report(
                    Severity::Error, es->expr->span.begin,
                    "this expression statement discards a `Task` result — an "
                    "unawaited task's exceptions and lifetime are silently "
                    "dropped; `await` it, use `Task.FireAndForget(expression);` "
                    "for supervised fire-and-forget, or bind it explicitly "
                    "(`using var t := expression;`) to dispose it at scope end "
                    "(§21.9)");
            }
        }
    } else if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(&s)) {
        // §12.7: "Reassignment within the controlled block invalidates the
        // narrowing." If the assignment target is the narrowed variable, drop
        // its narrowing from this point so the rest of the block re-widens it to
        // its declared type (this also makes the assignability check below use
        // the declared target type, not the narrowed one). The map is restored
        // when the block ends. Only a bare-identifier target can match a
        // narrowed simple variable.
        if (as->target) {
            if (auto* tid = dynamic_cast<const ast::IdentifierExpr*>(
                    as->target.get())) {
                if (tid->name != "this" && tid->name != "base") {
                    if (Symbol* tsym = lookupName(tid->name))
                        narrowings_.erase(tsym);
                }
            }
        }
        // §23 (#1192): mark the plain-`:=` target node so resolveMemberAccess
        // exempts it from the write-only-property read diagnostic (a plain `:=`
        // to a set-only property is a write; a compound `op=` reads first, so it
        // is NOT exempt). Restored after the target is visited.
        const ast::Expr* savedPlainSet = plainSetAssignTargetNode_;
        plainSetAssignTargetNode_ = (as->op == ":=") ? as->target.get() : nullptr;
        if (as->target) visitExpr(*as->target);
        plainSetAssignTargetNode_ = savedPlainSet;
        // §23.5: "Assigning to a member of a value-typed indexer result is a
        // compile error: `g[0].X := 5` would modify a discarded copy." A
        // value-typed indexer get returns a COPY (records/enums are value
        // types — §9), so a member write to it is silently lost. Reject when
        // the assignment target is `<index>.member` and the index expression's
        // result is a value type (a record or enum). A class/interface indexer
        // result is a reference — `g[0].X := …` writes through it to the real
        // object — so that case stays accepted.
        if (auto* ma =
                dynamic_cast<const ast::MemberAccessExpr*>(as->target.get())) {
            if (auto* ix =
                    dynamic_cast<const ast::IndexExpr*>(ma->target.get())) {
                if (TypeRef* rt = ctx_.exprType(*ix)) {
                    bool valueTyped =
                        rt->kind == TypeRef::Kind::Named && rt->declaration &&
                        (rt->declaration->kind == ast::TypeDeclKind::Record ||
                         rt->declaration->kind == ast::TypeDeclKind::Enum);
                    if (valueTyped)
                        ctx_.diagnostics().report(
                            Severity::Error, as->target->span.begin,
                            "assigning to a member of a value-typed indexer "
                            "result modifies a discarded copy — the indexer "
                            "returns a value, so the write is lost; read it "
                            "into a local, modify it, then write the whole "
                            "value back through the indexer setter (§23.5)");
                }
            }
            // §23.5 (#1312/#1313): the same rule for a value-typed PROPERTY
            // result. A property GET returns a COPY of a value type
            // (records/enums are value types, §9), so `h.P.X := …` modifies a
            // discarded copy — the write is lost (a block property never sees
            // its setter; an auto-property mutates in place, bypassing value-copy
            // semantics). Reject when the target is `<propertyAccess>.member` and
            // the property's result is value-typed. A value-typed FIELD is an
            // lvalue stored inline, so `h.f.X := …` writes through and stays
            // accepted — only a PROPERTY get (auto or block) returns a copy.
            else if (auto* innerMa = dynamic_cast<const ast::MemberAccessExpr*>(
                         ma->target.get())) {
                if (dynamic_cast<PropertySymbol*>(ctx_.exprSymbol(*innerMa))) {
                    if (TypeRef* rt = ctx_.exprType(*innerMa)) {
                        bool valueTyped =
                            rt->kind == TypeRef::Kind::Named && rt->declaration &&
                            (rt->declaration->kind == ast::TypeDeclKind::Record ||
                             rt->declaration->kind == ast::TypeDeclKind::Enum);
                        if (valueTyped)
                            ctx_.diagnostics().report(
                                Severity::Error, as->target->span.begin,
                                "assigning to a member of a value-typed property "
                                "result modifies a discarded copy — the property "
                                "get returns a value, so the write is lost (and "
                                "bypasses the setter); read it into a local, "
                                "modify it, then assign the whole value back "
                                "through the property setter (§23.5)");
                    }
                }
            }
        }
        // §6.5: a compound bitwise/shift assignment (`&= |= ^= <<= >>=`) carries
        // the same operand-type restriction as the binary bitwise operators — it
        // is not valid on a `bool` / `char` / floating-point target. Compound
        // assignments do NOT route through typeBinary, so guard here.
        if (as->target &&
            (as->op == "&=" || as->op == "|=" || as->op == "^=" ||
             as->op == "<<=" || as->op == ">>=")) {
            if (TypeRef* tt = ctx_.exprType(*as->target)) {
                if (tt->kind == TypeRef::Kind::Primitive) {
                    PrimitiveType p = tt->primitive;
                    const char* badKind =
                        (p == PrimitiveType::Float || p == PrimitiveType::Double)
                            ? "floating-point types"
                        : (p == PrimitiveType::Bool)
                            ? "`bool` (boolean operations use `and`/`or`/`not`)"
                        : (p == PrimitiveType::Char) ? "`char`" : nullptr;
                    if (badKind)
                        ctx_.diagnostics().report(Severity::Error, as->span.begin,
                            std::string("operator `") + std::string(as->op) +
                            "` is not valid on " + badKind + " — bitwise and "
                            "shift operators require integer operands (§6.5)");
                }
            }
        }
        // per §24.4: `event += handler` / `event -= handler` — type the
        // handler lambda's param as the event payload `T` (these forms desugar
        // to Add/Remove; they are not real compound assignments).
        TypeRef* eventPayload = nullptr;
        if ((as->op == "+=" || as->op == "-=") && as->target) {
            if (auto* es = dynamic_cast<EventSymbol*>(
                    ctx_.exprSymbol(*as->target))) {
                // per §24.3: the `+=` / `-=` forms desugar to Add / Remove,
                // which live on the EventSource<T> face — available only
                // inside the declaring class (and inside a derived class for a
                // `protected` event). Outside, the event is IEvent<T>, which
                // has no Add / Remove.
                checkEventVisibilityFence(es, as->op, as->span.begin);
                if (auto* ed = dynamic_cast<const ast::EventDecl*>(es->decl))
                    if (ed->type) eventPayload = ctx_.resolvedType(*ed->type);
            }
        }
        if (as->value) {
            TypeRef* savedPending = pendingLambdaParamType_;
            if (eventPayload &&
                dynamic_cast<const ast::LambdaExpr*>(as->value.get()))
                pendingLambdaParamType_ = eventPayload;
            // per §4.2 (v0.182): an assignment target whose declared type is
            // a wider integer type (long/ulong/uint) is an admitting typed
            // context — an unsuffixed literal exceeding `int`'s range but
            // fitting the target's type widens to it, mirroring the typed-
            // local hint (#184) instead of being rejected for want of a
            // suffix. `as->target` was already visited above, so its
            // exprType is available. This context joined the admitting-
            // context list in v0.182 alongside the `return` case.
            PrimitiveType savedAssignHint = currentLiteralWideningHint_;
            if (as->target) {
                if (TypeRef* tgtTy = ctx_.exprType(*as->target)) {
                    if (tgtTy->kind == TypeRef::Kind::Primitive) {
                        using P = PrimitiveType;
                        switch (tgtTy->primitive) {
                            case P::Long: case P::ULong: case P::UInt:
                                currentLiteralWideningHint_ = tgtTy->primitive;
                                break;
                            default: break;
                        }
                    }
                }
            }
            visitExpr(*as->value);
            currentLiteralWideningHint_ = savedAssignHint;
            pendingLambdaParamType_ = savedPending;
        }
        // per §24.4: an event += / -= is not a real compound assignment, so
        // the readonly / assignability checks below do not apply to it.
        if (!eventPayload) {
            // §4.15 (#549) / §4.14 (#779): ReadOnlyArraySpan<T>[i] := v and
            // ReadOnlySpan<T>[i] := v are disallowed — a read-only span has no
            // indexer setter.
            if (as->op == ":=" && as->target) {
                if (auto* ix = dynamic_cast<const ast::IndexExpr*>(
                        as->target.get())) {
                    if (ix->target) {
                        if (auto* rt = ctx_.exprType(*ix->target)) {
                            if (rt->kind == TypeRef::Kind::Named &&
                                !rt->declaration &&
                                rt->astNode && rt->astNode->name &&
                                !rt->astNode->name->parts.empty() &&
                                (rt->headName() ==
                                    "ReadOnlyArraySpan" ||
                                 rt->headName() ==
                                    "ReadOnlySpan")) {
                                ctx_.diagnostics().report(
                                    Severity::Error, as->span.begin,
                                    "cannot assign through a read-only span "
                                    "indexer — a read-only span has no setter "
                                    "(§4.14 / §4.15)");
                            }
                        }
                    }
                }
            }
            // fix #830 / §28.10: `using var` is implicitly non-reassignable —
            // reassigning a `using var` local is a compile error. Before this
            // fix the reassignment was accepted silently and only the SECOND
            // resource was disposed at scope end; the first leaked. Detect any
            // assignment (`:=`, `+=`, …) whose target is a local declared
            // `using var`. (A `_` discard target is not a using-var rebind.)
            if (as->target) {
                if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                        as->target.get())) {
                    if (id->name != "_") {
                        if (auto* loc = dynamic_cast<LocalSymbol*>(
                                ctx_.exprSymbol(*as->target))) {
                            if (loc->isUsing)
                                ctx_.diagnostics().report(
                                    Severity::Error, as->span.begin,
                                    std::string("cannot reassign `using var` "
                                    "local `") + std::string(id->name) +
                                    "` — `using var` is implicitly non-"
                                    "reassignable; use a new `do ... end` block "
                                    "for a different resource (§28.10)");
                        }
                    }
                }
            }
            // §9.4 (#624): DictionaryEntry<K,V>.Key and .Value are positional
            // read-only fields — any assignment through the entry loop variable
            // (e.Key := …, e.Value := …, e.Value += …, etc.) must be rejected.
            // The synthetic DictionaryEntry TypeRef has importedName="DictionaryEntry"
            // and no user TypeSymbol (declaration == nullptr); detect by that tag.
            if (as->target) {
                if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(
                        as->target.get())) {
                    if (ma->member == "Key" || ma->member == "Value") {
                        if (ma->target) {
                            if (auto* tt = ctx_.exprType(*ma->target)) {
                                if (tt->kind == TypeRef::Kind::Named &&
                                        !tt->declaration &&
                                        tt->importedName == "DictionaryEntry") {
                                    ctx_.diagnostics().report(
                                        Severity::Error, as->span.begin,
                                        std::string("cannot assign to `") +
                                        std::string(ma->member) +
                                        "` — DictionaryEntry<K,V> fields are "
                                        "read-only positional fields (§9.4)");
                                }
                            }
                        }
                    }
                }
            }
            // §23.3 (#747 R42): assignment to a property with NO setter — an
            // expression-bodied property (`P: T => e;`) or a block-form property
            // with only a getter — is rejected outright (no setter exists).
            // §23.1 (#747 R42): a readonly AUTO-property has an implicit setter,
            // but is assignable only in a field initializer or constructor (like
            // a readonly field), so a write OUTSIDE a constructor is also
            // rejected. Without these the write was silently accepted/dropped.
            // per §23.1 (#1173): the assignment target may be a qualified
            // member access (`this.X`, `obj.X`) OR an UNQUALIFIED implicit-this
            // identifier (`X := 9` inside the declaring class). Both resolve to
            // the same PropertySymbol and must obey the readonly / no-setter /
            // setter-visibility rules below. Previously only the
            // MemberAccessExpr shape was guarded, so an unqualified self-write
            // to a readonly auto-property slipped through (an ACCEPT_TRAP). A
            // local variable that shadows the property resolves to a
            // LocalSymbol, so the dynamic_cast below leaves it untouched.
            if (as->target) {
                PropertySymbol* ps = nullptr;
                std::string propName;
                if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(
                        as->target.get())) {
                    if ((ps = dynamic_cast<PropertySymbol*>(
                             ctx_.exprSymbol(*as->target))))
                        propName = std::string(ma->member);
                } else if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                               as->target.get())) {
                    if ((ps = dynamic_cast<PropertySymbol*>(
                             ctx_.exprSymbol(*as->target))))
                        propName = std::string(id->name);
                }
                if (ps) {
                        if (auto* pd = dynamic_cast<const ast::PropertyDecl*>(
                                ps->decl)) {
                            bool hasSetter = true;
                            if (pd->bodyKind ==
                                ast::PropertyBodyKind::Expression) {
                                hasSetter = false;
                            } else if (pd->bodyKind ==
                                       ast::PropertyBodyKind::Block) {
                                hasSetter = false;
                                for (const auto& acc : pd->accessors)
                                    if (acc.isSet) { hasSetter = true; break; }
                            }
                            bool isReadonlyAuto = false;
                            if (pd->bodyKind == ast::PropertyBodyKind::Auto)
                                for (auto mod : pd->modifiers)
                                    if (mod == "readonly") {
                                        isReadonlyAuto = true;
                                        break;
                                    }
                            if (!hasSetter) {
                                ctx_.diagnostics().report(
                                    Severity::Error, as->span.begin,
                                    "cannot assign to read-only property `" +
                                    propName +
                                    "` — it has no setter (§23.3)");
                            } else if (isReadonlyAuto && !inConstructor_) {
                                ctx_.diagnostics().report(
                                    Severity::Error, as->span.begin,
                                    "cannot assign to readonly property `" +
                                    propName +
                                    "` outside a field initializer or "
                                    "constructor (§23.1)");
                            } else if (hasSetter) {
                                // §13.6 (#747 R42): a property write must respect
                                // the SETTER accessor's visibility, which may be
                                // more restrictive than the property's (e.g.
                                // `private set`). Compute the setter's effective
                                // visibility and apply the §13.1 access rules;
                                // without this a `private set` was writable from
                                // any class.
                                ast::Visibility sv = pd->visibility;
                                if (pd->bodyKind ==
                                    ast::PropertyBodyKind::Block) {
                                    for (const auto& acc : pd->accessors)
                                        if (acc.isSet &&
                                            acc.visibility !=
                                                ast::Visibility::None) {
                                            sv = acc.visibility;
                                            break;
                                        }
                                } else if (pd->bodyKind ==
                                               ast::PropertyBodyKind::Auto &&
                                           pd->hasAutoAccessorVis &&
                                           pd->autoAccessorIsSet) {
                                    sv = pd->autoAccessorVis;
                                }
                                TypeSymbol* declaring = ps->parent;
                                bool bad = false;
                                if (sv == ast::Visibility::Private)
                                    bad = (currentType_ != declaring);
                                else if (sv == ast::Visibility::Protected)
                                    bad = !(currentType_ == declaring ||
                                            (currentType_ &&
                                             isSubtype(currentType_,
                                                       declaring)));
                                else if (sv == ast::Visibility::Internal)
                                    bad = ctx_.isImported(ps);
                                if (bad) {
                                    ctx_.diagnostics().report(
                                        Severity::Error, as->span.begin,
                                        "cannot assign to property `" +
                                        propName +
                                        "` — its setter is not accessible "
                                        "here (§13.6)");
                                }
                            }
                        }
                }
            }
            // Slice S7: readonly field assignment outside a ctor.
            checkReadonlyAssignment(*as);
            // Slice T2: plain `:=` assignment must be primitive-
            // assignable (compound ops like `+=` have their own
            // operator semantics and are left to a later slice).
            if (as->op == ":=" && as->target && as->value) {
                // §5.1 special (mirrors LocalDecl init narrowing above):
                // an int CONSTANT EXPRESSION on the RHS assigns to a narrower
                // integer target without a diagnostic — retype it so
                // checkAssignable sees matching primitive types. This covers
                // Buffer<byte>[i] := 1, local byte b := 1, etc. #738: gate on the
                // RHS being a compile-time int constant (foldIntConstExprTC) — a
                // non-constant int value (`b := someIntVar`) does not implicitly
                // narrow and is reported by checkAssignable.
                TypeRef* fromTy = ctx_.exprType(*as->value);
                TypeRef* toTy   = ctx_.exprType(*as->target);
                if (fromTy && toTy &&
                    fromTy->kind == TypeRef::Kind::Primitive &&
                    toTy->kind   == TypeRef::Kind::Primitive &&
                    fromTy->primitive == PrimitiveType::Int) {
                    PrimitiveType tgt = toTy->primitive;
                    bool narrowerIntTarget =
                        tgt == PrimitiveType::Byte  ||
                        tgt == PrimitiveType::SByte ||
                        tgt == PrimitiveType::Short ||
                        tgt == PrimitiveType::UShort||
                        tgt == PrimitiveType::UInt  ||
                        tgt == PrimitiveType::ULong;
                    if (narrowerIntTarget) {
                        auto cv = foldIntConstExprTC(*as->value);
                        // #738 / §5.1: retype only an IN-RANGE int constant; an
                        // out-of-range constant / a non-constant falls through to
                        // checkAssignable's §5.1 rejection.
                        if (cv && fitsInIntType(*cv, tgt)) {
                            ctx_.setExprType(as->value.get(), toTy);
                            fromTy = toTy;
                        }
                    }
                }
                checkAssignable(
                    fromTy,
                    toTy,
                    as->value->span.begin, "assigned value",
                    as->value.get());  // §27.6 #756 property-ref
            }
            // §25.1 (#851): a compound assignment `lhs <op>= rhs` desugars to
            // `lhs := lhs <op> rhs` using the binary operator's overload. When
            // `lhs` is a user type (record/class) whose `operator <op>` returns
            // a type NOT assignable back to `lhs`, the implied assignment is a
            // type error and must be rejected — it was silently accepted,
            // storing the foreign result reinterpreted as the LHS type (e.g.
            // `operator +(Point, Point): int` made `p += q` store an `int` into
            // a `Point` slot). Only USER operator overloads are checked; a
            // built-in primitive compound op keeps the operand type. (Event
            // `+=`/`-=` and `??=` are excluded: neither resolves to a 2-param
            // `operator <bin>` here.)
            if (as->op != ":=" && as->target && as->value) {
                auto binTok = [](std::string_view o) -> std::string_view {
                    if (o == "+=")  return "+";
                    if (o == "-=")  return "-";
                    if (o == "*=")  return "*";
                    if (o == "/=")  return "/";
                    if (o == "&=")  return "&";
                    if (o == "|=")  return "|";
                    if (o == "^=")  return "^";
                    if (o == "<<=") return "<<";
                    if (o == ">>=") return ">>";
                    return {};
                }(as->op);
                TypeRef* toTy = ctx_.exprType(*as->target);
                // Only a USER record/class LHS is in scope: a built-in primitive
                // LHS (string, int, decimal, …) keeps the operand type under the
                // language's own compound rules, and its `operator <op>` lives on
                // a built-in type whose result is the same primitive (so e.g.
                // `s += "x"` for `string` must not be flagged — `string` is a
                // Primitive while `String.operator +` is declared on a Named
                // built-in, a benign kind mismatch).
                if (!binTok.empty() && toTy &&
                    toTy->kind == TypeRef::Kind::Named && toTy->declaration &&
                    (toTy->declaration->kind == ast::TypeDeclKind::Record ||
                     toTy->declaration->kind == ast::TypeDeclKind::Class)) {
                    if (TypeSymbol* lts = memberTargetType(as->target.get())) {
                        MethodSymbol* opm = nullptr;
                        for (Symbol* m : lts->members) {
                            auto* ms = dynamic_cast<MethodSymbol*>(m);
                            if (ms && ms->isOperator &&
                                ms->operatorToken == binTok &&
                                ms->params.size() == 2) { opm = ms; break; }
                        }
                        TypeRef* resTy = nullptr;
                        if (opm)
                            if (auto* od = dynamic_cast<const ast::OperatorDecl*>(
                                    opm->decl))
                                if (od->returnType)
                                    resTy = ctx_.resolvedType(*od->returnType);
                        if (resTy) {
                            // The result must be assignable back to the user
                            // type. Conservative: report only a clear mismatch
                            // (a non-Named result, or a different unrelated
                            // record/class).
                            bool ok = resTy->kind == TypeRef::Kind::Named &&
                                      (resTy->declaration
                                           ? (resTy->declaration ==
                                                  toTy->declaration ||
                                              isSubtype(resTy->declaration,
                                                        toTy->declaration))
                                           : true);  // unresolved — be lenient
                            if (!ok)
                                ctx_.diagnostics().report(
                                    Severity::Error, as->span.begin,
                                    std::string("compound assignment `") +
                                    std::string(as->op) + "` desugars to "
                                    "`lhs := lhs " + std::string(binTok) +
                                    " rhs`, but `operator " + std::string(binTok)
                                    + "` returns a value that is not assignable "
                                    "to the left-hand side's type — the "
                                    "operator's result type differs from the "
                                    "target (§25.1)");
                        }
                    }
                }
            }
        }
        // §7.4 (issue #418): a plain `:=` of a non-null value to a simple
        // variable proves it non-null from here; assigning a (possibly-)null
        // value clears that knowledge. (Compound ops keep the existing value's
        // nullability, so they don't change the analysis.)
        if (as->op == ":=" && as->target && as->value) {
            if (auto* tid = dynamic_cast<const ast::IdentifierExpr*>(
                    as->target.get())) {
                if (tid->name != "this" && tid->name != "base") {
                    if (Symbol* tsym = lookupName(tid->name))
                        narrowOnAssignment(tsym, ctx_.exprType(*as->value));
                }
            }
        }
    } else if (auto* ds = dynamic_cast<const ast::DiscardStmt*>(&s)) {
        // per §15.4: `_ := [...]` has no target type — a compile error.
        // The discard position never provides a target type for a collection
        // literal, since there is no type annotation or parameter context.
        if (ds->value) {
            if (dynamic_cast<const ast::CollectionLiteralExpr*>(
                    ds->value.get())) {
                ctx_.diagnostics().report(
                    Severity::Error, ds->value->span.begin,
                    "collection literal has no determinable target type in "
                    "discard position — add a type annotation (§15.4)");
                visitExpr(*ds->value);
                return;
            }
        }
        if (ds->value) visitExpr(*ds->value);
        // §17.13 v0.150 (#1009): a discard `_ := expr;` of a NON-trivial-dispose
        // CLASS or INTERFACE is now PERMITTED — it disposes the value at the end
        // of the discard statement (the "unbound owned temporary" form, §28.3).
        // The ownership pass marks an owned-PRODUCER class/interface discard
        // (DiscardStmt::disposeOwnedValue) and codegen emits the end-of-statement
        // disposal; a borrow discard carries no obligation. So this site no longer
        // rejects class/interface discards.
        // §17.13 / §15.3 (#1016): a discard `_ := expr;` of a move-only OWNING
        // INLINE ARRAY (`[N owned]T` / `[N owned]T?`) is now also PERMITTED —
        // the ownership pass sets DiscardStmt::disposeInlineOwnedArray and codegen
        // emits end-of-statement slot disposal via the inlineOwnedArray path.
        // Remaining case that stays rejected:
        //   * `_ := <event>.Subscribe(...)` — the owned Subscription's discard is
        //     a likely bug (immediate unsubscribe) and stays an error (§2.11 /
        //     #949); the result is untyped, so it is recognized structurally.
        if (ds->value) {
            const TypeRef* t = ctx_.exprType(*ds->value);
            // §17.13 / §30.10 (#1014): discarding a `Task`/`Task<T>`/`OwningTask<T>`
            // is a compile error — disposing a task abandons a running operation
            // (a semantic, not a leak, concern), so the discard must direct the
            // caller to `Task.FireAndForget` (the error names it specifically).
            // `await`ing a task yields its RESULT type (not Task-shaped), so
            // `_ := await t;` is unaffected; `Task.FireAndForget(...)` returns Unit
            // so its own discard stays legal. Reported before the generic reject so
            // the message is task-specific.
            if (isTaskShapedRef(t)) {
                ctx_.diagnostics().report(
                    Severity::Error, ds->value->span.begin,
                    "cannot discard a `Task` — disposing it abandons a running "
                    "operation; use `Task.FireAndForget(expr)` for supervised "
                    "fire-and-forget, or `using var _ := expr;` to dispose it at "
                    "scope end (§17.13 / §30.10)");
            }
            bool reject = isEventSubscribeDrop(ds->value.get());
            if (reject) {
                ctx_.diagnostics().report(
                    Severity::Error, ds->value->span.begin,
                    "cannot discard an event subscription value — the subscription "
                    "would immediately unsubscribe; bind it (`using var _ := "
                    "expr;`) or store it (`var sub := expr;`) to keep it alive "
                    "(§17.13 / §2.11)");
            }
        }
        // per §14.1 + §A.7 (issue #391): `_ := Generic.Create();` discards the
        // result, so a return-only type parameter has no target. Diagnose.
        if (ds->value) {
            if (auto* dCall =
                    dynamic_cast<const ast::CallExpr*>(ds->value.get()))
                reportNoTargetInferenceIfNeeded(*dCall);
        }
    } else if (auto* ts = dynamic_cast<const ast::ThrowStmt*>(&s)) {
        if (ts->value) {
            visitExpr(*ts->value);
            checkThrowableType(*ts->value);  // §31.1 (#739)
        } else if (catchDepth_ == 0) {
            // Slice S27 (§21.8 / §31.5): bare `throw;` re-raise is
            // valid only inside a catch arm body.
            ctx_.diagnostics().report(
                Severity::Error, s.span.begin,
                "bare `throw;` (re-raise) is only valid inside a "
                "catch arm body (§31.5)");
        }
    } else if (auto* ifs = dynamic_cast<const ast::IfStmt*>(&s)) {
        // §12.7 flow narrowing (type) and §7.4 / §8.4 nullability narrowing.
        // Each branch body is walked with the narrowings its guards establish,
        // restored afterward so they never leak past the controlled block. The
        // `if`/`elif` bodies see their own condition's true-branch narrowings;
        // the `else` body sees the else-branch narrowings of every preceding
        // condition (each one was false). Both narrowing maps are saved/restored
        // around each body for scoping (and a reassignment inside a body updates
        // the affected entry — see AssignmentStmt above).
        //
        // §7.4: nullability narrowing additionally PERSISTS past the if when it
        // holds on every path that reaches the following statement: a branch
        // that always exits (early `return`/`throw`) contributes no fall-through
        // state, so e.g. `if x = null then return; end;` leaves x non-null
        // afterward, and an if whose branches all establish non-null on their
        // fall-through paths joins to non-null. The after-if set is the
        // intersection of the surviving paths' exit sets (§7.4 "Narrowing also
        // applies after early return").
        auto savedNarrowings = narrowings_;
        auto entryNullNarrowed = nullNarrowed_;
        // Collected exit sets of each fall-through path; null pointer means "no
        // path here" (always-exits). We intersect the non-null ones.
        std::vector<std::unordered_set<Symbol*>> fallThroughExits;
        if (ifs->ifBranch.condition) {
            visitExpr(*ifs->ifBranch.condition);
            checkConditionIsBool(*ifs->ifBranch.condition, "if");
        }
        nullNarrowed_ = entryNullNarrowed;
        collectNarrowings(ifs->ifBranch.condition.get(), /*forTrueBranch=*/true);
        for (const auto& bs : ifs->ifBranch.body) {
            if (bs) visitStmt(*bs);
        }
        if (!bodyAlwaysExits(ifs->ifBranch.body))
            fallThroughExits.push_back(nullNarrowed_);
        narrowings_ = savedNarrowings;
        for (std::size_t i = 0; i < ifs->elifBranches.size(); ++i) {
            const auto& el = ifs->elifBranches[i];
            if (el.condition) {
                visitExpr(*el.condition);
                checkConditionIsBool(*el.condition, "elif");
            }
            // This elif is reached only when every preceding condition was
            // false (their else-narrowings hold) and this condition is true.
            nullNarrowed_ = entryNullNarrowed;
            collectNarrowings(ifs->ifBranch.condition.get(),
                              /*forTrueBranch=*/false);
            for (std::size_t j = 0; j < i; ++j)
                collectNarrowings(ifs->elifBranches[j].condition.get(),
                                  /*forTrueBranch=*/false);
            collectNarrowings(el.condition.get(), /*forTrueBranch=*/true);
            for (const auto& bs : el.body) if (bs) visitStmt(*bs);
            if (!bodyAlwaysExits(el.body))
                fallThroughExits.push_back(nullNarrowed_);
            narrowings_ = savedNarrowings;
        }
        // The else body (explicit or implicit fall-through): all preceding
        // guards were false, so each contributes its else-branch narrowing.
        nullNarrowed_ = entryNullNarrowed;
        collectNarrowings(ifs->ifBranch.condition.get(), /*forTrueBranch=*/false);
        for (const auto& el : ifs->elifBranches)
            collectNarrowings(el.condition.get(), /*forTrueBranch=*/false);
        for (const auto& bs : ifs->elseBody) if (bs) visitStmt(*bs);
        // The else path falls through unless an explicit else body always exits.
        if (!ifs->hasElse || !bodyAlwaysExits(ifs->elseBody))
            fallThroughExits.push_back(nullNarrowed_);
        narrowings_ = savedNarrowings;
        // After-if nullability = intersection of all fall-through exit sets.
        if (fallThroughExits.empty()) {
            // Every path exits the method; the after-if point is unreachable.
            // Keep the entry set (harmless — nothing follows).
            nullNarrowed_ = entryNullNarrowed;
        } else {
            std::unordered_set<Symbol*> joined = fallThroughExits.front();
            for (std::size_t i = 1; i < fallThroughExits.size(); ++i) {
                std::unordered_set<Symbol*> next;
                for (Symbol* sym : joined)
                    if (fallThroughExits[i].count(sym)) next.insert(sym);
                joined = std::move(next);
            }
            nullNarrowed_ = std::move(joined);
        }
    } else if (auto* ws = dynamic_cast<const ast::WhileStmt*>(&s)) {
        if (ws->condition) {
            visitExpr(*ws->condition);
            checkConditionIsBool(*ws->condition, "while");
        }
        ++loopDepth_;
        for (const auto& bs : ws->body) if (bs) visitStmt(*bs);
        --loopDepth_;
    } else if (auto* fs = dynamic_cast<const ast::ForStmt*>(&s)) {
        if (fs->iterable) visitExpr(*fs->iterable);
        if (fs->withExpr) visitExpr(*fs->withExpr);
        // §16.5: symbolize the loop variable so the body can refer to it.
        // The variable's type is the explicit annotation if given; for a
        // range literal iterable (`1..3`) it is the element type inferred
        // from the lo/hi expression type. `lsym->decl = fs` anchors the
        // symbol on the ForStmt — codegen maps `locals_[fs]` to the
        // counter alloca so identifier `i` resolves to the slot.
        Scope forScope;
        forScope.parent = currentScope_;
        if (currentScope_ && !fs->varName.empty()) {
            auto* lsym = ctx_.makeSymbol<LocalSymbol>();
            lsym->name = fs->varName;
            lsym->decl = fs;
            if (fs->varType) {
                lsym->declType = ctx_.resolvedType(*fs->varType);
            } else if (auto* range =
                           dynamic_cast<const ast::RangeExpr*>(
                               fs->iterable.get())) {
                if (range->lo)
                    lsym->declType = ctx_.exprType(*range->lo);
                else if (range->hi)
                    lsym->declType = ctx_.exprType(*range->hi);
            } else if (fs->iterable) {
                // §15.1: `for var x in a` over an inline array — the
                if (auto* tt = ctx_.exprType(*fs->iterable)) {
                    // §15.1: `for var x in a` over an inline array.
                    if (tt->kind == TypeRef::Kind::InlineArray &&
                        tt->arrayElement) {
                        lsym->declType = tt->arrayElement;
                    // §4.6: `for var c in s` over a string → char.
                    } else if (tt->kind == TypeRef::Kind::Primitive &&
                               tt->primitive == PrimitiveType::String) {
                        lsym->declType =
                            ctx_.primitiveTypeRef(PrimitiveType::Char);
                    // §3 / §29.2: runtime Array<T> or other IEnumerable<T>
                    // with a Named TypeRef — element type from first generic arg.
                    // Guard: Dictionary<K,V> and OwningDictionary<K,V> are NOT
                    // directly iterable (stdlib §4.8 / §4.9 — no default
                    // `for var e in d` iteration is provided). Blindly taking
                    // genericArgs[0] (K) mis-types the element and causes a
                    // confusing late failure; emit a clear diagnostic instead.
                    } else if (tt->kind == TypeRef::Kind::Named &&
                               !tt->genericArgs.empty() &&
                               tt->genericArgs[0]) {
                        // Check whether the Named type is Dictionary or
                        // OwningDictionary.
                        // §4.8 v0.135 (#548): Dictionary<K,V> is directly
                        // iterable — `for var e in d` yields DictionaryEntry<K,V>
                        // (the ICollection element type), one per live entry in
                        // insertion order.
                        // §4.9 v0.135: OwningDictionary<K,V> is NOT directly
                        // iterable — the #530 diagnostic is narrowed to it only.
                        std::string_view colTypeName;
                        if (tt->astNode && tt->astNode->name &&
                            !tt->astNode->name->parts.empty())
                            colTypeName = tt->headName();
                        else if (!tt->importedName.empty())
                            colTypeName = tt->importedName;
                        if ((colTypeName == "Dictionary" ||
                             colTypeName == "SortedDictionary") &&
                            tt->genericArgs.size() >= 2 &&
                            tt->genericArgs[1]) {
                            // per stdlib §4.8 (v0.135) / §4.12 (v0.165) / §21:
                            // `for var e in d` on Dictionary<K,V> or
                            // SortedDictionary<K,V> yields DictionaryEntry<K,V>.
                            // DictionaryEntry<K,V> is a builtin record with no
                            // user TypeSymbol; represent it as a synthetic Named
                            // TypeRef with importedName="DictionaryEntry" and
                            // genericArgs=[K, V] so sema can type e.Key/e.Value.
                            static const char kDictEntryName[] = "DictionaryEntry";
                            auto* entryTy = ctx_.makeTypeRef();
                            entryTy->kind = TypeRef::Kind::Named;
                            entryTy->declaration = nullptr;
                            entryTy->astNode = nullptr;
                            entryTy->importedName = kDictEntryName;
                            entryTy->genericArgs.push_back(tt->genericArgs[0]); // K
                            entryTy->genericArgs.push_back(tt->genericArgs[1]); // V
                            lsym->declType = entryTy;
                        } else if (colTypeName == "OwningDictionary") {
                            // per §4.9 (v0.135): OwningDictionary direct iteration
                            // is not provided (#530 diagnostic, narrowed from
                            // both dict kinds to owning kind only).
                            ctx_.diagnostics().report(
                                Severity::Error, fs->span.begin,
                                "OwningDictionary<K,V> is not directly iterable "
                                "(stdlib \xC2\xA7""4.9); use `d.Drain()` "
                                "(ownership-transfer) instead of "
                                "`for var e in d`");
                        } else {
                            // Covers Array<T>, Stack<T>, Queue<T>, Set<T>,
                            // LinkedList<T>, and other IEnumerable<T> types that
                            // expose their element type T as genericArgs[0].
                            lsym->declType = tt->genericArgs[0];
                        }
                    } else if (tt->kind == TypeRef::Kind::Named &&
                               tt->declaration) {
                        // §29.2: user-defined IEnumerable<T> — find GetEnumerator
                        // and read its return type's first generic arg (T).
                        auto* ts = tt->declaration;
                        // Walk ts and its base types (simple BFS).
                        std::vector<const TypeSymbol*> queue = {ts};
                        for (std::size_t qi = 0; qi < queue.size(); ++qi) {
                            const TypeSymbol* t = queue[qi];
                            for (auto* m : t->members) {
                                auto* ms = dynamic_cast<MethodSymbol*>(m);
                                if (ms && ms->name == "GetEnumerator" && ms->decl) {
                                    auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl);
                                    if (md && md->returnType) {
                                        auto* rt = ctx_.resolvedType(*md->returnType);
                                        if (rt && !rt->genericArgs.empty() && rt->genericArgs[0])
                                            lsym->declType = rt->genericArgs[0];
                                    }
                                    break;
                                }
                            }
                            if (lsym->declType) break;
                            for (auto* bt : t->baseTypes) if (bt) queue.push_back(bt);
                        }
                    }
                } else if (auto* cl =
                               dynamic_cast<const ast::CollectionLiteralExpr*>(
                                   fs->iterable.get())) {
                    // §16: `for var x in [e1, e2, ...]` — infer element
                    // type from first element.
                    if (!cl->isDict && !cl->elements.empty() &&
                        cl->elements[0]) {
                        lsym->declType = ctx_.exprType(*cl->elements[0]);
                    }
                }
            }
            forScope.names[fs->varName] = lsym;
        }
        Scope* saveScope = currentScope_;
        currentScope_ = &forScope;
        ++loopDepth_;
        for (const auto& bs : fs->body) if (bs) visitStmt(*bs);
        --loopDepth_;
        currentScope_ = saveScope;
    } else if (dynamic_cast<const ast::BreakStmt*>(&s)) {
        // Slice S26 (§21.5): `break` is legal only inside a loop.
        if (loopDepth_ == 0) {
            ctx_.diagnostics().report(
                Severity::Error, s.span.begin,
                "`break` is not allowed outside a loop (§21.5)");
        } else if (finallyLoopBoundary_ != -1 &&
                   loopDepth_ <= finallyLoopBoundary_) {
            // per §21.5/§31.2 (v0.182, #1578): a `break` inside a `finally`
            // is a compile error when its target loop (the immediately
            // enclosing loop) encloses the `finally` — it was already open
            // when the `finally` was entered, so `loopDepth_` here has not
            // grown past `finallyLoopBoundary_`. A loop opened wholly
            // inside the `finally` is unaffected (§21.5).
            ctx_.diagnostics().report(
                Severity::Error, s.span.begin,
                "`break` inside a `finally` block may not target a loop "
                "that encloses the `finally` (§21.5/§31.2)");
        }
    } else if (dynamic_cast<const ast::ContinueStmt*>(&s)) {
        if (loopDepth_ == 0) {
            ctx_.diagnostics().report(
                Severity::Error, s.span.begin,
                "`continue` is not allowed outside a loop (§21.5)");
        } else if (finallyLoopBoundary_ != -1 &&
                   loopDepth_ <= finallyLoopBoundary_) {
            // per §21.5/§31.2 (v0.182, #1578): same rule as `break` above.
            ctx_.diagnostics().report(
                Severity::Error, s.span.begin,
                "`continue` inside a `finally` block may not target a loop "
                "that encloses the `finally` (§21.5/§31.2)");
        }
    } else if (auto* dob = dynamic_cast<const ast::DoEndBlock*>(&s)) {
        // per §21.6 (#1242): a do…end block introduces a lexical scope;
        // locals declared inside are NOT visible after the matching end.
        // Mirror the ForStmt pattern: push a child scope, visit the body,
        // then pop — exactly as codegen already does via lowerScopedBlock.
        Scope doScope;
        doScope.parent = currentScope_;
        Scope* saveScope = currentScope_;
        currentScope_ = &doScope;
        for (const auto& bs : dob->body) if (bs) visitStmt(*bs);
        currentScope_ = saveScope;
    } else if (auto* trys = dynamic_cast<const ast::TryStmt*>(&s)) {
        for (const auto& bs : trys->body) if (bs) visitStmt(*bs);
        for (const auto& arm : trys->catches) {
            // §31.2: a `catch var e: T` arm binds the caught exception
            // to `e` (a borrow of the caught type) in the guard and
            // body scope. The binding's decl anchors on the arm body so
            // codegen can map it to the exception value.
            Scope armScope;
            armScope.parent = currentScope_;
            if (arm.hasBinding && !arm.varName.empty() &&
                !arm.types.empty() && arm.types[0] && arm.body) {
                auto* bnd = ctx_.makeSymbol<LocalSymbol>();
                bnd->name = arm.varName;
                bnd->decl = arm.body.get();
                // per §31.2.1: for a multi-type arm the binding's static type
                // is the nearest common base (NCB) of all listed types; for a
                // single-type arm it is the type itself.
                TypeRef* ncb = catchArmNcb(arm.types);
                bnd->declType = ncb ? ncb : ctx_.resolvedType(*arm.types[0]);
                armScope.names[arm.varName] = bnd;
            }
            Scope* saveScope = currentScope_;
            currentScope_ = &armScope;
            if (arm.guard) visitExpr(*arm.guard);
            // Slice S27: a bare `throw;` is valid inside a catch
            // arm body (but not in the try body or finally).
            ++catchDepth_;
            if (arm.body) visitStmt(*arm.body);
            --catchDepth_;
            currentScope_ = saveScope;
        }
        // per §31.2: `return` inside a `finally` block is a compile error.
        // Track finally-body nesting so the ReturnStmt handler can reject it.
        // per §21.5/§31.2 (v0.182, #1578): also record `loopDepth_` as it
        // stood at finally-entry, so BreakStmt/ContinueStmt can reject a
        // jump whose target loop was already open at that point (i.e.
        // encloses this `finally`). Saved/restored so nested try/finally
        // (finally-within-finally) each check against their own boundary.
        int savedFinallyLoopBoundary = finallyLoopBoundary_;
        finallyLoopBoundary_ = loopDepth_;
        ++finallyDepth_;
        for (const auto& bs : trys->finallyBody) if (bs) visitStmt(*bs);
        --finallyDepth_;
        finallyLoopBoundary_ = savedFinallyLoopBoundary;
    } else if (auto* ys = dynamic_cast<const ast::YieldStmt*>(&s)) {
        // §29.9: `yield` is a method-level construct only — `yield return` and
        // `yield break` are both forbidden inside a lambda. Without this the
        // compiler silently accepted the yield and codegen built a malformed
        // iterator state machine for the lambda, SIGSEGV'ing at runtime
        // (#747 R42). `inLambda_` is set only while visiting a lambda body.
        if (inLambda_) {
            ctx_.diagnostics().report(
                Severity::Error, ys->span.begin,
                "`yield` is not allowed in a lambda — it is a method-level "
                "construct only (§29.9)");
        }
        if (ys->value) visitExpr(*ys->value);
    } else if (auto* is = dynamic_cast<const ast::IncrementStmt*>(&s)) {
        if (is->target) visitExpr(*is->target);
        // fix #913 / §28.10 / §17.6: `c++` / `c--` desugars to
        // `c := T.operator++(c)` — a reassignment. A `using var` local is
        // implicitly non-reassignable, so an increment/decrement on it is a
        // compile error just like a direct `c := …` (mirrors the AssignmentStmt
        // check above). Without this it was silently accepted and, for an owned
        // class operand whose `operator ++` returns a NEW instance, leaked the
        // previous value (§28.7). (The plain-`var`/field forms of #913 — where the
        // ownership pass does not yet model `c++` as a reassignment — remain.)
        if (is->target) {
            if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                    is->target.get())) {
                if (id->name != "_") {
                    if (auto* loc = dynamic_cast<LocalSymbol*>(
                            ctx_.exprSymbol(*is->target))) {
                        if (loc->isUsing)
                            ctx_.diagnostics().report(
                                Severity::Error, is->target->span.begin,
                                std::string("cannot increment/decrement "
                                "`using var` local `") + std::string(id->name) +
                                "` — `using var` is implicitly non-reassignable "
                                "and `" + std::string(is->op) +
                                "` reassigns it (§28.10 / §17.6)");
                    }
                }
            }
        }
    } else if (auto* cs = dynamic_cast<const ast::CaseStmt*>(&s)) {
        if (cs->scrutinee) visitExpr(*cs->scrutinee);
        validateCaseArms(cs->arms);  // §16.4 / §19.6 (#738)
        TypeRef* scrutTy =
            cs->scrutinee ? ctx_.exprType(*cs->scrutinee) : nullptr;
        // §5.1 / §19 (#1153): reject an out-of-range integer pattern literal
        // (statement-position `case` is exempt from exhaustiveness but not from
        // the literal-range gate — codegen would otherwise truncate it).
        checkCasePatternConstantRanges(cs->arms, scrutTy);
        // §10.6 (#772): bare enum member names may be used throughout a `case`
        // over an enum scrutinee, including arm bodies. Inject the scrutinee
        // enum's constants for the arm bodies (the statement-position analogue
        // of the CaseExpr handler). // per §10.6
        Scope armEnumScope;
        Scope* preArmEnumScope = currentScope_;
        bool savedEnumArms = inEnumScrutineeCaseArms_;
        bool scrutIsEnum = scrutTy && scrutTy->kind == TypeRef::Kind::Named &&
            scrutTy->declaration &&
            scrutTy->declaration->kind == ast::TypeDeclKind::Enum;
        if (scrutIsEnum && currentScope_) {
            armEnumScope.parent = currentScope_;
            for (Symbol* m : scrutTy->declaration->members)
                if (dynamic_cast<EnumConstantSymbol*>(m))
                    armEnumScope.names[m->name] = m;
            currentScope_ = &armEnumScope;
            inEnumScrutineeCaseArms_ = true;
        }
        for (const auto& arm : cs->arms) {
            // §19 / §A (#727): scrutinee-less arm — typecheck the bool CONDITION
            // expression(s) the ConstantPattern holds.
            // §19 / §A.8 (#957): a scrutinee-BEARING arm whose pattern is a
            // ConstantPattern referencing a named const (`C.K`, a module-level
            // const) must also be typechecked, so the const acquires an
            // exprType/symbol and codegen can lower it. Enum-constant patterns
            // over an enum scrutinee are excluded — those are bare/qualified
            // member names resolved by name against the scrutinee enum in
            // codegen (enumConstantOfPatternExpr); visiting them here would
            // either fail to resolve (plain scope) or divert that path.
            if (!cs->scrutinee || !scrutIsEnum) {
                if (auto* cp = dynamic_cast<const ast::ConstantPattern*>(
                        arm.pattern.get()))
                    for (const auto& c : cp->constants)
                        if (c) visitExpr(*c);
            }
            // §19.3 (#726): a relational pattern (`> limit`) operand must be
            // typed so codegen can lower a variable operand.
            if (auto* rop =
                    dynamic_cast<const ast::RelOpPattern*>(arm.pattern.get())) {
                if (rop->expr) visitExpr(*rop->expr);
            }
            // §19.3 (v0.183, #1584): a heterogeneous alternative list — type the
            // constant and relational members' operand exprs (mirroring the
            // top-level treatment) so codegen can lower a variable operand.
            if (auto* ap =
                    dynamic_cast<const ast::AltPattern*>(arm.pattern.get())) {
                for (const auto& m : ap->members) {
                    if (auto* mcp =
                            dynamic_cast<const ast::ConstantPattern*>(m.get())) {
                        if (cs->scrutinee && scrutIsEnum) continue;
                        for (const auto& c : mcp->constants)
                            if (c) visitExpr(*c);
                    } else if (auto* mrop =
                                   dynamic_cast<const ast::RelOpPattern*>(
                                       m.get())) {
                        if (mrop->expr) visitExpr(*mrop->expr);
                    }
                }
            }
            // §19: a BindingPattern arm introduces a new scope with the
            // bound name visible inside the guard and body (per §19.2).
            auto* bp =
                dynamic_cast<const ast::BindingPattern*>(arm.pattern.get());
            auto* dp =
                dynamic_cast<const ast::DestructuringPattern*>(arm.pattern.get());
            if (bp && currentScope_) {
                Scope armScope;
                armScope.parent = currentScope_;
                auto* lsym = ctx_.makeSymbol<LocalSymbol>();
                lsym->name = bp->name;
                lsym->decl = bp;
                // Explicit type annotation wins; otherwise inherit scrutinee
                // type (the binding is a borrow of the matched value).
                lsym->declType = bp->type ? ctx_.resolvedType(*bp->type)
                                          : scrutTy;
                armScope.names[bp->name] = lsym;
                // §19.5 (#1203): a typed binding pattern `var n: T` is a
                // type-test + binding. If T is a reference type (class,
                // interface, or string) but the scrutinee's static type is a
                // primitive value type (non-nullable, not string, not Object),
                // the test can NEVER match — a value-type primitive is never
                // a reference type. Reject it here rather than miscompiling
                // (an unconditional wrong-value match for string-typed binding,
                // or SIGABRT in emitIsA for a class-typed binding on an i32).
                // per §19.5 (#1203)
                if (bp->type && scrutTy && lsym->declType) {
                    bool scrutIsValuePrim =
                        scrutTy->kind == TypeRef::Kind::Primitive &&
                        !scrutTy->nullable &&
                        scrutTy->primitive != PrimitiveType::String &&
                        scrutTy->primitive != PrimitiveType::Object;
                    if (scrutIsValuePrim) {
                        TypeRef* bndTy = lsym->declType;
                        bool bndIsRefType =
                            (bndTy->kind == TypeRef::Kind::Primitive &&
                             bndTy->primitive == PrimitiveType::String) ||
                            (bndTy->kind == TypeRef::Kind::Named &&
                             bndTy->declaration &&
                             (bndTy->declaration->kind ==
                                  ast::TypeDeclKind::Class ||
                              bndTy->declaration->kind ==
                                  ast::TypeDeclKind::Interface));
                        if (bndIsRefType)
                            ctx_.diagnostics().report(
                                Severity::Error, bp->span.begin,
                                "binding pattern type is incompatible with "
                                "the scrutinee's static type — a value-type "
                                "scrutinee can never match a reference-typed "
                                "binding (§19.5)");
                    }
                }
                Scope* saveScope = currentScope_;
                currentScope_ = &armScope;
                if (arm.guard) visitExpr(*arm.guard);
                if (arm.exprBody) visitExpr(*arm.exprBody);
                for (const auto& bs : arm.stmts) if (bs) visitStmt(*bs);
                currentScope_ = saveScope;
            } else if (dp && currentScope_ && dp->typeName) {
                // §19: a DestructuringPattern arm introduces a scope with
                // one LocalSymbol per naked binding, typed as the
                // corresponding positional field of the record (§9.4).
                // Look up the record TypeSymbol by the pattern's type name.
                TypeSymbol* recTy = nullptr;
                std::string_view lastName = dp->typeName->parts.empty()
                    ? std::string_view{}
                    : dp->typeName->parts.back();
                // §19.3: "Destructuring applies to records only — classes do
                // not have a positional-destructuring surface … use a
                // type-test pattern (`Dog`, `var d: Dog`) instead."
                // Emit a compile error when the named type is a class.
                if (ctx_.module()) {
                    for (Symbol* child : ctx_.module()->children) {
                        if (auto* ts = dynamic_cast<TypeSymbol*>(child)) {
                            if (ts->name == lastName &&
                                ts->kind == ast::TypeDeclKind::Class) {
                                ctx_.diagnostics().report(
                                    Severity::Error, dp->span.begin,
                                    "positional destructuring pattern `" +
                                    std::string(lastName) +
                                    "(...)` is not valid on a class type — "
                                    "classes do not have a positional-"
                                    "destructuring surface (§19.3); use a "
                                    "type-test pattern (`" +
                                    std::string(lastName) +
                                    "` or `var x: " +
                                    std::string(lastName) +
                                    "`) instead");
                                break;
                            }
                        }
                    }
                }
                if (ctx_.module()) {
                    for (Symbol* child : ctx_.module()->children) {
                        if (auto* ts = dynamic_cast<TypeSymbol*>(child)) {
                            if (ts->name == lastName &&
                                ts->kind == ast::TypeDeclKind::Record) {
                                recTy = ts; break;
                            }
                        }
                    }
                }
                // Collect the record's positional (non-static) fields.
                std::vector<FieldSymbol*> fields;
                if (recTy) {
                    for (Symbol* m : recTy->members) {
                        if (auto* fs = dynamic_cast<FieldSymbol*>(m)) {
                            if (!fs->modifiers.empty()) {
                                // skip static fields
                                bool isStatic = false;
                                for (auto& mod : fs->modifiers)
                                    if (mod == "static") { isStatic = true; break; }
                                if (isStatic) continue;
                            }
                            fields.push_back(fs);
                        }
                    }
                }
                // §19.3 / Appendix C: the positional-binding count must match
                // the record's positional-field count (mirrors the CaseExpr
                // handler). An over-/under-bound pattern was accepted and then
                // crashed codegen with malformed IR.
                if (recTy && !fields.empty() &&
                    dp->subPatterns.size() != fields.size()) {
                    ctx_.diagnostics().report(
                        Severity::Error, dp->span.begin,
                        "record-destructuring pattern `" + std::string(lastName) +
                        "(...)` binds " + std::to_string(dp->subPatterns.size()) +
                        " value(s) but record `" + std::string(lastName) +
                        "` has " + std::to_string(fields.size()) +
                        " positional field(s); the binding count must match "
                        "(§19.3)");
                }
                Scope armScope;
                armScope.parent = currentScope_;
                for (std::size_t i = 0; i < dp->subPatterns.size(); ++i) {
                    const auto& sp = dp->subPatterns[i];
                    TypeRef* fieldTy = nullptr;
                    if (i < fields.size() && fields[i]->posParam &&
                        fields[i]->posParam->type) {
                        fieldTy = ctx_.resolvedType(*fields[i]->posParam->type);
                    }
                    if (!sp.nakedBinding.empty()) {
                        // Bare `Pt(x, y)` — nakedBinding carries the name.
                        // Use the PositionalPattern's address (stable since
                        // AST is immutable) as the unique decl anchor so
                        // codegen can key locals_[] on the same pointer.
                        auto* lsym = ctx_.makeSymbol<LocalSymbol>();
                        lsym->name = sp.nakedBinding;
                        lsym->decl = sp.nakedAnchor.get();  // #1271 real Node anchor
                        lsym->declType = fieldTy;
                        armScope.names[sp.nakedBinding] = lsym;
                    } else if (auto* vbp = dynamic_cast<const ast::BindingPattern*>(
                                   sp.pattern.get())) {
                        // §19.3: `Pt(var x, var y)` — equivalent to bare form;
                        // register the bound name keyed on the BindingPattern
                        // node (codegen keys its slot on the same pointer).
                        auto* lsym = ctx_.makeSymbol<LocalSymbol>();
                        lsym->name = vbp->name;
                        lsym->decl = vbp;
                        lsym->declType = vbp->type
                            ? ctx_.resolvedType(*vbp->type) : fieldTy;
                        armScope.names[vbp->name] = lsym;
                    }
                }
                // #726 / §19.3: bind names inside NESTED destructuring
                // sub-patterns (`Outer(Inner(v), b)`) — mirrors the CaseExpr
                // handler. #1281: set each nested binding's declType from its
                // inner record's positional field so a member access on a nested
                // class/record binding in the arm body resolves (see the CaseExpr
                // handler for the full rationale — a null declType left
                // `Outer(Inner(x)) => x.Id` unresolved / miscompiled).
                {
                    auto fieldsOfRecord =
                        [&](const ast::QualifiedName* tn) -> std::vector<FieldSymbol*> {
                        std::vector<FieldSymbol*> out;
                        if (!tn || tn->parts.empty()) return out;
                        std::string_view nm = tn->parts.back();
                        for (const auto& sym : ctx_.symbols()) {
                            auto* ts = dynamic_cast<TypeSymbol*>(sym.get());
                            if (!ts || ts->name != nm ||
                                ts->kind != ast::TypeDeclKind::Record)
                                continue;
                            for (Symbol* m : ts->members) {
                                auto* fs = dynamic_cast<FieldSymbol*>(m);
                                if (!fs) continue;
                                bool isStatic = false;
                                for (auto& md : fs->modifiers)
                                    if (md == "static") { isStatic = true; break; }
                                if (!isStatic) out.push_back(fs);
                            }
                            break;
                        }
                        return out;
                    };
                    std::vector<const ast::DestructuringPattern*> nestedWork;
                    for (const auto& sp : dp->subPatterns)
                        if (sp.nakedBinding.empty())
                            if (auto* nd = dynamic_cast<
                                    const ast::DestructuringPattern*>(
                                    sp.pattern.get()))
                                nestedWork.push_back(nd);
                    while (!nestedWork.empty()) {
                        const ast::DestructuringPattern* nd = nestedWork.back();
                        nestedWork.pop_back();
                        std::vector<FieldSymbol*> ndFields =
                            fieldsOfRecord(nd->typeName.get());
                        for (std::size_t i = 0; i < nd->subPatterns.size(); ++i) {
                            const auto& s = nd->subPatterns[i];
                            TypeRef* fieldTy = nullptr;
                            if (i < ndFields.size() && ndFields[i]->posParam &&
                                ndFields[i]->posParam->type)
                                fieldTy = ctx_.resolvedType(
                                    *ndFields[i]->posParam->type);
                            if (!s.nakedBinding.empty()) {
                                auto* lsym = ctx_.makeSymbol<LocalSymbol>();
                                lsym->name = s.nakedBinding;
                                lsym->decl = s.nakedAnchor.get();  // #1271 real Node anchor
                                lsym->declType = fieldTy;  // #1281
                                armScope.names[s.nakedBinding] = lsym;
                            } else if (auto* vbp =
                                           dynamic_cast<const ast::BindingPattern*>(
                                               s.pattern.get())) {
                                auto* lsym = ctx_.makeSymbol<LocalSymbol>();
                                lsym->name = vbp->name;
                                lsym->decl = vbp;
                                lsym->declType = vbp->type
                                    ? ctx_.resolvedType(*vbp->type) : fieldTy;  // #1281
                                armScope.names[vbp->name] = lsym;
                            } else if (auto* deeper = dynamic_cast<
                                           const ast::DestructuringPattern*>(
                                           s.pattern.get())) {
                                nestedWork.push_back(deeper);
                            }
                        }
                    }
                }
                Scope* saveScope = currentScope_;
                currentScope_ = &armScope;
                if (arm.guard) visitExpr(*arm.guard);
                if (arm.exprBody) visitExpr(*arm.exprBody);
                for (const auto& bs : arm.stmts) if (bs) visitStmt(*bs);
                currentScope_ = saveScope;
            } else {
                if (arm.guard) visitExpr(*arm.guard);
                if (arm.exprBody) visitExpr(*arm.exprBody);
                for (const auto& bs : arm.stmts) if (bs) visitStmt(*bs);
            }
        }
        currentScope_ = preArmEnumScope;  // §10.6 (#772): pop the enum scope
        inEnumScrutineeCaseArms_ = savedEnumArms;
    }
    // Break, Continue have no expressions.
}

bool typecheckExpressions(const ast::SourceFile& src, SemaContext& ctx) {
    ExprTC tc(ctx);
    tc.run(src);
    return !ctx.diagnostics().hasErrors();
}

} // namespace bezel::sema
