#include "sema/ExprTypecheck.hpp"
#include "sema/ExprTypecheck_impl.hpp"

#include <cerrno>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast/Node.hpp"
#include "common/BuiltinRegistry.hpp"
#include "common/OwnershipFacts.hpp"  // §35.4/§35.5 (#2208): substitutedChoicePayloadType
#include "sema/ConstFold.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeRef.hpp"
#include "sema/TypeResolution.hpp"

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

// §30.1 / §29.3 (v0.184, #1670): `ref`/`out` parameters are forbidden on an
// `async` method or an iterator method (synchronous or async, §29.3) — a
// `ref`/`out` argument is a reference to a caller stack slot, bounded by the
// synchronous call, but these bodies capture their parameters into a heap-
// allocated state-machine frame that outlives the synchronous call, so the
// captured slot reference dangles the moment the method returns its task or
// enumerable at the first suspension. The shared Param production
// over-admits the modifiers syntactically (Appendix A.4); the rejection is
// semantic — the same over-admit-and-reject pattern as §22.10's primary
// constructors and §23.4's indexer parameter lists. `owned` parameters are
// unaffected (§29.8 / §30.1).
static void checkNoRefOutOnAsyncOrIterator(
    const std::vector<ast::Param>& params, bool isAsync, bool isIterator,
    std::string_view methodName, SourceLocation methodLoc, SemaContext& ctx) {
    if (!isAsync && !isIterator) return;
    for (const auto& p : params) {
        if (p.modifier != "ref" && p.modifier != "out" &&
            p.modifier != "out owned")
            continue;
        const char* kind = isAsync ? "an `async` method" : "an iterator method";
        // §22.10/#1664 precedent (TypeResolution_Checks.cpp): ParameterSymbol
        // (and, here, the raw AST Param) carries no reliable per-parameter
        // location — `ast::Type::span` is never stamped by the parser — so
        // point at the enclosing method declaration, the same fallback the
        // operator-parameter-modifier check uses.
        ctx.diagnostics().report(
            Severity::Error, methodLoc,
            std::string("`") + std::string(p.modifier) + "` parameter `" +
            std::string(p.name) + "` is not permitted on " + kind + " `" +
            std::string(methodName) + "` — its arguments are captured into a "
            "heap-allocated state-machine frame that outlives the "
            "synchronous call, so a caller-stack-slot reference would dangle "
            "the moment the method returns its task or enumerable at the "
            "first suspension point (§30.1, §29.3, v0.184)");
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
    // §2.8.1 rule 4 (v0.188): this file's own module name, mirroring
    // Resolver::run's selfModuleName — resolveTypes already diagnosed a
    // self-import (it runs first, per Driver::compileFile/compileObject), but
    // this pass rebuilds its own sourceImports_/moduleScope_ independently
    // from src.imports, so a self-import must be excluded here too or the
    // same-named-artifact shape (`module Dup; import Dup;` against `dup1.o`,
    // issue #1730) would still expose the artifact's publics during
    // expression typecheck even though the compile is already rejected.
    std::string selfModuleName;
    if (src.module && src.module->name) {
        for (std::size_t i = 0; i < src.module->name->parts.size(); ++i) {
            if (i != 0) selfModuleName += '.';
            selfModuleName += std::string(src.module->name->parts[i]);
        }
    }
    // §2.2 v0.164 / #1365: collect import aliases + sourceImports for
    // alias/module-prefix resolution in resolveMemberAccess (§2.7).
    importAliases_.clear();
    sourceImports_.clear();
    ambiguousImportNames_.clear();
    importedExtensions_.clear();
    for (const auto& imp : src.imports) {
        if (!imp || !imp->name) continue;
        std::string dotted;
        for (std::size_t i = 0; i < imp->name->parts.size(); ++i) {
            if (i != 0) dotted += '.';
            dotted += std::string(imp->name->parts[i]);
        }
        // §2.8.1 rule 4: skip a self-import (already diagnosed by
        // resolveTypes) — do not register it as a visible import here either.
        if (!selfModuleName.empty() && dotted == selfModuleName) continue;
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
            // §2.8.1 rule 4: a self-import never exposes a same-named
            // dependency artifact's publics (see the note above run()).
            if (!selfModuleName.empty() && dotted == selfModuleName) continue;
            // §26.5 step 2 (#735): an imported module's `public extension`
            // blocks join the extension search set for this file. Unlike a
            // type, an extension has no name to expose in scope — it is found
            // only by lookupExtensionMember matching its target type.
            if (const std::vector<ExtensionSymbol*>* exts =
                    ctx_.importedExtensionsOf(dotted)) {
                for (ExtensionSymbol* e : *exts) {
                    if (!e) continue;
                    importedExtensions_.push_back(e);
                    // §26.5 rule 4 (#1307): remember this block's defining
                    // module for the ambiguity diagnostic (see
                    // importedExtensionModule_'s declaration).
                    importedExtensionModule_[e] = dotted;
                }
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

namespace {

// §19.3: an unguarded `_` (DiscardPattern) or unconstrained `var x`
// (BindingPattern with no type annotation) matches any value — a
// catch-all that makes the `case` trivially exhaustive (§19.6). Used by
// both stmtAlwaysExits (clause (1) of the §21.7 statement-case totality
// rule) and caseArmsExhaustive, so it lives above both.
bool isUnguardedCatchAll(const ast::CaseArm& arm) {
    if (arm.guard) return false;  // a guard can fail at runtime
    const ast::Pattern* p = arm.pattern.get();
    if (dynamic_cast<const ast::DiscardPattern*>(p)) return true;
    if (auto* bp = dynamic_cast<const ast::BindingPattern*>(p))
        return bp->type == nullptr;  // `var x` (untyped) is total; `var x: T` is a type test
    return false;
}

}  // namespace

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
        // §21.7 / §19.6: does a case STATEMENT return on every path? Necessary
        // for any answer of "yes": EVERY arm body diverges (return / throw /
        // non-terminating loop). Then the no-match FALL-THROUGH edge must also be
        // closed — in STATEMENT position an unmatched value falls through with no
        // action (§19.6), NOT `MatchFailureException`, so it would reach the
        // method end with no return.
        for (const auto& arm : cs->arms)
            if (!armBodyAlwaysExits(arm)) return false;
        // (1) §21.7 irrefutable-arm bullet: an unguarded `_` OR an unguarded
        //     untyped `var x` is a catch-all that closes the fall-through for ANY
        //     scrutinee kind — "one proof in both spellings" (§19.6). #2133
        //     widened this from `_`-only (the historical DiscardPattern gate) to
        //     also admit the var-spelled irrefutable arm, so the untyped catch-all
        //     closes totality uniformly regardless of how the programmer wrote it.
        for (const auto& arm : cs->arms)
            if (isUnguardedCatchAll(arm)) return true;
        // (2) §21.7 closed-value-space bullets (#2129 choice, #2133 bool +
        //     sealed): a CHOICE (§35.1 — the case set is closed by declaration),
        //     a BOOL primitive (§4.1 — no conversion manufactures a third value),
        //     or a `sealed abstract` closed class hierarchy (§11.7 — every
        //     instance is one of the closed concrete-leaf set) each have a CLOSED
        //     VALUE SPACE, so an EXHAUSTIVE case-statement whose arms all diverge
        //     has no reachable fall-through and is total. The exhaustiveness
        //     verdict is REUSED from the expression-form check
        //     (caseArmsExhaustive), run quietly — a statement-position `case` is
        //     itself exempt from the §19.6 non-exhaustiveness warning.
        //
        //     For a Named CLASS scrutinee, caseArmsExhaustive returns false when
        //     the hierarchy is OPEN (its `closed` gate fails), so totality is
        //     correctly withheld there — admitting Class broadly does not grant
        //     totality for open hierarchies. An ENUM is deliberately NOT admitted:
        //     its value space is the underlying integer type, and an out-of-range
        //     / non-constant cast `E(n)` produces a "raw" value equal to no
        //     declared constant (§5.4) that falls through — so a constants-
        //     exhaustive enum case is genuinely non-total (accepting it would be
        //     unsound). The v0.196 N6 "totality does not key on the exhaustiveness
        //     proof" ruling is superseded for the closed-value-space kinds above
        //     (§21.7); a warning-only proof over an OPEN value space still grants
        //     nothing.
        TypeRef* scrutTy =
            cs->scrutinee ? ctx_.exprType(*cs->scrutinee) : nullptr;
        bool admitsTotality = scrutTy && (
            // bool bullet (§4.1)
            (scrutTy->kind == TypeRef::Kind::Primitive &&
             scrutTy->primitive == PrimitiveType::Bool) ||
            // choice bullet (§35.1) + sealed-hierarchy bullet (§11.7)
            (scrutTy->kind == TypeRef::Kind::Named &&
             scrutTy->declaration &&
             (scrutTy->declaration->kind == ast::TypeDeclKind::Choice ||
              scrutTy->declaration->kind == ast::TypeDeclKind::Class)));
        if (admitsTotality)
            return caseArmsExhaustive(cs->arms, cs->scrutinee.get(), scrutTy,
                                      s.span.begin, /*quiet=*/true);
        return false;
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
bool armsCoverNull(const std::vector<ast::CaseArm>& arms) {
    for (const auto& arm : arms) {
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

// §19.6 (v0.196, #1816): is `t` a FULLY GROUND type — one with no rigid/param
// component anywhere? A `Named` TypeRef with `declaration == nullptr` is a
// rigid type-parameter reference (a bare `T` left declaration-less by type
// resolution) — or a null-declaration stdlib builtin; BOTH are treated as
// non-ground. This is the soundness guard the ground-mismatch exemption rests
// on: typeRefEqual compares by `declaration` pointer and is BLIND to a null
// declaration (§ TypeResolution typeRefEqual), so a rigid `T` inside either
// operand could be mistaken for a ground type and wrongly report inequality —
// wrongly exempting an inhabited leaf. Requiring both operands fully ground
// before typeRefEqual makes a `false` result mean genuine ground inequality.
// Conservative on purpose: fewer exemptions, never a wrong one.
bool isFullyGroundTypeRef(const TypeRef* t) {
    if (!t) return false;
    switch (t->kind) {
        case TypeRef::Kind::Primitive:
        case TypeRef::Kind::Unit:
            return true;
        case TypeRef::Kind::Named:
            if (!t->declaration) return false;  // rigid param / null-decl builtin
            for (const auto* ga : t->genericArgs)
                if (!isFullyGroundTypeRef(ga)) return false;
            return true;
        case TypeRef::Kind::InlineArray:
            return isFullyGroundTypeRef(t->arrayElement);
        case TypeRef::Kind::Function: {
            for (const auto& fp : t->funcParams)
                if (!isFullyGroundTypeRef(fp.type)) return false;
            return isFullyGroundTypeRef(t->funcReturn);
        }
        case TypeRef::Kind::Error:
            return false;
    }
    return false;
}

// §19.6 (v0.196, #1816 s2): the tri-state outcome of matching one component of a
// leaf's root instantiation against the corresponding GROUND scrutinee component
// under COMPONENT-WISE INVARIANT EXACT identity (§12.9 — class hierarchies are
// invariant; no §5.1 conversion, no variance).
//   Fail    — a decidable structural mismatch (distinct ground constructors or
//             ground arguments, or a repeated leaf parameter bound two
//             inconsistent ways). By invariance this proves the leaf uninhabited.
//   Ok      — matched; any leaf parameters encountered were bound in `solved`.
//   Unknown — undecidable HERE (a nullable-marker occurrence / an alias-shaped
//             or arity edge / a rigid non-leaf reference). Never a false exempt:
//             the caller keeps such a leaf, deferring to the name-match / later
//             lanes. Fail dominates Unknown dominates Ok when composing.
enum class InvMatch { Fail, Unknown, Ok };

// Structural-invariant match of a root-instantiation component `declared`
// (expressed in the leaf's own parameters) against a GROUND scrutinee component
// `target`, solving `paramNames` into `solved`. Modeled on
// unifyReturnAgainstTarget (ExprTypecheck_Calls.cpp) but STRICT — the lenient
// "concrete vs concrete ⇒ succeed" arm there is here a real ground-equality test
// (a ground mismatch is Fail, not a silent pass) — and three-valued rather than
// a bool, per §19.6's determination (all solutions, not one witness).
static InvMatch matchLeafInvariant(
    const TypeRef* declared, const TypeRef* target,
    const std::unordered_set<std::string_view>& paramNames,
    std::unordered_map<std::string_view, TypeRef*>& solved, SemaContext& ctx,
    std::unordered_set<std::string_view>& splitParams) {
    if (!declared || !target) return InvMatch::Unknown;

    // (1) A bare leaf-parameter reference on the DECLARED (root-instantiation)
    // side — a Named TypeRef with no declaration whose head names a leaf param
    // (mirrors unifyReturnAgainstTarget's bare-parameter recognition). This is a
    // SOLVE VARIABLE.
    if (declared->kind == TypeRef::Kind::Named && !declared->declaration &&
        paramNames.count(declared->headName())) {
        std::string_view nm = declared->headName();
        if (declared->nullable) {
            // §7.10 v0.196 (#1816 s7): a nullable-marker occurrence `Y?`. `?` is
            // idempotent (`(U?)? ≡ U?`) — the one non-injective type constructor —
            // so its pre-image is a finite SET. Against a NULLABLE target `U?` the
            // split admits BOTH `Y := U` (`U? = U?`) and `Y := U?` (`(U?)? = U?`):
            // two inhabiting solutions. Bind the NON-null pre-image `Y := U` here
            // (the primary solution) and record `Y` as a split param;
            // solveLeafInstantiations then decides whether the nullable pre-image
            // ALSO survives (⇒ >1 solution ⇒ undetermined) or a §14.2 constraint on
            // `Y` (notnull / class / value / a type or type-parameter bound — all
            // imply non-null `Y`) kills it (⇒ the split collapses to this single
            // binding ⇒ determined). Against a NON-nullable target `Y?` can never
            // equal it (`Y?` is always nullable) — an undecidable edge, kept
            // (Unknown), deferred to the name-match, never a false exempt.
            if (!target->nullable) return InvMatch::Unknown;
            // The non-nullable pre-image `U` of the nullable target `U?`.
            TypeRef* nn;
            if (target->kind == TypeRef::Kind::Primitive) {
                nn = ctx.primitiveTypeRef(target->primitive, /*nullable=*/false);
            } else {
                nn = ctx.makeTypeRef();
                *nn = *target;
                nn->nullable = false;
            }
            auto it = solved.find(nm);
            if (it != solved.end()) {
                // A prior binding of `Y`. The non-null pre-image is consistent only
                // if it equals that binding; decide only on ground bindings
                // (typeRefEqual is safe there), else keep (Unknown). A prior bare
                // (non-`?`) occurrence already erased `Y` from splitParams and
                // pins it non-nullable, so no split is (re-)recorded here.
                if (isFullyGroundTypeRef(it->second) && isFullyGroundTypeRef(nn))
                    return typeRefEqual(it->second, nn) ? InvMatch::Ok
                                                        : InvMatch::Fail;
                return InvMatch::Unknown;
            }
            solved[nm] = nn;
            splitParams.insert(nm);
            return InvMatch::Ok;
        }
        // A bare (non-nullable) occurrence pins `Y` to the target exactly and
        // COLLAPSES any prior `?`-split on `Y` — a non-nullable occurrence forbids
        // the nullable pre-image (§7.10 v0.196).
        splitParams.erase(nm);
        auto it = solved.find(nm);
        if (it != solved.end()) {
            // Repeated occurrence: consistent only if the two ground bindings are
            // identical (invariant). `D<Y,Y>` against `D<int,string>` fails here.
            // Both operands are ground scrutinee components, so typeRefEqual's
            // null-declaration blindness (the s1 guard) cannot bite.
            return typeRefEqual(it->second, target) ? InvMatch::Ok
                                                    : InvMatch::Fail;
        }
        solved[nm] = const_cast<TypeRef*>(target);
        return InvMatch::Ok;
    }
    // A declaration-less Named that is NOT a leaf parameter: a rigid enclosing-
    // body parameter or a null-declaration builtin. Its identity to a ground
    // target is undecidable at this declaration — never exempt on it.
    if (declared->kind == TypeRef::Kind::Named && !declared->declaration)
        return InvMatch::Unknown;

    // (2) Nullability is part of invariant identity, but a mismatch here rides an
    // edge (the scrutinee's nested nullability) — stay conservative: only ACT on
    // agreeing nullability; a mismatch is Unknown (kept), never a false exempt.
    if (declared->nullable != target->nullable) return InvMatch::Unknown;

    switch (declared->kind) {
        case TypeRef::Kind::Primitive:
            if (target->kind != TypeRef::Kind::Primitive)
                return InvMatch::Unknown;  // primitive/Named alias edge — keep
            return declared->primitive == target->primitive ? InvMatch::Ok
                                                             : InvMatch::Fail;
        case TypeRef::Kind::Named: {
            if (target->kind != TypeRef::Kind::Named) return InvMatch::Unknown;
            if (declared->declaration != target->declaration)
                return InvMatch::Fail;  // distinct ground constructors
            if (declared->genericArgs.size() != target->genericArgs.size())
                return InvMatch::Unknown;  // arity edge — keep, never exempt
            InvMatch acc = InvMatch::Ok;
            for (std::size_t i = 0; i < declared->genericArgs.size(); ++i) {
                InvMatch r = matchLeafInvariant(declared->genericArgs[i],
                                                target->genericArgs[i],
                                                paramNames, solved, ctx,
                                                splitParams);
                if (r == InvMatch::Fail) return InvMatch::Fail;  // Fail dominates
                if (r == InvMatch::Unknown) acc = InvMatch::Unknown;
            }
            return acc;
        }
        case TypeRef::Kind::Unit:
            return target->kind == TypeRef::Kind::Unit ? InvMatch::Ok
                                                       : InvMatch::Fail;
        case TypeRef::Kind::InlineArray:
            if (target->kind != TypeRef::Kind::InlineArray) return InvMatch::Unknown;
            return matchLeafInvariant(declared->arrayElement, target->arrayElement,
                                      paramNames, solved, ctx, splitParams);
        case TypeRef::Kind::Function:
        case TypeRef::Kind::Error:
            return InvMatch::Unknown;  // structural function match — later lane
    }
    return InvMatch::Unknown;
}

// §19.6 (v0.196, #1816 s4): the tri-state-plus outcome of matching one component
// of a leaf's root instantiation against the corresponding NON-GROUND (rigid)
// scrutinee component. Adds `Unattainable` to the ground matcher's trichotomy:
//   Fail         — a decidable ground-structure mismatch (distinct ground
//                  constructors / ground arguments, both sides concrete). By
//                  invariance no instantiation of the enclosing body reconciles
//                  it; the leaf is genuinely uninhabited on that component.
//   Unattainable — an UNDECIDABLE rigid identity: a ground/composite (non-solve-
//                  variable) declared component equated against a rigid
//                  enclosing parameter. Satisfiable at some enclosing
//                  instantiations, not others (`IntError: Result<int>` at
//                  `Result<T>` is inhabited exactly at `T = int`) ⇒ neither
//                  exempt nor determined, coverage statically unattainable.
//   Ok           — matched; leaf solve variables bound in `solved` (a solve
//                  variable binds to a rigid target too: `Ok<Y>` ⇒ `Y := T`).
//   Unknown      — undecidable edge (nullability / arity / function / a
//                  null-decl target that is not an in-scope rigid parameter):
//                  kept, deferred to the name-match, never a false verdict.
// Composition precedence: Fail > Unattainable > Unknown > Ok.
enum class RigidInv { Fail, Unattainable, Unknown, Ok };

// Rigid-aware structural-invariant match of a root-instantiation component
// `declared` (in the leaf's own parameters) against a NON-GROUND scrutinee
// component `target`. Rigidity is keyed SYNTACTICALLY via `isRigidParam` (a
// null-declaration Named whose head names an in-scope enclosing type parameter)
// — NEVER via null-declaration typeRefEqual, whose blindness would treat two
// distinct null-decl Named refs as equal/solvable and reopen the false-
// exemption hole (the #1816 soundness invariant).
static RigidInv matchLeafRigid(
    const TypeRef* declared, const TypeRef* target,
    const std::unordered_set<std::string_view>& paramNames,
    std::unordered_map<std::string_view, TypeRef*>& solved,
    const std::function<bool(std::string_view)>& isRigidParam) {
    if (!declared || !target) return RigidInv::Unknown;

    // (1) DECLARED side: a bare leaf-parameter reference is a SOLVE VARIABLE — it
    // binds to the target WHATEVER the target is (ground OR rigid). `Ok<Y>:
    // Result<Y>` against `Result<T>` solves `Y := T`: the determined rigid shape
    // (§19.6 v0.196 — "a solution mentioning [a rigid parameter] is expressible
    // in an arm"). A `Y?` occurrence is the §7.10 ?-split, a later lane.
    if (declared->kind == TypeRef::Kind::Named && !declared->declaration &&
        paramNames.count(declared->headName())) {
        if (declared->nullable) return RigidInv::Unknown;  // ?-split — later lane
        std::string_view nm = declared->headName();
        auto it = solved.find(nm);
        if (it != solved.end()) {
            // Repeated occurrence: consistent only if the two bindings are
            // identical. Decide ONLY on fully-ground bindings (typeRefEqual is
            // safe there); any non-ground binding — a rigid parameter — is
            // undecidable here (kept), never compared via null-decl typeRefEqual.
            if (isFullyGroundTypeRef(it->second) && isFullyGroundTypeRef(target))
                return typeRefEqual(it->second, target) ? RigidInv::Ok
                                                        : RigidInv::Fail;
            return RigidInv::Unknown;
        }
        solved[nm] = const_cast<TypeRef*>(target);
        return RigidInv::Ok;
    }
    // A declaration-less DECLARED Named that is not a leaf parameter — a
    // well-formed leaf root instantiation should not carry one; be conservative.
    if (declared->kind == TypeRef::Kind::Named && !declared->declaration)
        return RigidInv::Unknown;

    // (2) TARGET side: a RIGID enclosing-body type parameter matched against a
    // concrete (ground/composite) declared component — case (1) already peeled
    // off a declared solve variable, so this is the rigid-vs-ground /
    // rigid-vs-composite identity, UNDECIDABLE at the declaration ⇒ statically
    // UNATTAINABLE. A null-decl target that is NOT an in-scope type parameter (a
    // null-decl builtin edge) is merely Unknown — kept, never forced to warn.
    if (target->kind == TypeRef::Kind::Named && !target->declaration)
        return isRigidParam(target->headName()) ? RigidInv::Unattainable
                                                : RigidInv::Unknown;

    // (3) Both sides concrete: decidable structural-invariant identity, exactly
    // as the ground matcher — a ground mismatch is a genuine Fail (no
    // instantiation reconciles distinct ground structure, §19.6 "fails
    // statically only on ground structure").
    if (declared->nullable != target->nullable) return RigidInv::Unknown;
    switch (declared->kind) {
        case TypeRef::Kind::Primitive:
            if (target->kind != TypeRef::Kind::Primitive) return RigidInv::Unknown;
            return declared->primitive == target->primitive ? RigidInv::Ok
                                                            : RigidInv::Fail;
        case TypeRef::Kind::Named: {
            if (target->kind != TypeRef::Kind::Named) return RigidInv::Unknown;
            if (declared->declaration != target->declaration)
                return RigidInv::Fail;  // distinct ground constructors
            if (declared->genericArgs.size() != target->genericArgs.size())
                return RigidInv::Unknown;  // arity edge — keep
            RigidInv acc = RigidInv::Ok;
            for (std::size_t i = 0; i < declared->genericArgs.size(); ++i) {
                RigidInv r = matchLeafRigid(declared->genericArgs[i],
                                            target->genericArgs[i], paramNames,
                                            solved, isRigidParam);
                if (r == RigidInv::Fail) return RigidInv::Fail;  // Fail dominates
                if (r == RigidInv::Unattainable) acc = RigidInv::Unattainable;
                else if (r == RigidInv::Unknown && acc == RigidInv::Ok)
                    acc = RigidInv::Unknown;
            }
            return acc;
        }
        case TypeRef::Kind::Unit:
            return target->kind == TypeRef::Kind::Unit ? RigidInv::Ok
                                                       : RigidInv::Fail;
        case TypeRef::Kind::InlineArray:
            if (target->kind != TypeRef::Kind::InlineArray) return RigidInv::Unknown;
            return matchLeafRigid(declared->arrayElement, target->arrayElement,
                                  paramNames, solved, isRigidParam);
        case TypeRef::Kind::Function:
        case TypeRef::Kind::Error:
            return RigidInv::Unknown;
    }
    return RigidInv::Unknown;
}

// §14.2 / §7.10 (v0.196, #1816 s7): does the leaf's OWN `where`-clause impose a
// constraint on parameter `param` that EXCLUDES a nullable type argument? Per
// §14.2 the `notnull`, `class`, and `value` constraints and EVERY type bound
// (`where Y: I`, or the type-parameter bound `where Y: U`) imply a non-null
// argument. Used to collapse the §7.10 `?`-idempotence pre-image split: when such
// a constraint is present the nullable pre-image `Y := U?` is not a well-formed
// argument, so the split's two solutions reduce to the single non-null pre-image
// (⇒ determined); absent one, both pre-images inhabit (⇒ undetermined). A purely
// SYNTACTIC check on the constraint KIND — never a census of a bound's conformers
// (the §19.6 census prohibition). `new` is deliberately EXCLUDED: a nullable type
// satisfies `new` via its `null` default, so §14.2 does not make `new` imply
// non-null (the nullable pre-image's non-`new`-exclusion is why this runs BEFORE
// clause (b), which would otherwise EXEMPT a `new`-constrained non-`new` non-null
// pre-image while the nullable pre-image still inhabits).
static bool leafParamExcludesNullable(const TypeSymbol* leaf,
                                      std::string_view param) {
    if (!leaf) return false;
    if (auto* td = dynamic_cast<const ast::TypeDecl*>(leaf->decl)) {
        for (const auto& gc : td->genericConstraints) {
            if (gc.typeParam != param) continue;
            for (const auto& item : gc.items)
                if (item.kind == "notnull" || item.kind == "class" ||
                    item.kind == "value" || item.kind == "type")
                    return true;  // §14.2: all imply non-null Y
        }
    }
    return false;
}

// §19.6 (v0.196, #1816 s3/s7/s8): format the missing-coverage obligation for an
// UNDETERMINED / statically-unattainable generic leaf. The leaf is completable
// only by an irrefutable arm OR by restructuring the hierarchy into the
// determined shape (§19.6 v0.196: the warning "names the leaf and its
// undetermined parameter(s)" and prescribes both remedies). Three shapes reach
// here: a leaf parameter with NO occurrence in the root instantiation (s3 — free
// after solving, infinitely many inhabiting instantiations); a §7.10 `?`-split
// parameter whose nullable pre-image survives (s7 — two inhabiting instantiations
// per §7.10); and a rigid-identity-undecidable leaf with no free parameter to
// name (s4 — the undecidability is in the scrutinee). `params` names the
// undetermined leaf parameter(s), empty for the rigid-identity case.
static std::string formatMissingGenericLeaf(
    const TypeSymbol* leaf, const std::vector<std::string_view>& params) {
    std::string s(leaf->name);
    if (params.empty()) {
        // s4: a rigid-identity-undecidable leaf (e.g. `IntError: Result<int>`
        // matched at `Result<T>`) — no free leaf parameter to name. Both remedies:
        // an irrefutable arm, or restructuring so the leaf's instantiation is
        // determined at the scrutinee type.
        s += " (coverage statically unattainable — an undecidable rigid scrutinee "
             "component): either add a `_` arm (or an unguarded untyped `var`), or "
             "restructure the hierarchy so the leaf's instantiation is determined "
             "at the scrutinee type";
        return s;
    }
    // s3/s7: name the undetermined parameter(s) and prescribe BOTH remedies —
    // (a) an irrefutable arm, or (b) restructuring to the determined shape by
    // mentioning every leaf parameter in the base clause and, for a `?`-split
    // parameter, constraining it non-nullable (§7.10 / §19.6 v0.196).
    s += params.size() == 1 ? " (undetermined parameter `"
                            : " (undetermined parameters `";
    for (std::size_t i = 0; i < params.size(); ++i) {
        if (i) s += "`, `";
        s += std::string(params[i]);
    }
    s += "`): coverage is statically unattainable — either add a `_` arm (or an "
         "unguarded untyped `var`), or restructure to the determined shape by "
         "mentioning every leaf parameter in the base clause (and, for a `?`-split "
         "parameter, constraining it non-nullable)";
    return s;
}

// §19.6 (v0.196, #1816 s5): the three-valued value/reference category of a
// FULLY-GROUND solved type, mirroring Resolver::categorize (§14.2 value table).
// Used to decide a `class` / `value` scalar `where`-constraint against a leaf's
// ground solution. A ground solution carries a declaration on every Named node
// (isFullyGroundTypeRef), so Unknown is only the defensive fallback — and the
// caller acts ONLY on the definite Value / Reference verdict, never on Unknown
// (never a false exemption of an inhabited leaf).
enum class GroundCat { Reference, Value, Unknown };
static GroundCat groundCategorize(const TypeRef* a) {
    if (!a) return GroundCat::Unknown;
    switch (a->kind) {
        case TypeRef::Kind::Unit:         // the unit type is a value type (§14.2)
        case TypeRef::Kind::InlineArray:  // inline arrays are value types (§15.1)
            return GroundCat::Value;
        case TypeRef::Kind::Function:     // function values are reference (§14.2)
            return GroundCat::Reference;
        case TypeRef::Kind::Primitive:
            // `string` / `Object` are reference; every other primitive
            // (bool/char/numerics) is a value type (§14.2 / §3).
            return (a->primitive == PrimitiveType::String ||
                    a->primitive == PrimitiveType::Object)
                       ? GroundCat::Reference
                       : GroundCat::Value;
        case TypeRef::Kind::Named:
            // Classes and interfaces are reference types; records, enums, and
            // choices are value types (§9 / §10 / §14.2 / §35, v0.200 #2044).
            // A ground Named always carries a declaration.
            if (a->declaration) {
                switch (a->declaration->kind) {
                    case ast::TypeDeclKind::Class:
                    case ast::TypeDeclKind::Interface:
                        return GroundCat::Reference;
                    case ast::TypeDeclKind::Record:
                    case ast::TypeDeclKind::Enum:
                    case ast::TypeDeclKind::Choice:
                        return GroundCat::Value;
                }
            }
            return GroundCat::Unknown;
        case TypeRef::Kind::Error:
            return GroundCat::Unknown;
    }
    return GroundCat::Unknown;
}

} // namespace

// §19.6 (v0.196, #1816): see the header comment. Walk `leaf`'s single-
// inheritance class chain up to `root`, composing the base-clause type-arg
// substitutions level by level, and return `root`'s instantiation expressed
// in `leaf`'s own type parameters. Template: the base-chain compose loop in
// ExprTypecheck_Calls.cpp (substituteMemberType), which walks a derived class
// up to a parent composing type-arg bindings and substitutes with
// substituteReturnType.
TypeRef* ExprTC::leafRootInstantiation(TypeSymbol* leaf, TypeSymbol* root) {
    if (!leaf || !root || leaf == root) return nullptr;
    const TypeSymbol* cur = leaf;
    // curSubst maps cur's generic-param NAME -> the TypeRef it stands for,
    // expressed in `leaf`'s own params. Empty at `leaf` itself = identity
    // (leaf's params are not keys, so substituteReturnType passes them through
    // unchanged). Composed forward at each derivation step.
    std::unordered_map<std::string_view, TypeRef*> curSubst;
    std::unordered_set<const TypeSymbol*> seen;
    while (cur && cur != root) {
        if (!seen.insert(cur).second) return nullptr;  // cycle guard
        // The single class base (base clauses list the base class first, then
        // interfaces; §11 single inheritance). baseTypeRefs is parallel to
        // baseTypes and carries the base instantiation's generic args.
        const TypeSymbol* base = nullptr;
        TypeRef* baseRef = nullptr;
        for (std::size_t bi = 0; bi < cur->baseTypes.size(); ++bi) {
            if (cur->baseTypes[bi] &&
                cur->baseTypes[bi]->kind == ast::TypeDeclKind::Class) {
                base = cur->baseTypes[bi];
                baseRef =
                    (bi < cur->baseTypeRefs.size()) ? cur->baseTypeRefs[bi]
                                                    : nullptr;
                break;
            }
        }
        if (!base || !baseRef) return nullptr;  // broken / unresolved chain
        // Re-express the base instantiation in leaf's params by substituting
        // cur's params (which curSubst already maps to leaf params).
        TypeRef* baseInLeaf = substituteReturnType(baseRef, curSubst);
        if (base == root) return baseInLeaf;  // the composed root instantiation
        // Compose forward: base's param -> the arg it received, in leaf params.
        std::unordered_map<std::string_view, TypeRef*> nextSubst;
        if (!base->genericParams.empty()) {
            if (!baseInLeaf ||
                baseInLeaf->genericArgs.size() != base->genericParams.size())
                return nullptr;  // arity mismatch — unresolvable generic link
            for (std::size_t gi = 0; gi < base->genericParams.size(); ++gi)
                if (base->genericParams[gi])
                    nextSubst[base->genericParams[gi]->name] =
                        baseInLeaf->genericArgs[gi];
        }
        cur = base;
        curSubst = std::move(nextSubst);
    }
    return nullptr;  // `root` not reached on the class chain
}

// §19.6 (v0.196, #1816 s2): see the header comment. Solve `leaf`'s parameters
// against the GROUND scrutinee `scrutTy` by composing `leaf`'s root
// instantiation and matching it against `scrutTy` under component-wise invariant
// exact identity (matchLeafInvariant). The caller has already established
// `isFullyGroundTypeRef(scrutTy)` — this solver is the ground-scrutinee branch of
// the determination rule; a rigid `S` is a later lane's domain.
ExprTC::LeafInstSolutions ExprTC::solveLeafInstantiations(
    TypeSymbol* leaf, TypeSymbol* root, const TypeRef* scrutTy) {
    LeafInstSolutions result;  // defaults to Undetermined (caller keeps the leaf)
    if (!leaf || !root || !scrutTy) return result;

    TypeRef* rootInst = leafRootInstantiation(leaf, root);
    if (!rootInst) return result;  // uncomposable chain — keep (name-match)

    // The leaf's own parameters are the solve variables; a non-generic leaf has
    // none and its only candidate is the empty substitution.
    std::unordered_set<std::string_view> paramNames;
    for (const Symbol* gp : leaf->genericParams)
        if (gp) paramNames.insert(gp->name);

    std::unordered_map<std::string_view, TypeRef*> solved;
    // §7.10 (v0.196, #1816 s7): leaf params occurring as `Y?` against a nullable
    // scrutinee argument — the idempotence pre-image split points (see below).
    std::unordered_set<std::string_view> splitParams;
    InvMatch m =
        matchLeafInvariant(rootInst, scrutTy, paramNames, solved, ctx_, splitParams);

    if (m == InvMatch::Fail) {
        // No inhabiting instantiation — uninhabited at S (§19.6), exempt.
        result.outcome = LeafInstSolutions::Outcome::NoSolution;
        return result;
    }
    if (m == InvMatch::Unknown) {
        result.outcome = LeafInstSolutions::Outcome::Undetermined;
        return result;
    }
    // m == Ok: a clean solve. Determined iff every leaf parameter is bound; an
    // unbound parameter (no occurrence in the root instantiation) leaves the
    // solution set unbounded — undetermined (§19.6: "a parameter with no
    // occurrence in the root instantiation … never exempt or determined").
    // (#1816 s3) Collect the FREE parameters in declaration order, so the
    // caller can name them in the unattainable-coverage warning; a free
    // parameter is the OPPOSITE of s2's NoSolution — every substitution
    // inhabits, so coverage is statically unattainable rather than exempt.
    for (const Symbol* gp : leaf->genericParams)
        if (gp && !solved.count(gp->name))
            result.freeParams.push_back(gp->name);
    if (!result.freeParams.empty()) {
        result.outcome = LeafInstSolutions::Outcome::Undetermined;
        return result;
    }
    // (#1816 s7) §7.10 `?`-IDEMPOTENCE PRE-IMAGE SPLIT. A `Y?` occurrence in the
    // root instantiation matched against a NULLABLE scrutinee argument admits the
    // finite pre-image set {`Y := U`, `Y := U?`} (`(U?)? ≡ U?`, §7.10). The
    // NON-null pre-image is already bound in `solved`; the NULLABLE pre-image ALSO
    // inhabits UNLESS a §14.2 constraint on `Y` excludes nullable arguments
    // (leafParamExcludesNullable). A split param whose nullable pre-image survives
    // ⇒ ≥2 inhabiting solutions ⇒ the leaf is UNDETERMINED — coverage statically
    // unattainable, so it is named like a free parameter. When every split param's
    // nullable pre-image is excluded, the split collapses to the sole non-null
    // pre-image and the leaf falls through to clause (b) / Determined. Checked
    // BEFORE clause (b) so an unconstrained (or only `new`-constrained) `Y` whose
    // nullable pre-image inhabits is never wrongly EXEMPTED by its non-null
    // pre-image failing clause (b) (§7.10 v0.196).
    for (const Symbol* gp : leaf->genericParams)
        if (gp && splitParams.count(gp->name) &&
            !leafParamExcludesNullable(leaf, gp->name))
            result.freeParams.push_back(gp->name);  // nullable pre-image survives
    if (!result.freeParams.empty()) {
        result.outcome = LeafInstSolutions::Outcome::Undetermined;
        return result;
    }
    // (#1816 s5) CLAUSE (b): filter the sole ground solution by the leaf-chain
    // scalar `where`-constraints. A solution that PROVABLY violates a
    // class/value/notnull/new constraint on the leaf or any base along its
    // derivation chain is not a well-formed instantiation (§14.2) — the
    // singleton solution set is emptied ⇒ the leaf is uninhabited at `S` and
    // EXEMPT (§19.6 v0.196: "every solution fails clause (b) … uninhabited …
    // exempt"). Only the fully-bound Determined case is filtered; a free
    // parameter (above) already left the set unbounded whatever its bounds.
    if (leafSolutionViolatesScalarConstraint(leaf, root, solved)) {
        result.outcome = LeafInstSolutions::Outcome::NoSolution;
        return result;
    }
    result.outcome = LeafInstSolutions::Outcome::Determined;
    result.solution = std::move(solved);
    return result;
}

// §19.6 (v0.196, #1816 s4): see the header comment. Solve `leaf`'s parameters
// against a NON-GROUND (rigid) scrutinee `scrutTy` using the rigid-aware
// three-valued identity (matchLeafRigid, keyed on isInScopeTypeParam). A leaf is
// NEVER exempt under a rigid `S`: an undecidable rigid-identity component
// (`IntError: Result<int>` at `Result<T>`) or a free leaf parameter marks the
// leaf UNATTAINABLE (the caller reports it unconditionally); a clean determined
// solve (`Ok<T>: Result<T>` at `Result<T>` ⇒ `Y := T`) is DETERMINED and left
// for the name-match, so the determined rigid shape stays exhaustive (§19.6
// Test 2). A decidable ground-structure Fail or an undecidable edge is KEPT
// (deferred to the name-match) — conservative: at worst a spurious missing-leaf
// warning, never a false exemption.
ExprTC::LeafInstSolutions ExprTC::solveLeafInstantiationsRigid(
    TypeSymbol* leaf, TypeSymbol* root, const TypeRef* scrutTy) {
    LeafInstSolutions result;  // defaults: Undetermined, unattainable = false
    if (!leaf || !root || !scrutTy) return result;

    TypeRef* rootInst = leafRootInstantiation(leaf, root);
    if (!rootInst) return result;  // uncomposable chain — keep (name-match)

    std::unordered_set<std::string_view> paramNames;
    for (const Symbol* gp : leaf->genericParams)
        if (gp) paramNames.insert(gp->name);

    std::unordered_map<std::string_view, TypeRef*> solved;
    auto isRigid = [this](std::string_view n) { return isInScopeTypeParam(n); };
    RigidInv m = matchLeafRigid(rootInst, scrutTy, paramNames, solved, isRigid);

    // A rigid-identity undecidable component ⇒ statically UNATTAINABLE. The leaf
    // is never exempt and no specific-instantiation type-test arm discharges it
    // — only an irrefutable arm (§19.6 v0.196). Reported with NO parameter names
    // (the undecidability is in the scrutinee, not a free leaf parameter).
    if (m == RigidInv::Unattainable) {
        result.unattainable = true;
        return result;
    }
    // Fail (a genuine ground-structure mismatch on a partially-rigid `S`) or an
    // undecidable edge (Unknown): KEEP — never exempt under a rigid `S`; the
    // name-match settles coverage.
    if (m != RigidInv::Ok) return result;

    // m == Ok: a clean solve. A leaf parameter left FREE (no occurrence in the
    // root instantiation) leaves infinitely many instantiations inhabiting `S`
    // ⇒ statically UNATTAINABLE, named in the warning (as in the ground s3
    // case). Otherwise every leaf parameter is bound (to a rigid or ground type)
    // — the leaf is DETERMINED and its obligation is expressible as an arm
    // (§19.6 v0.196); KEPT, the name-match discharges it.
    for (const Symbol* gp : leaf->genericParams)
        if (gp && !solved.count(gp->name))
            result.freeParams.push_back(gp->name);
    if (!result.freeParams.empty()) {
        result.unattainable = true;
        return result;
    }
    result.outcome = LeafInstSolutions::Outcome::Determined;
    result.solution = std::move(solved);
    return result;
}

// §19.6 (v0.196, #1816 s5): does a FULLY-GROUND solved type PROVABLY satisfy a
// `new` (accessible parameterless ctor) constraint? Mirrors
// Resolver::argSatisfiesNew (§14.2 / §17.11), reoriented to a ground leaf
// solution. `decided` is left false whenever the answer is not verifiable here —
// the caller then KEEPS the solution (a `new` filter never causes a false
// exemption of an inhabited leaf).
bool ExprTC::groundSatisfiesNew(const TypeRef* arg, bool& decided) {
    decided = false;
    if (!arg) return false;
    if (arg->kind == TypeRef::Kind::Primitive) {
        // `string` / `Object` are reference primitives — undecided here (the
        // conservative default). Every value primitive has a well-defined zero.
        if (arg->primitive == PrimitiveType::String ||
            arg->primitive == PrimitiveType::Object)
            return false;
        decided = true;
        return true;
    }
    if (arg->kind == TypeRef::Kind::Unit ||
        arg->kind == TypeRef::Kind::InlineArray) {
        decided = true;  // value types with a well-defined default (§17.11)
        return true;
    }
    if (arg->kind == TypeRef::Kind::Named && arg->declaration) {
        TypeSymbol* d = arg->declaration;
        if (d->kind == ast::TypeDeclKind::Enum) {
            decided = true;  // enum default is the underlying-0 value (§17.11)
            return true;
        }
        if (d->kind == ast::TypeDeclKind::Interface) {
            decided = true;
            return false;  // interfaces are not constructible (§11.2)
        }
        // §35.10 (v0.200, #2044): "Choices never satisfy the value-type arm:
        // `default(C)` is undefined for every choice type." So `where T: new()`
        // is unsatisfiable for a choice substitution.
        if (d->kind == ast::TypeDeclKind::Choice) {
            decided = true;
            return false;
        }
        if (d->kind == ast::TypeDeclKind::Record) {
            // A positional record satisfies new() iff default(R) is well-defined
            // — every field has a well-defined default (§14.2 / §17.11): reject a
            // non-nullable reference field.
            auto* td = dynamic_cast<const ast::TypeDecl*>(d->decl);
            if (!td) return false;  // can't verify — defer (decided stays false)
            decided = true;
            for (const auto& pf : td->positionalFields) {
                if (!pf.type) continue;
                TypeRef* ft = ctx_.resolvedType(*pf.type);
                if (!ft) { decided = false; return false; }  // can't verify
                if (ft->nullable) continue;  // nullable field → defaults null
                if (groundCategorize(ft) == GroundCat::Reference)
                    return false;  // non-nullable reference field: no default
            }
            return true;
        }
        if (d->kind == ast::TypeDeclKind::Class) {
            // A class satisfies new() iff it has an accessible parameterless
            // constructor, or declares no constructor at all (the implicit
            // default). A non-empty primary param list is a non-parameterless
            // constructor (mirroring Resolver::argSatisfiesNew).
            auto* td = dynamic_cast<const ast::TypeDecl*>(d->decl);
            if (td && td->hasPositionalFields && !td->positionalFields.empty()) {
                decided = true;
                return false;  // primary ctor has parameters
            }
            std::vector<const ConstructorSymbol*> ctors;
            for (Symbol* m : d->members)
                if (auto* cs = dynamic_cast<const ConstructorSymbol*>(m))
                    if (!cs->isStatic) ctors.push_back(cs);
            decided = true;
            if (ctors.empty()) return true;  // implicit default ctor
            for (const ConstructorSymbol* cs : ctors)
                if (cs->params.empty() &&
                    cs->visibility != ast::Visibility::Private)
                    return true;  // accessible parameterless ctor
            return false;
        }
    }
    // Built-ins / generic-param / function args: defer (decided stays false).
    return false;
}

// §19.6 (v0.196, #1816 s6): does a FULLY-GROUND solved type PROVABLY satisfy a
// type-bound `where Y: I` (`item.kind == "type"`)? A type bound is discharged by
// §14.2 declaration-based subtyping — ExprTC::isSubtype walks the class- and
// interface-base chain and (deliberately) IGNORES generic arguments, which is
// decidable ONLY on an already-GROUND σ(Y). A still-free parameter is NEVER
// rescued here by enumerating the bound's conformers: that is the §19.6 CENSUS
// PROHIBITION — a bound's conformer set is module-visibility-dependent, so a
// census would make the verdict cross-module-unstable and could WRONGLY prove
// exhaustiveness (declines D8: zero-conformer / sealed bounds do NOT become
// census-resolved). A free param never reaches this filter anyway — the s3
// no-occurrence check already returned Undetermined. `decided` is left false
// whenever satisfaction is not verifiable here, so the caller KEEPS the solution
// (a type-bound filter never causes a FALSE exemption of an inhabited leaf).
bool ExprTC::groundSatisfiesTypeBound(const TypeRef* arg, const TypeRef* bound,
                                      bool& decided) {
    decided = false;
    if (!arg || !bound) return false;
    // The bound must resolve to a concrete Named interface/class. A type-
    // PARAMETER bound (`where Y: U`, U another enclosing parameter) carries no
    // declaration — undecidable here, defer (keep). A non-Named bound (e.g.
    // `Object`) likewise: every type is its subtype, so never a violation.
    if (bound->kind != TypeRef::Kind::Named || !bound->declaration) return false;
    // §14.2: a type-bound constraint excludes every nullable type argument — a
    // nullable σ(Y) is not assignable to the non-nullable bound (mirrors the
    // method-call-site check, ExprTypecheck_Calls.cpp). Provably violated.
    if (arg->nullable) { decided = true; return false; }
    if (arg->kind == TypeRef::Kind::Named && arg->declaration) {
        // §14.2 declaration-based subtyping — decidable on a ground σ(Y). It
        // conservatively ignores generic args, so a same-declaration but wrong-
        // generic-arg conformer reads as SATISFYING (a spurious KEEP, never a
        // false exemption — the sound direction).
        decided = true;
        return isSubtype(arg->declaration, bound->declaration);
    }
    if (arg->kind == TypeRef::Kind::Primitive) {
        // A primitive conforms ONLY to the compiler-provided builtin interfaces
        // (§3.5 / §8.6 / stdlib §2.19) — it can never subtype a USER-declared
        // interface or class. Against a builtin interface bound, DEFER (it may
        // conform via a shape not modeled here — keep, conservative); against a
        // user-declared bound, the primitive PROVABLY does not conform ⇒ drop.
        if (bound->declaration ==
            ctx_.builtinInterfaceSym(bound->declaration->name))
            return false;  // builtin bound — defer (decided stays false, keep)
        decided = true;
        return false;  // user-declared bound: primitive can't conform → violates
    }
    // Unit / InlineArray / Function ground σ(Y): value/function types whose
    // conformance to an interface bound is not modeled here — defer (keep).
    return false;
}

// §19.6 (v0.196, #1816 s5+s6): CLAUSE (b) of the determination rule. After the
// ground solver (s2) yields the leaf's SOLE inhabiting substitution (a
// Determined singleton solution), FILTER it by the leaf's own + every chain-
// base's `where`-constraints — the SCALAR fragment (`class` / `value` /
// `notnull` / `new`, s5) AND the type-bound `where Y: I` (s6, decided by
// declaration-based subtyping on the ground σ(Y), never a census of conformers).
// A ground solution σ that PROVABLY violates such a constraint is not a
// well-formed instantiation (§14.2) ⇒ not a real inhabitant ⇒ the singleton
// solution set becomes empty ⇒ the leaf is uninhabited at `S` and EXEMPT (the
// caller collapses this to s2's NoSolution outcome). Only a PROVABLE violation
// drops a solution; a satisfied or undecidable constraint keeps it (dropping a
// satisfying/undecidable solution would wrongly exempt an inhabited leaf).
// Walks the single-inheritance derivation chain `leaf` → `root`, mapping each
// decl level's own generic-param NAMES to their ground types under σ (mirroring
// leafRootInstantiation's compose loop), so a base clause's `where` reaches the
// ground argument it constrains.
bool ExprTC::leafSolutionViolatesScalarConstraint(
    TypeSymbol* leaf, TypeSymbol* root,
    const std::unordered_map<std::string_view, TypeRef*>& solution) {
    if (!leaf) return false;

    // Apply ONE scalar constraint item to a ground argument; true iff it is
    // PROVABLY violated (the §14.2 reported-violation conditions of
    // Resolver::checkOneConstraint, restricted to the ground/decidable cases).
    auto violates = [&](const ast::ConstraintItem& item,
                        const TypeRef* arg) -> bool {
        if (!arg || !isFullyGroundTypeRef(arg))
            return false;  // non-ground (rigid) σ component — s6, never dropped here
        if (item.kind == "notnull")
            return arg->nullable;  // §14.2: excludes every nullable type
        if (item.kind == "class") {
            // §14.2: non-nullable reference types only.
            if (arg->nullable) return true;
            return groundCategorize(arg) == GroundCat::Value;
        }
        if (item.kind == "value") {
            // §14.2: non-nullable value types only.
            if (arg->nullable) return true;
            return groundCategorize(arg) == GroundCat::Reference;
        }
        if (item.kind == "new") {
            bool decided = false;
            bool ok = groundSatisfiesNew(arg, decided);
            return decided && !ok;  // provably no accessible parameterless ctor
        }
        if (item.kind == "type") {
            // §19.6 v0.196 (s6): a type-bound `where Y: I`. Drop σ(Y) ONLY on a
            // PROVABLE non-subtype of the resolved bound (declaration-based, on
            // the ground σ(Y)). The census prohibition holds structurally — a
            // free param never reaches this filter, and isSubtype is used only on
            // ground σ(Y), never to enumerate a bound's conformers.
            if (!item.typeArg) return false;  // malformed — keep
            TypeRef* bound = ctx_.resolvedType(*item.typeArg);
            bool decided = false;
            bool ok = groundSatisfiesTypeBound(arg, bound, decided);
            return decided && !ok;  // provably outside the bound ⇒ σ drops out
        }
        return false;  // unknown kind: not a scalar filter
    };

    const TypeSymbol* cur = leaf;
    // argSubst: cur's own generic-param NAME -> the ground TypeRef it stands for
    // under σ. At the leaf itself each param maps to its σ binding; at each base
    // level the param maps to the arg it received (expressed in leaf params,
    // then σ-substituted). curSubst: cur's param -> its arg expressed in LEAF
    // params, the composition state that carries to the next base (identity at
    // the leaf), mirroring leafRootInstantiation.
    std::unordered_map<std::string_view, TypeRef*> argSubst;
    for (const Symbol* gp : leaf->genericParams)
        if (gp) {
            auto it = solution.find(gp->name);
            argSubst[gp->name] = (it != solution.end()) ? it->second : nullptr;
        }
    std::unordered_map<std::string_view, TypeRef*> curSubst;
    std::unordered_set<const TypeSymbol*> seen;
    while (cur) {
        if (!seen.insert(cur).second) break;  // cycle guard
        // (b): this decl's own scalar where-constraints against the ground args.
        if (auto* td = dynamic_cast<const ast::TypeDecl*>(cur->decl)) {
            for (const auto& gc : td->genericConstraints) {
                auto ai = argSubst.find(gc.typeParam);
                if (ai == argSubst.end() || !ai->second)
                    continue;  // param not solved to a ground type — keep
                for (const auto& item : gc.items)
                    if (violates(item, ai->second))
                        return true;  // provable violation ⇒ σ drops out
            }
        }
        if (cur == root) break;
        // Advance to the single class base, composing the substitution level by
        // level (identical shape to leafRootInstantiation).
        const TypeSymbol* base = nullptr;
        TypeRef* baseRef = nullptr;
        for (std::size_t bi = 0; bi < cur->baseTypes.size(); ++bi)
            if (cur->baseTypes[bi] &&
                cur->baseTypes[bi]->kind == ast::TypeDeclKind::Class) {
                base = cur->baseTypes[bi];
                baseRef = (bi < cur->baseTypeRefs.size()) ? cur->baseTypeRefs[bi]
                                                          : nullptr;
                break;
            }
        if (!base || !baseRef) break;  // broken / unresolved chain — stop (keep)
        TypeRef* baseInLeaf = substituteReturnType(baseRef, curSubst);
        std::unordered_map<std::string_view, TypeRef*> nextArg, nextSubst;
        if (!base->genericParams.empty()) {
            if (!baseInLeaf ||
                baseInLeaf->genericArgs.size() != base->genericParams.size())
                break;  // arity mismatch — stop (keep)
            for (std::size_t gi = 0; gi < base->genericParams.size(); ++gi)
                if (base->genericParams[gi]) {
                    TypeRef* argInLeaf = baseInLeaf->genericArgs[gi];
                    nextSubst[base->genericParams[gi]->name] = argInLeaf;
                    nextArg[base->genericParams[gi]->name] =
                        substituteReturnType(argInLeaf, solution);
                }
        }
        cur = base;
        curSubst = std::move(nextSubst);
        argSubst = std::move(nextArg);
    }
    return false;
}

// §19.6 v0.196 (#1862): sound structural equality of a case-arm generic
// argument `arm` against a determined leaf-instantiation component `det`, for
// the arg-SENSITIVE coverage match below. Ground components compare via
// typeRefEqual; a RIGID enclosing-body type parameter (a bare Named TypeRef
// with no declaration, §19.6's rigid component) is compared by HEAD NAME —
// never by declaration pointer, since typeRefEqual is null-declaration blind
// and would equate two DISTINCT rigid parameters (an `Ok<U>` arm wrongly
// covering the determined `Ok<T>`), reopening the false-exemption hole. Any
// component this cannot PROVE identical returns false, so an uncertain arm does
// NOT discharge the obligation — the sound, warn-leaning direction.
static bool sameInstantiationArg(const TypeRef* arm, const TypeRef* det) {
    if (!arm || !det) return false;
    if (arm->nullable != det->nullable) return false;
    bool armRigid = arm->kind == TypeRef::Kind::Named && !arm->declaration;
    bool detRigid = det->kind == TypeRef::Kind::Named && !det->declaration;
    if (armRigid || detRigid) {
        // A rigid parameter is covered only by the SAME-named rigid parameter.
        return armRigid && detRigid && arm->genericArgs.empty() &&
               det->genericArgs.empty() && !arm->headName().empty() &&
               arm->headName() == det->headName();
    }
    if (arm->kind != det->kind) return false;
    if (arm->kind == TypeRef::Kind::Named) {
        if (arm->declaration != det->declaration) return false;
        if (arm->genericArgs.size() != det->genericArgs.size()) return false;
        for (std::size_t i = 0; i < arm->genericArgs.size(); ++i)
            if (!sameInstantiationArg(arm->genericArgs[i], det->genericArgs[i]))
                return false;
        return true;
    }
    // Primitive / Unit / InlineArray / Function: ground-decidable, no rigid-
    // parameter blindness for these kinds — defer to typeRefEqual.
    return typeRefEqual(arm, det);
}

// §19.6 v0.196 (#1862): does the case arm whose resolved typed-binding type is
// `armTy` COVER the DETERMINED generic leaf `leaf`, whose sole inhabiting
// instantiation is `leaf<solution…>`? Arg-SENSITIVE: the arm must name `leaf`
// itself with generic arguments that match the determined substitution σ
// component-wise (the determination rule's "covered exactly as a non-generic
// leaf is — by an unguarded type pattern it is assignable to"). A wrong-
// instantiation arm (`Ok<string>` at determined `Ok<int>`, or a rigid `Ok<int>`
// against determined `Ok<T>`) — or a bare arm carrying no instantiation
// (`armTy == nullptr`) — does NOT discharge it, so a match that is not
// exhaustive is no longer wrongly deemed exhaustive by declaration NAME alone.
static bool armCoversDeterminedLeaf(
    const TypeRef* armTy, const TypeSymbol* leaf,
    const std::unordered_map<std::string_view, TypeRef*>& solution) {
    if (!armTy || armTy->kind != TypeRef::Kind::Named) return false;
    if (armTy->declaration != leaf) return false;
    if (armTy->genericArgs.size() != leaf->genericParams.size()) return false;
    for (std::size_t i = 0; i < leaf->genericParams.size(); ++i) {
        const Symbol* gp = leaf->genericParams[i];
        if (!gp) return false;
        auto it = solution.find(gp->name);
        if (it == solution.end() || !it->second) return false;  // unsolved param
        if (!sameInstantiationArg(armTy->genericArgs[i], it->second))
            return false;
    }
    return true;
}

// Forward declaration (defined below): §12.9 variance-aware generic
// assignability, reused by checkCaseExhaustiveness's #784/#1874 typed-binding
// coverage check to make a same-declaration generic binding arg-SENSITIVE.
static bool variantGenericAssignable(const TypeRef*, const TypeRef*,
                                     bool (*)(TypeSymbol*, TypeSymbol*));

// §19.6 v0.196 (#1874/#1912): does an unguarded typed binding `var x: bt` cover
// EVERY non-null value of a scrutinee whose instantiation projected to `bt`'s
// declaration is `proj`? See the header-comment for the contract. The variance
// check (variantGenericAssignable) treats a RIGID enclosing-body type parameter
// as a free variable compatible with any `bt` argument — too lenient for the
// "covers ALL values" proof, which must hold at EVERY instantiation of the
// enclosing body (§31.2.1). So after the variance check, reject any position
// where `proj`'s arg is a rigid parameter and `bt`'s arg is not that SAME rigid
// parameter; ground args are left to the variance check, so covariant coverage
// is undisturbed.
bool ExprTC::typedBindingCoversAllValues(const TypeRef* proj,
                                         const TypeRef* bt) {
    if (!proj || !bt) return false;
    bool covers = variantGenericAssignable(proj, bt, isSubtype);
    if (covers && proj->genericArgs.size() == bt->genericArgs.size()) {
        for (std::size_t gi = 0; gi < proj->genericArgs.size(); ++gi) {
            const TypeRef* sa = proj->genericArgs[gi];
            const TypeRef* ba = bt->genericArgs[gi];
            bool saRigid = sa && sa->kind == TypeRef::Kind::Named &&
                           sa->genericArgs.empty() &&
                           isInScopeTypeParam(sa->headName());
            if (!saRigid) continue;  // ground arg — variance ruled it
            bool baSameRigid = ba && ba->kind == TypeRef::Kind::Named &&
                               ba->genericArgs.empty() &&
                               sa->nullable == ba->nullable &&
                               !ba->headName().empty() &&
                               ba->headName() == sa->headName();
            if (!baSameRigid) return false;
        }
    }
    return covers;
}

// §19.6 v0.196 (#1874): the ANCESTOR/subtype-walk counterpart of
// armCoversDeterminedLeaf. A pattern naming a proper ancestor `matchTy` of leaf
// `leaf` covers `leaf` via the subtype walk (§19.3) — but when `leaf` is a
// DETERMINED generic leaf, that cover is arg-SENSITIVE exactly as the direct
// self-match is: the determination rule fixes `leaf`'s obligation to its sole
// admitted instantiation `leaf<σ>` and requires it "covered … by an unguarded
// type pattern it is assignable to". `leaf<σ>` is assignable to the ancestor arm
// `armTy` (an instantiation of `matchTy`) iff `leaf<σ>`'s instantiation projected
// up to `matchTy` — leafRootInstantiation(leaf, matchTy) substituted by σ — is
// IDENTICAL to `armTy` (class hierarchies being invariant, §12.9). So a
// wrong-instantiation ancestor arm (`Result<string>` at scrutinee `Result<int>`)
// does NOT discharge the determined `Ok<int>`/`Err<int>`, closing the residual
// the direct-path fix left. A bare NamedPattern ancestor (`armTy == nullptr`)
// carries no instantiation and never discharges a determined generic leaf,
// symmetric with armCoversDeterminedLeaf.
bool ExprTC::ancestorArmCoversDeterminedLeaf(
    const TypeRef* armTy, TypeSymbol* leaf, TypeSymbol* matchTy,
    const std::unordered_map<std::string_view, TypeRef*>& solution) {
    if (!armTy || armTy->kind != TypeRef::Kind::Named) return false;
    if (armTy->declaration != matchTy) return false;
    // The leaf's instantiation of the ancestor `matchTy`, in the leaf's own
    // params (`Ok<Y>: Result<Y>` ⇒ `Result<Y>`), then substituted by the
    // determined σ so it names `leaf`'s sole obligation at `matchTy`.
    TypeRef* ancInst = leafRootInstantiation(leaf, matchTy);
    if (!ancInst || ancInst->kind != TypeRef::Kind::Named ||
        ancInst->declaration != matchTy)
        return false;  // uncomposable / mismatched chain — cannot prove cover
    TypeRef* ancDet = substituteReturnType(ancInst, solution);
    if (!ancDet || ancDet->genericArgs.size() != armTy->genericArgs.size())
        return false;
    for (std::size_t i = 0; i < armTy->genericArgs.size(); ++i)
        if (!sameInstantiationArg(armTy->genericArgs[i], ancDet->genericArgs[i]))
            return false;  // any component not provably identical ⇒ no cover
    return true;
}

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
    // Expression-position `case`: emit the §19.6 non-exhaustiveness WARNING via
    // the shared verdict (the discarded return value carries the same fact).
    caseArmsExhaustive(ce.arms, ce.scrutinee.get(), scrutTy, ce.span.begin,
                       /*quiet=*/false);
}

// §19.6 / §35.6: the shared exhaustiveness VERDICT — returns true iff `arms`
// provably cover every value of the scrutinee (the spec-listed shapes: an
// unguarded `_`/untyped-`var x` catch-all, a total `..` range, a covering typed
// binding, and enum / bool / choice / sealed-abstract closed-hierarchy coverage,
// each in its nullable variant). Reused by BOTH the expression-position warning
// (checkCaseExhaustiveness, quiet=false — emits the §19.6 warning and runs the
// §5.1 pattern-range side-check exactly as before) and the statement-position
// totality analysis (stmtAlwaysExits, quiet=true — §21.7 / #2129: a case
// STATEMENT whose arms are exhaustive AND all diverge returns on every path,
// because an unmatched value in statement position falls through (§19.6) rather
// than raising, so only exhaustiveness closes the fall-through edge). In quiet
// mode no diagnostic is emitted and the range side-check is skipped (the
// statement-position caller already ran it — re-running would double-report).
bool ExprTC::caseArmsExhaustive(
    const std::vector<ast::CaseArm>& arms, const ast::Expr* scrutinee,
    TypeRef* scrutTy, const SourceLocation& warnLoc, bool quiet) {
    // §5.1 / §19 (#1153): range-check integer pattern literals against the
    // scrutinee FIRST — the editable hook every expression-position `case`
    // reaches (it runs even when a `_` catch-all makes the check below return
    // early).
    if (!quiet) checkCasePatternConstantRanges(arms, scrutTy);
    // An unguarded catch-all (`_` / untyped `var x`) covers everything.
    for (const auto& arm : arms) {
        if (isUnguardedCatchAll(arm)) return true;
    }

    auto warn = [&](const std::string& detail) {
        if (quiet) return;
        ctx_.diagnostics().report(
            Severity::Warning, warnLoc,
            "non-exhaustive `case`: " + detail +
            " (add a `_` arm or cover the remaining value(s)) (§19.6)");
    };

    // A nullable scrutinee additionally requires an explicit `null` arm
    // for exhaustiveness (§19.6: "the rules above plus an explicit `null`
    // pattern"). If the value can be null but no arm matches null, the
    // case is non-exhaustive regardless of the non-null coverage below.
    bool nullableUncovered =
        scrutTy && scrutTy->nullable && !armsCoverNull(arms);

    // §19.3 / §19.5 / §19.6 (#1472): a bare `..` RangePattern (no lo/hi/step,
    // grammar A.7) is vacuously satisfied by every value in the value domain,
    // so an unguarded `..` arm covers all NON-NULL values — but a range pattern
    // never matches null (§19.5: null is matched only by `null`/untyped `var
    // x`/`_`). For a non-nullable scrutinee this is a total catch-all
    // (exhaustive); for a nullable scrutinee the non-null domain is covered but
    // `null` still needs its own arm (§19.6 nullable rule). Mirrors the
    // typed-binding-covers-the-full-type logic below. Without this a valid
    // total `..` case drew a spurious non-exhaustiveness warning.
    for (const auto& arm : arms) {
        if (arm.guard) continue;
        auto* rp = dynamic_cast<const ast::RangePattern*>(arm.pattern.get());
        if (!rp) continue;
        auto* re = dynamic_cast<const ast::RangeExpr*>(rp->rangeExpr.get());
        if (re && !re->lo && !re->hi && !re->step) {
            if (!nullableUncovered) return true;  // total over the value domain
            warn("the nullable scrutinee's non-null values are covered by a "
                 "`..` range pattern, but `null` is not");
            return false;
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
        for (const auto& arm : arms) {
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
                     scrutTy->declaration) {
                // §19.6 v0.196 (#1874/#1912): a typed binding `var x: bt` covers
                // all non-null values of the scrutinee only when the scrutinee
                // REFERENCE-CONVERTS to `bt` (§5.1) — which for a generic type is
                // arg-SENSITIVE. When `bt` names the scrutinee's OWN declaration
                // (`var x: Result<string>` at scrutinee `Result<int>`), the args
                // must be compatible under §12.9 variance (an invariant class
                // requires identity); when `bt` names a proper ANCESTOR of the
                // scrutinee's declaration (`var x: Result<string>` at scrutinee
                // `Ok<int>`/`Ok<T>`, the #1912 cross-declaration residual of
                // #1874), the scrutinee is PROJECTED up the class chain to `bt`'s
                // declaration and compared arg-sensitively the same way. A wrong-
                // instantiation binding does NOT cover, and control falls through
                // to the arg-sensitive closed-hierarchy leaf analysis below.
                if (bt->declaration == scrutTy->declaration) {
                    coversNonNull = typedBindingCoversAllValues(scrutTy, bt);
                } else if (!isSubtype(scrutTy->declaration, bt->declaration)) {
                    coversNonNull = false;  // not a declaration-level supertype
                } else if (TypeRef* proj = projectTypeToAncestorDecl(
                               scrutTy, bt->declaration)) {
                    // §19.6 v0.196 (#1912): `bt` is a CLASS-chain ancestor
                    // reachable by projection — compare the projected scrutinee
                    // instantiation against `bt` arg-sensitively, mirroring the
                    // same-declaration branch, so an ancestor arm no longer
                    // discharges coverage by declaration NAME alone (invariant
                    // class hierarchies, §12.9).
                    coversNonNull = typedBindingCoversAllValues(proj, bt);
                } else {
                    // `bt` is an INTERFACE ancestor / uncomposable chain —
                    // projection is not provable via the class chain. Keep the
                    // conservative declaration-level cover (preserves legitimate
                    // covariant-interface conversions), matching checkAssignable's
                    // conservative treatment of cross-declaration generics (a
                    // documented §12.9 residual).
                    coversNonNull = true;
                }
            }
            if (!coversNonNull) continue;
            if (!scrutTy->nullable || armsCoverNull(arms)) return true;  // exhaustive
            // Non-null fully covered, but null is not (§19.6 nullable rule).
            warn("the nullable scrutinee's non-null values are covered by a "
                 "typed binding, but `null` is not");
            return false;
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
        for (const auto& arm : arms) {
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
        if (!(t && f)) {
            warn("bool scrutinee does not cover both `true` and `false`");
            return false;
        }
        if (nullableUncovered) {
            warn("nullable `bool?` scrutinee has no `null` arm");
            return false;
        }
        return true;
    }

    // --- Enum / closed-hierarchy scrutinee both resolve to a Named type.
    TypeSymbol* sub = (scrutTy && scrutTy->kind == TypeRef::Kind::Named)
                          ? scrutTy->declaration
                          : nullptr;

    if (sub && sub->kind == ast::TypeDeclKind::Enum) {
        // §19.6: every declared enum constant must have a matching pattern.
        std::unordered_set<std::string_view> covered;
        for (const auto& arm : arms) collectEnumPatternNames(arm, covered);
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
            return false;
        }
        return true;
    }

    // §35.6 (v0.200, #2045): a CHOICE scrutinee is exhaustive when every
    // case is covered, counted per case NAME one level deep — a payload-
    // free case by a constant-style pattern naming it (NamedPattern); a
    // payload case by a destructuring whose every slot is irrefutable
    // (naked binding, `var x`, or `_`). Guarded arms contribute nothing
    // to the static proof; refutable-slot arms (e.g. `Some(0)` — a
    // constant slot) likewise. The case set is closed by declaration
    // (§35.1), so exhaustiveness holds across module boundaries.
    if (sub && sub->kind == ast::TypeDeclKind::Choice) {
        // Collect the case names declared on the choice.
        std::vector<std::string_view> allCases;
        for (const Symbol* m : sub->members) {
            if (dynamic_cast<const ChoiceCaseSymbol*>(m))
                allCases.push_back(m->name);
        }
        // Gather covered case names per §35.6.
        std::unordered_set<std::string_view> covered;
        // §35.6: a destructuring slot is irrefutable when it is a naked
        // binding, a `var x` BindingPattern, or a `_` DiscardPattern.
        // Anything else (a constant, a relational, a nested destructuring
        // whose inner slots are refutable) makes the arm contribute
        // nothing to the static proof.
        auto slotsIrrefutable = [](const ast::DestructuringPattern& dp) -> bool {
            for (const auto& sp : dp.subPatterns) {
                if (!sp.nakedBinding.empty()) continue;  // naked binding
                if (!sp.pattern) return false;  // defensive
                if (dynamic_cast<const ast::DiscardPattern*>(sp.pattern.get()))
                    continue;
                if (dynamic_cast<const ast::BindingPattern*>(sp.pattern.get()))
                    continue;  // `var x` (an untyped binding is irrefutable)
                return false;  // any other slot shape is refutable
            }
            return true;
        };
        for (const auto& arm : arms) {
            // §19.6 / §35.6: a guarded arm contributes nothing.
            if (arm.guard) continue;
            if (auto* np = dynamic_cast<const ast::NamedPattern*>(
                    arm.pattern.get())) {
                // A bare case name — payload-free case only. (A payload
                // case matched bare was rejected by sema at checkTopLevel
                // NamedPattern; here it would not be in `covered` either
                // way, and the warn-missing path below reports it.)
                if (np->name && !np->name->parts.empty()) {
                    std::string_view last = np->name->parts.back();
                    for (Symbol* m : sub->members) {
                        if (auto* cc = dynamic_cast<ChoiceCaseSymbol*>(m)) {
                            if (cc->name == last &&
                                cc->caseDecl &&
                                cc->caseDecl->payload.empty())
                                covered.insert(last);
                        }
                    }
                }
            } else if (auto* dp = dynamic_cast<const ast::DestructuringPattern*>(
                           arm.pattern.get())) {
                // A payload case by destructuring — covers the case iff
                // every slot is irrefutable (§35.6).
                if (dp->typeName && !dp->typeName->parts.empty() &&
                    slotsIrrefutable(*dp)) {
                    std::string_view last = dp->typeName->parts.back();
                    for (Symbol* m : sub->members) {
                        if (auto* cc = dynamic_cast<ChoiceCaseSymbol*>(m)) {
                            if (cc->name == last) covered.insert(last);
                        }
                    }
                }
            }
        }
        std::vector<std::string_view> missingCases;
        for (std::string_view cn : allCases)
            if (!covered.count(cn)) missingCases.push_back(cn);
        if (!missingCases.empty() || nullableUncovered) {
            std::string detail = "choice `" + std::string(sub->name) + "`";
            if (!missingCases.empty()) {
                detail += " is missing case(s):";
                for (auto n : missingCases) detail += " " + std::string(n);
            }
            if (nullableUncovered)
                detail += (missingCases.empty() ? " nullable value not covered"
                                                : " (and a `null` arm)");
            warn(detail);
            return false;
        }
        return true;
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
            return false;
        }

        // §19.6 (v0.196, #1816): generic-leaf determination. When the root or
        // any leaf/intermediate is generic — or the scrutinee is an
        // instantiation `D<s…>` — *leaf type* means the leaf INSTANTIATION
        // admitted at the scrutinee's static type, not merely the leaf
        // declaration. This gate is OFF for a wholly non-generic hierarchy
        // (`scrutTy` has no generic args and no class in the closed set is
        // generic), in which case the name-match below runs unchanged.
        bool genericsInPlay =
            (scrutTy && !scrutTy->genericArgs.empty()) ||
            !sub->genericParams.empty();
        for (auto* d : descendants)
            if (!d->genericParams.empty()) genericsInPlay = true;

        // FIRST TRICHOTOMY SLICE (v0.196 "No inhabiting instantiation" edge —
        // ground mismatch ⇒ exempt): a leaf whose composed ROOT INSTANTIATION
        // is fully ground and DECIDABLY UNEQUAL to the (non-nullable) scrutinee
        // type is uninhabited at `S` by invariance — no value of static type
        // `S` has that leaf's instantiation as its runtime class — so it is
        // EXEMPT and needs no arm (e.g. `IntBox: Box<int>` under `Box<string>`).
        // Both operands are required fully ground before typeRefEqual so its
        // null-declaration blindness cannot mistake a rigid `T` for a ground
        // type and wrongly exempt an inhabited leaf. Every OTHER shape (a
        // non-ground root instantiation, a ground-equal one, or an
        // uncomposable chain) falls through to the name-match unchanged,
        // pending later determination lanes (rigid identity, `where`-clause
        // filtering, the `?`-split, undetermined-warning naming).
        //
        // (#1816 s3) Leaves the solver classifies UNDETERMINED because a
        // parameter has NO occurrence in the root instantiation (free after a
        // clean solve → infinitely many inhabiting instantiations) — coverage
        // statically UNATTAINABLE. Kept in the leaf set like any other, but
        // recorded here with their free parameter(s) so the missing-leaf
        // computation reports them unconditionally: no type-test arm can cover
        // such a leaf, only an irrefutable arm (already returned above).
        std::unordered_map<const TypeSymbol*, std::vector<std::string_view>>
            unattainable;
        // §19.6 v0.196 (#1862): the DETERMINED generic leaves' sole inhabiting
        // substitution σ (leaf-param name -> its determined type), recorded so
        // the coverage match below is arg-SENSITIVE — a determined generic leaf
        // is discharged only by an arm naming it with generic args matching σ,
        // not by declaration name alone. Only GENERIC determined leaves are
        // recorded; a non-generic determined leaf has no args to check and is
        // covered by name as before.
        std::unordered_map<const TypeSymbol*,
                           std::unordered_map<std::string_view, TypeRef*>>
            determined;
        if (genericsInPlay && scrutTy) {
            TypeRef scrutNonNull = *scrutTy;  // stack copy; typeRefEqual reads
            scrutNonNull.nullable = false;    // through the pointer only here
            // SECOND TRICHOTOMY SLICE (v0.196, #1816 s2 — the determination
            // engine for a GROUND scrutinee): when `S` is FULLY GROUND, solve
            // each leaf's parameters against `S` (solveLeafInstantiations,
            // structural-invariant, per §12.9). 0 solutions — a structural
            // mismatch OR a repeated-parameter inconsistency (`L<Y>: D<Y,Y>`
            // against `D<int,string>`) — ⇒ the leaf is uninhabited at `S` and is
            // EXEMPT (dropped); exactly-one (determined) or undetermined ⇒ the
            // leaf is KEPT and the name-match below settles coverage (the
            // determined idiom `Ok<int>`/`Err<int>` at `Result<int>` stays
            // exhaustive; later lanes s3–s7 refine the >1 / undetermined /
            // rigid-`S` cases). ENGAGE ONLY on a fully-ground `S`: a rigid `S` is
            // a later lane's exclusive domain — solving against it would hit the
            // typeRefEqual/unify null-declaration blindness and could wrongly
            // exempt or keep, so the solver does not run and the leaf set is
            // unchanged. s1's ground-mismatch check is retained ahead of it
            // (a strict subset — same-module ground-vs-ground inequality — kept
            // intact so its behavior and test are undisturbed).
            bool groundScrut = isFullyGroundTypeRef(&scrutNonNull);
            std::vector<TypeSymbol*> kept;
            kept.reserve(leaves.size());
            for (auto* lf : leaves) {
                TypeRef* rootInst = leafRootInstantiation(lf, sub);
                if (rootInst && isFullyGroundTypeRef(rootInst) && groundScrut &&
                    !typeRefEqual(rootInst, &scrutNonNull))
                    continue;  // s1: ground-mismatch ⇒ exempt from coverage
                if (groundScrut) {
                    LeafInstSolutions sol =
                        solveLeafInstantiations(lf, sub, &scrutNonNull);
                    if (sol.outcome == LeafInstSolutions::Outcome::NoSolution)
                        continue;  // s2: no inhabiting instantiation ⇒ exempt
                    // s3: a FREE parameter (no occurrence in the root
                    // instantiation) leaves infinitely many instantiations
                    // inhabiting `S` ⇒ coverage statically unattainable. The
                    // OPPOSITE of s2's exempt: recorded, not dropped, so the
                    // leaf is always reported missing below (§19.6). Distinct
                    // from the undecidable Undetermined sub-case (freeParams
                    // empty), which stays deferred to the name-match.
                    if (sol.outcome ==
                            LeafInstSolutions::Outcome::Undetermined &&
                        !sol.freeParams.empty())
                        unattainable.emplace(lf, std::move(sol.freeParams));
                    // §19.6 v0.196 (#1862): record a determined GENERIC leaf's
                    // σ so its coverage requires an arg-matching arm below.
                    else if (sol.outcome ==
                                 LeafInstSolutions::Outcome::Determined &&
                             !lf->genericParams.empty())
                        determined.emplace(lf, std::move(sol.solution));
                } else {
                    // s4: NON-GROUND (rigid) scrutinee — the case s1–s3 defer.
                    // A leaf is NEVER exempt under a rigid `S`; classify by the
                    // rigid-aware three-valued identity (solveLeafInstantiations-
                    // Rigid, keyed on isInScopeTypeParam — never null-decl
                    // typeRefEqual). An undecidable rigid-identity component
                    // (`IntError: Result<int>` at `Result<T>`) or a free leaf
                    // parameter makes coverage statically UNATTAINABLE: recorded
                    // and reported unconditionally (§19.6 v0.196, Test 8), so a
                    // specific-instantiation arm does not discharge it. A clean
                    // determined solve (`Ok<T>` at `Result<T>` ⇒ `T`) is KEPT and
                    // the name-match discharges it — the determined rigid shape
                    // stays exhaustive (Test 2).
                    LeafInstSolutions sol =
                        solveLeafInstantiationsRigid(lf, sub, &scrutNonNull);
                    if (sol.unattainable)
                        unattainable.emplace(lf, std::move(sol.freeParams));
                    // §19.6 v0.196 (#1862): a determined GENERIC leaf under a
                    // rigid `S` (`Ok<Y>: Result<Y>` ⇒ `Y := T`) is likewise
                    // arg-checked below — its σ maps the leaf param to the rigid
                    // scrutinee component, so `Ok<int>` no longer covers `Ok<T>`.
                    else if (sol.outcome ==
                                 LeafInstSolutions::Outcome::Determined &&
                             !lf->genericParams.empty())
                        determined.emplace(lf, std::move(sol.solution));
                }
                kept.push_back(lf);
            }
            leaves = std::move(kept);
        }

        // Collect the leaf types covered by an unguarded type-test arm.
        std::unordered_set<const TypeSymbol*> coveredLeaves;
        for (const auto& arm : arms) {
            if (arm.guard) continue;
            const ast::Pattern* p = arm.pattern.get();
            TypeSymbol* matchTy = nullptr;
            // A type-test arm names the matched class either as a bare
            // NamedPattern (`Circle => …`) or as a typed binding
            // (`var c: Circle => …`). The named class is located by its last
            // name part against the closed hierarchy's leaf set (the
            // NamedPattern path carries no resolved type). §19.6 v0.196
            // (#1862): a BindingPattern's `bp->type` IS resolved by
            // TypeResolution's resolveTypesInPattern, so `armTy` carries the
            // arm's full INSTANTIATION — used below to make coverage of a
            // determined generic leaf arg-SENSITIVE (a bare NamedPattern leaves
            // `armTy` null and never discharges a determined generic leaf).
            std::string_view typeName;
            const TypeRef* armTy = nullptr;
            if (auto* np = dynamic_cast<const ast::NamedPattern*>(p)) {
                if (np->name && !np->name->parts.empty())
                    typeName = np->name->parts.back();
            } else if (auto* bp =
                           dynamic_cast<const ast::BindingPattern*>(p)) {
                if (bp->type && bp->type->name &&
                    !bp->type->name->parts.empty()) {
                    typeName = bp->type->name->parts.back();
                    armTy = ctx_.resolvedType(*bp->type);
                }
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
                    if (lf == matchTy) {
                        // §19.6 v0.196 (#1862): a DETERMINED generic leaf is
                        // discharged only by an arm whose instantiation matches
                        // the leaf's sole admitted instantiation — not by its
                        // declaration name alone. A wrong-instantiation arm
                        // (`Ok<string>` at determined `Ok<int>`, or a rigid
                        // `Ok<int>` at `Ok<T>`) does NOT cover it, so a match
                        // that is not exhaustive is correctly reported (the
                        // false-exhaustive hole this closes). Non-generic /
                        // undetermined / exempt leaves are not in `determined`,
                        // so the plain name-match discharge is undisturbed.
                        auto di = determined.find(lf);
                        if (di != determined.end() &&
                            !armCoversDeterminedLeaf(armTy, lf, di->second))
                            continue;  // arg mismatch — leaf stays uncovered
                        coveredLeaves.insert(lf);
                        continue;
                    }
                    std::vector<TypeSymbol*> work = lf->baseTypes;
                    std::unordered_set<TypeSymbol*> seen;
                    while (!work.empty()) {
                        TypeSymbol* b = work.back();
                        work.pop_back();
                        if (!b || !seen.insert(b).second) continue;
                        if (b == matchTy) {
                            // §19.6 v0.196 (#1874): the ANCESTOR/subtype-walk
                            // cover is arg-SENSITIVE for a DETERMINED generic
                            // leaf, exactly as the direct self-match above — a
                            // wrong-instantiation ancestor arm (`Result<string>`
                            // at scrutinee `Result<int>`) does NOT discharge the
                            // determined `Ok<int>`/`Err<int>` (residual of #1862).
                            // Non-generic / undetermined / exempt leaves are not
                            // in `determined`, so the plain subtype discharge is
                            // undisturbed.
                            auto di = determined.find(lf);
                            if (di != determined.end() &&
                                !ancestorArmCoversDeterminedLeaf(
                                    armTy, lf, matchTy, di->second))
                                break;  // arg mismatch — leaf stays uncovered
                            coveredLeaves.insert(lf);
                            break;
                        }
                        for (TypeSymbol* bb : b->baseTypes)
                            work.push_back(bb);
                    }
                }
            }
        }
        std::vector<std::string> missing;
        for (auto* lf : leaves) {
            // s3: an undetermined free-parameter leaf is UNATTAINABLE by any
            // type-test arm — always reported, naming the leaf and its
            // undetermined parameter(s) (§19.6), regardless of name-match.
            auto un = unattainable.find(lf);
            if (un != unattainable.end()) {
                missing.push_back(formatMissingGenericLeaf(lf, un->second));
                continue;
            }
            if (!coveredLeaves.count(lf)) missing.push_back(std::string(lf->name));
        }
        if (!missing.empty() || nullableUncovered) {
            std::string detail =
                "closed hierarchy `" + std::string(sub->name) + "`";
            if (!missing.empty()) {
                detail += " is missing leaf type(s):";
                for (const auto& n : missing) detail += " " + n;
            }
            if (nullableUncovered)
                detail += (missing.empty() ? " nullable value not covered"
                                           : " (and a `null` arm)");
            warn(detail);
            return false;
        }
        return true;
    }

    // --- The scrutinee-less boolean-condition form (§19.1) has no value
    // set to enumerate and no `_` (we already returned on a catch-all), so
    // it cannot be proven exhaustive (§19.6).
    if (!scrutinee) {
        warn("scrutinee-less `case` has no catch-all `_` arm");
        return false;
    }

    // A scrutinee whose type failed to resolve (Error sentinel) or was not
    // inferred is suppressed here: a non-exhaustiveness warning layered on
    // top of an upstream type error would be a confusing cascade (the Error
    // kind exists precisely to suppress such follow-on diagnostics —
    // TypeRef.hpp). The real diagnostic is reported by the resolution pass.
    if (!scrutTy || scrutTy->kind == TypeRef::Kind::Error) return false;

    // --- Every other scrutinee shape (int / string / range / float /
    // open class) cannot be proven exhaustive (§19.6): there is no finite,
    // statically-knowable value set to enumerate. Without a catch-all arm
    // (handled above) the case is non-exhaustive.
    warn("scrutinee type cannot be proven exhaustive");
    return false;
}

// §19.3 (#1847): validate a TOP-LEVEL bare-identifier `case`/`when` pattern.
// See the declaration comment (ExprTypecheck_impl.hpp) for the defect. The
// grammar (§A.7) makes a single-/qualified-identifier pattern a TYPE TEST when
// the name resolves to a type or a type parameter (v0.182), a CONSTANT pattern
// when it resolves to a named `const`, and an ENUM-CONSTANT pattern when it
// names a constant of the scrutinee enum (§10.6/§19.8). Anything else — a
// runtime local/parameter/field value, or an unresolved name — is neither and
// is rejected here (codegen would otherwise silently drop the arm / emit
// malformed IR).
void ExprTC::checkTopLevelNamedPattern(const ast::NamedPattern* np,
                                       TypeRef* scrutTy) {
    if (!np || !np->name || np->name->parts.empty()) return;
    // A scrutinee-less `case`/`when` (its arms are boolean CONDITION patterns,
    // §19.1) and a scrutinee whose type failed to resolve (Error kind — a
    // follow-on diagnostic here would be a confusing cascade, cf. line ~877)
    // are both out of scope for this type-vs-constant check.
    if (!scrutTy || scrutTy->kind == TypeRef::Kind::Error) return;
    // Only a BARE single identifier reaches codegen's silent-drop path; a
    // qualified name (`C.K`, `Color.Red`, `System.IO.Stream`) is resolved by
    // codegen's constValueForPatternName / enum / type-descriptor handlers and
    // is not the reported defect.
    if (np->name->parts.size() != 1) return;
    std::string_view last = np->name->parts.back();

    // (1) Enum-constant pattern: a bare name that is a constant of the scrutinee
    //     ENUM (nullable or not, §10.6 / §19.8). Checked directly on the
    //     scrutinee's enum declaration so it holds for `Color?` as well as
    //     `Color`.
    if (scrutTy->declaration &&
        scrutTy->declaration->kind == ast::TypeDeclKind::Enum) {
        for (Symbol* m : scrutTy->declaration->members)
            if (dynamic_cast<EnumConstantSymbol*>(m) && m->name == last)
                return;  // valid enum-constant pattern
    }

    // (1c) §35.5 (v0.200, #2045) choice-case pattern. The scrutinee-type
    //      prefix-omission rule (extended from §10.6 / §19.8) lets a bare
    //      case name resolve against the choice scrutinee. A PAYLOAD-FREE
    //      case (`NotModified`) is matched by its bare name — a constant-
    //      style pattern, valid here. A PAYLOAD-CARRYING case (`Fetched`)
    //      MAY NOT be matched bare per §35.5 "Case patterns": "A payload
    //      case may not be matched bare: `Fetched` without parentheses is
    //      a compile error naming the arity." Reject it with the spec's
    //      remedy (the explicit discard form `Fetched(_)`).
    if (auto* cc = findChoiceCaseOnScrutinee(scrutTy, last)) {
        if (cc->caseDecl && !cc->caseDecl->payload.empty()) {
            ctx_.diagnostics().report(
                Severity::Error, np->span.begin,
                "Payload case `" + std::string(last) + "` requires a payload "
                "pattern: write `" + std::string(last) + "(" +
                std::to_string(cc->caseDecl->payload.size()) + " slot" +
                (cc->caseDecl->payload.size() == 1 ? "" : "s") +
                ")`, or `" + std::string(last) + "(_)` to discard "
                "(\xC2\xA7""35.5)");
        }
        return;  // payload-free: valid; payload-carrying: reported above
    }

    // (2) Type-test pattern: `last` names a user/imported type (any
    //     TypeDeclKind — class/interface/record/enum), a type alias, or a
    //     built-in type. Mirrors codegen's descriptorForPatternName /
    //     record-enum / built-in-descriptor resolution.  §19.3 "Type patterns
    //     without binding: `string`, `int`, `Dog`".
    for (const auto& sym : ctx_.symbols()) {
        if (auto* ts = dynamic_cast<const TypeSymbol*>(sym.get()))
            if (ts->name == last) return;  // valid type-test pattern
        if (auto* ta = dynamic_cast<const TypeAliasSymbol*>(sym.get()))
            if (ta->name == last) return;  // type-alias type test
    }
    if (isBuiltinTypeName(last)) return;   // built-in type (authoritative table)

    // (2b) Type-parameter pattern (§19.3, v0.182): in a generic body a bare
    //      type parameter `T` is a type test against the substituted type
    //      argument, per instantiation. A generic parameter is not in the value
    //      scope (lookupName does not find it), so recognize it directly from
    //      the enclosing method's / type's declared parameters (the same lookup
    //      the `new T()` and type-argument-inference paths use).
    if (currentMethodDecl_)
        for (const auto& gp : currentMethodDecl_->genericParams)
            if (gp.name == last) return;
    if (currentType_ && currentType_->decl)
        if (auto* td = dynamic_cast<const ast::TypeDecl*>(currentType_->decl))
            for (const auto& gp : td->genericParams)
                if (gp.name == last) return;

    // (3) Named-`const` pattern: `last` is a `const` member anywhere (mirrors
    //     codegen's constValueForPatternName ConstMemberSymbol search — module
    //     or type const). §19.3 "Constants: … a named `const`".
    for (const auto& sym : ctx_.symbols()) {
        if (auto* cm = dynamic_cast<const ConstMemberSymbol*>(sym.get()))
            if (cm->name == last) return;
        if (auto* ts = dynamic_cast<const TypeSymbol*>(sym.get()))
            for (Symbol* m : ts->members)
                if (auto* cm2 = dynamic_cast<const ConstMemberSymbol*>(m))
                    if (cm2->name == last) return;
    }

    // Resolve in the current lexical scope. A compile-time-classified `const`
    // LOCAL (`const K := 5`) is a valid constant pattern (§17.14 / §19.3); a
    // type parameter (v0.182 — `case x of T`), an enum constant, a const
    // member, or a (nested) type all resolve here too and are accepted.
    // Anything that resolves to a *runtime value* — a `var` local, a
    // parameter, a mutable field/property, or a RUNTIME const local — is NOT
    // a type or compile-time constant and is the reported defect.
    Symbol* s = lookupName(last);
    if (s) {
        if (auto* ls = dynamic_cast<LocalSymbol*>(s)) {
            // §17.14 (v0.226, #1853): a reference to a local `const` is a
            // constant expression — and thus a valid §19.3 pattern constant —
            // ONLY when compile-time-classified. A RUNTIME const (`const K := n + 1;`
            // where n is non-const, §21.1) is a single-assignment runtime value,
            // not a foldable constant, so it may not be a `case` pattern
            // constant. Record the resolved symbol on the pattern so codegen
            // folds K's initializer at the case site (locals are not in
            // `symbols()`, so constValueForPatternName cannot recover it by name).
            if (ls->isConst) {
                if (!ls->isCompileTimeConst) {
                    ctx_.diagnostics().report(
                        Severity::Error, np->span.begin,
                        "`" + std::string(last) + "` is a runtime const, not a "
                        "compile-time constant, so it cannot be a `case` pattern "
                        "constant (\xC2\xA7""17.14 / \xC2\xA7""19.3): a pattern constant "
                        "must be a compile-time-classified const");
                    return;
                }
                ctx_.setPatternSymbol(np, ls);  // codegen folds via this
                return;  // compile-time const local — valid constant pattern
            }
            reportNamedPatternIsValue(np, last);
            return;
        }
        if (dynamic_cast<ParameterSymbol*>(s) ||
            dynamic_cast<FieldSymbol*>(s) ||
            dynamic_cast<PropertySymbol*>(s)) {
            reportNamedPatternIsValue(np, last);
            return;
        }
        // Any other resolved symbol kind (TypeSymbol / NestedTypeSymbol /
        // GenericParamSymbol / ConstMemberSymbol / EnumConstantSymbol /
        // TypeAliasSymbol / MethodSymbol …) is a type, constant, or otherwise
        // not a plain runtime value — accept conservatively (never over-reject).
        return;
    }

    // Unresolved: not a type, not a constant, and not in scope. An undefined
    // identifier in a top-level pattern position (§19.3).
    ctx_.diagnostics().report(
        Severity::Error, np->span.begin,
        "`" + std::string(last) + "` does not name a type or a constant, so it "
        "cannot be a top-level `case` pattern (\xC2\xA7""19.3): a bare "
        "identifier there is a type test, and this name is not in scope");
}

// §19.3 (#1847): shared diagnostic for a top-level bare-identifier pattern that
// resolves to a runtime VALUE (a local, parameter, or field) rather than a type
// or constant.
void ExprTC::reportNamedPatternIsValue(const ast::NamedPattern* np,
                                       std::string_view name) {
    ctx_.diagnostics().report(
        Severity::Error, np->span.begin,
        "`" + std::string(name) + "` is a value, not a type or constant, so it "
        "cannot be a top-level `case` pattern (\xC2\xA7""19.3): a bare "
        "identifier there is a type test. To compare the scrutinee against a "
        "value, use a guard: `var x when x = " + std::string(name) + " => …`");
}

// §35.5 (v0.200, #2045): find a choice case by bare name on a choice
// scrutinee's declaration. Returns null when `scrutTy` is not a choice or no
// case with that name exists. The scrutinee-type prefix-omission rule
// (§35.5 "Case patterns", extended from §10.6 / §19.8 for enums) lets a
// bare `NotModified` or `Fetched` resolve against the scrutinee choice's
// cases the same way `Red` resolves against an enum scrutinee.
const ChoiceCaseSymbol* ExprTC::findChoiceCaseOnScrutinee(
    TypeRef* scrutTy, std::string_view name) {
    if (!scrutTy || scrutTy->kind != TypeRef::Kind::Named || !scrutTy->declaration)
        return nullptr;
    if (scrutTy->declaration->kind != ast::TypeDeclKind::Choice) return nullptr;
    for (Symbol* m : scrutTy->declaration->members) {
        if (auto* cc = dynamic_cast<ChoiceCaseSymbol*>(m)) {
            if (cc->name == name) return cc;
        }
    }
    return nullptr;
}

// #2219: apply the scrutinee's per-instantiation generic-arg substitution to
// a generic CHOICE's case payload field type. Walks `ty` and rewrites every
// bare-name TypeRef whose declaration is one of `scrutTy->declaration`'s
// generic parameters (matched by NAME) with the corresponding entry from
// `scrutTy->genericArgs`. Recurses into Named genericArgs, InlineArray
// element types, Function parameter/return types, and the nullable wrapper,
// so `Array<T>?` at scrutinee `Result<Array<int>, string>` substitutes to
// `Array<int>?`. A bare `T` at the case-payload slot becomes the substituted
// `int`, which is what the destructuring pattern's binding type must be so
// the arms' §20 common-type computation sees both arms at `int`. Without
// this, both arms of `case r of Ok(v) => v, Err(e) => e` over `Result<int,
// int>` were typed against the bare `T` and `E` (different generic params
// of the same choice), and `commonTypeOf` rejected the unrelated named
// types with the §20 "no common type" diagnostic.
//
// Returns `ty` unchanged when no substitution applies (a non-generic choice,
// a non-parametric field type, a scrutinee without genericArgs) so callers
// can use the helper unconditionally.
//
// Mirrors the field-walk of Resolver::applyGenericSubst + the env-build part
// of Resolver::buildBaseGenericSubst (TypeResolution_Checks.cpp) — local
// copy because the ExprTC worker has no Resolver handle (#1621 / §31.2.1
// split). Per §35.5 "Case patterns" + §14.5 "Type-argument inference"
// posture (type-level arguments are NEVER inferred; the scrutinee's args are
// the ones substituted).
TypeRef* ExprTC::substituteGenericCaseFieldType(
        const TypeRef* ty, const TypeRef* scrutTy) {
    if (!ty || !scrutTy) return const_cast<TypeRef*>(ty);
    // Collect the substitution map from the scrutinee's choice declaration.
    // A non-Named scrutinee, a non-choice, or an instantiated-without-args
    // candidate carries no substitution (return unchanged).
    if (scrutTy->kind != TypeRef::Kind::Named || !scrutTy->declaration ||
        scrutTy->declaration->kind != ast::TypeDeclKind::Choice) {
        return const_cast<TypeRef*>(ty);
    }
    const auto& params = scrutTy->declaration->genericParams;
    const auto& args = scrutTy->genericArgs;
    if (params.empty() || args.empty()) return const_cast<TypeRef*>(ty);
    if (params.size() != args.size()) return const_cast<TypeRef*>(ty);
    // std::function yields a self-referential walker for the recursive kinds
    // (Named-with-genericArgs, InlineArray element, Function param/return).
    std::function<TypeRef*(const TypeRef*)> substituteInto =
        [&](const TypeRef* sub) -> TypeRef* {
        if (!sub) return nullptr;
        // A bare-Named TypeRef naming a generic parameter of the scrutinee's
        // choice — type resolution mints generic params as Named refs with
        // `declaration == nullptr` (the resolveNamed path at
        // TypeResolution_Names.cpp:1308 deliberately nulls the declaration so
        // sema slices distinguish "unresolved-but-parameter" from real Named
        // types). Match by NAME against the choice's `genericParams`.
        if (sub->kind == TypeRef::Kind::Named && !sub->declaration &&
            sub->astNode && sub->astNode->name &&
            !sub->astNode->name->parts.empty() &&
            sub->astNode->name->parts.size() == 1) {
            for (std::size_t i = 0; i < params.size(); ++i) {
                if (auto* gp = dynamic_cast<const GenericParamSymbol*>(
                        params[i])) {
                    if (gp->name == sub->astNode->name->parts.front()) {
                        TypeRef* rep = args[i];
                        if (!rep) return const_cast<TypeRef*>(sub);
                        if (sub->nullable && !rep->nullable) {
                            TypeRef* nn = ctx_.makeTypeRef(*rep);
                            nn->nullable = true;
                            return nn;
                        }
                        return rep;
                    }
                }
            }
            return const_cast<TypeRef*>(sub);
        }
        // Recursive kinds: walk nested positions, copy through when at
        // least one changed, return the input otherwise.
        if (sub->kind == TypeRef::Kind::Named &&
            !sub->genericArgs.empty()) {
            std::vector<TypeRef*> newArgs;
            newArgs.reserve(sub->genericArgs.size());
            bool anyChange = false;
            for (const auto* arg : sub->genericArgs) {
                TypeRef* after = substituteInto(arg);
                newArgs.push_back(after);
                if (after != arg) anyChange = true;
            }
            if (!anyChange) return const_cast<TypeRef*>(sub);
            TypeRef* out = ctx_.makeTypeRef(*sub);
            out->genericArgs = std::move(newArgs);
            return out;
        }
        if (sub->kind == TypeRef::Kind::InlineArray) {
            TypeRef* after = substituteInto(sub->arrayElement);
            if (after == sub->arrayElement) return const_cast<TypeRef*>(sub);
            TypeRef* out = ctx_.makeTypeRef(*sub);
            out->arrayElement = after;
            return out;
        }
        if (sub->kind == TypeRef::Kind::Function) {
            bool anyChange = false;
            std::vector<TypeRef::FuncParam> newParams;
            newParams.reserve(sub->funcParams.size());
            for (const auto& p : sub->funcParams) {
                TypeRef* after = substituteInto(p.type);
                if (after != p.type) anyChange = true;
                newParams.push_back({p.modifier, p.name, after});
            }
            TypeRef* afterRet = substituteInto(sub->funcReturn);
            if (afterRet != sub->funcReturn) anyChange = true;
            if (!anyChange) return const_cast<TypeRef*>(sub);
            TypeRef* out = ctx_.makeTypeRef(*sub);
            out->funcParams = std::move(newParams);
            out->funcReturn = afterRet;
            return out;
        }
        // Primitive / Unit / Error — no nested positions to walk.
        return const_cast<TypeRef*>(sub);
    };
    return substituteInto(ty);
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

// §8.6 / §5.1 (#1311): see header doc. A primitive value type boxes into a
// builtin generic interface it implements (IComparable<T>/IEquatable<T>,
// stdlib §9.1–§9.5). T must be exactly the primitive's own alias — no widening
// (Byte implements IComparable<byte>, not IComparable<int>). Boolean
// implements IEquatable<bool> only (§9.4 — no IComparable<bool>).
bool ExprTC::primitiveBoxesIntoBuiltinIface(PrimitiveType fromPrim,
                                            const TypeRef* to) {
    if (!to || to->kind != TypeRef::Kind::Named || !to->declaration ||
        to->declaration->kind != ast::TypeDeclKind::Interface ||
        to->genericArgs.size() != 1 || !to->genericArgs[0])
        return false;
    bool isCmp = to->declaration->name == "IComparable";
    bool isEq  = to->declaration->name == "IEquatable";
    if (!isCmp && !isEq) return false;
    // Boolean implements IEquatable<bool> but NOT IComparable<bool> (§9.4).
    if (isCmp && fromPrim == PrimitiveType::Bool) return false;
    const TypeRef* targ = to->genericArgs[0];
    if (targ->kind != TypeRef::Kind::Primitive) return false;
    return targ->primitive == fromPrim;
}

// §27.1.1 (v0.180): exact structural equality of two TypeRefs — the
// INVARIANT fallback for a function-value slot that is not a bare, non-
// generic class/interface reference (a value-typed, record-typed, primitive,
// nested-function-typed, generic, or inline-array slot). An already-
// constructed function value has no adapter object to perform a numeric-
// widening, boxing, or other representation-changing conversion (§27.1.1
// third bullet), so those slots require an EXACT match rather than the usual
// §5.1 widening. Mirrors Resolver::typeRefEqual (TypeResolution_Checks.cpp)
// but is duplicated locally — ExprTC has no handle on a Resolver instance —
// rather than threading one through for this single use.
// §27.3 (v0.226, #2107): external linkage (declared in ExprTypecheck_impl.hpp)
// so ExprTypecheck_Exprs.cpp's divergent-return check can reuse it.
bool functionSlotTypeRefEqual(const TypeRef* a, const TypeRef* b) {
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;
    if (a->nullable != b->nullable) return false;
    switch (a->kind) {
        case TypeRef::Kind::Error:
            return false;
        case TypeRef::Kind::Unit:
            return true;
        case TypeRef::Kind::Primitive:
            return a->primitive == b->primitive;
        case TypeRef::Kind::Named: {
            // §27.1.1 (#2108): two Named slots are equal only when their
            // declaration pointers AND generic args match. Builtin stdlib
            // collection/wrapper types (Array<T>, Set<T>, Task<T>, …) carry a
            // null declaration — reconstructed from module metadata or a
            // compiler-known synthetic astNode with no user TypeDecl (§2.5 /
            // #495) — so the pointer-identity check alone treats ANY two null-
            // declaration builtins with equal generic args as EQUAL (e.g.
            // Array<int> vs Set<int>). When both declarations are null, also
            // compare the alias-transparent head name (headName) so distinct
            // builtin heads compare UNEQUAL; identical builtin heads still
            // compare equal and fall through to the generic-args check.
            if (a->declaration != b->declaration) return false;
            if (a->declaration == nullptr &&
                a->headName() != b->headName())
                return false;
            if (a->genericArgs.size() != b->genericArgs.size()) return false;
            for (std::size_t i = 0; i < a->genericArgs.size(); ++i) {
                if (!functionSlotTypeRefEqual(a->genericArgs[i], b->genericArgs[i]))
                    return false;
            }
            return true;
        }
        case TypeRef::Kind::InlineArray:
            return a->arrayOwned == b->arrayOwned &&
                   functionSlotTypeRefEqual(a->arrayElement, b->arrayElement);
        case TypeRef::Kind::Function:
            if (a->funcParams.size() != b->funcParams.size()) return false;
            for (std::size_t i = 0; i < a->funcParams.size(); ++i) {
                if (a->funcParams[i].modifier != b->funcParams[i].modifier)
                    return false;
                if (!functionSlotTypeRefEqual(a->funcParams[i].type,
                                              b->funcParams[i].type))
                    return false;
            }
            return a->funcReturnOwned == b->funcReturnOwned &&
                   functionSlotTypeRefEqual(a->funcReturn, b->funcReturn);
    }
    return false;
}

// §27.1.1 (v0.180): does a value of slot-type `from` convert to slot-type
// `to` under the function-VALUE assignability rule? A non-generic class or
// interface slot uses the standard §5.1 reference conversion (declaration
// identity/subtype plus nullable-widening, mirroring the Named-Named branch
// of checkAssignable below); every other slot shape — value-typed, record-
// typed, primitive, generic, nested-function, or array — is INVARIANT (exact
// structural match, functionSlotTypeRefEqual above). Callers apply this with
// the operands swapped for a CONTRAVARIANT parameter slot and in-order for a
// COVARIANT return slot (§27.1.1 first/second bullets).
static bool functionSlotConvertible(
        const TypeRef* from, const TypeRef* to,
        bool (*isSubtypeFn)(TypeSymbol*, TypeSymbol*)) {
    if (!from || !to) return from == to;  // both absent: both `()`-shaped
    if (from->kind == TypeRef::Kind::Named && to->kind == TypeRef::Kind::Named &&
        from->declaration && to->declaration &&
        from->genericArgs.empty() && to->genericArgs.empty() &&
        (from->declaration->kind == ast::TypeDeclKind::Class ||
         from->declaration->kind == ast::TypeDeclKind::Interface) &&
        (to->declaration->kind == ast::TypeDeclKind::Class ||
         to->declaration->kind == ast::TypeDeclKind::Interface)) {
        if (from->nullable && !to->nullable) return false;
        return from->declaration == to->declaration ||
               isSubtypeFn(from->declaration, to->declaration);
    }
    return functionSlotTypeRefEqual(from, to);
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
    // §11.8/§22.4/§1585 (fixes #1597 regression): a null-declaration Named
    // `from` — a generic type parameter (`T?`, declaration == nullptr) — is
    // also unwrapped when narrowed. The `if T.TryParse(s, out var v) then
    // return v; end` bridge narrows `v: T?` non-null via the `when true`
    // out-postcondition (narrowFromOutPostcond); `return v` must then satisfy
    // the non-nullable `T` return exactly as the concrete-type case does. The
    // clone-and-clear `else` branch below handles any Named `from` uniformly;
    // codegen's return coercion (unwrapNarrowedNullable / coerceReturnToNullable
    // Value) extracts the value when the monomorphized `T?` is a tagged optional
    // and is a no-op when it is a bare reference pointer.
    if (fromExpr && from->nullable && !to->nullable &&
        (from->kind == TypeRef::Kind::Primitive ||
         from->kind == TypeRef::Kind::Named)) {
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
    // §8.6 / §5.1 (#1311): a primitive value type boxes into a builtin generic
    // interface it implements (IComparable<T>/IEquatable<T>, stdlib §9.1–§9.5)
    // — `var c: IComparable<int> := 5` boxes the int. The interface's type
    // argument must be the primitive's own alias (no widening); an invalid
    // boxing (`IComparable<long> := 5`, `IComparable<int> := false`) stays
    // rejected by primitiveBoxesIntoBuiltinIface. A nullable source into a
    // non-nullable interface reference is still a §7 drop and is reported.
    if (from->kind == TypeRef::Kind::Primitive &&
        to->kind == TypeRef::Kind::Named &&
        primitiveBoxesIntoBuiltinIface(from->primitive, to)) {
        if (from->nullable && !to->nullable) { reportNullable(); return; }
        return;  // accepted — codegen boxes the primitive at the store site
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
            // §11 / §12.9 / §5.1 (#1940): cross-declaration or arity mismatch.
            // The base conformance must still hold nominally — `from`'s
            // declaration must BE `to`'s declaration or a subtype of it (§11:
            // conformance is by nominally listing the interface among the
            // bases). Without this gate a user class assigned to an unrelated
            // *generic declared* interface it does not implement (e.g. a bare
            // `Box` to `IComparable<Box>`) was ACCEPTED — the DECLARED-interface
            // analogue of #1935's builtin residual — and a later interface call
            // would dispatch through an itable the object lacks (unsound accept,
            // runtime type confusion). `isSubtype` is reflexive, so the
            // same-declaration arity-mismatch case stays accepted; a genuine
            // subtype (incl. a covariant/contravariant generic, whose
            // declaration relationship must hold regardless of the type
            // arguments) stays accepted with the per-argument variance the
            // conservatively-accepted residual (deferred to the full §12.9
            // milestone), exactly as before. Only a PROVEN non-conformance — no
            // nominal path at all — is now rejected. This mirrors the non-
            // generic Named-Named path just below (which already gates on the
            // same `isSubtype`).
            if (!isSubtype(from->declaration, to->declaration)) {
                reportNotConvertible();
                return;
            }
            // Base conformance established: conservative skip (the per-argument
            // variance is the residual).
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
        // §11 / §12.9 / §5.1 (#1935): a USER-declared `from` (non-null
        // declaration) reaching a BUILTIN generic interface `to` must actually
        // implement `to`'s base interface — nominal conformance, verified by
        // walking `from`'s transitive declared-base closure. Without this,
        // variantGenericAssignable's user→builtin residual conservatively
        // ACCEPTS, letting an unrelated `MyBag<Dog>` pass as `IList<Dog>` and a
        // later interface call dispatch through an itable the object lacks (an
        // unsound accept — runtime type confusion).
        if (from->declaration &&
            !userTypeImplementsBuiltinIface(from, namedTypeSimpleName(to))) {
            reportNotConvertible();
            return;
        }
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
    // §5.1 / §27.1.1 (#2067): a non-function source is NEVER implicitly
    // convertible to a function-typed target — there is no scalar-to-function
    // conversion (§5.1 v0.204 enumerates identity/widening/integer-promotion/
    // reference/numeric/null but not a function-formation rule). The §27.6
    // property-getter promotion above is the only exception (zero-argument
    // function whose return matches the property's declared type, returning
    // early when accepted). Every other non-function → function pair (e.g. an
    // `int` literal in a `(int) -> int` parameter slot) was previously
    // silently accepted and reached codegen, producing a malformed LLVM call
    // whose scalar argument did not match the callee's pointer/function-value
    // parameter. Reject with a §5.1 diagnostic so the user sees a clean
    // compile-time error instead of an IR-verifier abort.
    //
    // §5.1 narrow exception (#2067 follow-up): a `null` LITERAL into a
    // NULLABLE function-typed target — `null` implicitly converts to `T?`
    // for any T (§5.1 v0.204 "Nullable conversions"), and function types are
    // no exception: `(int) -> int` admits `null` only at its nullable form
    // `((int) -> int)?`. The first cut of #2067 over-rejected this as a
    // non-function → function pair because the source's static type is the
    // pre-target `nullable Object` placeholder the literal carries before
    // target-directed refinement; gate the reject on a non-null literal
    // source so the legitimate §5.1 null→T? path survives.
    if (to->kind == TypeRef::Kind::Function &&
        from->kind != TypeRef::Kind::Function) {
        const bool nullLitToNullableFn =
            to->nullable && fromExpr &&
            dynamic_cast<const ast::NullLiteralExpr*>(fromExpr);
        if (!nullLitToNullableFn) {
            reportNotConvertible();
            return;
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
    // §27.1.1 (v0.180): function-VALUE assignability. §27.6 draws a line
    // between forming a fresh method/property reference at THIS use site
    // (which may bridge any §5.1 implicit conversion by synthesizing an
    // adapter at the formation site) and rebinding an ALREADY-FORMED
    // function value to a new function type (the narrower rule enforced
    // below). Detect the formation case the same way codegen's
    // isPlusOneClosureProducer does: a bare IdentifierExpr or
    // MemberAccessExpr whose resolved symbol is a MethodSymbol/
    // PropertySymbol, evaluated directly as this assignment's source —
    // NOT a value already sitting in a variable/field/parameter. A bare
    // LambdaExpr is the same story for a different reason: its param/return
    // shape is target-typed (§27.4) — inferred FROM `to`, not independently —
    // so there is no prior, independently-typed value to compare; its body is
    // separately checked against the target elsewhere (pendingLambdaReturnType_
    // / pendingLambdaParamTypes_ above). Formation sites keep their prior
    // (unchanged) permissive behavior for the param/return SHAPES; every
    // function-typed assignment still requires exact arity and exact
    // modifier slots (§27.1, §27.6's own "modifier slots ... must match
    // exactly" — that part is not part of the bridge).
    if (from->kind == TypeRef::Kind::Function &&
        to->kind == TypeRef::Kind::Function) {
        bool freshReferenceFormation = fromExpr &&
            ((dynamic_cast<const ast::IdentifierExpr*>(fromExpr) ||
              dynamic_cast<const ast::MemberAccessExpr*>(fromExpr)) &&
             (dynamic_cast<MethodSymbol*>(ctx_.exprSymbol(*fromExpr)) ||
              dynamic_cast<PropertySymbol*>(ctx_.exprSymbol(*fromExpr))));
        bool freshLambda = fromExpr &&
            dynamic_cast<const ast::LambdaExpr*>(fromExpr);
        bool skipShapeCheck = freshReferenceFormation || freshLambda;

        // Arity mismatch is never assignable (§27.1.1), formation or not.
        if (from->funcParams.size() != to->funcParams.size()) {
            reportNotConvertible();
            return;
        }
        // The FUNCTION TYPE ITSELF may be nullable (`((int) -> int)?`); a
        // nullable value into a non-nullable target is the ordinary §7 rule
        // (unchanged from before this block existed — restored here because
        // this block now returns unconditionally on the accepted paths below,
        // where it used to fall through to the general nullable check).
        if (from->nullable && !to->nullable) { reportNullable(); return; }
        // Modifier slots (owned/ref/out/out owned/scoped) must match
        // EXACTLY, always — §27.1's identity rule; §27.1.1's fourth bullet
        // restates it for value-assignability, and §27.6 confirms it is not
        // part of what a formation-site adapter may bridge.
        for (std::size_t i = 0; i < from->funcParams.size(); ++i) {
            if (from->funcParams[i].modifier != to->funcParams[i].modifier) {
                const bool fromScoped = from->funcParams[i].modifier == "scoped";
                const bool toScoped = to->funcParams[i].modifier == "scoped";
                std::string detail;
                if (fromScoped != toScoped) {
                    // Preserve the more specific §27.2/§28.14 `scoped` wording
                    // when that is the mismatched modifier.
                    detail = fromScoped ? " is `scoped` but the target's is not"
                                        : " is not `scoped` but the target's is";
                } else {
                    auto modName = [](std::string_view m) {
                        return m.empty() ? std::string("(none)")
                                         : "`" + std::string(m) + "`";
                    };
                    detail = std::string(" is ") +
                             modName(from->funcParams[i].modifier) +
                             " but the target's is " +
                             modName(to->funcParams[i].modifier);
                }
                ctx_.diagnostics().report(
                    Severity::Error, loc,
                    std::string("the ") + what +
                    " has a function type whose parameter " +
                    std::to_string(i + 1) + detail +
                    " — modifier slots are part of a function signature's "
                    "identity; there is no conversion between them (§27.1, "
                    "§27.1.1 v0.180, §27.2/§28.14 v0.175)");
                return;
            }
        }
        // §27.3 (v0.217, #2101): a fresh lambda's param/return SHAPES stay
        // target-typed (the skipShapeCheck carve above) — EXCEPT that a
        // return-type ASCRIPTION, when written, must agree with the target's
        // return slot under §27.1.1's covariant return rule: identity, or a
        // reference conversion for class/interface returns. The ascription
        // behaves as a declared return type and never bridges a conversion
        // the function value itself could not (no numeric widening, no
        // boxing).
        if (freshLambda) {
            auto* lamFrom = dynamic_cast<const ast::LambdaExpr*>(fromExpr);
            if (lamFrom && lamFrom->retType && from->funcReturn &&
                !functionSlotConvertible(from->funcReturn, to->funcReturn,
                                         isSubtype)) {
                ctx_.diagnostics().report(
                    Severity::Error, loc,
                    std::string("the ") + what + " is a lambda whose "
                    "return-type ascription does not agree with the target "
                    "function type's return slot \xe2\x80\x94 the ascription "
                    "behaves as a declared return type and must satisfy "
                    "\xc2\xa7""27.1.1's covariant return rule (identity, or "
                    "a reference conversion for a class/interface return); "
                    "it never bridges a conversion (\xc2\xa7""27.3 v0.217, "
                    "\xc2\xa7""27.1.1)");
                return;
            }
        }
        if (!skipShapeCheck) {
            // Parameter slots are CONTRAVARIANT (§27.1.1 first bullet): the
            // TARGET's declared param type must convert to the VALUE's — a
            // (Animal) -> () value is usable where (Dog) -> () is expected,
            // so the check is applied with the operands swapped.
            for (std::size_t i = 0; i < from->funcParams.size(); ++i) {
                if (!functionSlotConvertible(to->funcParams[i].type,
                                             from->funcParams[i].type,
                                             isSubtype)) {
                    reportNotConvertible();
                    return;
                }
            }
            // The return slot is COVARIANT (§27.1.1 second bullet): the
            // VALUE's return type must convert to the TARGET's — a
            // () -> Dog value is usable as () -> Animal.
            if (!functionSlotConvertible(from->funcReturn, to->funcReturn,
                                         isSubtype)) {
                reportNotConvertible();
                return;
            }
        }
        return;
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
// #1621 / §31.2.1 / §20: see ExprTypecheck_impl.hpp for the full rationale.
// The simple name of a Named TypeRef: its TypeSymbol's name when declared,
// else the AST use-site's name (a built-in named directly in source, e.g.
// `IOException`), else `importedName` (a cross-module TypeRef, or a
// synthetic built-in-ancestor TypeRef minted by `builtinAncestorTypeRef`,
// neither of which has an astNode). Empty for a non-Named ref.
std::string_view ExprTC::namedRefSimpleName(const TypeRef* ref) {
    if (!ref || ref->kind != TypeRef::Kind::Named) return {};
    if (ref->declaration) return ref->declaration->name;
    if (!ref->importedName.empty()) return ref->importedName;
    if (ref->astNode && ref->astNode->name &&
            !ref->astNode->name->parts.empty())
        return ref->astNode->name->parts.back();
    return {};
}

// #1621 / §31.2.1: a headless Named TypeRef standing for a built-in
// exception ancestor name reached only through the built-in exception
// hierarchy table (no AST node names it directly — e.g. `Exception` reached
// while walking up from `OverflowException`). `declaration` stays null (it
// IS a built-in); `importedName` carries the name.
TypeRef* ExprTC::builtinAncestorTypeRef(std::string_view name) {
    TypeRef* ref = ctx_.makeTypeRef();
    ref->kind = TypeRef::Kind::Named;
    ref->declaration = nullptr;
    ref->importedName = name;
    return ref;
}

// #1621 / §31.2.1: the ancestor-name chain of `ty` (most-specific first, `ty`
// itself included), walking the TypeSymbol base-type chain, the AST
// TypeDecl's declared bases (built-in bases have no TypeSymbol and are only
// visible here), AND — once the walk reaches a built-in Named type with no
// further AST bases of its own — the built-in exception hierarchy table
// (`common::builtinExceptionParentOf`), so a purely built-in ancestor chain
// (e.g. `OverflowException` -> `ArithmeticException` -> `Exception`) is still
// discoverable. Before this, a multi-type catch-arm binding of two SIBLING
// built-ins (`OverflowException | ArgumentException`, both `Exception`-
// derived but neither the AST-declared base of the other) could not find
// their shared ancestor at all.
//
// §31.2.1 (NCB) — TERMINATION (#1624): the visited-set MUST gate the base
// WALK, not only the result set. The base-type graph can contain cycles that
// survive into sema via error recovery (an erroneous `class A: B; class B:
// A` is reported by the §12 cycle check but still reaches this walk during
// catch-arm binding); if a revisited node re-pushes its bases, the walk loops
// forever. A node is "visited" the first time its name (logical type
// identity) is seen; empty-name nodes (synthetic/built-in TypeRefs without a
// resolvable name) are deduped by TypeRef pointer. A hard iteration cap is
// kept as defense-in-depth so a malformed generic cycle can never make the
// NCB computation unbounded.
std::vector<std::pair<std::string_view, TypeRef*>>
ExprTC::namedAncestorChain(TypeRef* ty) {
    std::vector<std::pair<std::string_view, TypeRef*>> result;
    std::unordered_set<std::string_view> seenName;
    std::unordered_set<const TypeRef*> seenPtr;  // empty-name nodes
    std::vector<TypeRef*> queue{ty};
    const std::size_t kMaxWalk = 8192;  // defensive bound; ancestor sets are shallow
    std::size_t walked = 0;
    while (!queue.empty()) {
        if (++walked > kMaxWalk) break;  // §31.2.1 NCB must terminate (#1624)
        TypeRef* cur = queue.back();
        queue.pop_back();
        if (!cur || cur->kind != TypeRef::Kind::Named) continue;
        std::string_view curName = namedRefSimpleName(cur);
        // Cycle guard: a node is fresh only the first time it is seen, and
        // the guard gates the base walk below — a revisit continues WITHOUT
        // re-pushing bases, so a base-type cycle cannot loop forever.
        bool fresh = !curName.empty()
            ? seenName.insert(curName).second
            : seenPtr.insert(cur).second;
        if (!fresh) continue;
        if (!curName.empty())
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
        } else if (!curName.empty()) {
            // #1621: `cur` is itself a built-in (no TypeSymbol, so no AST base
            // list to walk) — consult the built-in exception hierarchy to keep
            // walking upward, so two sibling built-ins still meet at their
            // shared built-in ancestor.
            std::string_view parentName = common::builtinExceptionParentOf(curName);
            if (!parentName.empty())
                queue.push_back(builtinAncestorTypeRef(parentName));
        }
    }
    return result;
}

// #1621 / §31.2.1: does `ty` (or an ancestor reached by `namedAncestorChain`)
// carry the simple name `ancestorName`?
bool ExprTC::hasNamedAncestor(TypeRef* ty, std::string_view ancestorName) {
    if (ancestorName.empty()) return false;
    // Direct name match (the type IS the ancestor).
    if (namedRefSimpleName(ty) == ancestorName) return true;
    for (const auto& [name, unused] : namedAncestorChain(ty)) {
        (void)unused;
        if (name == ancestorName) return true;
    }
    return false;
}

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
        if (from->kind == TypeRef::Kind::Named) {
            if (from->declaration && to->declaration) {
                // Defer generics to the variance slice (conservative skip).
                if (!from->genericArgs.empty() || !to->genericArgs.empty())
                    return true;
                if (from->declaration == to->declaration) return true;
                return isSubtype(from->declaration, to->declaration);
            }
            // #1621: at least one side is a built-in Named type (no
            // TypeSymbol, so `isSubtype` above cannot run). Resolve by NAME
            // through the same ancestor-name walk `catchArmNcb`'s Step 2 uses
            // — identity, a user-declared subtype of a built-in base, or a
            // built-in ancestor/descendant pair — instead of the old blanket
            // "declaration-less Named types are compatible" fallback that
            // used to live here. That fallback was meant for genuinely
            // different-KIND pairs (InlineArray/Function/Unit, handled below)
            // but fired for ANY two Named types where either side lacked a
            // declaration, so it spuriously treated unrelated built-ins (or a
            // built-in vs. an unrelated user class, e.g. `Dog` vs.
            // `IOException`) as mutually convertible — the root cause of
            // #1621's unreliable multi-type catch-arm NCB.
            return hasNamedAncestor(from, namedRefSimpleName(to));
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
// Step 1 — commonTypeOf fold: handles the case where one listed type is
//   (transitively) a subtype of another — the fold collapses to that base.
//   `commonTypeOf`'s `convertsInto` (#1621) walks both the TypeSymbol
//   base-types list (§12 inheritance, for user hierarchies) and the built-in
//   exception hierarchy by name (for built-ins), so this step is now correct
//   whether or not a built-in exception type participates.
//
// Step 2 — ancestor-name walk (`namedAncestorChain`): handles the general
//   case a pairwise fold cannot — e.g. two SIBLING types (neither a subtype
//   of the other) that nonetheless share a more distant common ancestor,
//   built-in or user-declared. The NCB is the first ancestor of the first
//   listed type (most-specific first) that is also an ancestor of every
//   other listed type.
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

    // --- Step 2: ancestor-name walk (handles sibling / built-in common
    // bases — see namedAncestorChain, #1621) --------------------------------
    TypeRef* first = ctx_.resolvedType(*types[0]);
    auto firstAncestors = namedAncestorChain(first);
    for (const auto& [name, candidateRef] : firstAncestors) {
        if (name.empty()) continue;
        bool allMatch = true;
        for (std::size_t i = 1; i < types.size(); ++i) {
            TypeRef* ti = types[i] ? ctx_.resolvedType(*types[i]) : nullptr;
            if (!hasNamedAncestor(ti, name)) { allMatch = false; break; }
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
    TypeRef* common = nullptr;     // running common type of the value arms
    bool haveValueArm = false;     // at least one non-diverging arm seen
    bool reported = false;         // emit the §20 error at most once
    // #1952 / §20 / §19.2 (v0.210): a bare `null` arm "imposes no constraint of
    // its own … it target-types to the nullable form of the other branches'
    // common type" — `(if c then s else null)` with `s: string` has type
    // `string?`; §19.2 ties the expression-position case/when result to that
    // SAME rule. typeCase folds the `null` arm's own placeholder type (`nullable
    // Object`) through commonTypeOf, so a `string` value arm meeting a `null`
    // arm widened to `Object?` (string boxes to Object) instead of `string?`.
    // That mis-typing defeated isPlusOneStringProducer's `string` check and
    // leaked the nullable producer arm's minted string (#1952). We detect that
    // exact STRING shape below and record `string?`.
    //
    // SCOPED TO `string` (recorded). Only the `string` value-arm shape is
    // overridden. A VALUE-type null-arm result (`int?`, `bool?`, record?, enum?)
    // keeps the pre-#1952 fold UNCHANGED — its lowerCaseExpr Path A/B handling
    // depends on the folded (`Object?`) type, and re-typing it via the general
    // nullable-form rule regressed the value-nullable result (#573,
    // codegen_cg28_573). typeConditional applies the nullable-form rule for ALL
    // types because the if-expression lowering path differs; the case/when path
    // is brought into line only for `string`, which is the leak #1952 concerns.
    bool sawNullArm = false;
    bool haveNonNullValueArm = false;
    bool allNonNullValueArmsString = true;
    for (const auto& arm : cse.arms) {
        // Only expression-bodied arms produce a value (statement / `do…end`
        // arms are statement-position only, §19.2). A bottom-typed arm body
        // (e.g. `throw E`) has a null exprType and imposes no constraint.
        if (!arm.exprBody) continue;
        bool isNullLit =
            dynamic_cast<const ast::NullLiteralExpr*>(arm.exprBody.get()) !=
            nullptr;
        if (isNullLit) sawNullArm = true;
        TypeRef* at = ctx_.exprType(*arm.exprBody);
        if (!at) continue;  // diverging (bottom) arm — no constraint
        if (!isNullLit) {
            haveNonNullValueArm = true;
            if (!(at->kind == TypeRef::Kind::Primitive &&
                  at->primitive == PrimitiveType::String && !at->nullable))
                allNonNullValueArmsString = false;
        }
        // Fold every arm — INCLUDING a `null` arm's `Object?` placeholder — so
        // value-type null-arm results keep their pre-#1952 typing (the string
        // shape is overridden after the loop).
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
    // #1952: a `string` value arm meeting a bare `null` arm has type `string?`
    // (§20's null-arm rule). Record it directly, replacing the folded `Object?`.
    if (sawNullArm && haveNonNullValueArm && allNonNullValueArmsString) {
        TypeRef* strq = ctx_.makeTypeRef();
        strq->kind = TypeRef::Kind::Primitive;
        strq->primitive = PrimitiveType::String;
        strq->nullable = true;
        ctx_.setExprType(&e, strq);
        return;
    }
    // Record the folded common type (when there was a value-producing arm) so
    // member access on the case result (`(case … end).M`) resolves. When every
    // arm diverges, no type is recorded — the case produces no value.
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
    // §21.1 v0.201 (#2049): a `const` LOCAL — compile-time OR runtime — is a
    // single-assignment binding: assignment, compound assignment, `??=` are
    // all compile errors naming the declaration. This covers `K := ...`,
    // `K += ...`, `K ??= ...`, and every other AssignOp, in any method or
    // constructor. (Per §21.1: "A `const` local of either classification is
    // not an assignable location: assignment, compound assignment, `++`/`--`,
    // and binding to a `ref` or `out` parameter are compile errors naming the
    // declaration.") `++`/`--` are rejected by the IncrementStmt check; ref/out
    // by checkRefOutOperandCategory.
    if (auto* ls = dynamic_cast<LocalSymbol*>(ctx_.exprSymbol(*as.target))) {
        if (ls->isConst) {
            ctx_.diagnostics().report(
                Severity::Error, as.span.begin,
                std::string("cannot assign to const local `") +
                std::string(ls->name) +
                "` — a const local is a single-assignment binding (§21.1)");
            return;
        }
        // §17.15 (v0.216, #2098): a binding-atom binding is not an
        // assignable location (the §21.1 runtime-`const` rule). Assignment
        // forms are statements, so this fires only through a `do`-bodied
        // lambda inside the atom that captures the binding — exactly the
        // hole the spec names.
        if (ls->isBindAtomClause) {
            ctx_.diagnostics().report(
                Severity::Error, as.span.begin,
                std::string("cannot assign to binding-atom binding `") +
                std::string(ls->name) +
                "` — a `(var …)` binding is a single-assignment binding and "
                "not an assignable location (§17.15, v0.216)");
            return;
        }
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

// §6.6 / A.6 (v0.184, #1690): grammar A.6 restricts an `LValue`'s base to
// `Identifier | "this"`, extended only by `.field` and `[index]` hops —
// "A.6's lvalue shape applies unchanged: a method-call result cannot base
// the chain — extract it to a local first" (§6.6). `target` is walked from
// the outside in, peeling off `.member` (MemberAccessExpr), `[index]`
// (IndexExpr), and the null-forgiving `!` (PostfixBangExpr) — each of these
// denotes the SAME underlying storage location as its operand (a read-
// through, not a fresh value; #206's `a!.V := 7` is a real lvalue and must
// stay accepted) — looking for the chain's actual base. A CallExpr or
// NullCondCallExpr found there means the base is a method-call (or
// null-conditional-invocation) RESULT, which grammar A.6 does not admit; a
// NewExpr base (`new C().field := v`, #1744) is likewise a fresh value, not
// a storage location, and is rejected the same way.
void ExprTC::checkLValueChainNotCallBased(const ast::Expr& target) {
    const ast::Expr* e = &target;
    for (;;) {
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(e)) {
            e = ma->target.get();
        } else if (auto* ix = dynamic_cast<const ast::IndexExpr*>(e)) {
            e = ix->target.get();
        } else if (auto* pb = dynamic_cast<const ast::PostfixBangExpr*>(e)) {
            e = pb->operand.get();
        } else {
            break;
        }
        if (!e) return;
    }
    if (dynamic_cast<const ast::CallExpr*>(e) ||
        dynamic_cast<const ast::NullCondCallExpr*>(e)) {
        ctx_.diagnostics().report(
            Severity::Error, target.span.begin,
            "a method-call result cannot be the base of an assignment "
            "target — grammar A.6's lvalue base must be an identifier or "
            "`this`; extract the call result into a local first "
            "(`var c := getCache(); c.items[0] := v;`) (§6.6)");
    } else if (dynamic_cast<const ast::NewExpr*>(e)) {
        // §6.6 / A.6 (v0.196, #1744): a `new` expression is a fresh value,
        // not a storage location — `new C().field := v` bases the chain on a
        // NewExpr, which A.6 does not admit. Bind it to a local first.
        ctx_.diagnostics().report(
            Severity::Error, target.span.begin,
            "a newly-constructed object cannot be the base of an assignment "
            "target — grammar A.6's lvalue base must be an identifier or "
            "`this`; bind the `new` expression to a local first "
            "(`var c := new C(); c.items[0] := v;`) (§6.6)");
    }
}

// §22.3 "ref and out argument locations" (v0.184, #1682). See the header for
// the full contract; this covers the DECLARATION-based categories only (the
// dataflow-based owned-state exclusion is MethodOwnership::
// checkRefOutOwnedOperand in the ownership pass).
void ExprTC::checkRefOutOperandCategory(const ast::CallArgument& a) {
    if (!a.value) return;
    if (a.modifier != "ref" && a.modifier != "out") return;
    const ast::Expr& target = *a.value;
    const char* kind = (a.modifier == "ref") ? "ref" : "out";

    // Structural shape (§6.6 / A.6's lvalue shape, reused from
    // checkLValueChainNotCallBased): strip `.field` / `[index]` / postfix
    // `!` hops down to the chain's base, which must be an Identifier
    // (including `this`) — anything else (a call result, a binary/unary
    // expression, a literal, a `new` expression, …) is not a storage
    // location at all, and is exactly the ICE-inducing shape #1682 found
    // reaching codegen's lvalueAddr with nothing to return (internal
    // "lowerExpr returned null" diagnostic, #1386).
    const ast::Expr* base = &target;
    for (;;) {
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(base)) {
            base = ma->target.get();
        } else if (auto* ix = dynamic_cast<const ast::IndexExpr*>(base)) {
            base = ix->target.get();
        } else if (auto* pb = dynamic_cast<const ast::PostfixBangExpr*>(base)) {
            base = pb->operand.get();
        } else {
            break;
        }
        if (!base) return;
    }
    if (!dynamic_cast<const ast::IdentifierExpr*>(base)) {
        ctx_.diagnostics().report(
            Severity::Error, target.span.begin,
            std::string("a `") + kind + "` argument must be an assignable "
            "storage location — a variable, field, or indexer/property "
            "slot — not the result of an expression; extract it into a "
            "local first (§22.3)");
        return;
    }

    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(&target)) {
        if (id->name == "this" || id->name == "base") return;
        Symbol* sym = ctx_.exprSymbol(target);
        if (auto* ls = dynamic_cast<LocalSymbol*>(sym)) {
            // §21.1 v0.201 (#2049): a `const` LOCAL — compile-time OR runtime
            // — is single-assignment; binding it to `ref` or `out` is a
            // compile error naming the declaration. Per §21.1: "A `const`
            // local of either classification is not an assignable location:
            // ... binding to a `ref` or `out` parameter are compile errors
            // naming the declaration."
            if (ls->isConst) {
                ctx_.diagnostics().report(
                    Severity::Error, target.span.begin,
                    std::string("a `") + kind + "` argument may not be a "
                    "const local `" + std::string(id->name) +
                    "` — a const local is a single-assignment binding "
                    "(§22.3 / §21.1)");
                return;
            }
            // §17.15 (v0.216, #2098): a binding-atom binding is not an
            // assignable location — binding it to `ref`/`out` is a compile
            // error naming the rule (the §21.1 runtime-`const` mirror).
            if (ls->isBindAtomClause) {
                ctx_.diagnostics().report(
                    Severity::Error, target.span.begin,
                    std::string("a `") + kind + "` argument may not be a "
                    "binding-atom binding `" + std::string(id->name) +
                    "` — a `(var …)` binding is a single-assignment binding "
                    "and not an assignable location (§17.15, v0.216)");
                return;
            }
            // §28.10's pin: a `using var` binding's disposal is scope-bound;
            // a `ref`/`out` write through it is invisible to that machinery.
            if (ls->isUsing) {
                ctx_.diagnostics().report(
                    Severity::Error, target.span.begin,
                    std::string("a `") + kind + "` argument may not be a "
                    "`using var` binding `" + std::string(id->name) +
                    "` — its disposal is pinned to scope exit (§22.3 / "
                    "§28.10)");
            }
            return;  // owned-LOCAL exclusion: ownership pass (dataflow)
        }
        if (dynamic_cast<ParameterSymbol*>(sym)) {
            return;  // owned-PARAMETER exclusion: ownership pass (dataflow)
        }
        if (auto* fs = dynamic_cast<FieldSymbol*>(sym)) {
            checkRefOutFieldOperand(*fs, target, kind);
            return;
        }
        if (dynamic_cast<ConstMemberSymbol*>(sym)) {
            ctx_.diagnostics().report(
                Severity::Error, target.span.begin,
                std::string("a `") + kind + "` argument may not be a "
                "`const` member — a const is a compile-time constant, not "
                "a storage location (§22.3)");
        }
        return;
    }
    if (dynamic_cast<const ast::MemberAccessExpr*>(&target)) {
        Symbol* sym = ctx_.exprSymbol(target);
        if (auto* fs = dynamic_cast<FieldSymbol*>(sym)) {
            checkRefOutFieldOperand(*fs, target, kind);
            return;
        }
        if (auto* ps = dynamic_cast<PropertySymbol*>(sym)) {
            checkRefOutPropertyOperand(*ps, target, kind);
            return;
        }
        if (dynamic_cast<ConstMemberSymbol*>(sym)) {
            ctx_.diagnostics().report(
                Severity::Error, target.span.begin,
                std::string("a `") + kind + "` argument may not be a "
                "`const` member — a const is a compile-time constant, not "
                "a storage location (§22.3)");
        }
        return;
    }
    if (dynamic_cast<const ast::IndexExpr*>(&target)) {
        // §17.6 admits an indexer slot; §22.3 further requires an
        // ACCESSIBLE SETTER (mirrors the plain-property rule below) — a
        // user-declared indexer with no `set` accessor is not a storage
        // location. Built-in collection indexers (Array<T>, OwningArray<T>,
        // …) carry no PropertySymbol here and are left unchecked by this
        // sema-level pass; their owning-slot exclusion is handled by the
        // ownership pass (indexerSetterTakesOwned /
        // targetIsOwningInlineArraySlot).
        Symbol* sym = ctx_.exprSymbol(target);
        if (auto* ps = dynamic_cast<PropertySymbol*>(sym)) {
            checkRefOutPropertyOperand(*ps, target, kind);
        }
        return;
    }
}

// §22.3 / §22.8 / §22.12 rule 2. Shared field-operand checks for
// checkRefOutOperandCategory: readonly-outside-context and mutable-static.
// Does NOT check the `owned` modifier — that exclusion is dataflow-owned
// (see checkRefOutOperandCategory's header comment) and is applied by the
// ownership pass via exprIsOwnedFieldRead, which this function's caller
// leaves to a separate pass.
void ExprTC::checkRefOutFieldOperand(FieldSymbol& fs, const ast::Expr& target,
                                     const char* kind) {
    bool isReadonly = false, isStatic = false;
    for (auto mod : fs.modifiers) {
        if (mod == "readonly") isReadonly = true;
        else if (mod == "static") isStatic = true;
    }
    // §22.8: a readonly field is assignable only in a field initializer or
    // the declaring class's constructor — `inConstructor_` covers both
    // (see checkReadonlyAssignment above, the `:=`-target sibling of this
    // check).
    if (isReadonly && !inConstructor_) {
        ctx_.diagnostics().report(
            Severity::Error, target.span.begin,
            std::string("a `") + kind + "` argument may not be readonly "
            "field `" + std::string(fs.name) + "` outside the declaring "
            "class's constructor (§22.3 / §22.8)");
        return;
    }
    // §22.12 rule 2: a mutable (non-readonly, non-`const` — `const` members
    // are ConstMemberSymbol, never reach here) static field's store
    // semantics are defined for direct assignment only; a ref/out alias
    // write is outside that contract.
    if (isStatic && !isReadonly) {
        ctx_.diagnostics().report(
            Severity::Error, target.span.begin,
            std::string("a `") + kind + "` argument may not be the mutable "
            "static field `" + std::string(fs.name) + "` — its store "
            "semantics under §22.12 rule 2 are defined for direct "
            "assignment only (§22.3)");
    }
}

// §22.3 / §23.1 / §23.3 / §13.6. Shared property-operand check for
// checkRefOutOperandCategory (both a plain property and an indexer resolve
// to a PropertySymbol): no setter, a readonly auto-property's setter used
// outside its assignable context, or a setter that is not accessible here —
// mirrors the plain-`:=`-assignment-target checks inline in visitStmt
// (§23.3/§23.1/§13.6) verbatim, applied to a ref/out operand instead of an
// assignment target.
void ExprTC::checkRefOutPropertyOperand(PropertySymbol& ps,
                                        const ast::Expr& target,
                                        const char* kind) {
    auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps.decl);
    if (!pd) {
        // fix #2454 (§22.3/§13.6/§23.3, §2.8.2 clause 1/§2.8.3 rule 4,
        // v0.235): an IMPORTED PropertySymbol has no ast::PropertyDecl
        // (`ps.decl` is unconditionally null — ModuleLoader never
        // reconstructs a body-less imported property's decl), so this
        // function previously returned immediately for ANY imported
        // property, skipping BOTH the "no setter" check and the §13.6
        // setter-visibility check — the ref/out sibling of #2412 (hasSetter)
        // and #2415 (setter visibility), which fixed the identical gap for
        // the plain `:=` assignment-target check in the same file. Falls
        // back to the SAME side tables #2412/#2415/#2461 publish:
        // `ctx_.importedPropertyHasSetter(ps)`,
        // `ctx_.importedPropertySetterVisibility(ps)`, and (fix #2461)
        // `ctx_.importedPropertyHasGetter(ps)`.
        //
        // fix #2461 (§22.3/§23, v0.236): the getter-presence check this
        // branch previously lacked. §22.3: "for `ref`, the getter is called
        // once to produce the callee's slot value (for `out` no getter call
        // occurs)" — so a write-only property (setter but NO getter) is a
        // legal `out` target but NOT a legal `ref` target. Before this bit
        // existed, a write-only imported property used as `ref` fell through
        // this check (and the separate ExprTypecheck_Calls.cpp
        // propertyDeclIsSetOnly READ gate, which is also `pd`-gated) all the
        // way to a raw LINKER error (undefined `...$propget` symbol — the
        // exporter never emitted one), rather than a clean §23 rejection.
        // The LOCAL same-module shape was already correctly rejected, but by
        // that earlier propertyDeclIsSetOnly gate on the `d.W` member-access
        // read, which (deliberately per §23) treats ANY non-`:=`-target
        // access of a write-only property as an illegal read — including the
        // `d.W` inside `out d.W`, which is why that gate is NOT widened to
        // imported properties here (it would false-reject the spec-legal
        // write-only-as-`out` case, see xmod_refout_prop_visibility_2454's
        // T6). This check instead keys off the `kind` string so `out` is
        // correctly admitted (§22.3: no getter call for `out`).
        if (!ctx_.importedPropertyHasSetter(&ps)) {
            ctx_.diagnostics().report(
                Severity::Error, target.span.begin,
                std::string("a `") + kind + "` argument must be a property "
                "with an accessible setter — this property has no setter "
                "(§22.3 / §23.3)");
            return;
        }
        // fix #2461 (§22.3/§23): `ref` additionally requires a getter (the
        // copy-in calls it once); `out` does not ("for `out` no getter call
        // occurs"). A write-only imported property (hasSetter && !hasGetter)
        // as `ref` is therefore rejected here with a clean §22.3/§23 message
        // instead of the pre-fix raw linker error. The property name is
        // included to match the LOCAL propertyDeclIsSetOnly gate's message
        // shape ("property `W` is write-only ...").
        if (kind[0] == 'r' /* "ref", not "out" */ &&
            !ctx_.importedPropertyHasGetter(&ps)) {
            ctx_.diagnostics().report(
                Severity::Error, target.span.begin,
                std::string("a `ref` argument requires a readable property — "
                            "`") + std::string(ps.name) +
                "` is write-only (it declares only a `set` accessor) and "
                "has no getter for the copy-in (§22.3 / §23)");
            return;
        }
        ast::Visibility sv = ctx_.importedPropertySetterVisibility(&ps);
        TypeSymbol* declaring = ps.parent;
        bool bad = false;
        if (sv == ast::Visibility::Private) {
            bad = true;  // declaring is an imported TypeSymbol; currentType_
                         // is always a type compiled in THIS module — the
                         // two can never be the same symbol (mirrors #2415).
        } else if (sv == ast::Visibility::Protected) {
            bad = !(currentType_ && isSubtype(currentType_, declaring));
        } else if (sv == ast::Visibility::Internal) {
            bad = true;  // the importing module is by definition a
                         // different module than the declaring one
                         // (mirrors #2415).
        }
        if (bad) {
            ctx_.diagnostics().report(
                Severity::Error, target.span.begin,
                std::string("a `") + kind + "` argument must be a property "
                "with an accessible setter — its setter is not accessible "
                "here (§22.3 / §13.6)");
        }
        return;
    }
    bool hasSetter = true;
    if (pd->bodyKind == ast::PropertyBodyKind::Expression) {
        hasSetter = false;
    } else if (pd->bodyKind == ast::PropertyBodyKind::Block) {
        hasSetter = false;
        for (const auto& acc : pd->accessors)
            if (acc.isSet) { hasSetter = true; break; }
    }
    if (!hasSetter) {
        ctx_.diagnostics().report(
            Severity::Error, target.span.begin,
            std::string("a `") + kind + "` argument must be a property "
            "with an accessible setter — this property has no setter "
            "(§22.3 / §23.3)");
        return;
    }
    bool isReadonlyAuto = false;
    if (pd->bodyKind == ast::PropertyBodyKind::Auto)
        for (auto mod : pd->modifiers)
            if (mod == "readonly") { isReadonlyAuto = true; break; }
    if (isReadonlyAuto && !inConstructor_) {
        ctx_.diagnostics().report(
            Severity::Error, target.span.begin,
            std::string("a `") + kind + "` argument must be a property "
            "with an accessible setter — this readonly property's setter "
            "may be used only in a field initializer or constructor (§22.3 "
            "/ §23.1)");
        return;
    }
    // §13.6: the setter accessor's own (possibly more restrictive)
    // visibility gates access, mirroring the plain-`:=` assignment check.
    ast::Visibility sv = pd->visibility;
    if (pd->bodyKind == ast::PropertyBodyKind::Block) {
        for (const auto& acc : pd->accessors)
            if (acc.isSet && acc.visibility != ast::Visibility::None) {
                sv = acc.visibility;
                break;
            }
    } else if (pd->bodyKind == ast::PropertyBodyKind::Auto &&
               pd->hasAutoAccessorVis && pd->autoAccessorIsSet) {
        sv = pd->autoAccessorVis;
    }
    TypeSymbol* declaring = ps.parent;
    bool bad = false;
    if (sv == ast::Visibility::Private)
        bad = (currentType_ != declaring);
    else if (sv == ast::Visibility::Protected)
        bad = !(currentType_ == declaring ||
                (currentType_ && isSubtype(currentType_, declaring)));
    else if (sv == ast::Visibility::Internal)
        bad = ctx_.isImported(&ps);
    if (bad) {
        ctx_.diagnostics().report(
            Severity::Error, target.span.begin,
            std::string("a `") + kind + "` argument must be a property "
            "with an accessible setter — its setter is not accessible "
            "here (§22.3 / §13.6)");
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
// The hierarchy (per stdlib §4.1–§4.12):
//   Array<T>      : IList<T>
//   OwningArray<T> : IOwningList<T>
//   Set<T>        : ICollection<T>            // mutating borrowing collection
//   LinkedList<T> : ICollection<T>            // mutating borrowing collection
//   Stack<T>      : IReadOnlyCollection<T>    // read-only chain only (v0.165)
//   Queue<T>      : IReadOnlyCollection<T>
//   SortedSet<T>  : IReadOnlyCollection<T>
//   IList<T>      : IReadOnlyList<T>, ICollection<T>
//   IOwningList<T> : IReadOnlyList<T>, IOwningCollection<T>
//   IReadOnlyList<T>       : IReadOnlyCollection<T>
//   ICollection<T>         : IReadOnlyCollection<T>
//   IOwningCollection<T>    : IReadOnlyCollection<T>
//   IReadOnlyCollection<T> : IEnumerable<T>
//   IEnumerable<T>         : (root)
// The concrete borrowing collections above each implement exactly one collection
// interface (transitively closed here to the full read-only chain), so their
// complete generic-interface set is captured — this is what
// builtinBorrowingCollectionFullyModeled relies on to let the top-level variance
// check REJECT a proven non-subtype instead of conservatively accepting (#1851).
// The owning concretes (Owning*) map to IReadOnlyCollection conservatively (their
// owning graph — IOwningEnumerable etc. — is not modeled), so they are NOT
// treated as fully modeled and retain conservative-accept.
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
    else if (name == "Set" || name == "LinkedList")
        // #1851: Set<T> and LinkedList<T> implement ICollection<T> (mutating
        // borrowing collections, stdlib §4.6/§4.9), not merely
        // IReadOnlyCollection<T>. The read-only chain stays reachable
        // transitively (ICollection → IReadOnlyCollection → IEnumerable).
        ancestors.push_back(makeBuiltin("ICollection", {T}));
    else if (name == "Stack" || name == "OwningStack" ||
             name == "Queue" || name == "OwningQueue" ||
             name == "OwningSet" || name == "SortedSet" ||
             name == "OwningLinkedList")
        // Stack/Queue/SortedSet conform only to the read-only chain (v0.165,
        // #1354). The Owning* concretes map here conservatively (see header).
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

// §5.1/§12.9 (#1851): the arity-1 BORROWING-collection builtin generics whose
// COMPLETE generic-interface set is captured by builtinInterfaceAncestors above
// — the concrete borrowing collections (each implements exactly one collection
// interface, transitively closed to the read-only chain) and the borrowing
// collection interfaces themselves. For a `from` in this set, the both-builtin
// variance search finding no candidate for `to` is PROOF that `from` does not
// conform to `to` (not merely an unmodeled gap), so the top-level assignability
// check may soundly REJECT rather than conservatively accept — e.g. `Stack<Dog>`
// is not assignable to the unrelated sibling `IList<Dog>`.
//
// Deliberately EXCLUDED (retain conservative-accept): the owning family
// (Owning*, IOwning*) whose owning graph — IOwningEnumerable, IOwningEnumerator
// — is not modeled here, and the arity-2 maps (Dictionary, SortedDictionary)
// whose element interface carries DictionaryEntry rather than the receiver's own
// arg. Those non-conformances remain a residual (tracked separately).
static bool builtinBorrowingCollectionFullyModeled(std::string_view name) {
    return name == "Array" || name == "LinkedList" || name == "Set" ||
           name == "Stack" || name == "Queue" || name == "SortedSet" ||
           name == "IList" || name == "ICollection" ||
           name == "IReadOnlyList" || name == "IReadOnlyCollection" ||
           name == "IEnumerable";
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
//
// #2007: the composite non-generic kinds InlineArray (`[N]T`) and Function
// (`(T1,…)->T2`) have no separate "base + arguments" decomposition — the
// element/parameter/return types ARE the identity. Their shallow base check
// therefore compares the structural SHAPE: ownedness + folded size + element
// base for InlineArray (mirroring the #2001 typeRefEqual rule, §15.1: "The
// size N is part of the type"); parameter count + modifiers + return-ownedness
// for Function (deep type identity is typeArgIdentical's recursion).
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
    if (a->kind == TypeRef::Kind::InlineArray) {
        // §15.1: size N is part of the type. Distinguish ONLY when both sizes
        // are FOLDED (arraySizeValue >= 0, the #2001 rule): if either is still
        // unfolded (-1) the size is not statically known here, so a fold-state
        // mismatch must not be read as a base difference. Ownedness always
        // distinguishes (`[N]T` vs `[N owned]T`). Element base recurses.
        if (a->arrayOwned != b->arrayOwned) return false;
        if (a->arraySizeValue >= 0 && b->arraySizeValue >= 0 &&
            a->arraySizeValue != b->arraySizeValue)
            return false;
        return typeArgSameBase(a->arrayElement, b->arrayElement);
    }
    if (a->kind == TypeRef::Kind::Function) {
        // Structural shape: parameter count + per-parameter modifiers + return
        // ownedness. Parameter/return TYPE identity is typeArgIdentical's job.
        if (a->funcParams.size() != b->funcParams.size()) return false;
        for (std::size_t i = 0; i < a->funcParams.size(); ++i)
            if (a->funcParams[i].modifier != b->funcParams[i].modifier)
                return false;
        return a->funcReturnOwned == b->funcReturnOwned;
    }
    return false;
}

// §12.9: recursive type-argument IDENTITY. Two arguments are identical when
// their bases match (typeArgSameBase) AND every nested generic argument is
// identical, component-wise. This is the only relation an INVARIANT position
// (a concrete generic type — Array, records, classes) or a value-typed
// argument admits (§12.9 "concrete generic types are always invariant"): it is
// what makes `Array<Dog>` distinct from `Array<Animal>`. Nullability is ignored,
// matching typeArgSameBase (variance operates on the base type).
//
// #2007: InlineArray and Function have no `genericArgs` (their identity lives
// in `arrayElement` / `funcParams` + `funcReturn`), so the genericArgs loop
// below would return true even when the element/parameter/return types differ.
// Recurse into those components directly so e.g. `Box<[3]Array<int>>` is
// distinct from `Box<[3]Array<string>>` and `Box<(int)->int>` from
// `Box<(string)->int>` (per §5.1 identity and §15.1 "size is part of the type").
static bool typeArgIdentical(const TypeRef* a, const TypeRef* b) {
    if (!a || !b) return a == b;
    if (!typeArgSameBase(a, b)) return false;
    if (a->kind == TypeRef::Kind::InlineArray)
        return typeArgIdentical(a->arrayElement, b->arrayElement);
    if (a->kind == TypeRef::Kind::Function) {
        for (std::size_t i = 0; i < a->funcParams.size(); ++i)
            if (!typeArgIdentical(a->funcParams[i].type, b->funcParams[i].type))
                return false;
        return typeArgIdentical(a->funcReturn, b->funcReturn);
    }
    if (a->genericArgs.size() != b->genericArgs.size()) return false;
    for (std::size_t i = 0; i < a->genericArgs.size(); ++i)
        if (!typeArgIdentical(a->genericArgs[i], b->genericArgs[i]))
            return false;
    return true;
}

// §12.9: does `from` reference-convert to `to` in a COVARIANT position — i.e.
// is `from` `to` or a subtype under §5.1 reference conversions, composed
// recursively through nested generic arguments? This is the single relation the
// covariant/contravariant argument checks share (contravariant just swaps the
// operands). It composes, per §12.9 ("Position is composed across nested type
// constructors"):
//   - recursive identity (always convertible; the ONLY relation an invariant
//     base admits, so `Array<Dog>` does NOT convert to `Array<Animal>`),
//   - value-typed arguments admit identity only (§12.9 L1592 — no variant
//     conversion; checked BEFORE the Object root so `IEnumerable<int>` does not
//     convert to `IEnumerable<Object>`),
//   - `Object` as the universal root reference type,
//   - a base-type reference conversion — builtin interface hierarchy walk
//     (Array<T>→…→IEnumerable<T>) or user-declared subtype chain — followed by
//     recursion into each shared type parameter per its own variance.
// Returns false on a genuine mismatch; the caller reports the §5.1 error.
static bool typeArgCovariantAssignable(
    const TypeRef* fromArg, const TypeRef* toArg,
    bool (*isSubtypeFn)(TypeSymbol*, TypeSymbol*)) {
    if (!fromArg || !toArg) return fromArg == toArg;
    // Identity (recursive) is always a legal conversion.
    if (typeArgIdentical(fromArg, toArg)) return true;
    // §12.9 L1592: a value-typed argument admits no variant conversion — only
    // identity (already ruled out). Checked before the Object root so
    // `IEnumerable<int>` is NOT covariantly assignable to `IEnumerable<Object>`.
    if (isValueTypeArg(fromArg) || isValueTypeArg(toArg)) return false;
    // `Object` (a Primitive TypeRef) is the universal root reference type.
    if (toArg->kind == TypeRef::Kind::Primitive &&
        toArg->primitive == PrimitiveType::Object)
        return true;
    if (fromArg->kind != TypeRef::Kind::Named ||
        toArg->kind != TypeRef::Kind::Named)
        return false;
    // Same base (same declaration, or same builtin name): the base is invariant
    // or variant per its own kind — recurse into arguments via
    // variantGenericAssignable, which reads the declared/hardcoded variance and
    // requires argument identity for an invariant base (e.g. Array).
    if (typeArgSameBase(fromArg, toArg))
        return variantGenericAssignable(fromArg, toArg, isSubtypeFn);
    // Different base, both builtin: `fromArg`'s base must reference-convert to
    // `toArg`'s base via the builtin interface hierarchy (e.g.
    // Array<Dog>→IEnumerable<Dog>). Find the ancestor whose base matches
    // `toArg`, then recurse into its arguments per `toArg`'s variance. No
    // matching ancestor ⇒ `fromArg` is not a subtype of `toArg` ⇒ reject.
    // (This is NOT variantGenericAssignable's conservative no-candidate accept,
    // which is safe only at the top level where the base relation is established
    // separately — at an argument position the base relation must be proven.)
    if (!fromArg->declaration && !toArg->declaration) {
        std::string_view toName = namedTypeSimpleName(toArg);
        std::vector<std::unique_ptr<TypeRef>> owned;
        for (const TypeRef* anc : builtinInterfaceAncestorsOf(fromArg, owned))
            if (namedTypeSimpleName(anc) == toName)
                return variantGenericAssignable(anc, toArg, isSubtypeFn);
        return false;
    }
    // Different base, both user-declared: the base must be a subtype. Recursing
    // into the substituted base arguments is a residual (cross-type user-declared
    // variance), conservatively accepted once the base subtype holds — matching
    // the top-level cross-declaration treatment.
    if (fromArg->declaration && toArg->declaration)
        return isSubtypeFn(fromArg->declaration, toArg->declaration);
    // Mixed builtin/user-declared base: no known relation — reject.
    return false;
}

// §11 / §12.9 / §5.1 (#1935): see header doc. A user-declared type conforms to
// a builtin interface only by nominally listing it (directly or transitively)
// among its bases; walk the full declared-base closure — resolved user bases
// (`baseTypeRefs`), the syntactic AST base list (recovers builtin bases, which
// have no TypeSymbol and so are absent from `baseTypeRefs`), and the builtin
// interface hierarchy on any builtin node (so implementing IList<T> also counts
// as implementing IEnumerable<T>) — and report whether the builtin base named
// `builtinBaseName` is reachable. A name-only walk suffices for the base-
// conformance gate; the type-argument variance on the matched base stays the
// (conservatively-accepted) residual that #1851 deliberately left.
bool ExprTC::userTypeImplementsBuiltinIface(TypeRef* from,
                                            std::string_view builtinBaseName) {
    if (builtinBaseName.empty() || !from ||
        from->kind != TypeRef::Kind::Named)
        return false;
    // Only a target whose builtin super-hierarchy is COMPLETELY modeled by
    // builtinInterfaceAncestors (the borrowing collection interfaces) permits a
    // sound "not reachable ⇒ does not implement" conclusion. For any other
    // builtin `to`, absence from the walk is not proof — keep the conservative
    // accept (#1851's discipline: reject only where the model is complete).
    if (!builtinBorrowingCollectionFullyModeled(builtinBaseName))
        return true;
    // Ancestors synthesised by builtinInterfaceAncestors are owned here so their
    // TypeRefs outlive the walk (the frontier holds raw pointers into them).
    std::vector<std::unique_ptr<TypeRef>> owned;
    std::unordered_set<std::string_view> seenName;
    std::unordered_set<const TypeRef*> seenPtr;  // empty-name nodes
    std::vector<const TypeRef*> queue{from};
    // If ANY builtin base encountered lies OUTSIDE the fully-modeled collection
    // family (e.g. IGrouping<K,T>/IOrderedEnumerable<T>, whose `: IEnumerable<T>`
    // super is not in builtinInterfaceAncestors, or an arity-2 / owning base),
    // its builtin super-closure is unknown here — so a not-found result is not
    // proof of non-conformance and we must conservatively accept.
    bool sawUnmodeledBuiltinBase = false;
    const std::size_t kMaxWalk = 8192;  // defensive bound; base sets are shallow
    std::size_t walked = 0;
    while (!queue.empty()) {
        // Defensive: on a malformed base-type cycle that survives error recovery,
        // do NOT over-reject — bail out to the conservative accept.
        if (++walked > kMaxWalk) return true;
        const TypeRef* cur = queue.back();
        queue.pop_back();
        if (!cur || cur->kind != TypeRef::Kind::Named) continue;
        std::string_view curName = namedTypeSimpleName(cur);
        bool fresh = !curName.empty() ? seenName.insert(curName).second
                                      : seenPtr.insert(cur).second;
        if (!fresh) continue;
        if (curName == builtinBaseName) return true;
        if (cur->declaration) {
            // User-declared node: walk resolved user bases plus the AST base
            // list (the only place a builtin base survives).
            for (TypeRef* b : cur->declaration->baseTypeRefs)
                queue.push_back(b);
            if (const auto* astDecl =
                    dynamic_cast<const ast::TypeDecl*>(cur->declaration->decl))
                for (const auto& baseAst : astDecl->baseTypes) {
                    if (!baseAst) continue;
                    TypeRef* br = ctx_.resolvedType(*baseAst);
                    // An UNRESOLVED base — null, or an Error/non-Named TypeRef
                    // (already reported elsewhere) — leaves the conformance
                    // closure incomplete: can't prove absence, so mark unknown
                    // and don't pile a spurious §5.1 error on the real one.
                    if (!br || br->kind != TypeRef::Kind::Named) {
                        sawUnmodeledBuiltinBase = true;
                        continue;
                    }
                    queue.push_back(br);
                }
        } else if (!curName.empty()) {
            // Builtin interface base. If it is not in the fully-modeled family,
            // its super-closure is unknown — record that so a not-found result
            // stays a conservative accept.
            if (!builtinBorrowingCollectionFullyModeled(curName))
                sawUnmodeledBuiltinBase = true;
            for (const TypeRef* anc : builtinInterfaceAncestors(
                     curName, const_cast<std::vector<TypeRef*>&>(cur->genericArgs),
                     owned))
                queue.push_back(anc);
        }
    }
    // Not reachable. Reject only when every builtin base along the way was fully
    // modeled (proof of non-conformance); otherwise conservatively accept.
    return sawUnmodeledBuiltinBase;
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
//      User-declared cross-type generics: base conformance is verified by the
//      caller (checkAssignable, via userTypeImplementsBuiltinIface, #1935) for a
//      builtin `to`; the type-argument variance on the matched base stays a
//      conservatively-accepted residual (deferred to the full §12.9 milestone).
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
                // §5.1/§12.9 (#1851): both `from` and `to` are builtin generics.
                // When `from` is a fully-modeled borrowing collection, a null
                // candidate is PROOF `to`'s base is not an ancestor of `from` —
                // `from` does not conform to `to` — so REJECT. The prior
                // fall-through to conservative-accept below was unsound at this
                // top level (no separate base relation is established for two
                // builtins), letting `Stack<Dog>` pass as `IList<Dog>`. Types
                // outside the modeled family (owning collections, arity-2 maps)
                // are not proven non-conforming here and keep conservative-accept.
                if (!candidate &&
                    builtinBorrowingCollectionFullyModeled(fromName))
                    return false;
            }
        }
        // `from` has a user declaration: user-declared → builtin. The BASE
        // conformance (does `from` implement `to`'s base interface at all) is
        // verified by the caller for the checkAssignable site
        // (userTypeImplementsBuiltinIface, #1935) — a non-conforming `from` is
        // rejected there before reaching here. What remains here is the
        // type-argument variance on the matched base, a conservatively-accepted
        // residual (would need a baseTypeRefs walk with substitution).
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
            // Covariant: fromArg must reference-convert to toArg, composed
            // recursively through nested generic arguments (§12.9 — a nested
            // invariant base such as Array requires argument identity, so
            // `IEnumerable<Array<Dog>>` does NOT admit `IEnumerable<Array<Animal>>`).
            ok = typeArgCovariantAssignable(fromArg, toArg, isSubtypeFn);
        } else if (variance == "in") {
            // Contravariant: toArg must reference-convert to fromArg (operands
            // swapped), same recursive composition.
            ok = typeArgCovariantAssignable(toArg, fromArg, isSubtypeFn);
        } else {
            // Invariant (or unknown variance): recursive argument identity —
            // NOT a shallow same-base check, which would ignore nested args and
            // treat `Array<Dog>` as identical to `Array<Animal>` (§12.9).
            ok = typeArgIdentical(fromArg, toArg);
        }

        if (!ok) return false;
    }
    return true;
}

// §19.3/§5.1/§12.9 (v0.196, #1913): see the header comment. Project `sub` up to
// its instantiation of the class-chain ancestor `ancDecl`, in `sub`'s own
// concrete generic arguments.
TypeRef* ExprTC::projectTypeToAncestorDecl(const TypeRef* sub,
                                           TypeSymbol* ancDecl) {
    if (!sub || sub->kind != TypeRef::Kind::Named || !sub->declaration ||
        !ancDecl)
        return nullptr;
    if (sub->declaration == ancDecl) return const_cast<TypeRef*>(sub);
    // `ancDecl`'s instantiation expressed in `sub`'s declaration's params — null
    // when `ancDecl` is not a class ancestor reachable on the class chain (an
    // interface ancestor, an uncomposable/arity-mismatched link).
    TypeRef* inst = leafRootInstantiation(sub->declaration, ancDecl);
    if (!inst) return nullptr;
    // Substitute `sub`'s actual generic args for its declaration's params.
    const auto& params = sub->declaration->genericParams;
    if (params.size() != sub->genericArgs.size()) return nullptr;
    std::unordered_map<std::string_view, TypeRef*> solution;
    for (std::size_t i = 0; i < params.size(); ++i)
        if (params[i]) solution[params[i]->name] = sub->genericArgs[i];
    return substituteReturnType(inst, solution);
}

// §19.3/§19.5/§5.1/§12.9 (v0.196, #1913): see the header comment. Reject a typed
// binding pattern `var x: patTy` only when NO runtime value of scrutinee type
// `scrutTy` could satisfy it — provable on ground structure via a common CLASS
// declaration whose invariant args are disjoint under §12.9.
bool ExprTC::typeTestGenericInstantiationImpossible(const TypeRef* scrutTy,
                                                    const TypeRef* patTy) {
    if (!scrutTy || !patTy) return false;
    if (scrutTy->kind != TypeRef::Kind::Named ||
        patTy->kind != TypeRef::Kind::Named)
        return false;
    TypeSymbol* sd = scrutTy->declaration;
    TypeSymbol* pd = patTy->declaration;
    if (!sd || !pd) return false;  // builtin/free type — conservative accept
    auto isClassOrIface = [](TypeSymbol* d) {
        return d->kind == ast::TypeDeclKind::Class ||
               d->kind == ast::TypeDeclKind::Interface;
    };
    if (!isClassOrIface(sd) || !isClassOrIface(pd)) return false;
    // Only a generic instantiation can be incompatible while the class tag still
    // matches; a bare (non-generic) type test is already handled elsewhere.
    if (scrutTy->genericArgs.empty() && patTy->genericArgs.empty()) return false;

    // Project the descendant side up to the common declaration so both operands
    // name the SAME declaration; then compare under §12.9 variance. The
    // projection is non-null only when the common declaration is a CLASS reached
    // on the class chain (leafRootInstantiation walks class bases), so an
    // interface common ancestor / cross-interface arg reasoning — where a value
    // could conceivably satisfy both via distinct implementations — never
    // produces a rejection.
    const TypeRef* a = nullptr;  // scrutinee side, at the common declaration
    const TypeRef* b = nullptr;  // pattern side, at the common declaration
    if (sd == pd) {
        // Same declaration: a value's runtime class carries exactly one
        // instantiation of a CLASS (single inheritance), so disjoint invariant
        // args are provably impossible. Restrict to a class base to avoid the
        // (unsettled) interface-implemented-twice hazard — conservative accept
        // for a same-interface pair.
        if (sd->kind != ast::TypeDeclKind::Class) return false;
        a = scrutTy;
        b = patTy;
    } else if (isSubtype(pd, sd)) {  // pattern's decl is a CLASS-chain subtype
        a = scrutTy;
        b = projectTypeToAncestorDecl(patTy, sd);
    } else if (isSubtype(sd, pd)) {  // scrutinee's decl is a CLASS-chain subtype
        a = projectTypeToAncestorDecl(scrutTy, pd);
        b = patTy;
    } else {
        return false;  // unrelated declarations — out of scope, no new rejection
    }
    if (!a || !b) return false;  // uncomposable projection — conservative accept

    // Admissible iff some value common to both static types could exist: `a`
    // reference-converts to `b`, or `b` to `a`, under §12.9 (covariance/
    // contravariance handled by variantGenericAssignable; a rigid/free type
    // parameter is treated as compatible, so a rigid scrutinee is never
    // over-rejected — a type pattern is tested per instantiation, §19.3 v0.182).
    if (variantGenericAssignable(a, b, isSubtype) ||
        variantGenericAssignable(b, a, isSubtype))
        return false;
    return true;  // provably incompatible instantiation — reject
}

// §12.4 (#1920): is `d` a FINAL class — plain `sealed`, non-`abstract`? Such a
// class admits no subclass anywhere (`sealed` prevents inheritance; only the
// `sealed abstract` combination stays inheritable within its module, §11.7), so
// its implemented-interface set is exhaustive over all runtime values that pass
// the erased class-tag type-test.
static bool isFinalClass(const TypeSymbol* d) {
    if (!d || d->kind != ast::TypeDeclKind::Class) return false;
    const auto* td = dynamic_cast<const ast::TypeDecl*>(d->decl);
    if (!td) return false;
    bool sealed = false, abstract = false;
    for (std::string_view m : td->modifiers) {
        if (m == "sealed") sealed = true;
        else if (m == "abstract") abstract = true;
    }
    return sealed && !abstract;
}

// §19.3/§5.1/§12.9 (v0.197, #1920): see the header comment. BFS the ancestor
// graph of `classTy`, substituting the carried concrete args at each level, and
// collect every instantiation of `ifaceDecl`.
void ExprTC::collectAncestorInstantiationsOf(const TypeRef* classTy,
                                             TypeSymbol* ifaceDecl,
                                             std::vector<TypeRef*>& out) {
    if (!classTy || classTy->kind != TypeRef::Kind::Named ||
        !classTy->declaration || !ifaceDecl)
        return;
    std::vector<TypeRef*> work{const_cast<TypeRef*>(classTy)};
    std::unordered_set<const TypeSymbol*> seenNonGeneric;
    std::vector<TypeRef*> seenGeneric;
    const std::size_t kMaxWalk = 4096;  // defensive; ancestor sets are shallow
    std::size_t walked = 0;
    while (!work.empty()) {
        if (++walked > kMaxWalk) break;  // malformed cycle must terminate
        TypeRef* cur = work.back();
        work.pop_back();
        if (!cur || cur->kind != TypeRef::Kind::Named || !cur->declaration)
            continue;
        TypeSymbol* d = cur->declaration;
        // Bound diamonds WITHOUT merging distinct instantiations of a repeated
        // declaration (the implemented-twice shape): a non-generic ancestor
        // dedups by declaration, a generic one by structural identity.
        if (cur->genericArgs.empty()) {
            if (!seenNonGeneric.insert(d).second) continue;
        } else {
            bool dup = false;
            for (TypeRef* s : seenGeneric)
                if (typeRefEqual(s, cur)) { dup = true; break; }
            if (dup) continue;
            seenGeneric.push_back(cur);
        }
        if (d == ifaceDecl) out.push_back(cur);
        // Re-express each declared base (single class base + interfaces) in
        // `cur`'s concrete args and enqueue it.
        std::unordered_map<std::string_view, TypeRef*> subst;
        const auto& params = d->genericParams;
        if (params.size() == cur->genericArgs.size())
            for (std::size_t i = 0; i < params.size(); ++i)
                if (params[i]) subst[params[i]->name] = cur->genericArgs[i];
        for (TypeRef* br : d->baseTypeRefs)
            if (br)
                work.push_back(subst.empty() ? br
                                             : substituteReturnType(br, subst));
    }
}

// §19.3/§19.5/§5.1/§12.9 (v0.197, #1920): see the header comment. The
// interface-scrutinee counterpart of typeTestGenericInstantiationImpossible,
// reasoning over the pattern class's conforming interface instantiations rather
// than a single class-chain projection.
bool ExprTC::typeTestInterfaceInstantiationImpossible(const TypeRef* scrutTy,
                                                      const TypeRef* patTy) {
    if (!scrutTy || !patTy) return false;
    if (scrutTy->kind != TypeRef::Kind::Named ||
        patTy->kind != TypeRef::Kind::Named)
        return false;
    TypeSymbol* sd = scrutTy->declaration;
    TypeSymbol* pd = patTy->declaration;
    if (!sd || !pd) return false;  // builtin/free type — conservative accept
    // Scrutinee must be a GENERIC INTERFACE — this is the interface counterpart
    // of the #1913 class check (a class scrutinee is handled there via the
    // class-chain projection). A non-generic interface carries no instantiation
    // to mismatch.
    if (sd->kind != ast::TypeDeclKind::Interface || scrutTy->genericArgs.empty())
        return false;
    // Pattern must be a CLASS: the erased type-test binds the value at the
    // class's static instantiation. (An interface pattern would need the
    // implemented-twice reasoning on BOTH sides — a residual, conservative
    // accept here.)
    if (pd->kind != ast::TypeDeclKind::Class) return false;
    // SOUND SUBSET (#1920): restrict to a FINAL class — no subclass can exist
    // (§12.4), so its implemented-interface set is exhaustive over every value
    // that could pass the erased class-tag test, and no subclass can add a
    // compatible implementation at a different instantiation (the
    // implemented-twice hazard). For a non-final pattern class a subclass
    // deriving from `patTy` could add a compatible `sd` implementation, so we
    // conservatively ACCEPT (residual — tracked for the clash-reasoning
    // extension).
    if (!isFinalClass(pd)) return false;

    // Every instantiation of the scrutinee interface `sd` that the FINAL pattern
    // class implements, in `patTy`'s own concrete args. The implemented-twice
    // shape (`C<T>: I<T>, I<int>`) contributes multiple entries — a value that
    // satisfies ANY compatible one keeps the pattern admissible.
    std::vector<TypeRef*> impls;
    collectAncestorInstantiationsOf(patTy, sd, impls);
    if (impls.empty())
        return false;  // patTy does not implement `sd` at all — not a
                       // wrong-INSTANTIATION case; conservative accept.

    // Admissible iff SOME implemented instantiation reference-converts to/from
    // the scrutinee under §12.9 (variantGenericAssignable in both directions,
    // matching the #1913 posture — a rigid/free parameter is treated as
    // compatible, so a rigid scrutinee or pattern arg is never over-rejected).
    // Provably impossible iff NONE does: a FINAL class means no subclass can
    // supply another instantiation, so no value of the scrutinee type can be a
    // `patTy`, and the erased test would bind at a wrong static type.
    for (TypeRef* impl : impls)
        if (variantGenericAssignable(impl, scrutTy, isSubtype) ||
            variantGenericAssignable(scrutTy, impl, isSubtype))
            return false;  // a compatible implementation exists — admit
    return true;  // every implemented instantiation is incompatible — reject
}

// §14.1 (#1927): does `t` mention a free/rigid type variable anywhere — a Named
// TypeRef with no declaration and a non-empty head (a generic type or method
// parameter, the same shape variantGenericAssignable and matchLeafInvariant
// treat as a solve/free variable)? Used to hold #1927's uniqueness check off any
// scrutinee or pattern carrying a rigid parameter, whose per-instantiation match
// is undecidable at the declaration (§19.3 v0.182 / §19.6 rigid identity).
static bool refMentionsTypeVariable(const TypeRef* t) {
    if (!t) return false;
    switch (t->kind) {
        case TypeRef::Kind::Named:
            if (!t->declaration && t->genericArgs.empty() &&
                !namedTypeSimpleName(t).empty())
                return true;
            for (const TypeRef* a : t->genericArgs)
                if (refMentionsTypeVariable(a)) return true;
            return false;
        case TypeRef::Kind::InlineArray:
            return refMentionsTypeVariable(t->arrayElement);
        case TypeRef::Kind::Function:
            for (const auto& p : t->funcParams)
                if (refMentionsTypeVariable(p.type)) return true;
            return refMentionsTypeVariable(t->funcReturn);
        default:
            return false;
    }
}

// §12.2/§19.3 (#1942): does `t` mention any of `names` as a bare type-variable
// occurrence (Named, no declaration, no args, head ∈ names)? The set-restricted
// sibling of refMentionsTypeVariable above, used by the clash-protection check.
static bool refMentionsAnyOf(const TypeRef* t,
                             const std::unordered_set<std::string_view>& names) {
    if (!t || names.empty()) return false;
    switch (t->kind) {
        case TypeRef::Kind::Named:
            if (!t->declaration && t->genericArgs.empty() &&
                names.count(namedTypeSimpleName(t)))
                return true;
            for (const TypeRef* a : t->genericArgs)
                if (refMentionsAnyOf(a, names)) return true;
            return false;
        case TypeRef::Kind::InlineArray:
            return refMentionsAnyOf(t->arrayElement, names);
        case TypeRef::Kind::Function:
            for (const auto& p : t->funcParams)
                if (refMentionsAnyOf(p.type, names)) return true;
            return refMentionsAnyOf(t->funcReturn, names);
        default:
            return false;
    }
}

// §12.2 (#1942): is the interface `itd` CLASH-PROTECTED — does every generic
// parameter appear in the RESULT position (method return / property or indexer
// type) of some direct member whose (index) parameters mention NO generic
// parameter of the interface? If so, a class cannot implement the interface at
// TWO instantiations differing in any parameter subset S: pick P ∈ S — P's
// protecting member has an identical substituted parameter list in both
// instantiations but a differing member type, a §12.2 hiding clash (verified in
// #1942's record: `IMarker<T>` with `Get(): T` makes `class D : C<string>,
// IMarker<int>` a compile error), so the second implementation is rejected at
// its own declaration and the open-world subclass hole is closed.
//
// Conservative in the SOUND direction (a false "unprotected" only over-rejects):
// only DIRECT instance methods/properties/indexers are credited (no inherited-
// interface members — their substitution mapping is not chased here; no statics
// or events), and a member declaring its OWN generic parameters is skipped
// (name shadowing would make the mention test ambiguous).
static bool ifaceClashProtectedAllParams(const ast::TypeDecl* itd,
                                         SemaContext& ctx) {
    if (!itd) return false;
    std::unordered_set<std::string_view> allParams;
    for (const auto& gp : itd->genericParams) allParams.insert(gp.name);
    if (allParams.empty()) return false;
    auto isStaticMember = [](const std::vector<std::string_view>& mods) {
        for (std::string_view m : mods)
            if (m == "static") return true;
        return false;
    };
    for (const auto& gp : itd->genericParams) {
        std::unordered_set<std::string_view> one{gp.name};
        bool pinned = false;
        for (const auto& mem : itd->members) {
            if (!mem) continue;
            if (const auto* md =
                    dynamic_cast<const ast::MethodDecl*>(mem.get())) {
                if (!md->genericParams.empty() || isStaticMember(md->modifiers))
                    continue;
                if (!md->returnType) continue;
                const TypeRef* rt = ctx.resolvedType(*md->returnType);
                if (!rt || !refMentionsAnyOf(rt, one)) continue;
                bool paramsClean = true;
                for (const auto& p : md->params) {
                    const TypeRef* pt =
                        p.type ? ctx.resolvedType(*p.type) : nullptr;
                    if (!pt || refMentionsAnyOf(pt, allParams)) {
                        paramsClean = false;
                        break;
                    }
                }
                if (paramsClean) { pinned = true; break; }
            } else if (const auto* pdcl =
                           dynamic_cast<const ast::PropertyDecl*>(mem.get())) {
                if (isStaticMember(pdcl->modifiers)) continue;
                if (!pdcl->type) continue;
                const TypeRef* rt = ctx.resolvedType(*pdcl->type);
                if (!rt || !refMentionsAnyOf(rt, one)) continue;
                bool paramsClean = true;
                if (pdcl->isIndexer) {
                    for (const auto& p : pdcl->indexerParams) {
                        const TypeRef* pt =
                            p.type ? ctx.resolvedType(*p.type) : nullptr;
                        if (!pt || refMentionsAnyOf(pt, allParams)) {
                            paramsClean = false;
                            break;
                        }
                    }
                }
                if (paramsClean) { pinned = true; break; }
            }
        }
        if (!pinned) return false;
    }
    return true;
}

// §19.3/§19.5/§5.1/§12.9 (v0.197, #1927; narrowed/refined #1946): admission of a
// typed binding `var x: C<A>` (a class pattern) against a generic-INTERFACE
// scrutinee `I<B>`. Three-way result (see the header):
//
//   Unreachable    — `C<A>` provably cannot conform to `I<B>` (`Box<string>` at
//                    `IBox<int>` where `Box<T>: IBox<T>`): no value of the
//                    scrutinee type is ever a `C<A>` — a DEAD arm (#1920's
//                    non-final / generic counterpart).
//   ErasedAmbiguous— `C<A>` conforms, but its differing type argument is a
//                    still-ERASED kind (#1939: a function type, inline array, …)
//                    AND another instantiation of `C` also conforms. Post-#1843
//                    the runtime `is C<A>` is precise ONLY when every arg carries
//                    a runtime identity token; an erased arg makes the descriptor
//                    collapse siblings, so the test would bind a `C<Q≠A>` value at
//                    `C<A>` — the OLD #1927 uniqueness rejection, still needed here.
//   None           — admit.
//
// HISTORY (#1946): as landed under #1927 this check rejected EVERY non-unique
// conformer, on the premise that the runtime type-test ERASES generic arguments.
// #1843 (per-monomorphization RTTI descriptors) made `is C<A>` PRECISE *for a
// reified type argument* — it then matches only genuine `C<A>` values, so a live
// implemented-twice / free-parameter arm with a CONCRETE differing argument is
// sound and must be admitted (the over-rejection #1946 removed). But #1843 does
// NOT reify a #1939 kind (function type, inline array, …): the descriptor key
// (appendDescKey) collapses those, so for such a differing argument the sibling
// still mis-binds and the old uniqueness rejection must stand. Hence the reified-
// vs-erased split below.
ExprTC::IfaceBindReject ExprTC::typeTestInterfaceBindAmbiguous(
    const TypeRef* scrutTy, const TypeRef* patTy) {
    if (!scrutTy || !patTy) return IfaceBindReject::None;
    if (scrutTy->kind != TypeRef::Kind::Named ||
        patTy->kind != TypeRef::Kind::Named)
        return IfaceBindReject::None;
    TypeSymbol* sd = scrutTy->declaration;
    TypeSymbol* pd = patTy->declaration;
    if (!sd || !pd) return IfaceBindReject::None;  // builtin/free — accept
    // Scrutinee: a GENERIC INTERFACE (the interface counterpart of #1913's class
    // check). Pattern: a GENERIC CLASS — a non-generic class pattern has no
    // argument to be left undetermined (any value that passes `is C` IS a `C`),
    // so its only failure mode is unreachability, which the #1920 reachability
    // check already handles.
    if (sd->kind != ast::TypeDeclKind::Interface || scrutTy->genericArgs.empty())
        return IfaceBindReject::None;
    if (pd->kind != ast::TypeDeclKind::Class || patTy->genericArgs.empty())
        return IfaceBindReject::None;
    if (pd->genericParams.size() != patTy->genericArgs.size())
        return IfaceBindReject::None;

    // A rigid/free type parameter in either operand makes the per-instantiation
    // match undecidable AT THE DECLARATION (§19.3 v0.182 — a type pattern is
    // tested per instantiation; §19.6's rigid three-valued identity). Conservative
    // accept — never over-reject a pattern admissible at some instantiation
    // (matches #1920's rigid posture).
    if (refMentionsTypeVariable(scrutTy) || refMentionsTypeVariable(patTy))
        return IfaceBindReject::None;

    // Variance of the scrutinee interface. A variant (`in`/`out`) parameter
    // admits a subtype/supertype RANGE of conforming class instantiations
    // (§12.9), so the invariant uniqueness reasoning below does not apply to
    // it. Variance is read from the interface declaration's GenericParam
    // annotations, mirroring variantGenericAssignable; when unavailable (an
    // imported interface with no AST), conservatively treat as possibly-variant
    // and accept (the pre-#1941 posture, kept for the no-AST case).
    const auto* itd = dynamic_cast<const ast::TypeDecl*>(sd->decl);
    if (!itd) return IfaceBindReject::None;
    bool variantIface = false;
    for (const auto& gp : itd->genericParams)
        if (gp.variance == "in" || gp.variance == "out") {
            variantIface = true;
            break;
        }

    // Split on runtime PRECISION (post-#1843). `is C<A>` is precise iff EVERY type
    // argument of `A` is runtime-reified (a concrete class/record/enum/primitive
    // all the way down — descArgsConcrete / typeRefIdentityToken). The two regimes
    // need DIFFERENT reasoning, and — importantly — `variantGenericAssignable`
    // cannot reliably compare a still-erased kind (a function type), so the
    // concrete-conformance path below is used ONLY for the reified regime.
    bool allReified = true;
    for (const TypeRef* a : patTy->genericArgs)
        if (!typeArgRuntimeReified(a)) { allReified = false; break; }

    // §19.3/§12.9/§14.2 (#1941): a VARIANT scrutinee interface.
    //
    //   * All-REIFIED pattern arguments: the post-#1843 runtime test matches
    //     only genuine `C<A>` values (verified: a `Box<Animal>` behind a
    //     covariant `IRead<Animal>` does NOT bind at a `Box<Dog>` arm), so the
    //     variant arm is sound — admit, preserving the #1920/#1927 covariant
    //     accepts. (The dead-arm refinement for variant scrutinees would need
    //     variance-aware conformance; unreachability stays #1913/#1920's job.)
    //
    //   * A still-ERASED argument (a function type — appendDescKey collapses
    //     Kind::Function, §14.2 mints no identity token for it): the runtime
    //     test degrades to the shared class tag, and §12.9's variance range
    //     admits sibling instantiations of `C` as values of the scrutinee
    //     type, so the arm would bind a `C<Q≠A>` value at `C<A>` — the
    //     #2001/#2006 memory-unsafe type-confusion class (verified live: a
    //     `Prod<(string)->string>` behind `ISrc<(string)->string>` bound at
    //     `Prod<(int)->int>`). The invariant-regime uniqueness proof below is
    //     unavailable through a variant parameter (matchLeafInvariant reasons
    //     invariantly), so REJECT — except a provably DEAD arm: a `sealed`
    //     pattern class that implements the scrutinee interface nowhere has no
    //     conforming value at all (structural, reliable even through erased
    //     args), which is #1920's unreachability domain, not an erasure
    //     ambiguity.
    if (variantIface) {
        if (allReified) return IfaceBindReject::None;
        TypeRef symbolicSelfV;
        symbolicSelfV.kind = TypeRef::Kind::Named;
        symbolicSelfV.declaration = pd;
        std::vector<TypeRef*> symImplsV;
        collectAncestorInstantiationsOf(&symbolicSelfV, sd, symImplsV);
        bool sealedPd = false;
        for (std::string_view m : pd->modifiers)
            if (m == "sealed") { sealedPd = true; break; }
        if (symImplsV.empty() && sealedPd) return IfaceBindReject::None;
        return IfaceBindReject::ErasedAmbiguous;
    }

    if (allReified) {
        // (A) REIFIED. The runtime test is PRECISE: it matches only genuine `C<A>`
        // values, so an implemented-twice / free-parameter sibling `C<Q≠A>` is
        // distinguished and never mis-binds. So the ONLY unsound case is a DEAD
        // arm — `C<A>` cannot conform to `I<B>`, unreachable. Reason over the
        // CONCRETE `patTy`: its implemented instantiations of `I` are exactly the
        // interface instantiations a `C<A>` value carries; if NONE reference-
        // converts to/from the scrutinee (§12.9) the arm is unreachable.
        // (`Box<string>` at `IBox<int>` — implements only `IBox<string>`.) A
        // conforming instantiation (incl. the implemented-twice `C<string>` via a
        // ground `IMarker<int>`, and `Pair<int, string>`) is ADMITTED.
        std::vector<TypeRef*> concImpls;
        collectAncestorInstantiationsOf(patTy, sd, concImpls);
        if (concImpls.empty())
            return IfaceBindReject::None;  // doesn't implement `I` — #1920's job.
        for (TypeRef* impl : concImpls)
            if (variantGenericAssignable(impl, scrutTy, isSubtype) ||
                variantGenericAssignable(scrutTy, impl, isSubtype))
                return IfaceBindReject::None;  // conforms — LIVE + precise, admit.
        return IfaceBindReject::Unreachable;   // DEAD arm.
    }

    // (B) ERASED. Some type argument of `A` is a still-erased #1939 kind (function
    // type, inline array, …): the descriptor collapses it, so `is C<A>` degrades
    // to a test that also matches a sibling `C<Q≠A>`. Keep EXACTLY the OLD #1927
    // uniqueness rejection here (variantGenericAssignable is unreliable through an
    // erased arg, so do NOT use the concrete-conformance path) — reject iff a
    // PROVABLE second conformer exists (a GROUND/free implemented instantiation,
    // or one whose invariant match pins a parameter to a value other than `A`),
    // else admit (the unique conformer, or a harmless unreachable arm — as base
    // did). Reason over the SYMBOLIC impls (in `C`'s own parameters) so an
    // unmentioned/ground parameter shows as free.
    TypeRef symbolicSelf;
    symbolicSelf.kind = TypeRef::Kind::Named;
    symbolicSelf.declaration = pd;
    std::vector<TypeRef*> symImpls;
    collectAncestorInstantiationsOf(&symbolicSelf, sd, symImpls);
    std::unordered_set<std::string_view> cParams;
    for (const Symbol* gp : pd->genericParams)
        if (gp) cParams.insert(gp->name);
    bool uniqueConformer = false;
    for (TypeRef* impl : symImpls) {
        std::unordered_map<std::string_view, TypeRef*> solved;
        std::unordered_set<std::string_view> splitParams;
        InvMatch m =
            matchLeafInvariant(impl, scrutTy, cParams, solved, ctx_, splitParams);
        if (m != InvMatch::Ok)
            continue;  // Fail: never matches `I<B>`; Unknown: undecidable.
        // (a) FREE parameter — a sibling varying it also conforms.
        for (const Symbol* gp : pd->genericParams)
            if (gp && !solved.count(gp->name))
                return IfaceBindReject::ErasedAmbiguous;
        // (b) DISTINCT PIN — the impl conforms only at a different argument, i.e.
        // a `C<Q≠A>` sibling conforms via it.
        for (std::size_t i = 0; i < pd->genericParams.size(); ++i) {
            const Symbol* gp = pd->genericParams[i];
            if (!gp) continue;
            auto it = solved.find(gp->name);
            TypeRef* pinned = (it == solved.end()) ? nullptr : it->second;
            TypeRef* arg = patTy->genericArgs[i];
            if (pinned && arg && isFullyGroundTypeRef(pinned) &&
                isFullyGroundTypeRef(arg) && !typeRefEqual(pinned, arg))
                return IfaceBindReject::ErasedAmbiguous;
        }
        uniqueConformer = true;  // conforms, pinned exactly at the pattern args.
    }
    // §19.3/§12.2 (#1942): the unique-conformer (and never-conforms dead-arm)
    // admit above is CLOSED-WORLD — it reasons over same-class instantiations
    // only. For a NON-final pattern class a subclass `D : C<Q≠A>, I<A>` in this
    // or another module is a `C<Q≠A>` value that IS an `I<A>`, passes the
    // ERASED `is C` class-tag test (the erased argument mints no identity
    // token), and binds at `C<A>` — a wrong static type. Such a `D` is legal
    // exactly when no §12.2 member clash forbids implementing `I` twice, so
    // #1942's recorded direction admits a non-final class when the UNIQUE
    // conformer was proven AND the interface is clash-protected (every
    // parameter pinned by a result-position member — a second implementation
    // would then be rejected at `D`'s declaration). With NO conforming impl,
    // protection cannot help: a subclass would implement `I` only ONCE
    // (nothing to clash with), so only `sealed` admits — the arm is then
    // provably dead (#1920's domain).
    //
    // #2186 UNBLOCKED: the clash-protection admit is now ENABLED. The premise
    // — that a subclass implementing the interface at a second instantiation
    // must §12.2-clash on a same-named member — now holds because the §12.5
    // conformance check (TypeResolution_Checks.cpp) verifies the SUBSTITUTED
    // signature, so `class D : K<(string)->string>, IM<(int)->int>` with no
    // new members is correctly REJECTED (the inherited `Get(): ((string)-
    // >string)?` does not satisfy `IM<(int)->int>`'s `Get(): ((int)->int)?`).
    // Pre-#2186 the conformance check satisfied abstract members by NAME+arity
    // only, so D was wrongly accepted and the §12.2 clash the admit relies on
    // never fired (probe exploit_1942.bzl bound a `K<(string)->string>` value
    // at `K<(int)->int>` through this admit). Sealed-only was the hold-off.
    {
        bool sealedPd = false;
        for (std::string_view m : pd->modifiers)
            if (m == "sealed") { sealedPd = true; break; }
        constexpr bool kClashProtectionAdmitEnabled = true;  // #2186 unblocked
        if (!sealedPd &&
            !(kClashProtectionAdmitEnabled && uniqueConformer &&
              ifaceClashProtectedAllParams(itd, ctx_)))
            return IfaceBindReject::ErasedAmbiguous;
    }
    return IfaceBindReject::None;  // erased but the UNIQUE conformer — sound.
}

// §19.3/§14.1 (#1946): mirror of codegen typeRefIdentityToken/descArgsConcrete
// (LlvmBackend_impl.hpp / LlvmBackend_Setup.cpp). A type argument is runtime-
// reified — the per-monomorphization descriptor (#1843) distinguishes it from a
// sibling — iff it is a concrete Named type (with a resolved declaration), a
// primitive, or a FOLDED-size inline array whose element is itself reified,
// RECURSIVELY through generic/element arguments. A function type, an
// unfolded-size inline array, Unit/Error, or an unresolved bare parameter
// carries no identity token, so the descriptor key (appendDescKey) collapses it
// (`Kind::Function` →"F" at any depth): NOT distinguishable.
//
// #2001/#1971: the inline-array case was formerly a blanket non-reified (erased)
// — necessary ONLY while appendDescKey omitted the size N, which made every
// `[N]T` share one descriptor (a memory-unsafe collision). Now that the key
// encodes N (arraySizeValue), a folded inline array IS runtime-distinguishable,
// so admitting the now-sound type-tests here resolves #1971's over-reject. An
// UNFOLDED size (arraySizeValue < 0) stays conservative: its key still collapses,
// so it must be treated as erased (never under-rejected).
// Hoisted to a free `bezel::sema::typeArgRuntimeReified` (declared in
// TypeResolution.hpp) so codegen's #2069 collapse guard shares the SAME strict
// predicate (its recursion into declared-Named args is what descTypeArgConcrete
// lacks — the nested `is Wrap<Inner<T>>` collapse, #2184). The ExprTC member
// below forwards here so no sema call site changes.
bool typeArgRuntimeReified(const TypeRef* t) {
    if (!t) return false;
    switch (t->kind) {
        case TypeRef::Kind::Named:
            if (!t->declaration) {
                // #2022 (§12.9, §14.2, §17.4, v0.197): a null-DECLARATION builtin
                // generic (Array<T>, Dictionary<K,V>, … — declaration null per
                // §2.5/§29.3) with NON-EMPTY recursively-reified genericArgs IS
                // runtime-reified: appendDescKey (the descriptor-key factory)
                // recurses into the args per #2013, and the codegen gate
                // (descTypeArgConcrete / descArgsConcrete) now admits it, so the
                // runtime mints a precise per-monomorphization descriptor for
                // Wrap<Array<int>> vs Wrap<Array<string>> (distinct descriptor
                // globals, so `is`/`as` distinguishes them). A bare parameter
                // placeholder (null-decl + no args) stays erased — it collapses
                // onto a single key. The arity split mirrors
                // refMentionsTypeVariable / typeTestTargetMentionsTypeVariable,
                // which already treat a null-decl-no-args Named as a type-
                // variable mention distinct from a constructed null-decl builtin.
                if (t->genericArgs.empty()) return false;
                for (const TypeRef* a : t->genericArgs)
                    if (!typeArgRuntimeReified(a)) return false;
                return true;
            }
            for (const TypeRef* a : t->genericArgs)
                if (!typeArgRuntimeReified(a)) return false;
            return true;
        case TypeRef::Kind::Primitive:
            return true;
        case TypeRef::Kind::InlineArray:
            // #2001/#1971: reified iff the size is folded (encoded in the key)
            // AND the element is itself reified; an unfolded size stays erased.
            if (t->arraySizeValue < 0) return false;
            return typeArgRuntimeReified(t->arrayElement);
        default:
            return false;  // Function / Unit / Error — erased.
    }
}

// Forwarding alias (#2069): the hoisted free function above is the single source
// of truth; existing sema call sites keep calling the ExprTC member unchanged.
bool ExprTC::typeArgRuntimeReified(const TypeRef* t) {
    return bezel::sema::typeArgRuntimeReified(t);
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
    // §26.2/§26.5 (#1308, #1314): `Object` (a Primitive TypeRef) is the implicit
    // base of EVERY type — spec §2 "every type derives from Object", §12 "if no
    // base class is specified, the implicit base is Object". A receiver's
    // `baseTypeRefs` chain does NOT materialise this implicit Object base, so
    // the walk below never reaches Object for a class/interface receiver whose
    // declared bases are empty (or don't transitively list Object). Treat Object
    // as a matching (least-specific, §26.5.1 rule 1) ancestor of any reference-
    // OR value-typed receiver so `extension Object` methods resolve for:
    //   (a) reference-typed (class/interface) — no boxing needed
    //   (b) value-typed (record/enum/primitive) — codegen must box the value to
    //       an Object reference before the call (§8.6 / #1314 follow-up).
    if (target->kind == TypeRef::Kind::Primitive &&
        target->primitive == PrimitiveType::Object) {
        if (recv->kind == TypeRef::Kind::Named && recv->declaration) {
            auto k = recv->declaration->kind;
            if (k == ast::TypeDeclKind::Class ||
                k == ast::TypeDeclKind::Interface ||
                k == ast::TypeDeclKind::Record ||
                k == ast::TypeDeclKind::Enum)
                return target;
        } else if (recv->kind == TypeRef::Kind::Primitive) {
            // Value-typed primitives (numeric, bool, char) are not string/Object;
            // they box to Object (§8.6). The receiver's `extensionThisType` will
            // return ptrTy() — and codegen boxes before the call.
            using P = PrimitiveType;
            switch (recv->primitive) {
                case P::Bool: case P::Byte: case P::SByte: case P::Short:
                case P::UShort: case P::Int: case P::UInt: case P::Long:
                case P::ULong: case P::Float: case P::Double: case P::Decimal:
                case P::Char:
                    return target;
                default: break;  // String/Object/Unit/Function — fall through
            }
        }
    }
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

// §26.5 rule 4 (#1307): a printable spelling of a receiver-substituted (or
// declared) extension head from its resolved TypeRef — e.g. "IEnumerable<int>"
// for a substituted head, or "IEnumerable<T>" for the declared head of a
// generic block (a free type variable's TypeRef is a null-declaration Named
// ref whose name IS "T", per namedTypeSimpleName). Extension targets are
// always Named or a Primitive keyword (§26.2 admits classes, interfaces,
// records, enums, and primitives only — never an inline-array or function
// type), so those two TypeRef kinds are all this needs to cover.
static std::string spellExtHeadTypeRef(const TypeRef* t) {
    if (!t) return "?";
    if (t->kind == TypeRef::Kind::Primitive) {
        using P = PrimitiveType;
        switch (t->primitive) {
            case P::Bool:    return "bool";
            case P::Char:    return "char";
            case P::String:  return "string";
            case P::Byte:    return "byte";
            case P::SByte:   return "sbyte";
            case P::Short:   return "short";
            case P::UShort:  return "ushort";
            case P::Int:     return "int";
            case P::UInt:    return "uint";
            case P::Long:    return "long";
            case P::ULong:   return "ulong";
            case P::Float:   return "float";
            case P::Double:  return "double";
            case P::Decimal: return "decimal";
            case P::Object:  return "Object";
            default:         return "?";
        }
    }
    if (t->kind == TypeRef::Kind::Named) {
        std::string s(namedTypeSimpleName(t));
        if (s.empty()) s = "?";
        if (!t->genericArgs.empty()) {
            s += "<";
            for (std::size_t i = 0; i < t->genericArgs.size(); ++i) {
                if (i) s += ", ";
                s += spellExtHeadTypeRef(t->genericArgs[i]);
            }
            s += ">";
        }
        return s;
    }
    return "?";
}

// §26.5 rule 4 (#1307): a printable spelling of an AST type node, used for the
// `where` clause of a rule-4 ambiguity candidate. Read directly from the AST
// (ExtensionDecl::genericConstraints) rather than a resolved TypeRef, because
// an imported extension's synthesized AST (ModuleLoader.cpp Pass 4) never
// reconstructs genericConstraints — module metadata does not currently
// serialize a block's `where` clause — so spellExtWhereClause below
// intentionally returns "" for an imported candidate rather than fabricating
// text; see the diagnostic-site comment for the recorded follow-up.
static std::string spellExtWhereAstType(const ast::Type& t) {
    std::string s;
    if (t.name)
        for (std::size_t i = 0; i < t.name->parts.size(); ++i) {
            if (i) s += ".";
            s += std::string(t.name->parts[i]);
        }
    if (s.empty()) s = "?";
    if (!t.genericArgs.empty()) {
        s += "<";
        for (std::size_t i = 0; i < t.genericArgs.size(); ++i) {
            if (i) s += ", ";
            s += spellExtWhereAstType(*t.genericArgs[i]);
        }
        s += ">";
    }
    if (t.nullable) s += "?";
    return s;
}

static std::string spellExtWhereClause(
    const std::vector<ast::GenericConstraint>& gcs) {
    std::string out;
    for (std::size_t gi = 0; gi < gcs.size(); ++gi) {
        const auto& gc = gcs[gi];
        if (gi) out += " ";
        out += "where " + std::string(gc.typeParam) + ": ";
        if (!gc.modifierConstraint.empty()) {
            out += std::string(gc.modifierConstraint);
            continue;
        }
        for (std::size_t ii = 0; ii < gc.items.size(); ++ii) {
            if (ii) out += ", ";
            const auto& item = gc.items[ii];
            if (item.kind == "type" && item.typeArg)
                out += spellExtWhereAstType(*item.typeArg);
            else
                out += std::string(item.kind);
        }
    }
    return out;
}

Symbol* ExprTC::lookupExtensionMember(
    const TypeRef* receiver,
    std::string_view name,
    std::vector<std::pair<std::string, TypeRef*>>* outBinding,
    SourceLocation loc,
    bool reportAmbiguity) {
    if (!receiver) return nullptr;
    // §26.5 step 2: "the compiler searches the current module AND all imported
    // modules for extension blocks on x's type". The candidate set is this
    // file's own module children plus the blocks reconstructed from each
    // `import`ed dependency artifact (#735) — before that carriage existed, a
    // cross-module extension call reported a spurious "no member" reject.
    std::vector<ExtensionSymbol*> candidates;
    if (ctx_.module()) {
        for (Symbol* child : ctx_.module()->children)
            if (auto* e = dynamic_cast<ExtensionSymbol*>(child))
                candidates.push_back(e);
    }
    candidates.insert(candidates.end(), importedExtensions_.begin(),
                      importedExtensions_.end());
    // §26.5.1 rule 1 / §26.5 rule 4 (#1307): collect every APPLICABLE
    // candidate — a name-matching member from a block whose target
    // structurally matches the receiver — instead of returning on the first
    // match, so the full candidate set can be ranked by subtype specificity
    // below rather than resolved by declaration order (the bug this fixes:
    // the compiler previously took whichever block's ancestor-match loop
    // reached first, silently re-routing a call whenever declaration order or
    // imports changed). `ownedAncs` is function-scoped (not per-iteration) so
    // every candidate's substituted-head TypeRef stays alive through the
    // ranking step below — a synthesized builtin ancestor (e.g. the
    // `IEnumerable<int>` wrapper matched for an `Array<int>` receiver) is
    // owned here, not by the receiver's own tree.
    struct ExtCandidate {
        Symbol* member = nullptr;
        const TypeRef* substHead = nullptr;  // receiver-substituted head, §26.5.1
        const TypeRef* declaredHead = nullptr;  // unsubstituted declared head
        ExtensionSymbol* ext = nullptr;
        const ast::ExtensionDecl* decl = nullptr;
        std::vector<std::pair<std::string, TypeRef*>> binding;
    };
    std::vector<ExtCandidate> found;
    std::vector<std::unique_ptr<TypeRef>> ownedAncs;
    for (ExtensionSymbol* ext : candidates) {
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
        // for the bool match AND for extracting the concrete T-binding — and,
        // per §26.5.1 (#1307), it IS the receiver-substituted head the
        // specificity ranking below compares.
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
            // §26.5 (pre-existing, unchanged by #1307): the first same-named
            // member within ONE block wins — per-block overload selection by
            // arity/argument type is a separate, unimplemented gap (member
            // access is typed independent of the enclosing call's argument
            // list throughout this front-end), not touched here.
            found.push_back({m, matchedAnc, target, ext, decl, std::move(binding)});
            break;
        }
    }
    if (found.empty()) return nullptr;
    if (found.size() == 1) {
        if (outBinding) *outBinding = found[0].binding;
        return found[0].member;
    }
    // §26.5.1 rule 1 (#1307): "An extension on E1 is more specific than one on
    // E2 when E1 is a subtype of E2 and E2 is not a subtype of E1. The
    // subtype test is evaluated on the receiver-substituted heads." subtypeOrEq
    // reuses extensionFindMatchingAncestor itself as the general subtype test:
    // for two already-CONCRETE (receiver-substituted) heads, "is `sub` an
    // ancestor-or-self match for `base`" is exactly "is `sub` a subtype-or-
    // equal of `base`" over the same class/interface/builtin-generic universe
    // §26.5 step 2 already walks — so running it both directions reproduces
    // the spec's own "identical substituted heads → neither wins" case for
    // free (extensionTargetMatchesReceiver's exact-match branch returns true
    // both ways when the heads are structurally equal).
    auto subtypeOrEq = [&](const TypeRef* sub, const TypeRef* base) -> bool {
        return extensionFindMatchingAncestor(
                   base, sub, std::unordered_set<std::string_view>{},
                   ownedAncs) != nullptr;
    };
    auto moreSpecific = [&](std::size_t i, std::size_t j) {
        return subtypeOrEq(found[i].substHead, found[j].substHead) &&
               !subtypeOrEq(found[j].substHead, found[i].substHead);
    };
    // The maximal elements of the "more specific" partial order: candidates
    // no OTHER candidate strictly dominates. §26.5 rule 3 ("most specific
    // type wins") removes every dominated (less-specific) candidate from
    // consideration entirely — only a non-unique top tier is ambiguous.
    std::vector<std::size_t> maximal;
    for (std::size_t i = 0; i < found.size(); ++i) {
        bool dominated = false;
        for (std::size_t j = 0; j < found.size(); ++j) {
            if (i != j && moreSpecific(j, i)) { dominated = true; break; }
        }
        if (!dominated) maximal.push_back(i);
    }
    if (maximal.size() == 1) {
        const ExtCandidate& winner = found[maximal[0]];
        if (outBinding) *outBinding = winner.binding;
        return winner.member;
    }
    // §26.5 rule 4 (#1307): two or more equally-specific candidates — a
    // compile error naming each one by "its defining module, its declared
    // head including any where clause, and its declaration site" (spec's own
    // wording, since extension blocks are nameless).
    if (reportAmbiguity) {
        std::string msg = "ambiguous extension member `" + std::string(name) +
            "`: " + std::to_string(maximal.size()) +
            " extensions are equally specific for this receiver — neither is "
            "a subtype of the other (§26.5 rule 4, §26.5.1); candidates:";
        for (std::size_t idx : maximal) {
            const ExtCandidate& c = found[idx];
            std::string moduleName;
            if (auto it = importedExtensionModule_.find(c.ext);
                it != importedExtensionModule_.end())
                moduleName = it->second;
            else if (ctx_.module())
                moduleName = std::string(ctx_.module()->name);
            std::string head = "extension " + spellExtHeadTypeRef(c.declaredHead);
            std::string where = c.decl ? spellExtWhereClause(c.decl->genericConstraints)
                                       : std::string();
            if (!where.empty()) head += " " + where;
            std::string site;
            if (c.decl && c.decl->span.begin.line != 0)
                site = "declared at line " +
                       std::to_string(c.decl->span.begin.line) + ", column " +
                       std::to_string(c.decl->span.begin.column);
            else
                // §1307 follow-up: an imported extension's synthesized AST
                // carries no source span (module metadata doesn't serialize
                // cross-module positions) — say so rather than print 0:0.
                site = "declaration site not tracked across module imports";
            msg += "\n  " + head + " — module `" + moduleName + "`, " + site;
        }
        msg += "\nremedies: narrow the import bringing in one of these "
               "extension blocks; hoist the intended behavior into a "
               "static-class helper method that takes the receiver as a "
               "parameter; or, if both blocks are under your control, give "
               "the colliding members distinct names (§26.5 rule 4).";
        ctx_.diagnostics().report(Severity::Error, loc, msg);
    }
    // Error-recovery (mirrors resolveOverload's OverloadOutcome::Ambiguous
    // handling, ExprTypecheck_Calls.cpp): return a deterministic candidate so
    // downstream typing stays best-effort even though compilation fails.
    const ExtCandidate& fallback = found[maximal[0]];
    if (outBinding) *outBinding = fallback.binding;
    return fallback.member;
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
    // §1307: this is a pure existence PROBE ("does any extension match this
    // receiver/name at all") for the note text below — it must not itself
    // trigger the §26.5 rule 4 ambiguity diagnostic (the instance-member call
    // it annotates already failed for its own, unrelated reason; a second,
    // premature error here would be confusing noise, not new information).
    if (!lookupExtensionMember(receiver, name, nullptr, SourceLocation{},
                               /*reportAmbiguity=*/false))
        return std::string();
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
    // §14.2 type-parameter bounds: a constraint bound may itself be another type
    // parameter (`where T: U`). In that case `T` inherits `U`'s constraint-
    // derived member surface ("T conforms to every interface that U's own
    // constraints guarantee"), so the `where T: U` chain is followed
    // transitively. A self-bound (`where T: T`) or a cycle (`where T: U`,
    // `where U: T`) is a declaration error checked elsewhere; the `visited` set
    // still guards this walk against looping defensively.
    const ast::TypeDecl* typeDecl =
        (currentType_ && currentType_->decl)
            ? dynamic_cast<const ast::TypeDecl*>(currentType_->decl)
            : nullptr;
    auto isDeclaredParam = [&](std::string_view nm) -> bool {
        if (currentMethodDecl_)
            for (const auto& gp : currentMethodDecl_->genericParams)
                if (gp.name == nm) return true;
        if (typeDecl)
            for (const auto& gp : typeDecl->genericParams)
                if (gp.name == nm) return true;
        return false;
    };
    // Visit every constraint item declared for `nm` — the method's clause first
    // (most specific), then the enclosing type's — applying `g`; stop when it
    // returns true. (A constraint clause only references params declared in the
    // same clause, so scanning both lists never cross-attributes a bound.)
    auto forItems =
        [&](std::string_view nm,
            const std::function<bool(const ast::ConstraintItem&)>& g) -> bool {
        auto scan =
            [&](const std::vector<ast::GenericConstraint>& gcs) -> bool {
            for (const auto& gc : gcs) {
                if (gc.typeParam != nm) continue;
                for (const auto& item : gc.items)
                    if (g(item)) return true;
            }
            return false;
        };
        if (currentMethodDecl_ && scan(currentMethodDecl_->genericConstraints))
            return true;
        if (typeDecl && scan(typeDecl->genericConstraints)) return true;
        return false;
    };
    std::unordered_set<std::string_view> visited;
    std::vector<std::string_view> worklist{paramName};
    while (!worklist.empty()) {
        std::string_view nm = worklist.back();
        worklist.pop_back();
        if (!visited.insert(nm).second) continue;
        if (!isDeclaredParam(nm)) continue;
        bool stop = forItems(nm, [&](const ast::ConstraintItem& item) -> bool {
            if (item.kind != "type" || !item.typeArg) return false;
            TypeRef* ctr = ctx_.resolvedType(*item.typeArg);
            if (!ctr || ctr->kind != TypeRef::Kind::Named) return false;
            if (ctr->declaration) return fn(ctr);  // concrete class/iface bound
            // A bound with no declaration whose single-part name is another
            // in-scope type parameter is a `where nm: <param>` bound — enqueue
            // that parameter so its own constraint surface is searched too.
            if (ctr->astNode && ctr->astNode->name &&
                ctr->astNode->name->parts.size() == 1) {
                std::string_view boundParam = ctr->headName();
                if (isDeclaredParam(boundParam))
                    worklist.push_back(boundParam);
            }
            return false;
        });
        if (stop) return true;
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
    // §11.8 (#1425 sub-item 1): `where T: IRich<T>` where `IRich<T>: INumber<T>`
    // — IRich declares no operators of its own, they live on the ANCESTOR
    // INumber. forEachConstraintBound yields only the DIRECT bound (IRich<T>);
    // scanning `iface->members` alone (the pre-fix body) misses every operator
    // declared on an interface `iface` merely EXTENDS. Fix: from each direct
    // bound, BFS the interface's own `baseTypes` (populated for an interface's
    // `: Base<...>` list exactly like a class's, TypeResolution_Names.cpp) to
    // visit the whole ancestor closure. The "both params are the recurrence
    // parameter" check below is entirely local to whichever interface in the
    // closure is being scanned (it tests the OPERATOR's own declared shape,
    // `static operator +(a: T, b: T)` against THAT interface's own type
    // parameter) — it needs no generic-argument composition across the chain,
    // because the caller (the BinaryExpr handler in ExprTypecheck_Exprs.cpp)
    // re-derives the call-site result type from the ROOT paramName's own
    // operand type, not from the chain. A visited set guards a diamond
    // re-visit (`IRich<T>: INumber<T>, IPrintable<T>` both possibly reaching a
    // common ancestor) against a double report; it can never be a true cycle
    // (interface inheritance cycles are rejected elsewhere, §12.10).
    auto scanClosure = [&](TypeSymbol* rootIface, TypeRef* rootBound) -> bool {
        std::vector<TypeSymbol*> frontier{rootIface};
        std::unordered_set<TypeSymbol*> visited;
        std::size_t i = 0;
        while (i < frontier.size()) {
            TypeSymbol* iface = frontier[i++];
            if (!iface || !visited.insert(iface).second) continue;
            if (iface->kind != ast::TypeDeclKind::Interface) continue;
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
                // Both parameters must be the recurrence parameter itself so
                // the substituted signature is (paramName, paramName).
                // §2.8.2/§11.8 (#2359 bug 2a): an IMPORTED interface's
                // operator parameter carries no syntactic `declType` at all
                // (ModuleLoader::convertParams populates `resolvedDeclType`
                // instead, since there is no ast::Type to attach) — fall
                // back to it exactly as the #2294 conformance-check fix
                // does for imported members elsewhere. Read the bare-
                // parameter name via the shared alias-transparent
                // `TypeRef::headName()` (falls back to `importedName` when
                // `astNode` is null) instead of requiring `astNode`
                // directly, so an imported bare-type-parameter reference is
                // recognized rather than unconditionally rejected.
                bool bothRecur = true;
                for (Symbol* ps : ms->params) {
                    auto* par = dynamic_cast<ParameterSymbol*>(ps);
                    TypeRef* pt = par ? (par->declType
                                             ? ctx_.resolvedType(*par->declType)
                                             : par->resolvedDeclType)
                                       : nullptr;
                    if (!pt || pt->kind != TypeRef::Kind::Named ||
                        pt->declaration || pt->nullable ||
                        pt->headName().empty() ||
                        !ifaceParams.count(pt->headName())) {
                        bothRecur = false;
                        break;
                    }
                }
                if (!bothRecur) continue;
                if (exact) {
                    found = ms;
                    if (outBound) *outBound = rootBound;
                    return true;  // exact token wins — stop scanning
                }
                // Remember the first group sibling; keep scanning for an
                // exact hit.
                if (!groupWitness) { groupWitness = ms; groupBound = rootBound; }
            }
            for (TypeSymbol* b : iface->baseTypes)
                if (b) frontier.push_back(b);
        }
        return false;
    };
    forEachConstraintBound(paramName, [&](TypeRef* ctr) -> bool {
        TypeSymbol* iface = ctr->declaration;
        if (!iface || iface->kind != ast::TypeDeclKind::Interface) return false;
        return scanClosure(iface, ctr);
    });
    if (found) return found;
    if (groupWitness && outBound) *outBound = groupBound;
    return groupWitness;
}

Symbol* ExprTC::lookupName(std::string_view name, bool memberBeforeModuleType) {
    for (Scope* s = currentScope_; s; s = s->parent) {
        // §2.7 rule 1 (v0.217, #2097): resolving the leftmost bare name walks
        // "local scope, then the current module's declarations". Its value
        // result set is "(a local, field, or property)" — so an accessible
        // instance member of the enclosing type (an implicit-`this.member`
        // reference) precedes the module's top-level TYPE declarations. Apply
        // that precedence just before the MODULE scope is consulted, so the
        // order is local/param (inner scopes walked first) > enclosing-type
        // member > top-level type. Only VALUE position requests this
        // (`memberBeforeModuleType`): the caller passes it for a bare name that
        // is NOT the head of a dotted access. Without it, a record/class whose
        // auto-property or field shares a name with a top-level type — e.g.
        // `property C` inside a program that also declares `class C` — resolved
        // the bare `C := v` assignment target to the TYPE, so codegen's
        // implicitFieldAddr found no storage member and the constructor's field
        // store was silently dropped (the record kept its zero-initialized
        // field, and two distinct constructions compared equal, #2097). A
        // dotted HEAD (`Color.Red`, `Data.M()`) does NOT request it — that head
        // keeps resolving to the top-level type (the historical order below),
        // because §2.7's "type-wins, no backtracking" chain reading is served
        // by the existing exprType-based member-access resolution and flipping
        // the head would break static access through a same-named field whose
        // own type cannot host the accessed name. (Whether §2.7 rule 1's
        // "local scope" is intended to place enclosing-type members explicitly,
        // and how the unrelated-typed-member head shape should read, is raised
        // as a spec-clarification issue.)
        if (memberBeforeModuleType && s == moduleScope_.get() && currentType_) {
            if (Symbol* m = lookupMember(currentType_, name)) return m;
        }
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
    // (Reached when the current scope chain did not include `moduleScope_`, so
    // the in-loop member check above never fired.)
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

// §17.14 (v0.184, #1688): same identifier resolution as foldIntConstExprTC
// above (kept as a separate resolver here rather than sharing one closure,
// matching foldIntConstExprTC's own self-contained style) so a local const's
// unfoldable initializer can be diagnosed specifically as a division/modulo
// by zero rather than silently accepted as "not a foldable constant".
bool ExprTC::localConstDivByZeroTC(const ast::Expr& e) {
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
    return containsConstDivByZero(e, resolve);
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
    // §7.4 v0.201 (#2049): a `const` local's narrowing is NEVER invalidated
    // (no invalidating form can apply: it cannot be reassigned, cannot be
    // passed as ref/out, cannot be mutably captured — all compile errors).
    // An assignment reaching here for a const local is itself a compile
    // error already diagnosed by checkReadonlyAssignment; do not let the
    // error-recovery path also wrongly invalidate the narrowing.
    if (auto* ls = dynamic_cast<LocalSymbol*>(sym); ls && ls->isConst)
        return;
    // §7.4 (v0.214, #2085/#2116): reassigning a local root rebinds it to a
    // possibly-different object, so any `local._f` facts riding it no longer
    // hold (they were about the old referent). Drop them regardless of the new
    // value's nullability.
    dropFieldFactsRooted(sym);
    if (valueType && !valueType->nullable)
        nullNarrowed_.insert(sym);
    else
        nullNarrowed_.erase(sym);
}

// §7.4 (issue #2118): join the narrowing state across a loop body's two
// reaching paths. A loop body may run zero times, so the after-loop state is
// the INTERSECTION of the entry sets (zero-iteration path) and the current
// sets as the body left them (>=1-iteration path): a narrowing established
// inside the body does NOT survive (it is not an entry fact), while an entry
// narrowing the body invalidated is already gone from the current sets. This
// mirrors the if-statement all-paths join (see the IfStmt handler) applied to
// while/for bodies. Iteration order over the pointer-keyed containers does not
// affect the result — set/map intersection is order-independent and no
// diagnostic is emitted — so sema output is stable across shard processes
// (#2116).
//
// §7.4 (#2152): that two-path model missed the break/continue edges. A `break`
// exits the loop directly and a `continue` jumps to the cond re-check (which on
// cond-false also exits), so the state at each such edge is a possible
// after-loop state. `edges` carries one snapshot per edge; each is folded into
// the same intersection so that a local an edge left possibly-null is dropped
// (and a bare member access depending on it is correctly rejected). The fold is
// pure set/map intersection — sound (it only drops facts) and order-independent
// (#2116).
void ExprTC::intersectNarrowingSetsAfterLoop(
    const std::unordered_set<Symbol*>& entryNull,
    const std::unordered_map<Symbol*, TypeRef*>& entryNarrowings,
    const std::set<std::pair<std::uint32_t, std::uint32_t>>& entryField,
    const LoopEdgeNarrowing& edges) {
    intersectNarrowingSetsWith(entryNull, entryNarrowings, entryField);
    // §7.4 (#2152): fold each break/continue edge snapshot into the
    // intersection. A `break`/`continue` edge state is a possible after-loop
    // state (see the function header); a local the edge left possibly-null is
    // absent from the edge's null set, so the intersection drops it — the bare
    // member access after the loop is then correctly rejected. The three
    // parallel vectors are pushed together by recordLoopEdgeNarrowing, so they
    // share one length.
    for (std::size_t i = 0; i < edges.nullEdges.size(); ++i)
        intersectNarrowingSetsWith(edges.nullEdges[i], edges.typeEdges[i],
                                   edges.fieldEdges[i]);
}

// §7.4 (#2258): the one intersection primitive — see the declaration in
// ExprTypecheck_impl.hpp for why every "restore" of the narrowing sets must be
// this and never an assignment.
void ExprTC::intersectNarrowingSetsWith(
    const std::unordered_set<Symbol*>& otherNull,
    const std::unordered_map<Symbol*, TypeRef*>& otherNarrowings,
    const std::set<std::pair<std::uint32_t, std::uint32_t>>& otherField) {
    // §7.4 (v0.214, #2116): the readonly-field facts join the same way — a fact
    // established inside the joined region does not survive it; a fact the
    // region invalidated stays dropped.
    intersectFieldFactsWith(otherField);
    for (auto it = nullNarrowed_.begin(); it != nullNarrowed_.end();) {
        if (otherNull.count(*it))
            ++it;
        else
            it = nullNarrowed_.erase(it);
    }
    // §12.7 type-narrowings are keyed by symbol AND narrowed type: an entry
    // whose narrowed type differs from the snapshot's is not the same fact, so
    // it does not survive the join.
    for (auto it = narrowings_.begin(); it != narrowings_.end();) {
        auto e = otherNarrowings.find(it->first);
        if (e != otherNarrowings.end() && e->second == it->second)
            ++it;
        else
            it = narrowings_.erase(it);
    }
}

// §7.4 (#2152): see the header comment. Bounds-checked: an erroneous jump
// (loopDepth_ == 0 already reported, or an unknown label whose offset stays at
// its default 0) may route to a nominally-innermost collector, but the program
// is already in error and the snapshot is harmless either way — and a
// still-valid enclosing loop's join only ever DROPS facts (intersection), so a
// stray snapshot cannot manufacture an unsound acceptance.
void ExprTC::recordLoopEdgeNarrowing(int targetOffset) {
    if (loopEdgeNarrowing_.empty()) return;
    auto idx = static_cast<std::ptrdiff_t>(loopEdgeNarrowing_.size()) - 1
             - static_cast<std::ptrdiff_t>(targetOffset);
    if (idx < 0 || idx >= static_cast<std::ptrdiff_t>(loopEdgeNarrowing_.size()))
        return;
    auto& col = loopEdgeNarrowing_[static_cast<std::size_t>(idx)];
    col.nullEdges.push_back(nullNarrowed_);
    col.typeEdges.push_back(narrowings_);
    col.fieldEdges.push_back(fieldNarrowed_);
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

// §7.4 (v0.214, #2085/#2116): dense-id accessors for narrowFactIds_. These are
// the table's ONLY accessors; neither iterates it (#2116 determinism audit —
// the pointer-keyed map contributes no iteration order to any decision).
std::uint32_t ExprTC::narrowFactIdOf(const Symbol* s) {
    if (narrowFactIdSyms_.empty()) narrowFactIdSyms_.push_back(nullptr);  // [0]
    auto [it, inserted] = narrowFactIds_.try_emplace(
        s, static_cast<std::uint32_t>(narrowFactIdSyms_.size()));
    if (inserted) narrowFactIdSyms_.push_back(s);
    return it->second;
}

std::uint32_t ExprTC::narrowFactIdIfTracked(const Symbol* s) const {
    auto it = narrowFactIds_.find(s);
    return it == narrowFactIds_.end() ? 0u : it->second;
}

// §7.4 (v0.214, #2085/#2116): `readonly` fields narrow. Detect the modifier on
// a field symbol, or a positional-record field (read-only by construction).
bool ExprTC::isReadonlyField(const FieldSymbol* fs) {
    if (!fs) return false;
    // §9.1/§9.4 (#2189): a positional-record field's auto-generated property is
    // read-only (no setter; a purely positional record is immutable), so it
    // participates in §7.4 narrowing like a `readonly` body field. The
    // FieldSymbol is synthesized without a "readonly" modifier
    // (SymbolCollection's positional-field loop), so recognize the posParam
    // flag directly — this also brings fieldTypeOf's posParam branch (which
    // follows an isReadonlyField pass) into reach. Limited to RECORD parents:
    // a CLASS primary-constructor parameter (§22.10) is captured into a
    // private field that is NOT public-readonly (an `owned` capture is
    // disposed with the object — mutated), so it must not narrow.
    if (fs->posParam && fs->parent &&
        fs->parent->kind == ast::TypeDeclKind::Record)
        return true;
    for (auto mod : fs->modifiers)
        if (mod == "readonly") return true;
    return false;
}

// §7.4 (v0.214, #2085/#2116): the resolved TypeRef of a field symbol — a body
// `FieldDecl` (its `type`) or a positional-record parameter (its `posParam`
// type). Null when neither is available/resolved. Pure side-table reads
// (ctx_.resolvedType is a const map lookup; it creates nothing).
TypeRef* ExprTC::fieldTypeOf(const FieldSymbol* fs) {
    if (!fs) return nullptr;
    if (auto* fd = dynamic_cast<const ast::FieldDecl*>(fs->decl)) {
        if (fd->type) return ctx_.resolvedType(*fd->type);
    }
    if (fs->posParam && fs->posParam->type)
        return ctx_.resolvedType(*fs->posParam->type);
    return nullptr;
}

// §7.4 (v0.214, #2085/#2116): recognize a trackable nullable `readonly`-field
// access path — spec §7.4 (v0.214): "The tracked shape is exactly one field
// off an admitted root — `this._f`, `TypeName._s`, or `local._f` where `local`
// is itself narrowing-tracked; … any non-final field segment makes a path
// non-trackable (`a.b._f` establishes nothing — `a.b` is a mutable read)."
// Returns the (rootId, fieldId) narrowing key or nullopt. Recognition only:
// the caller applies the construction-window carve-out and consults
// `fieldNarrowed_`. Symbols come from the PURE ctx_.exprSymbol side table
// stamped when the operand expression was visited — recognition performs no
// lookupName and allocates nothing, so a declined path leaves compiler state
// bit-identical to the feature-absent compiler (#2116 determinism audit).
std::optional<std::pair<std::uint32_t, std::uint32_t>>
ExprTC::readonlyFieldPathOf(const ast::Expr* e, bool assignIds) {
    if (!e) return std::nullopt;
    auto keyFor = [&](const Symbol* root, const FieldSymbol* fs)
        -> std::optional<std::pair<std::uint32_t, std::uint32_t>> {
        std::uint32_t rootId = 0;
        if (root) {
            rootId = assignIds ? narrowFactIdOf(root)
                               : narrowFactIdIfTracked(root);
            if (rootId == 0) return std::nullopt;  // never tracked → no fact
        }
        std::uint32_t fieldId = assignIds ? narrowFactIdOf(fs)
                                          : narrowFactIdIfTracked(fs);
        if (fieldId == 0) return std::nullopt;
        return std::make_pair(rootId, fieldId);
    };
    // Bare field identifier `_f`: an implicit-`this` instance field, or a bare
    // static readonly field of the enclosing type. Both take the ambient root
    // (rootId 0); the FieldSymbol disambiguates (a type's instance and static
    // members share one name namespace, §11.3). A bare static field of the
    // enclosing GENERIC type is admitted: one body has a single instantiation
    // context, so its facts cannot cross instantiations (#2114).
    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(e)) {
        if (id->name == "this" || id->name == "base") return std::nullopt;
        if (!id->typeArgs.empty()) return std::nullopt;
        auto* fs = dynamic_cast<FieldSymbol*>(ctx_.exprSymbol(*id));
        if (!fs || !isReadonlyField(fs)) return std::nullopt;
        TypeRef* ft = fieldTypeOf(fs);
        if (!ft || !ft->nullable) return std::nullopt;
        return keyFor(nullptr, fs);
    }
    // A member access `<target>._f`. The member must resolve to a readonly
    // nullable field, and the target must be an admitted root — `this`/`base`,
    // a NON-GENERIC type name (static), or a local/parameter — NOT a further
    // member access (that would be a non-final field segment) or any other
    // expression.
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(e)) {
        auto* fs = dynamic_cast<FieldSymbol*>(ctx_.exprSymbol(*ma));
        if (!fs || !isReadonlyField(fs)) return std::nullopt;
        TypeRef* ft = fieldTypeOf(fs);
        if (!ft || !ft->nullable) return std::nullopt;
        auto* tid = dynamic_cast<const ast::IdentifierExpr*>(ma->target.get());
        if (!tid) return std::nullopt;  // non-final segment / non-root target
        if (tid->name == "this" || tid->name == "base")
            return keyFor(nullptr, fs);
        Symbol* rs = ctx_.exprSymbol(*tid);
        if (!rs) return std::nullopt;
        // A type-name root (`TypeName._s`) is a static readonly field —
        // ambient root, keyed by the (unique static) FieldSymbol. #2114: a
        // GENERIC type name is NOT trackable — sema shares one FieldSymbol
        // across instantiations while each instantiation has separate static
        // storage, so a shared key would conflate `Cache<int>._s` with
        // `Cache<string>._s`. Both the spelled-type-args and the
        // declared-generic-params forms are declined.
        if (auto* ts = dynamic_cast<TypeSymbol*>(rs)) {
            if (!tid->typeArgs.empty() || !ts->genericParams.empty())
                return std::nullopt;
            return keyFor(nullptr, fs);
        }
        // A local/parameter root (`local._f`): the fact rides this root.
        if (dynamic_cast<LocalSymbol*>(rs) || dynamic_cast<ParameterSymbol*>(rs))
            return keyFor(rs, fs);
        return std::nullopt;
    }
    return std::nullopt;
}

// §7.4 (v0.214, #2085/#2116): establish a readonly-field narrowing fact from a
// `<path> <> null` / `= null` test. `<> null` narrows the true branch, `= null`
// the else branch (the parser desugars `is not null`/`is null` to these).
// Spec §7.4 (v0.214): "The rule does not apply inside the declaring type's
// constructors, field initializers, static constructor, or static field
// initializers (the assignment window); elsewhere it composes with
// construction's §22.7 `this`-escape residual exactly as stated there: a
// during-construction narrowing test observes either the null watermark
// (establishing nothing) or the field's single assigned value, final by §22.8
// — the residual is §22.7's, inherited, not enlarged."
bool ExprTC::narrowFromFieldNullTest(const ast::Expr* cond, bool forTrueBranch) {
    auto* be = dynamic_cast<const ast::BinaryExpr*>(cond);
    if (!be) return false;
    bool narrowsThis;
    if (be->op == "<>") narrowsThis = forTrueBranch;       // true branch
    else if (be->op == "=") narrowsThis = !forTrueBranch;  // else branch
    else return false;
    if (!narrowsThis) return false;
    const ast::Expr* varSide = nullptr;
    if (dynamic_cast<const ast::NullLiteralExpr*>(be->right.get()))
        varSide = be->left.get();
    else if (dynamic_cast<const ast::NullLiteralExpr*>(be->left.get()))
        varSide = be->right.get();
    if (!varSide) return false;
    // Peek the field symbol FIRST (without assigning ids) so the construction-
    // window carve-out can decline before any id is minted — establishment
    // must be a no-op there, not a tracked-then-suppressed path.
    auto* probeMa = dynamic_cast<const ast::MemberAccessExpr*>(varSide);
    auto* probeId = dynamic_cast<const ast::IdentifierExpr*>(varSide);
    const FieldSymbol* fsProbe = nullptr;
    if (probeMa) fsProbe = dynamic_cast<FieldSymbol*>(ctx_.exprSymbol(*probeMa));
    else if (probeId) fsProbe =
        dynamic_cast<FieldSymbol*>(ctx_.exprSymbol(*probeId));
    if (!fsProbe) return false;
    // Construction-window carve-out (§7.4, v0.214). `inConstructor_` covers
    // both instance and static constructors; `inFieldInitializer_` covers
    // field and static-field initializers. Restricted to the field's OWN
    // declaring type: a base field read in a derived ctor is already assigned
    // (base-first construction, §22.8) and narrows soundly, and a field of an
    // unrelated type held in a local is not in its own construction window.
    if ((inConstructor_ || inFieldInitializer_) &&
        fsProbe->parent == currentType_)
        return false;
    auto key = readonlyFieldPathOf(varSide, /*assignIds=*/true);
    if (!key) return false;
    fieldNarrowed_.insert(*key);
    return true;
}

// §7.4 (v0.214, #2085/#2116): the field-only establishment walk. Mirrors
// collectNarrowings' condition coverage (single test; `and` contributes both
// conjuncts to the true branch) but touches ONLY `fieldNarrowed_` — used to
// seed a branch context (e.g. an elif condition's entry state) without
// re-running the legacy sets' establishment.
void ExprTC::collectFieldNarrowings(const ast::Expr* cond, bool forTrueBranch) {
    if (!cond) return;
    if (narrowFromFieldNullTest(cond, forTrueBranch)) return;
    if (auto* be = dynamic_cast<const ast::BinaryExpr*>(cond)) {
        if (be->op == "and" && forTrueBranch) {
            collectFieldNarrowings(be->left.get(), /*forTrueBranch=*/true);
            collectFieldNarrowings(be->right.get(), /*forTrueBranch=*/true);
        }
    }
}

// §7.4 (v0.214, #2085/#2116): drop every readonly-field fact rooted at `root`.
// Facts are ordered by (rootId, fieldId), so the victims form one contiguous
// range. A symbol that was never tracked has no facts (narrowFactIdIfTracked
// returns 0 = the ambient root id, whose facts are exactly the ones this
// function must never drop — the early return keeps that invariant).
void ExprTC::dropFieldFactsRooted(const Symbol* root) {
    if (!root) return;
    const std::uint32_t rootId = narrowFactIdIfTracked(root);
    if (rootId == 0) return;
    // §7.4 #2116 (pre-gate review): record the drop EDGE even when the
    // current working set holds no facts for the root — the try walk keeps
    // side copies (tryJoinField) whose facts for this root must not outlive
    // a rebinding the working set never saw (a finally-body `ref` pass of a
    // root whose facts were established inside the try body).
    ++fieldFactDropEpoch_;
    if (fieldNarrowed_.empty()) return;
    fieldNarrowed_.erase(fieldNarrowed_.lower_bound({rootId, 0u}),
                         fieldNarrowed_.lower_bound({rootId + 1, 0u}));
}

// §7.4 / #2116: fieldNarrowed_ ∩= other. Pure integer-set algebra; result
// membership is independent of iteration order.
void ExprTC::intersectFieldFactsWith(
    const std::set<std::pair<std::uint32_t, std::uint32_t>>& other) {
    for (auto it = fieldNarrowed_.begin(); it != fieldNarrowed_.end();) {
        if (other.count(*it)) ++it;
        else it = fieldNarrowed_.erase(it);
    }
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
        auto savedField = fieldNarrowed_;  // §7.4 #2116: probe-local
        collectNarrowings(returnValue, /*forTrueBranch=*/whenTrue);
        const bool ok = isKnownNonNull(psym);
        nullNarrowed_ = std::move(savedNull);
        narrowings_ = std::move(savedNarrow);
        fieldNarrowed_ = std::move(savedField);
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
    // §7.4 (v0.214, #2085/#2116): a `<readonly-field path> <> null` / `= null`
    // test narrows the field path (`this._f`, `TypeName._s`, `local._f`).
    if (narrowFromFieldNullTest(cond, forTrueBranch)) return;
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
//
// §7.4 v0.201 (#2049): a `const` local's narrowing is NEVER invalidated —
// mutable-capture of a const local is a compile error (the same single-
// assignment rule), so a name appearing in `mutablyCapturedNames_` does NOT
// imply a const local of that name can actually be mutated at runtime. Skip
// const locals here so a const local narrowed by a null test stays narrowed
// for the remainder of its scope, across calls and `await`s.
void ExprTC::invalidateCapturedNarrowings() {
    if (mutablyCapturedNames_.empty()) return;
    for (auto it = nullNarrowed_.begin(); it != nullNarrowed_.end();) {
        Symbol* s = *it;
        if (s && mutablyCapturedNames_.count(s->name) &&
            !(dynamic_cast<LocalSymbol*>(s) &&
              dynamic_cast<LocalSymbol*>(s)->isConst))
            it = nullNarrowed_.erase(it);
        else
            ++it;
    }
    for (auto it = narrowings_.begin(); it != narrowings_.end();) {
        Symbol* s = it->first;
        if (s && mutablyCapturedNames_.count(s->name) &&
            !(dynamic_cast<LocalSymbol*>(s) &&
              dynamic_cast<LocalSymbol*>(s)->isConst))
            it = narrowings_.erase(it);
        else
            ++it;
    }
    // §7.4 (v0.214, #2085/#2116): a `local._f` fact rides its local root; if
    // that root is mutably captured, a closure invoked by the control-transfer
    // operation could have rebound it, so drop the fact. Ambient-rooted facts
    // (rootId 0 — `this`/static) are never dropped: a readonly field survives
    // calls and `await`s (§7.4 v0.214). Iteration is over the integer-keyed
    // ordered set (deterministic order), and each per-element decision is
    // independent of the others — order cannot influence the outcome (#2116).
    // §7.4 #2116 (pre-gate review): a control-transfer op with mutable
    // captures in scope is a drop EDGE for the try walk's epoch even when the
    // working set holds no matching fact right now — the walk keeps side
    // copies (tryJoinField) whose captured-root facts must not outlive a
    // possible closure rebinding. Membership-only test: no decision depends
    // on container iteration order.
    if (!mutablyCapturedNames_.empty()) ++fieldFactDropEpoch_;
    for (auto it = fieldNarrowed_.begin(); it != fieldNarrowed_.end();) {
        const std::uint32_t rootId = it->first;
        const Symbol* r = (rootId != 0 && rootId < narrowFactIdSyms_.size())
                              ? narrowFactIdSyms_[rootId]
                              : nullptr;
        if (r && mutablyCapturedNames_.count(r->name) &&
            !(dynamic_cast<const LocalSymbol*>(r) &&
              dynamic_cast<const LocalSymbol*>(r)->isConst))
            it = fieldNarrowed_.erase(it);
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
    const std::vector<Symbol*>* paramSyms,
    const std::vector<ast::ContractClause>* clauses) {
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
    // §7.4 (v0.214, #2085/#2116): readonly-field narrowing facts are likewise
    // method-local. (The id tables persist — identity mappings, not facts.)
    fieldNarrowed_.clear();
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
        // §22.5 "Defaults and modifiers" (v0.184, issue #1684): a
        // default-value expression that produces a fresh owned value of
        // NON-trivial-dispose class type (§28.9) is admitted only when the
        // parameter itself is declared `owned` (the owned-allocation default
        // form this section already blesses) — the produced instance has no
        // owner to discharge it otherwise, a leak. Gate on `modifier.empty()`
        // (a genuinely plain/borrowing parameter): `owned` params are exempt
        // by rule, and `ref`/`out`/`scoped` params raise a separate,
        // syntactic-shape admissibility question this issue does not cover.
        // A trivial-dispose producer (e.g. `new StringBuilder()`) carries no
        // disposal obligation and stays admissible on either form, so it is
        // excluded by exprIsOwnedProducerOfNonTrivialDisposeClass itself.
        if (params[i].modifier.empty() &&
            exprIsOwnedProducerOfNonTrivialDisposeClass(
                *params[i].defaultValue)) {
            ctx_.diagnostics().report(
                Severity::Error, params[i].defaultValue->span.begin,
                std::string("default value of parameter `") +
                    std::string(params[i].name) +
                    "` produces an owned value of non-trivial-dispose class "
                    "type, which would have no owner to discharge it; "
                    "declare the parameter `owned` to receive it, or use a "
                    "trivial-dispose default (§22.5)");
        }
    }
    forbiddenForwardParams_.clear();
    if (exprBody) {
        visitExpr(*exprBody);
        // §5.1 / §22.2 (issue #1774): an expression body `=> E` is the method's
        // returned value — spec-equivalent to `{ return E; }` (§22.2) — so E
        // must satisfy the SAME return-type convertibility check the block-form
        // `return E;` runs (the ReturnStmt arm of visitStmt, "Slice T3" below).
        // Without this, a type-mismatched arrow body (`method Foo(): int =>
        // "x"`, or a paren-less method reference of type `() -> int`) skipped
        // §5.1 and reached codegen, which emitted a `ret` whose operand type
        // differed from the function's return type — malformed LLVM IR (a
        // verifier failure / miscompile). Gate exactly as the block form does:
        // a value-returning method/operator with a resolved return type. (A
        // `void`/procedure arrow body has returnCtx_ VoidMethod and is exempt;
        // an `async` value method carries the peeled element type in
        // currentReturnType_, so `=> await P()` is checked against that element,
        // matching the block-form `return await P();`.)
        if (returnCtx_ == ReturnCtx::ValueMethod && currentReturnType_) {
            checkAssignable(
                ctx_.exprType(*exprBody), currentReturnType_,
                exprBody->span.begin, "returned value",
                exprBody);  // §27.6 #756 property-ref
        }
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
    // §7.4 / §22.16 (#2188, mirroring the #2116 pre-gate review's field-set
    // fix): clear EVERY flow-narrowing set BEFORE the contract clauses are
    // visited. Clauses evaluate at the member's BOUNDARIES — `requires` at
    // entry (after argument binding, before ANY body guard has run — the
    // lowered requires prologue is #2053 slice 4a), `ensures` at every
    // return edge — so the body's FINAL flow state must not admit a nullable
    // access in a clause. The #2116 amendment closed this for the readonly-
    // FIELD set (`fieldNarrowed_`); the LEGACY local/parameter sets
    // (`nullNarrowed_`, `narrowings_`) carried the identical leak for
    // parameters — a body-final early-return guard let `requires(s.Length >= 0)`
    // compile on a `string?` parameter, then the lowered prologue dereffed
    // null on `Len(null)` before the body's guard could run (probe-confirmed
    // acceptance on main). The simple sound rule per §22.16's boundary
    // semantics: NO body flow facts survive into any clause; clause-INTERNAL
    // narrowing still applies (the visitExpr below re-establishes facts
    // inside a single clause expression, e.g. `requires(s <> null and
    // s.Length >= 0)` — §22.16 "Flow narrowing works inside clauses
    // unchanged"). §22.4's out-nullability postcondition machinery is
    // separate (`currentOutPostconds_`) and is not touched here.
    nullNarrowed_.clear();
    narrowings_.clear();
    fieldNarrowed_.clear();
    // §22.16 (v0.203, #2053): visit each contract clause's expression in the
    // method scope (params + this + enclosing statics) — body locals are not
    // registered into `methodScope`, so they are correctly out of reach —
    // and verify each is of type `bool`. Clauses are visited AFTER the body
    // so the type lattice for parameter/this reads is fully populated.
    if (clauses) {
        for (const auto& c : *clauses) {
            if (!c.expr) continue;
            visitExpr(*c.expr);
            // §22.16 "Clause typing: bool, pure, borrow-only": each clause
            // expression must be `bool` (§4.1 — no truthiness). Conservative
            // like checkConditionIsBool: skip when the type is unresolved so
            // this pass never false-positives on incomplete typing.
            TypeRef* t = ctx_.exprType(*c.expr);
            if (!t) continue;
            if (t->kind == TypeRef::Kind::Primitive &&
                t->primitive == PrimitiveType::Bool && t->nullable) {
                ctx_.diagnostics().report(
                    Severity::Error, c.expr->span.begin,
                    std::string("a contract clause's expression must be "
                                "`bool`, not `bool?` — a nullable bool cannot "
                                "be used in a clause (§22.16, §7.11)"));
                continue;
            }
            if (t->kind != TypeRef::Kind::Primitive) continue;
            if (t->primitive == PrimitiveType::Bool) continue;
            ctx_.diagnostics().report(
                Severity::Error, c.expr->span.begin,
                std::string("a contract clause's expression must be `bool` "
                            "(§22.16)"));
        }
    }
    currentScope_ = save;
    // §7.4 (v0.214, #2116): clear the readonly-field facts at body EXIT too.
    // Unlike the local sets — whose symbols are out of scope outside this body
    // and thus inert — an AMBIENT (this/static) field fact is method-
    // independent: left in place, it would bleed into the next non-body
    // expression visit (a FIELD or auto-property INITIALIZER, which never
    // passes through this function's entry clear) and wrongly admit a nullable
    // readonly-field access there. Cleared at both ends, the set is empty
    // whenever no method body is being visited.
    fieldNarrowed_.clear();
    // §22.4 (v0.183, #1585): restore the enclosing body's postcondition set.
    currentOutPostconds_ = std::move(savedPostconds);
}

// §28.9 / §22.5 (issue #1684): does `decl`'s class chain make it
// non-trivial-dispose — a destructor, or a non-static `owned` field
// (declared, or a captured `owned` primary-constructor parameter, §22.10),
// anywhere from `decl` up through its base classes? This is the same test
// visitStmt's ExpressionStmt arm applies inline for the §21.9 bare-discard
// check (below in this file); duplicated here as a standalone predicate
// rather than shared, since that walk is not exposed as a callable, but both
// apply the same §28.9 trivial-dispose classification. Cycle-guarded
// (mirrors the #1624 precedent) so a malformed base-type graph that
// survives error recovery cannot hang the compiler.
bool ExprTC::isNonTrivialDisposeClass(TypeSymbol* decl) const {
    bool reject = false;
    std::unordered_set<const TypeSymbol*> visitedBase;
    const std::size_t kMaxWalk = 4096;  // defensive; base chains are shallow
    std::size_t walked = 0;
    for (TypeSymbol* cur = decl; cur && !reject;) {
        if (++walked > kMaxWalk) break;
        if (!visitedBase.insert(cur).second) break;  // cycle guard (#1624)
        for (Symbol* m : cur->members) {
            if (dynamic_cast<DestructorSymbol*>(m)) {
                reject = true;
                break;
            }
            auto* fs = dynamic_cast<FieldSymbol*>(m);
            if (!fs) continue;
            if (fs->posParam) {
                if (fs->parent &&
                    fs->parent->kind == ast::TypeDeclKind::Class &&
                    fs->posParam->modifier == "owned" &&
                    fs->posParamBodyRef && !fs->posParamInitRef) {
                    reject = true;
                    break;
                }
                continue;
            }
            // A `static owned` field (§22.12 rule 4) is disposed at
            // process-exit order (§22.13), not by this instance's Dispose()
            // — exclude it, or a class whose only `owned` member is static
            // would be wrongly classified non-trivial-dispose here.
            bool isStatic = false, isOwned = false;
            for (auto mod : fs->modifiers) {
                if (mod == "static") isStatic = true;
                if (mod == "owned") isOwned = true;
            }
            if (isOwned && !isStatic) {
                reject = true;
                break;
            }
        }
        TypeSymbol* nextBase = nullptr;
        for (TypeSymbol* b : cur->baseTypes)
            if (b && b->kind == ast::TypeDeclKind::Class) {
                nextBase = b;
                break;
            }
        cur = nextBase;
    }
    return reject;
}

// §22.5 v0.184 (issue #1684): is `e` a fresh owned-producing expression —
// `new T()` or a call to an `owned`-returning method — whose produced class
// type is non-trivial-dispose? Scoped to the shapes §22.5's "Permitted
// default-value expressions" actually admits in a default position: `this`,
// instance state, and locals are all out of scope there, so the only
// owned-producing shapes that can reach a default-value expression are a
// `new` expression and a call to a static factory method — mirrors the
// ownedProduced classification the §21.9 bare-discard check performs (below
// in this file) for the general (any-statement) case.
bool ExprTC::exprIsOwnedProducerOfNonTrivialDisposeClass(
    const ast::Expr& e) const {
    bool ownedProduced = false;
    if (dynamic_cast<const ast::NewExpr*>(&e)) {
        ownedProduced = true;
    } else if (auto* ce = dynamic_cast<const ast::CallExpr*>(&e)) {
        if (ce->callee) {
            if (auto* sym = ctx_.exprSymbol(*ce->callee)) {
                if (auto* ms = dynamic_cast<MethodSymbol*>(sym)) {
                    if (auto* md =
                            dynamic_cast<const ast::MethodDecl*>(ms->decl)) {
                        ownedProduced = md->returnOwned;
                    }
                }
            }
        }
    }
    if (!ownedProduced) return false;
    const TypeRef* t = ctx_.exprType(e);
    if (!t || t->kind != TypeRef::Kind::Named || !t->declaration ||
        t->declaration->kind != ast::TypeDeclKind::Class ||
        t->declaration->isValueBuiltin) {
        return false;
    }
    return isNonTrivialDisposeClass(t->declaration);
}

// stdlib §4.13 (v0.196, #1645): the owning-enumeration interfaces
// `IOwningEnumerable<T>`, `IOwningEnumerator<T>`, and the async pair each
// constrain `where T: class` — ownership transfer through iteration is
// meaningful only for class-typed elements (they own a disposable value).
static bool isOwningEnumerableHead(std::string_view n) {
    return n == "IOwningEnumerable" || n == "IOwningEnumerator" ||
           n == "IOwningAsyncEnumerable" || n == "IOwningAsyncEnumerator";
}

// Is `ta` a class-like element type — a proper user class, or a compiler-known
// builtin named type with no user TypeDecl (Mutex, OwningArray<T>, …)?  Mirrors
// the Shared<T>/Weak<T> element-class detection (§28.13, ExprTypecheck_Exprs.cpp):
// primitives, records, enums, interfaces, and function types are NOT class-like.
static bool isClassLikeElementArg(const TypeRef* ta) {
    if (!ta) return false;
    if (ta->kind == TypeRef::Kind::Named && ta->declaration &&
        ta->declaration->kind == ast::TypeDeclKind::Class)
        return true;
    if (ta->kind == TypeRef::Kind::Named && !ta->declaration &&
        ta->astNode && ta->astNode->name && !ta->astNode->name->parts.empty())
        return true;  // builtin named type, no user decl — class-like
    return false;
}

// Human-readable name of a non-class element type for the §4.13 diagnostic.
// Mirrors the Shared<T>/Weak<T> diagnostic name mapping.
static std::string nonClassElementName(const TypeRef* ta) {
    if (!ta) return "<no type argument>";
    if (ta->kind == TypeRef::Kind::Primitive) {
        using P = sema::PrimitiveType;
        switch (ta->primitive) {
            case P::Bool:    return "bool";
            case P::Char:    return "char";
            case P::String:  return "string";
            case P::Byte:    return "byte";
            case P::SByte:   return "sbyte";
            case P::Short:   return "short";
            case P::UShort:  return "ushort";
            case P::Int:     return "int";
            case P::UInt:    return "uint";
            case P::Long:    return "long";
            case P::ULong:   return "ulong";
            case P::Float:   return "float";
            case P::Double:  return "double";
            case P::Decimal: return "decimal";
            case P::Object:  return "Object";
            default:         return "primitive type";
        }
    }
    if (ta->kind == TypeRef::Kind::Function) return "function type";
    if (ta->kind == TypeRef::Kind::Named && ta->declaration) {
        std::string name(ta->declaration->name);
        if (ta->declaration->kind == ast::TypeDeclKind::Interface)
            name += " (interface)";
        else if (ta->declaration->kind == ast::TypeDeclKind::Record)
            name += " (record/value type)";
        else if (ta->declaration->kind == ast::TypeDeclKind::Enum)
            name += " (enum/value type)";
        return name;
    }
    return "<non-class type>";
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
        // stdlib §4.13 (v0.196, #1645): an iterator method declared to return
        // one of the owning-enumeration interfaces (`IOwningEnumerable<T>`,
        // `IOwningEnumerator<T>`, or the async pair) must have a CLASS-like
        // element type T — those interfaces constrain `where T: class` because
        // iteration transfers ownership of each element, which is meaningful
        // only for a disposable class value. A value/primitive (or other
        // non-class) element type was accepted here and then crashed the
        // owning-iterator lowering (an ICmp type-mismatch on the take/advance
        // path). Reject it at the source, mirroring the Shared<T>/Weak<T>
        // element-class rule (§28.13). Plain (non-owning) `IEnumerable<T>`
        // carries no such constraint and is unaffected.
        if (isIteratorMethod && md->returnType) {
            if (TypeRef* rr = ctx_.resolvedType(*md->returnType)) {
                if (rr->kind == TypeRef::Kind::Named &&
                    isOwningEnumerableHead(rr->headName()) &&
                    !rr->genericArgs.empty() && rr->genericArgs[0] &&
                    !isClassLikeElementArg(rr->genericArgs[0])) {
                    ctx_.diagnostics().report(
                        Severity::Error, md->span.begin,
                        std::string("iterator method `") +
                        std::string(md->name) + "` returns `" +
                        std::string(rr->headName()) +
                        "<T>` but its element type `" +
                        nonClassElementName(rr->genericArgs[0]) +
                        "` is not a class — the owning-enumeration interfaces "
                        "require T to be a class type, because iteration "
                        "transfers ownership of each element (stdlib \xC2\xA7"
                        "4.13)");
                }
            }
        }
        // §30.1 / §29.3 (v0.184, #1670): reject `ref`/`out` params on an
        // `async` method or an iterator method (sync or async) — declaration-
        // level, independent of body kind (a block-bodied method is the only
        // shape that can be an iterator; `inAsyncContext_` already reflects
        // the `async` modifier for both block and expression bodies here).
        checkNoRefOutOnAsyncOrIterator(md->params, inAsyncContext_,
                                        isIteratorMethod, md->name,
                                        md->span.begin, ctx_);
        bool saveIter = inIteratorMethod_;
        inIteratorMethod_ = isIteratorMethod;
        // §22.16 (v0.203, #2053): pass the method's contract clauses so each
        // expression is type-checked (in the method scope) and bool-verified.
        runMethodBody(md->params, stmts, expr, paramSymbolsOf(m),
                      &md->contractClauses);
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
        // §22.16 (v0.203, #2053): pass the constructor's contract clauses
        // (requires/ensures). Sema (later slice) additionally rejects `this`
        // reads in a ctor's `requires` per §22.7; this slice types + bool-
        // verifies the expressions only.
        runMethodBody(cd->params, &cd->body, nullptr,
                      paramSymbolsOf(m), &cd->contractClauses);
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
        // §5.1 / §23.2 / §23.3 (issue #1777): a property GETTER's body — the
        // expression-body form (`property P: T => E;`, §23.2), a block
        // `get => E;`, or a block `get => do … return E; … end;` (§23.3) —
        // yields the property's value, exactly as a value method's body yields
        // its return (§22.2's `=> E ≡
        // return E`). It must therefore run the SAME §5.1 return-type
        // convertibility check a value method runs, against the property's
        // declared type as the "return type". Without this the getter body
        // skipped BOTH the arrow-body check (runMethodBody, #1774) and the
        // block-form `return E;` check (the ReturnStmt "Slice T3" arm): both
        // gate on `returnCtx_ == ValueMethod && currentReturnType_`, which a
        // property decl never set (returnCtx_ stayed at its ReturnCtx::None
        // default here), so a type-mismatched getter (`property P: int => "x"`)
        // reached codegen and emitted malformed LLVM IR ("Function return type
        // does not match operand type of return inst! ret ptr / i32"), a
        // verifier failure — the same gap #1774 fixed for methods. Wire the
        // getter bodies through the value-method return context (property type
        // as currentReturnType_) so they share that code path. A SETTER body is
        // a void statement context (its `=> E` runs E for effect, not as a
        // returned value), so it keeps the enclosing None context and is exempt
        // — matching a void method.
        ReturnCtx savePropRet = returnCtx_;
        TypeRef* savePropRetTy = currentReturnType_;
        TypeRef* propRetTy = pd->type ? ctx_.resolvedType(*pd->type) : nullptr;
        if (pd->exprBody) {
            returnCtx_ = ReturnCtx::ValueMethod;
            currentReturnType_ = propRetTy;
            runMethodBody(pdParams, nullptr, pd->exprBody.get(),
                          idxSymsPtr);
            returnCtx_ = savePropRet;
            currentReturnType_ = savePropRetTy;
        }
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
            // Getter: include indexer params. §5.1/§23.3 (#1777): run the
            // getter body against the property type as its return type (see the
            // note above the expression-body form), so a mismatched `get => E;`
            // or `get => do … return E; … end;` is rejected instead of reaching
            // codegen. Restored to the enclosing (None) context afterwards so a
            // sibling setter accessor stays a void context.
            returnCtx_ = ReturnCtx::ValueMethod;
            currentReturnType_ = propRetTy;
            if (a.exprBody)
                runMethodBody(pdParams, nullptr, a.exprBody.get(),
                              idxSymsPtr);
            if (!a.stmts.empty())
                runMethodBody(pdParams, &a.stmts, nullptr, idxSymsPtr);
            returnCtx_ = savePropRet;
            currentReturnType_ = savePropRetTy;
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

// §21.4 / §29: does a locally-declared user type (or any type in its base
// closure) support synchronous `for`-in iteration — i.e. expose a
// `GetEnumerator` method (the §29 `IEnumerable<T>` contract) or derive from a
// builtin `IEnumerable`/collection-family interface? Cycle-guarded BFS over the
// base closure.
static bool userTypeSupportsForIn(const TypeSymbol* root) {
    // The §29 iteration / §4 collection interface family — every builtin whose
    // element chain terminates at IEnumerable<T>. A base named from this set
    // makes the deriving type for-in iterable.
    auto isEnumerableFamily = [](std::string_view n) {
        return n == "IEnumerable" || n == "IEnumerator" ||
               n == "IOwningEnumerable" || n == "IOwningEnumerator" ||
               n == "IReadOnlyCollection" || n == "IReadOnlyList" ||
               n == "IList" || n == "ICollection" ||
               n == "IOwningList" || n == "IOwningCollection" ||
               n == "IGrouping";
    };
    std::vector<const TypeSymbol*> queue = {root};
    std::unordered_set<const TypeSymbol*> seen = {root};
    for (std::size_t qi = 0; qi < queue.size(); ++qi) {
        const TypeSymbol* t = queue[qi];
        if (!t) continue;
        // §29: a concrete type implementing IEnumerable<T> always declares
        // GetEnumerator (the interface is abstract), so a member scan across the
        // base closure is authoritative for such types.
        for (auto* m : t->members)
            if (auto* ms = dynamic_cast<const MethodSymbol*>(m))
                if (ms->name == "GetEnumerator") return true;
        // A builtin IEnumerable-family base carries declaration==null and is
        // therefore dropped from `baseTypes`/`baseTypeRefs` (TypeResolution only
        // records bases with a resolved TypeSymbol). Recover it from the
        // SYNTACTIC base list on the AST TypeDecl — this is what makes an
        // interface that merely EXTENDS IEnumerable<T> (declaring no
        // GetEnumerator of its own) count as iterable.
        if (auto* td = dynamic_cast<const ast::TypeDecl*>(t->decl))
            for (const auto& bt : td->baseTypes)
                if (bt && bt->name && !bt->name->parts.empty() &&
                    isEnumerableFamily(bt->name->parts.back()))
                    return true;
        // Recurse into user (declaration-bearing) base types.
        for (auto* bt : t->baseTypes)
            if (bt && seen.insert(bt).second) queue.push_back(bt);
    }
    return false;
}

// §21.5 (v0.201, #2051): reject a loop label that shadows an enclosing loop's.
void ExprTC::checkLoopLabelShadow(std::string_view label, SourceLocation loc) {
    if (label.empty()) return;
    for (const auto& l : loopLabelStack_) {
        if (l == label) {
            ctx_.diagnostics().report(
                Severity::Error, loc,
                std::string("loop label '") + std::string(label) +
                    "' duplicates the label of an enclosing loop; labels do not "
                    "shadow (\xc2\xa7""21.5)");
            return;
        }
    }
}

// §21.5 (v0.201): resolve a `break`/`continue` target loop, compute the codegen
// nesting offset, and apply the §31.2 finally-boundary rule.
void ExprTC::checkLoopJumpTarget(std::string_view label, bool isBreak,
                                 int& offsetOut, SourceLocation loc) {
    int targetDepth; // 1-based depth of the resolved target loop
    if (label.empty()) {
        // Unlabeled: the immediately enclosing loop.
        offsetOut = 0;
        targetDepth = loopDepth_;
    } else {
        // Labeled: the nearest enclosing loop carrying `label` (§21.5). At most
        // one such loop can be in scope — checkLoopLabelShadow rejects duplicates
        // at declaration — but scan innermost-first for robustness.
        int ti = -1;
        for (int i = static_cast<int>(loopLabelStack_.size()) - 1; i >= 0; --i) {
            if (loopLabelStack_[static_cast<std::size_t>(i)] == label) {
                ti = i;
                break;
            }
        }
        if (ti < 0) {
            std::string inScope;
            for (const auto& l : loopLabelStack_) {
                if (l.empty()) continue;
                if (!inScope.empty()) inScope += ", ";
                inScope += "'";
                inScope += std::string(l);
                inScope += "'";
            }
            std::string msg = std::string("unknown loop label '") +
                              std::string(label) + "' in `" +
                              (isBreak ? "break" : "continue") +
                              "` (\xc2\xa7""21.5); ";
            msg += inScope.empty() ? "no loop labels are in scope"
                                   : ("labels in scope: " + inScope);
            ctx_.diagnostics().report(Severity::Error, loc, std::move(msg));
            return;
        }
        offsetOut = static_cast<int>(loopLabelStack_.size()) - 1 - ti;
        targetDepth = ti + 1;
    }
    // per §21.5/§31.2 (v0.182, #1578; generalized to labeled targets in v0.201):
    // inside a `finally`, a `break`/`continue` whose target loop was already open
    // when the `finally` was entered (encloses it) is a compile error — it would
    // silently abandon an in-flight exception. A loop opened wholly inside the
    // `finally` is unaffected.
    if (finallyLoopBoundary_ != -1 && targetDepth <= finallyLoopBoundary_) {
        ctx_.diagnostics().report(
            Severity::Error, loc,
            isBreak ? "`break` inside a `finally` block may not target a loop "
                      "that encloses the `finally` (\xc2\xa7""21.5/\xc2\xa7""31.2)"
                    : "`continue` inside a `finally` block may not target a loop "
                      "that encloses the `finally` (\xc2\xa7""21.5/\xc2\xa7""31.2)");
    }
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
        TypeRef* savedLamTargetRet = pendingLambdaTargetReturn_;
        if (ld->type && ld->initializer) {
            if (dynamic_cast<const ast::LambdaExpr*>(ld->initializer.get())) {
                if (auto* dt = ctx_.resolvedType(*ld->type)) {
                    if (dt->kind == TypeRef::Kind::Function && dt->funcReturn &&
                        isTaskShapedRef(dt->funcReturn) &&
                        !dt->funcReturn->genericArgs.empty())
                        pendingLambdaReturnType_ =
                            dt->funcReturn->genericArgs.front();
                    // §27.3 (v0.226, #2107): thread the target function type's
                    // return slot so the block-body inference knows a target
                    // exists and suppresses the divergent-return error — under a
                    // target, divergent returns are checked against R via §5.1 at
                    // each `return` (a future slice), never unified by inference.
                    if (dt->kind == TypeRef::Kind::Function)
                        pendingLambdaTargetReturn_ = dt->funcReturn;
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
        pendingLambdaTargetReturn_ = savedLamTargetRet;
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
                // per §15.4 (#1866): a collection literal whose declared target
                // is a collection INTERFACE (`IEnumerable<T>`/`IList<T>`/
                // `ICollection<T>`/`IReadOnly*`/`ISet`/`IDictionary`/…) is not
                // admissible — §15.4's target list is a CLOSED set of concrete
                // collection types, and an interface has no concrete backing the
                // literal could materialize into. Left un-rejected, the literal
                // reached codegen with a `ptr`-typed target and emitted malformed
                // IR (an Invalid-GEP verifier abort — a compile-time ICE). Reject
                // it here, exactly as the call-argument position (#1174) and the
                // lambda expression-body position (#1693) already do. A CONCRETE
                // collection init (`var v: Array<T> := [...]`) is unaffected; the
                // remedy names a concrete backing and (covariantly) assigns it:
                // `var xs: Array<T> := [...]; var v: IEnumerable<T> := xs;`.
                if (dynamic_cast<const ast::CollectionLiteralExpr*>(
                        ld->initializer.get()) &&
                    isCollectionLiteralInterfaceTarget(lsym->declType)) {
                    reportCollectionLiteralInterfaceTarget(
                        ctx_.diagnostics(), ld->initializer->span.begin,
                        namedTypeSimpleName(lsym->declType));
                    currentScope_->names[ld->name] = lsym;
                    return;  // reported; skip the assignability check below
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
                // §9 / §3.2 (#1759): `var q := this` — `this` is left
                // deliberately untyped in visitExpr (memberTargetType resolves
                // `this.member` via currentType_ directly, NOT via `this`'s
                // exprType — see its comment "so we don't have to teach `this`
                // to be an expression with type currentType_"). An inferred
                // local seeded from a bare `this` therefore got no type, so
                // every later `q.member` failed to resolve and codegen folded it
                // to `ret i32 0` (records AND classes). Seed the local with the
                // current receiver type so `q` is a proper value of it: a record
                // COPY (value semantics §9.4 — the codegen side loads `*this`
                // into the slot, the #1759 A/B/C fix) or a class-reference alias.
                // Member lookup keys off the declaration only, so the bare Named
                // ref suffices; codegen carries any generic substitution through
                // its own receiver-specialization path (currentSpecMethodSymbol_).
                if (!lsym->declType && currentType_) {
                    if (auto* initId = dynamic_cast<const ast::IdentifierExpr*>(
                            ld->initializer.get())) {
                        if (initId->name == "this") {
                            auto* selfTy = ctx_.makeTypeRef();
                            selfTy->kind = TypeRef::Kind::Named;
                            selfTy->declaration = currentType_;
                            lsym->declType = selfTy;
                        }
                    }
                }
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
        // §21.1 v0.201 (#2049): classification split for a local `const`.
        // Compile-time-constant classification requires BOTH (a) the
        // initializer be a §17.14 constant expression AND (b) the declared
        // or inferred type be a primitive value type or `string`. Anything
        // else is a RUNTIME const — a single-assignment, never-owned binding
        // whose references are NOT constant expressions (no case-pattern /
        // inline-array-size / constant-narrowing positions). The earlier
        // "const must be primitive or string" hard error (pre-v0.201) is
        // removed: a non-primitive explicit type now classifies the local
        // as a runtime const instead of being rejected.
        TypeRef* lcExplicitTy = nullptr;
        if (lc->type) {
            if (auto* rt = ctx_.resolvedType(*lc->type))
                lcExplicitTy = rt;
        }
        TypeRef* lcInferredTy =
            lc->value ? ctx_.exprType(*lc->value) : nullptr;
        TypeRef* lcTy = lcExplicitTy ? lcExplicitTy : lcInferredTy;
        auto typeIsPrimitiveOrString = [](TypeRef* t) -> bool {
            if (!t) return false;
            if (t->kind == TypeRef::Kind::Primitive &&
                t->primitive != PrimitiveType::Object)
                return true;  // incl. String, Bool, numeric, Char
            return false;
        };
        const bool compileTimeClassified =
            lc->value && typeIsPrimitiveOrString(lcTy) &&
            isConstInitExpr(*lc->value);
        // §7.1 (#1084) parallel: a bare `null` initializer with no type
        // annotation gives the binding no determinable type. Per §21.1
        // v0.201 `null` is admissible as a runtime const ONLY with a type
        // annotation (the binding is "permanently null"); reject the bare
        // form just as `var x := null;` is rejected.
        if (!lc->type && lc->value &&
            dynamic_cast<const ast::NullLiteralExpr*>(lc->value.get())) {
            ctx_.diagnostics().report(
                Severity::Error, lc->value->span.begin,
                "cannot infer a type for `" + std::string(lc->name) +
                "` from a bare `null` — `null` has no type of its own; "
                "add a nullable type annotation (`const " +
                std::string(lc->name) + ": <T>? := null`) (§7.1 / §21.1)");
        }
        // §17.14 (v0.184, #1688): a COMPILE-TIME-classified local const's
        // initializer's compile-time evaluation fault (integer overflow,
        // division/mod by zero) is a compile error at the constant
        // expression, never deferred to runtime. checkTypeConsts
        // (TypeResolution_Checks.cpp) already enforces this for a CLASS-
        // level const member (#958/#1211); this mirrors that logic for a
        // LOCAL compile-time const. A runtime const's initializer faults
        // at runtime like any ordinary expression's — the rule does not
        // apply to it.
        if (compileTimeClassified && lcTy && lc->value) {
            using P = PrimitiveType;
            const P p = lcTy->primitive;
            const bool isIntConst =
                p == P::Byte || p == P::SByte || p == P::Short ||
                p == P::UShort || p == P::Int || p == P::UInt ||
                p == P::Long || p == P::ULong;
            if (isIntConst) {
                if (auto folded = foldIntConstExprTC(*lc->value)) {
                    if (!fitsConstIntRange(*folded, p)) {
                        ctx_.diagnostics().report(
                            Severity::Error, lc->span.begin,
                            std::string("const `") + std::string(lc->name) +
                            "` initializer value " +
                            std::to_string(*folded) +
                            " does not fit its declared type — a const "
                            "initializer must be a representable "
                            "compile-time constant (§17.14)");
                    }
                } else if (localConstDivByZeroTC(*lc->value)) {
                    ctx_.diagnostics().report(
                        Severity::Error, lc->span.begin,
                        std::string("const `") + std::string(lc->name) +
                        "` initializer contains a division or modulo by "
                        "zero — a const initializer must fold to a "
                        "representable compile-time-constant value "
                        "(§17.14)");
                    }
            }
        }
        if (currentScope_ && !lc->name.empty()) {
            auto* lsym = ctx_.makeSymbol<LocalSymbol>();
            lsym->name = lc->name;
            lsym->decl = lc;
            lsym->isConst = true;
            lsym->isCompileTimeConst = compileTimeClassified;
            // §21.1: a const local's effective type (explicit annotation
            // when present, else the initializer's inferred type) anchors
            // identifier references — required for runtime const of
            // reference type (a borrow, a string, …) so member access,
            // dispatch, and codegen's slot layout see the right type.
            if (lcTy) lsym->declType = lcTy;
            currentScope_->names[lc->name] = lsym;
            // §7.4 (issue #418, v0.201 #2049): a nullable-typed const
            // local initialized with a non-null value is known non-null
            // for the rest of its scope — its narrowing is never
            // invalidated (no invalidating form can apply to a const).
            if (lc->value && lsym->declType && lsym->declType->nullable) {
                if (TypeRef* initTy = ctx_.exprType(*lc->value))
                    if (!initTy->nullable) nullNarrowed_.insert(lsym);
            }
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
        // §27.3 (v0.226, #2107): thread the enclosing value-method's function
        // return slot when the returned value is a lambda — the same target the
        // typed-local initializer and call-argument positions thread. Without
        // this, `return lambda() => do return d; return c; end;` in a method
        // declared `(): () -> Animal` is wrongly rejected as divergent: under a
        // target the returns are checked against it via §5.1 (and the resulting
        // `() -> Dog` converts to the target under §27.1.1), never unified by
        // inference. Restored after the visit.
        TypeRef* savedLamTargetRet = pendingLambdaTargetReturn_;
        if (rs->value && returnCtx_ == ReturnCtx::ValueMethod &&
            currentReturnType_ &&
            currentReturnType_->kind == TypeRef::Kind::Function &&
            dynamic_cast<const ast::LambdaExpr*>(rs->value.get()))
            pendingLambdaTargetReturn_ = currentReturnType_->funcReturn;
        if (rs->value) visitExpr(*rs->value);
        pendingLambdaTargetReturn_ = savedLamTargetRet;
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
                // fix #1476 / stdlib §4.10: OwningSet<T>.Take(query: T) is the
                // same "builtin owned-transfer, no user MethodDecl" shape as
                // the drain names above — without this, `s.Take(q);` as a bare
                // discard statement silently leaked the removed element (no
                // §21.9 diagnostic). Kept separate from the `drain` list above:
                // Take's single positional arg is the query (not an index/key),
                // and it is defined on OwningSet alone, so a shared receiver-
                // head list would not fit it cleanly.
                if (!ownedProduced && ce->callee) {
                    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(
                            ce->callee.get())) {
                        if (ma->member == "Take" && ma->target) {
                            if (auto* rt = ctx_.exprType(*ma->target))
                                if (rt->kind == TypeRef::Kind::Named &&
                                    rt->astNode && rt->astNode->name &&
                                    !rt->astNode->name->parts.empty() &&
                                    rt->headName() == "OwningSet")
                                    ownedProduced = true;
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
                    // §21.9 / §31.2.1 (#1624 precedent) — TERMINATION: the
                    // base-type graph can contain cycles that survive into
                    // sema via error recovery (an erroneous `class A: B;
                    // class B: A` is reported by the §12 cycle check but its
                    // TypeSymbol baseTypes still link A↔B, so this walk reaches
                    // it during the §21.9 bare-discard non-trivial-dispose
                    // classification). Without a guard, a cyclic base chain
                    // whose members set no `reject` (no destructor / no owned
                    // field in the cycle — a trivial-dispose cycle reached via
                    // `ownedProduced`) loops forever, hanging/crashing the
                    // compiler on valid input. Mirror catchArmNcb's #1624 fix:
                    // a visited-set (by TypeSymbol pointer) gates the walk, and
                    // a hard iteration cap is defense-in-depth so a malformed
                    // deep/diamond graph can never make this unbounded.
                    std::unordered_set<const TypeSymbol*> visitedBase;
                    const std::size_t kMaxWalk = 4096;  // defensive; base chains are shallow
                    std::size_t walked = 0;
                    for (TypeSymbol* cur = t->declaration; cur && !reject;) {
                        if (++walked > kMaxWalk) break;              // §21.9 walk must terminate
                        if (!visitedBase.insert(cur).second) break;  // cycle guard (#1624)
                        for (Symbol* m : cur->members) {
                            if (dynamic_cast<DestructorSymbol*>(m)) {
                                reject = true;
                                break;
                            }
                            // §21.9 / §3.6 / #1594: non-trivial-dispose is not
                            // just "has a user destructor" — a class carrying
                            // ANY `owned` storage member is non-trivial-dispose
                            // too (§28.9's derived-property computation), since
                            // its generated Dispose() must run to release that
                            // member. Without this, a class whose only
                            // disposal obligation is an `owned` field (declared,
                            // or a captured `owned` primary-ctor parameter,
                            // §22.10 v0.180) was misclassified trivial-dispose:
                            // the bare-statement codegen path
                            // (LlvmBackend_Stmts.cpp ExpressionStmt) never calls
                            // Dispose() for a bare-statement discard, so the
                            // owned member genuinely leaked with zero
                            // diagnostics. Mirrors codegen's
                            // isCapturedOwnedPrimaryParam (LlvmBackend_impl.hpp)
                            // for the posParam case; sema has no access to that
                            // codegen predicate, so the equivalent check is
                            // reproduced here directly on the FieldSymbol.
                            auto* fs = dynamic_cast<FieldSymbol*>(m);
                            if (!fs) continue;
                            if (fs->posParam) {
                                if (fs->parent &&
                                    fs->parent->kind ==
                                        ast::TypeDeclKind::Class &&
                                    fs->posParam->modifier == "owned" &&
                                    fs->posParamBodyRef &&
                                    !fs->posParamInitRef) {
                                    reject = true;
                                    break;
                                }
                                continue;
                            }
                            // A `static owned` field (§22.12 rule 4) is
                            // disposed at process-exit order (§22.13), not by
                            // this instance's Dispose() — exclude it, or a
                            // class whose only `owned` member is static would
                            // be wrongly classified non-trivial-dispose here.
                            bool isStatic = false, isOwned = false;
                            for (auto mod : fs->modifiers) {
                                if (mod == "static") isStatic = true;
                                if (mod == "owned") isOwned = true;
                            }
                            if (isOwned && !isStatic) { reject = true; break; }
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
        // §6.6 / A.6 (v0.184, #1690): the lvalue chain's base must be an
        // identifier or `this` — a method-call result (`h.Self().Total`)
        // cannot serve as the chain's base. Checked before the shape-
        // specific rules below, which assume a resolvable storage location.
        if (as->target) checkLValueChainNotCallBased(*as->target);
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
        // §3.5/§6.5 (#2163, review follow-up to #2153): `string` and `decimal`
        // targets are rejected here too, mirroring the RHS-side kind list of
        // the #1689 block below — §3.5 defines bitwise on the eight integer
        // primitives only and shifts "on the integer types", so neither has a
        // §6.6 result. They were previously missing from this TARGET list:
        // `string <<= 2` crashed the compiler (cast<IntegerType> assert),
        // `string? &= 2` aborted on malformed IR, `decimal &= 3` was a silent
        // no-op (emitDecimalArith knows no `&`), and `decimal? &= 3` silently
        // NULLED the target (the #2141 lifted lowering's `if (pay)` fallback).
        // The nullable forms reject identically (tt->primitive is read
        // regardless of tt->nullable, exactly as for `char?`).
        if (as->target &&
            (as->op == "&=" || as->op == "|=" || as->op == "^=" ||
             as->op == "<<=" || as->op == ">>=")) {
            if (TypeRef* tt = ctx_.exprType(*as->target)) {
                if (tt->kind == TypeRef::Kind::Primitive) {
                    PrimitiveType p = tt->primitive;
                    const char* badKind =
                        (p == PrimitiveType::Float ||
                         p == PrimitiveType::Double ||
                         p == PrimitiveType::Decimal)
                            ? "floating-point types"
                        : (p == PrimitiveType::Bool)
                            ? "`bool` (boolean operations use `and`/`or`/`not`)"
                        : (p == PrimitiveType::Char)   ? "`char`"
                        : (p == PrimitiveType::String) ? "`string`" : nullptr;
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
        // per §22.12 rule 5 (v0.180, #1542): `+=` on a `static event` requires
        // a capture-free handler. Remember whether the target is a static
        // event here (target's symbol is available now); the actual
        // capture-freedom check runs below, after `as->value` is visited, so
        // isCaptureFreeEntry can see the visited lambda's captures.
        bool assignTargetIsStaticEvent = false;
        if ((as->op == "+=" || as->op == "-=") && as->target) {
            if (auto* es = dynamic_cast<EventSymbol*>(
                    ctx_.exprSymbol(*as->target))) {
                // per §24.3: the `+=` / `-=` forms desugar to Add / Remove,
                // which live on the EventSource<T> face — available only
                // inside the declaring class (and inside a derived class for a
                // `protected` event). Outside, the event is IEvent<T>, which
                // has no Add / Remove.
                checkEventVisibilityFence(es, as->op, as->span.begin);
                if (auto* ed = dynamic_cast<const ast::EventDecl*>(es->decl)) {
                    if (ed->type) eventPayload = ctx_.resolvedType(*ed->type);
                    // per §22.12 rule 5 (v0.180, #1542): staticness lives on
                    // ast::EventDecl::isStatic directly — unlike
                    // Field/Property/MethodDecl, EventDecl has no generic
                    // `modifiers` string list (Appendix A.4), and
                    // SymbolCollection.cpp never populates EventSymbol's
                    // inherited (always-empty) `modifiers` field for events.
                    assignTargetIsStaticEvent = ed->isStatic;
                }
            }
        }
        if (as->value) {
            TypeRef* savedPending = pendingLambdaParamType_;
            TypeRef* savedLamTargetRet = pendingLambdaTargetReturn_;
            if (eventPayload &&
                dynamic_cast<const ast::LambdaExpr*>(as->value.get()))
                pendingLambdaParamType_ = eventPayload;
            // §27.3 (v0.226, #2107): thread the assignment target's function
            // return slot when the value is a lambda — the same target the
            // typed-local initializer position threads for its inline form
            // (`var f: (…) -> R := lambda …`). A deferred reassignment
            // `var f: (…) -> Animal; … f := lambda() => do return d; return c;
            // end;` is the SAME shape and must NOT be rejected as divergent:
            // under the target the returns are checked against R via §5.1, not
            // unified by inference. `as->target` was visited above so its
            // exprType is available. Restored after the visit.
            if (as->target &&
                dynamic_cast<const ast::LambdaExpr*>(as->value.get())) {
                if (TypeRef* tgtTy = ctx_.exprType(*as->target)) {
                    if (tgtTy->kind == TypeRef::Kind::Function)
                        pendingLambdaTargetReturn_ = tgtTy->funcReturn;
                }
            }
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
            // per §22.12 rule 5 (v0.180, #1542): a handler subscribed to a
            // `static event` — the rule names `+=`/`Add`/`Subscribe`, not
            // `-=`/`Remove` — must be capture-free: a non-capturing lambda or
            // a static method reference, the same admission as a
            // Domain.Spawn entry (§30.14, #600). The process-global
            // subscriber list transports the handler to whichever domain
            // raises, and a capture-bearing closure carries borrows of its
            // origin domain's scopes.
            if (as->op == "+=" && assignTargetIsStaticEvent &&
                !isCaptureFreeEntry(as->value.get())) {
                ctx_.diagnostics().report(
                    Severity::Error, as->value->span.begin,
                    "handler subscribed to a `static event` must be capture-"
                    "free: a non-capturing lambda or a static method "
                    "reference — a capturing lambda is rejected because the "
                    "process-global subscriber list transports it to "
                    "whichever domain raises, and its captures are borrows "
                    "of the origin domain (\xC2\xA7""22.12 rule 5, #1542)");
            }
            currentLiteralWideningHint_ = savedAssignHint;
            pendingLambdaParamType_ = savedPending;
            pendingLambdaTargetReturn_ = savedLamTargetRet;
        }
        // §6.6 (v0.184, #1689): a compound assignment `x op= e` desugars to
        // `x := x op e`. Let T be the target's type and R the type of
        // `x op e` under this chapter's rules — §6.2's mixed-type promotion
        // for the arithmetic/bitwise forms, §6.5's "a shift's result already
        // has the left operand's type" for `<<=`/`>>=`. When R is T, or R
        // implicitly converts to T (§5.1), the store is direct; when T is an
        // integer type or `char` and R is an integer type, the store is a
        // checked narrowing (§5.2) at runtime, never a silent truncation.
        // "In every other case the compound form is a compile error" — most
        // often a binary-float/decimal R against a different-family integer
        // T (`b += 1.5` on `b: byte`; `i += 2.5` on `i: int`; `d += 5.0`
        // where `d: decimal` — double and decimal have no implicit
        // conversion either way). Before this check the mismatch reached
        // codegen unchecked: the checked-arith intrinsic (or the bitwise/
        // shift op) was fed one integer and one floating-point LLVM operand,
        // and LLVM IR verification crashed instead of the frontend rejecting
        // the program. §6.5's bitwise/shift restriction (above) checked only
        // the TARGET's kind against "not valid on bool/char/floating-point";
        // the right-hand OPERAND was unchecked, so e.g. `x &= 1.5` on
        // `x: int` hit the same IR-verification crash — checked here too,
        // now that the right-hand side has a type.
        //
        // §6.6/§7.11 (#2153): the check runs on NULLABLE primitive operands
        // too, on the UNDERLYING primitive types — §7.11 lifts an operator
        // exactly where it is defined on the underlying type, so the §6.2/
        // §6.6 admission is the underlying pair's (`int? += double?` is the
        // same family error as `int += double`), the §6.6 narrow-integer /
        // char write-back applies on the present branch (the #1964/#2140
        // lowering; `byte? += 300` raises OverflowException at runtime), and
        // three nullable-specific rules apply: a nullable RHS with a
        // NON-nullable target never stores back (§5.1's nullable conversions
        // run T → T? only); a `string?` target requires narrowing first
        // (§7.11 is confined to nullable VALUE types, §4.6/§7.4); and a
        // `bool` target has no arithmetic to lift (§3.5) — the plain-`bool`
        // target is rejected by the same new rule (it was excluded from the
        // family check, so `bool += bool` compiled to an i1 add). A NAMED
        // RHS (record/class/enum/interface/choice) is checked after the
        // primitive pair: no built-in operator exists between a primitive
        // and a named type (§6.2's promotion covers primitives), so it is
        // an error unless the RHS type declares an applicable §25.2
        // overload. Before #2153 the `!nullable`/primitive-RHS gate skipped
        // ALL of these; they reached codegen and produced garbage values
        // (`decimal? += record?` reinterpreted the record's bits as a
        // decimal; `int? += bool?` sign-extended the i1), runtime segfaults
        // (`string?(null) += "x"`), or malformed-IR aborts (`int += int?`).
        if (!eventPayload && as->target && as->value &&
            (as->op == "+=" || as->op == "-=" || as->op == "*=" ||
             as->op == "/=" || as->op == "&=" || as->op == "|=" ||
             as->op == "^=" || as->op == "<<=" || as->op == ">>=")) {
            TypeRef* tTy = ctx_.exprType(*as->target);
            TypeRef* rTy = ctx_.exprType(*as->value);
            using P = PrimitiveType;
            // Diagnostic-only spelling of a primitive type; mirrors
            // primitiveSpelling (ExprTypecheck_Exprs.cpp, anonymous
            // namespace, not visible here).
            auto spell = [](P p) -> const char* {
                switch (p) {
                    case P::Bool:    return "bool";
                    case P::Char:    return "char";
                    case P::String:  return "string";
                    case P::Byte:    return "byte";
                    case P::SByte:   return "sbyte";
                    case P::Short:   return "short";
                    case P::UShort:  return "ushort";
                    case P::Int:     return "int";
                    case P::UInt:    return "uint";
                    case P::Long:    return "long";
                    case P::ULong:   return "ulong";
                    case P::Float:   return "float";
                    case P::Double:  return "double";
                    case P::Decimal: return "decimal";
                    case P::Object:  return "Object";
                    case P::None:    return "<none>";
                }
                return "<unknown>";
            };
            // §6.6/§7.11 (#2153): spell a primitive operand WITH its own
            // nullability (`int?`) so the diagnostics name the types as
            // written; non-nullable operands keep the plain spelling (the
            // #1689 messages are unchanged).
            auto spellN = [&spell](const TypeRef* t) {
                return std::string(spell(t->primitive)) +
                       (t->nullable ? "?" : "");
            };
            // §6.2/§6.6 (#2155): a literal-derived RHS is "adoptable" — §5.1's
            // constant narrowing lets a compile-time constant implicitly
            // convert to any integer type whose range contains its value, so
            // `uint += 1` (RHS literal `1`) is admissible even though `int`
            // and `uint` have no implicit widening in either direction. The
            // binary op's §6.2 path (typeBinary) treats such an RHS the same
            // way; mirror that here so the §6.2 reject below fires only for a
            // NON-literal RHS (a variable or call result), matching the
            // issue's reproducers (`int += ulong`, `ulong &= int`). Mirrors
            // isLiteralDerivedExpr (ExprTypecheck_Exprs.cpp); the LHS target
            // is always an lvalue, so only the RHS is tested.
            const std::function<bool(const ast::Expr*)> isLitDerived =
                [&](const ast::Expr* e) -> bool {
                if (!e) return false;
                if (dynamic_cast<const ast::IntLiteralExpr*>(e)) return true;
                if (dynamic_cast<const ast::FloatLiteralExpr*>(e)) return true;
                if (auto* ue = dynamic_cast<const ast::UnaryExpr*>(e))
                    return isLitDerived(ue->operand.get());
                if (auto* be = dynamic_cast<const ast::BinaryExpr*>(e))
                    return isLitDerived(be->left.get()) &&
                           isLitDerived(be->right.get());
                return false;
            };
            if (tTy && rTy && tTy->kind == TypeRef::Kind::Primitive &&
                rTy->kind == TypeRef::Kind::Primitive) {
                // §6.6/§7.11/§5.1 (#2153): a nullable RHS with a NON-nullable
                // target can never store back — the lifted `x op e` result is
                // nullable wherever it is defined at all (§7.11: a mixed
                // expression "lifts as well … and the result is `T?`"), and
                // §5.1's nullable conversions run T → T? only, never back.
                // Applies to all nine operator forms; reported below once the
                // operand KINDS have passed their own checks.
                auto reportNullableRhs = [&]() {
                    std::string binOp(as->op.substr(0, as->op.size() - 1));
                    ctx_.diagnostics().report(
                        Severity::Error, as->value->span.begin,
                        std::string("compound assignment `") +
                        std::string(as->op) +
                        "` has a nullable right-hand operand (`" +
                        spellN(rTy) + "`) with a non-nullable target (`" +
                        spellN(tTy) + "`): the result of `" + spellN(tTy) +
                        " " + binOp + " " + spellN(rTy) + "` is nullable "
                        "where defined (§7.11) and cannot store back into "
                        "the non-nullable target — narrow the right-hand "
                        "operand first (`?? <fallback>`, `!`, or a §7.4 "
                        "check) (§6.6)");
                };
                // §6.2/§6.6 (#2155): the compound desugaring `x op= e` →
                // `x := x op e` (§6.6) inherits the binary op's §6.2
                // admissibility — if there is no implicit conversion between
                // the operand types in either direction (§5.1: "never
                // implicit: between signed and unsigned types of the same or
                // narrower width", and between decimal and binary floats),
                // the binary `x op e` is itself a §6.2 compile error, so the
                // compound form is too. Used by both the bitwise (non-shift)
                // branch and the arithmetic branch below.
                auto reportInadmissiblePair = [&]() {
                    std::string binOp(as->op.substr(0, as->op.size() - 1));
                    ctx_.diagnostics().report(
                        Severity::Error, as->value->span.begin,
                        std::string("compound assignment `") +
                        std::string(as->op) +
                        "` has no implicit conversion between operand types `" +
                        spellN(tTy) + "` and `" + spellN(rTy) +
                        "` — the desugared `" + spellN(tTy) + " " + binOp +
                        " " + spellN(rTy) + "` is itself a §6.2 compile "
                        "error (no implicit conversion in either direction "
                        "per §5.1), so the compound form is too; add an "
                        "explicit cast (§5.2) to convert one operand to the "
                        "other's type (§6.2/§6.6)");
                };
                bool isShift = (as->op == "<<=" || as->op == ">>=");
                bool isBitwise = (as->op == "&=" || as->op == "|=" ||
                                  as->op == "^=");
                if (isShift || isBitwise) {
                    // §6.5: both operands of a bitwise/shift op must be
                    // (true) integer types — never bool, char, string, or
                    // floating-point. The target side is checked above;
                    // this is the missing right-hand-operand half of the
                    // same rule.
                    const char* badKind =
                        (rTy->primitive == P::Float ||
                         rTy->primitive == P::Double ||
                         rTy->primitive == P::Decimal)
                            ? "floating-point types"
                        : (rTy->primitive == P::Bool)
                            ? "`bool` (boolean operations use "
                              "`and`/`or`/`not`)"
                        : (rTy->primitive == P::Char)   ? "`char`"
                        : (rTy->primitive == P::String) ? "`string`"
                                                         : nullptr;
                    if (badKind)
                        ctx_.diagnostics().report(
                            Severity::Error, as->value->span.begin,
                            std::string("operator `") + std::string(as->op) +
                            "` is not valid on " + badKind + " — bitwise and "
                            "shift operators require integer operands "
                            "(§6.5)");
                    // §6.6/§7.11 (#2153): an integer-kinded NULLABLE RHS with
                    // a non-nullable target (`int &= long?`, `int <<= int?`)
                    // has no storable result — same rule as the arithmetic
                    // forms below.
                    else if (rTy->nullable && !tTy->nullable)
                        reportNullableRhs();
                    // §6.2/§6.6 (#2155): the bitwise (non-shift) forms
                    // (`&= |= ^=`) carry the SAME pair-admissibility as the
                    // binary `&`/`|`/`^` — typeBinary runs the §6.2 promotion
                    // for the non-shift bitwise ops, so `ulong &= int` must
                    // be the same §6.2 error as `ulong & int`. (Shifts are
                    // exempt: the count "does not participate in the
                    // mixed-type promotion of §6.2", §6.5.) Both operands
                    // already passed the integer-kind check above, so the
                    // §5.1 "no conversion either way" pair is the only
                    // remaining §6.2 reject. A literal-derived RHS is exempt
                    // (§5.1 constant narrowing: `uint &= 1` admits). Before
                    // this the bitwise branch checked operand KINDS only, so
                    // `ulong &= int` compiled and stored a value under a
                    // conversion the spec does not define.
                    else if (!isShift &&
                             tTy->primitive != rTy->primitive &&
                             !primitiveWidens(tTy->primitive,
                                              rTy->primitive) &&
                             !primitiveWidens(rTy->primitive,
                                              tTy->primitive) &&
                             !isLitDerived(as->value.get()))
                        reportInadmissiblePair();
                    // A shift's result is always the left operand's type
                    // (§6.5) — once the operand-kind check above passes
                    // there is no R-vs-T mismatch left to check.
                } else if (tTy->primitive == P::Bool) {
                    // §6.6/§3.5 (#2153): no arithmetic is defined on `bool`
                    // (§3.5 lists the numeric types only), so `bool op= e`
                    // has no result to store — and §7.11 lifts an operator
                    // only where the underlying type defines it, so a
                    // `bool?` target is rejected identically. Previously the
                    // family check EXCLUDED a bool target: `bool += bool`
                    // compiled to an i1 add and raised OverflowException at
                    // RUNTIME instead of being rejected here.
                    ctx_.diagnostics().report(
                        Severity::Error, as->span.begin,
                        std::string("compound assignment `") +
                        std::string(as->op) + "` is not valid on a `" +
                        spellN(tTy) + "` target — the arithmetic operators "
                        "are not defined on `bool` (§3.5); boolean "
                        "combinations use `and`/`or`/`not` (§17.2) (§6.6)");
                } else if (tTy->primitive == P::String && tTy->nullable) {
                    // §6.6/§4.6/§7.4 (#2153): a `string?` target — §7.11's
                    // lifting is confined to nullable VALUE types and "does
                    // not extend" to `string` (§4.6 states the boundary for
                    // ordering; §7.4's use-requires-narrowing discipline
                    // governs every other operation on a nullable
                    // reference). Previously `string?(null) += "x"` compiled
                    // and the #857 concat path dereferenced the null string
                    // pointer — a runtime segfault.
                    ctx_.diagnostics().report(
                        Severity::Error, as->span.begin,
                        "compound assignment on a `string?` target requires "
                        "a non-nullable `string` — §7.11's lifting covers "
                        "nullable value types only and does not extend to "
                        "`string`; narrow the target first (§7.4) (§6.6)");
                } else if (rTy->nullable && !tTy->nullable) {
                    // §6.6/§7.11 (#2153): nullable RHS, non-nullable target
                    // (`int += int?`, `string += int?`) — see reportNullableRhs.
                    reportNullableRhs();
                } else if (tTy->primitive == P::String) {
                    // §6.6/§4.6 (#2157): a non-nullable STRING target. The
                    // only compound operator defined on strings is `+=`
                    // (concatenation, §4.6: "`s + t` concatenates two
                    // strings"), and it requires a `string` RHS — "The
                    // compiler does not implicitly convert other types to
                    // `string`; use interpolation or `.ToString()`." The
                    // bitwise/shift forms on a string target were already
                    // rejected by the §6.5 target-kind guard above. The
                    // desugaring `s op= e` → `s := s op e` (§6.6) makes the
                    // compound form's admission identical to the binary
                    // `s op e`: `s += 5` matches the §4.6 reject of `s + 5`,
                    // and `s -= t` / `s *= 1` / `s /= 1` have no result (no
                    // `-`/`*`/`/` is defined on strings, §4.6/§3.5). Before
                    // this check the arithmetic branch skipped a String
                    // target entirely (the old `else if (tTy->primitive !=
                    // P::String)` guard): `s += 5` was silently admitted and
                    // the #857 codegen path's retired `string + T` rule
                    // rendered the int, and `s -= t` reached codegen and
                    // ABORTED — `llvm.ssub.with.overflow` on string pointers
                    // failed IR verification.
                    if (as->op != "+=") {
                        std::string binOp(as->op.substr(0, as->op.size() - 1));
                        ctx_.diagnostics().report(
                            Severity::Error, as->value->span.begin,
                            std::string("compound assignment `") +
                            std::string(as->op) + "` is not defined on a "
                            "`string` target — only `+` (concatenation) is "
                            "defined on strings (§4.6); the desugared `s " +
                            binOp + " t` has no result (§4.6/§6.6)");
                    } else if (rTy->primitive != P::String) {
                        ctx_.diagnostics().report(
                            Severity::Error, as->value->span.begin,
                            std::string("compound assignment `+=` on a "
                            "`string` target requires a `string` right-hand "
                            "operand — the compiler does not implicitly "
                            "convert other types to `string` (§4.6); use "
                            "string interpolation, `.ToString()`, or "
                            "concatenate an explicit `string` (§4.6/§6.6)"));
                    }
                    // else: `s += "x"` (string += string) — admitted, the
                    // §4.6 concatenation form (codegens via the #857 path).
                } else {
                    // Arithmetic (`+= -= *= /=`) or a non-shift bitwise op
                    // whose operands both passed the integer-kind check
                    // above. Compute R = type of `target op value` per
                    // §6.2's promotion table (mirrors typeBinary's non-shift
                    // block, ExprTypecheck_Exprs.cpp): identical primitives
                    // keep that type; otherwise the narrower operand
                    // promotes to whichever of the two admits it.
                    P rPrim = tTy->primitive;
                    if (tTy->primitive != rTy->primitive) {
                        if (primitiveWidens(tTy->primitive, rTy->primitive))
                            rPrim = rTy->primitive;
                        else if (primitiveWidens(rTy->primitive,
                                                  tTy->primitive))
                            rPrim = tTy->primitive;
                        else
                            rPrim = rTy->primitive;  // no conversion either way
                    }
                    bool rEqualsT = rPrim == tTy->primitive;
                    bool rConvertsToT =
                        !rEqualsT && primitiveWidens(rPrim, tTy->primitive);
                    auto isTrueInt = [](P p) {
                        switch (p) {
                            case P::Byte: case P::SByte: case P::Short:
                            case P::UShort: case P::Int: case P::UInt:
                            case P::Long: case P::ULong:
                                return true;
                            default:
                                return false;
                        }
                    };
                    bool integerWriteBack =
                        !rEqualsT && !rConvertsToT &&
                        (isTrueInt(tTy->primitive) ||
                         tTy->primitive == P::Char) &&
                        isTrueInt(rPrim);
                    // §6.2/§6.6 (#2155): the integer/integer (or char/
                    // integer) pair with "no conversion either way" (the
                    // rPrim fall-through above) is §6.2-inadmissible — e.g.
                    // `int`/`ulong`, `long`/`uint`, `sbyte`/`uint` (§5.1:
                    // "never implicit: between signed and unsigned types of
                    // the same or narrower width"). integerWriteBack would
                    // otherwise admit ANY such integer/integer pair, so
                    // reject the §6.2-inadmissible ones here (mirroring the
                    // binary op's §6.2 reject, typeBinary). The legitimate
                    // narrow-integer write-back (`b += 10` on `byte`,
                    // `i += long` on `int`, `c += 1` on `char`) and a
                    // literal-derived RHS (`uint += 1`, §5.1 constant
                    // narrowing) keep an admissible pair and stay admitted.
                    // (#2153: lifted forms like `int? += ulong?` reach this
                    // branch on the underlying primitives and reject
                    // identically.)
                    if (integerWriteBack &&
                        !primitiveWidens(tTy->primitive,
                                         rTy->primitive) &&
                        !primitiveWidens(rTy->primitive,
                                         tTy->primitive) &&
                        !isLitDerived(as->value.get()))
                        reportInadmissiblePair();
                    if (!rEqualsT && !rConvertsToT && !integerWriteBack) {
                        std::string binOp(as->op.substr(0, as->op.size() - 1));
                        // §7.11 (#2153): with a nullable operand the computed
                        // R lifts to `R?`; spell it so (`int? + double?`
                        // computes `double?`). The `T?(x op e)` remedy stays
                        // valid spelling — §5.3's nullable cast is admitted
                        // exactly where the underlying conversion is.
                        std::string rSpell =
                            std::string(spell(rPrim)) +
                            ((tTy->nullable || rTy->nullable) ? "?" : "");
                        ctx_.diagnostics().report(
                            Severity::Error, as->value->span.begin,
                            std::string("compound assignment `") +
                            std::string(as->op) + "` has no valid result: `" +
                            spellN(tTy) + " " + binOp + " " +
                            spellN(rTy) + "` computes `" +
                            rSpell + "`, which is neither `" +
                            spellN(tTy) + "` nor implicitly "
                            "convertible to it, and `" +
                            spellN(tTy) + "` is not an integer/"
                            "char target receiving an integer result (the "
                            "only case §6.6's write-back narrows) — convert "
                            "the right-hand operand explicitly, or write "
                            "`x := " + spellN(tTy) + "(x " + binOp +
                            " e);` if the loss is intended (§6.6)");
                    }
                }
            } else if (tTy && rTy &&
                       tTy->kind == TypeRef::Kind::Primitive &&
                       rTy->kind == TypeRef::Kind::Named && rTy->declaration) {
                // §6.6/§6.2/§25.2 (#2153): a NAMED RHS (record/class/enum/
                // interface/choice) against a primitive-typed target. No
                // built-in operator exists between a primitive and a named
                // type — §6.2's promotion covers primitive numeric operands,
                // and §5.1 never implicitly converts an enum to its
                // underlying integer — so the compound form has no result
                // UNLESS the RHS type declares an applicable operator
                // overload: §25.2 admits "a binary overload may pair the
                // declaring type with any other type in either position",
                // matched IN ORDER (#1096: left operand → param 0, here the
                // primitive; right operand → param 1, the declaring type)
                // including declarations inherited from base classes
                // (§25.2 v0.181), with findBinaryOperator's §7.11
                // nullability gate (#1132: a nullable operand matches only a
                // nullable parameter — user-operator lifting is deferred).
                // When such an overload exists the compound stays admitted
                // here and desugars through it (§25.1). A Named RHS with NO
                // declaration (an unresolved name or a type parameter) is
                // left unchecked, conservatively, as before.
                std::string binOp(as->op.substr(0, as->op.size() - 1));
                bool hasApplicableOp = false;
                for (MethodSymbol* m : collectInheritedOperators(
                         rTy->declaration, binOp, 2)) {
                    auto paramTy = [&](Symbol* paramSym) -> TypeRef* {
                        auto* ps = dynamic_cast<ParameterSymbol*>(paramSym);
                        if (!ps) return nullptr;
                        return ps->declType
                                   ? ctx_.resolvedType(*ps->declType)
                                   : ps->resolvedDeclType;
                    };
                    TypeRef* p0 = paramTy(m->params[0]);
                    TypeRef* p1 = paramTy(m->params[1]);
                    if (!p0 || !p1) continue;
                    // Mirrors operandMatchesParam (ExprTypecheck_Exprs.cpp):
                    // a primitive param accepts a primitive operand (numeric
                    // coercion downstream); a Named param needs the operand's
                    // declaration (or a derived one) with nullability
                    // agreement.
                    bool leftOk = p0->kind == TypeRef::Kind::Primitive;
                    bool rightOk =
                        p1->kind == TypeRef::Kind::Named && p1->declaration &&
                        (p1->declaration == rTy->declaration ||
                         isSubtype(rTy->declaration, p1->declaration)) &&
                        (p1->nullable || !rTy->nullable);
                    if (leftOk && rightOk) {
                        hasApplicableOp = true;
                        break;
                    }
                }
                if (!hasApplicableOp) {
                    const char* kindWord = "";
                    switch (rTy->declaration->kind) {
                        case ast::TypeDeclKind::Record:
                            kindWord = "record"; break;
                        case ast::TypeDeclKind::Class:
                            kindWord = "class"; break;
                        case ast::TypeDeclKind::Enum:
                            kindWord = "enum"; break;
                        case ast::TypeDeclKind::Interface:
                            kindWord = "interface"; break;
                        case ast::TypeDeclKind::Choice:
                            kindWord = "choice"; break;
                    }
                    std::string rhsName =
                        std::string(rTy->declaration->name) +
                        (rTy->nullable ? "?" : "");
                    std::string tail =
                        rTy->nullable
                            ? std::string(
                                  "user-defined operators do not lift over a "
                                  "nullable operand (§7.11 — deferred) — "
                                  "narrow the operand or convert it "
                                  "explicitly (§6.6)")
                            : "`" + std::string(rTy->declaration->name) +
                                  "` declares no applicable `operator " +
                                  binOp + "` overload (§25.2) — convert the "
                                  "right-hand operand explicitly or declare "
                                  "the operator (§6.6)";
                    ctx_.diagnostics().report(
                        Severity::Error, as->value->span.begin,
                        std::string("compound assignment `") +
                        std::string(as->op) + "` has no valid result: no "
                        "built-in operator `" + binOp + "` is defined "
                        "between `" + spellN(tTy) + "` and " + kindWord +
                        " type `" + rhsName + "` (§6.2's promotion covers "
                        "primitive numeric operands only), and " + tail);
                }
            } else if (tTy && rTy &&
                       tTy->kind == TypeRef::Kind::Named &&
                       tTy->declaration &&
                       (rTy->kind == TypeRef::Kind::Primitive ||
                        (rTy->kind == TypeRef::Kind::Named &&
                         rTy->declaration))) {
                // §6.6/§25.2 (#2162): a NAMED TARGET (record/class/enum/
                // interface/choice) with a compound `op=` desugars to
                // `x := x op e` (§6.6). The binary `x op e` has a result R
                // ONLY via a §25.2 operator overload: §6.2's mixed-type
                // promotion covers primitive numeric operands, §6.5 bitwise
                // is the eight integer primitives, and §5.1 never implicitly
                // converts a named type to a primitive — §10: "An enum is not
                // implicitly convertible to or from its underlying integer
                // type" (§5.4 requires the explicit cast); records/classes/
                // choices have no numeric reading at all. So per §6.6 "In
                // every other case the compound form is a compile error"
                // unless an applicable `operator <op>` overload is declared
                // on the target's type — or, mirroring #2156's codegen
                // candidate set ("the union of the `operator <op>` overloads
                // declared on the static types of BOTH operands"), on the
                // RHS's type when the RHS is also Named. Matched IN ORDER
                // (#1096: left operand → param 0; here the target; right
                // operand → param 1, the RHS) including inherited overloads
                // (§25.2 v0.181) and findBinaryOperator's §7.11 nullability
                // gate (#1132). Before this check a Named TARGET fell through
                // BOTH the primitive-pair branch above AND the primitive-
                // target/Named-RHS branch above, so `x |= Flags.B` (and the
                // arithmetic twin `x += Flags.B`) on `x: Flags` compiled
                // silently and codegen emitted an unspecified value.
                std::string binOp(as->op.substr(0, as->op.size() - 1));
                auto operandMatchesParam = [&](const TypeRef* operandTy,
                                               Symbol* paramSym) -> bool {
                    auto* ps = dynamic_cast<ParameterSymbol*>(paramSym);
                    if (!ps) return false;
                    TypeRef* pt = ps->declType
                                      ? ctx_.resolvedType(*ps->declType)
                                      : ps->resolvedDeclType;
                    if (!pt || !operandTy) return false;
                    // Mirrors operandMatchesParam (ExprTypecheck_Exprs.cpp):
                    // a primitive param accepts a primitive operand (numeric
                    // coercion downstream); a Named param needs the operand's
                    // declaration (or a derived one) with §7.11 nullability
                    // agreement (user-operator lifting over T? is deferred).
                    if (pt->kind == TypeRef::Kind::Primitive)
                        return operandTy->kind == TypeRef::Kind::Primitive;
                    if (pt->kind != TypeRef::Kind::Named || !pt->declaration ||
                        operandTy->kind != TypeRef::Kind::Named ||
                        !operandTy->declaration)
                        return false;
                    return (pt->declaration == operandTy->declaration ||
                            isSubtype(operandTy->declaration,
                                      pt->declaration)) &&
                           (pt->nullable || !operandTy->nullable);
                };
                auto typeDeclaresApplicable = [&](TypeSymbol* decl) -> bool {
                    if (!decl) return false;
                    for (MethodSymbol* m :
                         collectInheritedOperators(decl, binOp, 2)) {
                        if (m->params.size() < 2) continue;
                        if (operandMatchesParam(tTy, m->params[0]) &&
                            operandMatchesParam(rTy, m->params[1]))
                            return true;
                    }
                    return false;
                };
                bool hasApplicableOp =
                    typeDeclaresApplicable(tTy->declaration) ||
                    (rTy->kind == TypeRef::Kind::Named && rTy->declaration &&
                     typeDeclaresApplicable(rTy->declaration));
                if (!hasApplicableOp) {
                    const char* kindWord = "";
                    switch (tTy->declaration->kind) {
                        case ast::TypeDeclKind::Record:
                            kindWord = "record"; break;
                        case ast::TypeDeclKind::Class:
                            kindWord = "class"; break;
                        case ast::TypeDeclKind::Enum:
                            kindWord = "enum"; break;
                        case ast::TypeDeclKind::Interface:
                            kindWord = "interface"; break;
                        case ast::TypeDeclKind::Choice:
                            kindWord = "choice"; break;
                    }
                    std::string targetName =
                        std::string(tTy->declaration->name) +
                        (tTy->nullable ? "?" : "");
                    ctx_.diagnostics().report(
                        Severity::Error, as->span.begin,
                        std::string("compound assignment `") +
                        std::string(as->op) + "` has no valid result on " +
                        kindWord + " type `" + targetName + "`: no built-in "
                        "operator `" + binOp + "` is defined on a named type "
                        "(§6.2/§6.5 cover primitive operands only), and `" +
                        std::string(tTy->declaration->name) +
                        "` declares no applicable `operator " + binOp +
                        "` overload (§25.2) — convert via the underlying "
                        "integer explicitly (§5.4) or declare the operator "
                        "(§6.6/§10.4)");
                }
            }
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
                        } else {
                            // fix #2412 (§23.3, §2.8.2/§2.8.3 rule 4,
                            // v0.235): an IMPORTED PropertySymbol has no
                            // ast::PropertyDecl (`ps->decl` is unconditionally
                            // null — ModuleLoader never reconstructs a
                            // body-less imported property's decl), so the
                            // `pd`-gated check above silently skipped every
                            // imported property, accepting a write to a
                            // getter-only imported property that the
                            // identical same-module shape correctly rejects
                            // (confirmed asymmetry — a #2374-family instance).
                            // Fall back to the hasSetter side table #2400
                            // publishes from MetaMember::hasSetter, which is
                            // false both for a genuinely getter-only property
                            // (expression-bodied or block-getter-only) and
                            // for a readonly auto-property (mirrors
                            // ModuleMetadataBuilder.cpp's export-time
                            // computation) — either way, no assignment is
                            // ever valid on it from outside its declaring
                            // module, so one message covers both source
                            // shapes the same way the local `!hasSetter`
                            // branch's message already does.
                            if (!ctx_.importedPropertyHasSetter(ps)) {
                                ctx_.diagnostics().report(
                                    Severity::Error, as->span.begin,
                                    "cannot assign to read-only property `" +
                                    propName +
                                    "` — it has no setter (§23.3)");
                            } else {
                                // fix #2415 (§13.6, §23.3, §2.8.2 clause 1 /
                                // §2.8.3 rule 4, v0.235): the setter EXISTS
                                // (hasSetter above), but its VISIBILITY was
                                // never enforced across the module boundary —
                                // a `private set` / `protected set` /
                                // `internal set` imported property was
                                // writable from anywhere through the import,
                                // even though the identical same-module shape
                                // is correctly rejected (§13.6: "A property or
                                // indexer's `get` or `set` may carry its own
                                // visibility, which must not be *less*
                                // restrictive than the property's overall
                                // visibility"). `ctx_.
                                // importedPropertySetterVisibility(ps)`
                                // publishes MetaMember::setterVisibility
                                // (#2415), the ALREADY-RESOLVED effective
                                // visibility computed at export time by
                                // mirroring the `sv` computation just above
                                // verbatim.
                                //
                                // Applying the §13.1 access levels across a
                                // module boundary:
                                //   - Private ("the declaring class only"):
                                //     always inaccessible here — `declaring`
                                //     is the imported TypeSymbol reconstructed
                                //     by ModuleLoader, and `currentType_` is
                                //     always a type compiled in THIS
                                //     (importing) module, so the two can never
                                //     be the same symbol — unconditional
                                //     reject.
                                //   - Protected ("the declaring class and all
                                //     classes derived from it", §13.1 — no
                                //     module qualifier): reuse `isSubtype`,
                                //     which already walks `baseTypes` through
                                //     a reconstructed imported base (proven by
                                //     #2400's inherited-setter itable tests),
                                //     so a LOCAL subclass of the imported
                                //     declaring type is still accepted.
                                //   - Internal ("code in the same module",
                                //     §13.1): the importing module is BY
                                //     DEFINITION a different module than the
                                //     declaring one, so this is also an
                                //     unconditional reject — mirroring how
                                //     the local check's `ctx_.isImported(ps)`
                                //     is unconditionally true for every ps
                                //     that reaches this `else` branch at all.
                                //   - Public / None (no restriction, or the
                                //     property's own — already-checked —
                                //     visibility with no accessor override):
                                //     no further restriction.
                                ast::Visibility sv =
                                    ctx_.importedPropertySetterVisibility(ps);
                                TypeSymbol* declaring = ps->parent;
                                bool bad = false;
                                if (sv == ast::Visibility::Private) {
                                    bad = true;
                                } else if (sv == ast::Visibility::Protected) {
                                    bad = !(currentType_ &&
                                            isSubtype(currentType_,
                                                      declaring));
                                } else if (sv == ast::Visibility::Internal) {
                                    bad = true;
                                }
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
                // §5.1 / §9 / §22 (#1770): a bare `this` on the RHS is left
                // deliberately UNTYPED in visitExpr (member access on `this`
                // resolves via the enclosing type directly, not via `this`'s
                // exprType — see the #1759 note there), so exprType(*this) is
                // null and checkAssignable's `if (!from) return` silently
                // ACCEPTED `slot := this` for ANY target type — a store of a
                // record's `this` into a slot of an unrelated record type
                // compiled and ran on garbage memory. The #1759 fix seeds the
                // receiver type for the `var q := this` local path but not for
                // this direct assignment path. Seed `from` with the current
                // receiver type here (a bare Named ref keyed on currentType_,
                // exactly the #1759 local seed) so the §5.1 convertibility gate
                // runs on `this` just as it does for a named-variable RHS —
                // accepting a same-type / base / implemented-interface store and
                // rejecting a mismatched-record store. Codegen carries the
                // value/reference semantics via its own receiver path.
                if (!fromTy && currentType_) {
                    if (auto* vid = dynamic_cast<const ast::IdentifierExpr*>(
                            as->value.get())) {
                        if (vid->name == "this") {
                            auto* selfTy = ctx_.makeTypeRef();
                            selfTy->kind = TypeRef::Kind::Named;
                            selfTy->declaration = currentType_;
                            fromTy = selfTy;
                        }
                    }
                }
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
        // value clears that knowledge.
        //
        // §7.4 v0.210 (issue #2104): `x ??= expr;` establishes the same
        // narrowing as `x := expr;` — "on every path `x` then holds a non-null
        // value" (§7.7): on the was-null path `x` becomes `expr`, on the
        // was-non-null path `x` is unchanged and already non-null — so `x` is
        // non-null after the statement exactly when `expr`'s static type is
        // non-nullable. A nullable RHS does not narrow (the was-null-then-null
        // path leaves `x` null), which `narrowOnAssignment` handles by clearing.
        // The arithmetic/bitwise compound family (`+=`, `-=`, `*=`, `/=`, `&=`,
        // `|=`, `^=`, `<<=`, `>>=`) is NOT included: those keep the LHS's
        // existing nullability, so they don't change the analysis.
        if ((as->op == ":=" || as->op == "??=") && as->target && as->value) {
            if (auto* tid = dynamic_cast<const ast::IdentifierExpr*>(
                    as->target.get())) {
                if (tid->name != "this" && tid->name != "base") {
                    if (Symbol* tsym = lookupName(tid->name))
                        narrowOnAssignment(tsym, ctx_.exprType(*as->value));
                }
            }
        } else if (as->target) {
            // §7.4 (v0.214, #2116 pre-gate review): the arithmetic/bitwise
            // compound family (`+=` …) desugars to `lhs := lhs <op> rhs`
            // (§6.6) — a REASSIGNMENT rebinding the target to the operator's
            // result; a user `operator +` (§25.1) can return an object whose
            // readonly field's value differs from the old referent's. The
            // legacy null/type sets are correctly unaffected (nullability is
            // preserved by the family), but a `local._f` fact rides the OLD
            // referent, so it must drop — the spec's "reassigning `local`
            // drops it" covers every reassignment form. (Probe-confirmed
            // SEGV without this: `+=` to an operator returning a null-field
            // instance, then the stale-fact-admitted `a._f.M`.)
            if (auto* tid = dynamic_cast<const ast::IdentifierExpr*>(
                    as->target.get())) {
                if (tid->name != "this" && tid->name != "base")
                    if (Symbol* tsym = lookupName(tid->name))
                        dropFieldFactsRooted(tsym);
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
        // §7.4 (v0.214, #2085/#2116): readonly-field facts flow through the
        // branches and join like `nullNarrowed_`, with two hygiene upgrades
        // the legacy sets lack (their absence for LOCALS is the pre-existing
        // #2164 hole family, filed from this lane):
        //   * each elif CONDITION is visited under its own correct state —
        //     the running condition-context (`fieldCondState`) plus the
        //     preceding conditions' false-branch facts — never the preceding
        //     branch BODY's leftovers;
        //   * branch entries derive from `fieldCondState`, the state as the
        //     condition visits LEFT it — a drop performed inside a condition
        //     expression (a `ref` argument rebinding a fact's root) stays
        //     dropped; resetting to the statement's entry would resurrect it.
        // All operations are set algebra over integer keys — no decision
        // depends on container iteration order (#2116).
        // Collected exit sets of each fall-through path; null pointer means "no
        // path here" (always-exits). We intersect the non-null ones.
        std::vector<std::unordered_set<Symbol*>> fallThroughExits;
        std::vector<std::set<std::pair<std::uint32_t, std::uint32_t>>>
            fieldExits;
        if (ifs->ifBranch.condition) {
            visitExpr(*ifs->ifBranch.condition);
            checkConditionIsBool(*ifs->ifBranch.condition, "if");
        }
        // The running condition-context: entry minus every drop the condition
        // visits performed so far (conditions can only DROP field facts —
        // establishment happens via collectNarrowings below, never inside a
        // plain expression visit).
        auto fieldCondState = fieldNarrowed_;
        nullNarrowed_ = entryNullNarrowed;
        fieldNarrowed_ = fieldCondState;
        collectNarrowings(ifs->ifBranch.condition.get(), /*forTrueBranch=*/true);
        for (const auto& bs : ifs->ifBranch.body) {
            if (bs) visitStmt(*bs);
        }
        if (!bodyAlwaysExits(ifs->ifBranch.body)) {
            fallThroughExits.push_back(nullNarrowed_);
            fieldExits.push_back(fieldNarrowed_);
        }
        narrowings_ = savedNarrowings;
        for (std::size_t i = 0; i < ifs->elifBranches.size(); ++i) {
            const auto& el = ifs->elifBranches[i];
            // §7.4 (v0.214, #2116): this elif CONDITION executes only when
            // every preceding condition was false. Seed its state BEFORE the
            // visit: the running condition-context plus the immediately
            // preceding condition's false-branch facts (earlier ones are
            // already folded in from previous iterations). The visit's own
            // drops then advance the running context.
            //
            // fix #2164 / §7.4: also seed `nullNarrowed_` to entry +
            // preceding conditions' false-branch facts BEFORE the visit.
            // The legacy local set was reset AFTER `visitExpr(*el.condition)`
            // (line ~10366 historically), so member-access gates inside the
            // elif condition judged against the THEN-body's exit state
            // (compile-accepted null deref that SIGSEGV'd at runtime). The
            // `else`-branch narrowing below correctly admits the sound
            // `if x = null then return 0; elif x.Length > 0 …` ACCEPT —
            // entry + prior false-branch facts sees x nullable, and the
            // else-branch narrowing seeds x non-null for the elif BODY.
            fieldNarrowed_ = fieldCondState;
            collectFieldNarrowings(
                i == 0 ? ifs->ifBranch.condition.get()
                       : ifs->elifBranches[i - 1].condition.get(),
                /*forTrueBranch=*/false);
            nullNarrowed_ = entryNullNarrowed;
            collectNarrowings(ifs->ifBranch.condition.get(),
                              /*forTrueBranch=*/false);
            for (std::size_t j = 0; j < i; ++j)
                collectNarrowings(ifs->elifBranches[j].condition.get(),
                                  /*forTrueBranch=*/false);
            if (el.condition) {
                visitExpr(*el.condition);
                checkConditionIsBool(*el.condition, "elif");
            }
            fieldCondState = fieldNarrowed_;
            // This elif is reached only when every preceding condition was
            // false (their else-narrowings hold) and this condition is true.
            nullNarrowed_ = entryNullNarrowed;
            fieldNarrowed_ = fieldCondState;
            collectNarrowings(ifs->ifBranch.condition.get(),
                              /*forTrueBranch=*/false);
            for (std::size_t j = 0; j < i; ++j)
                collectNarrowings(ifs->elifBranches[j].condition.get(),
                                  /*forTrueBranch=*/false);
            collectNarrowings(el.condition.get(), /*forTrueBranch=*/true);
            for (const auto& bs : el.body) if (bs) visitStmt(*bs);
            if (!bodyAlwaysExits(el.body)) {
                fallThroughExits.push_back(nullNarrowed_);
                fieldExits.push_back(fieldNarrowed_);
            }
            narrowings_ = savedNarrowings;
        }
        // The else body (explicit or implicit fall-through): all preceding
        // guards were false, so each contributes its else-branch narrowing.
        nullNarrowed_ = entryNullNarrowed;
        fieldNarrowed_ = fieldCondState;  // §7.4 #2116: drop-respecting base
        collectNarrowings(ifs->ifBranch.condition.get(), /*forTrueBranch=*/false);
        for (const auto& el : ifs->elifBranches)
            collectNarrowings(el.condition.get(), /*forTrueBranch=*/false);
        for (const auto& bs : ifs->elseBody) if (bs) visitStmt(*bs);
        // The else path falls through unless an explicit else body always exits.
        if (!ifs->hasElse || !bodyAlwaysExits(ifs->elseBody)) {
            fallThroughExits.push_back(nullNarrowed_);
            fieldExits.push_back(fieldNarrowed_);
        }
        narrowings_ = savedNarrowings;
        // After-if nullability = intersection of all fall-through exit sets.
        if (fallThroughExits.empty()) {
            // Every path exits the method; the after-if point is unreachable.
            // Keep the entry set (harmless — nothing follows). For the field
            // facts use the drop-respecting condition-context instead of the
            // entry set (the code after this point is runtime-unreachable
            // either way, but never resurrect a dropped fact).
            nullNarrowed_ = entryNullNarrowed;
            fieldNarrowed_ = std::move(fieldCondState);
        } else {
            std::unordered_set<Symbol*> joined = fallThroughExits.front();
            for (std::size_t i = 1; i < fallThroughExits.size(); ++i) {
                std::unordered_set<Symbol*> next;
                for (Symbol* sym : joined)
                    if (fallThroughExits[i].count(sym)) next.insert(sym);
                joined = std::move(next);
            }
            nullNarrowed_ = std::move(joined);
            // §7.4 (v0.214, #2085/#2116): the same all-paths intersection over
            // the field facts (fieldExits is parallel to fallThroughExits).
            fieldNarrowed_ = std::move(fieldExits.front());
            for (std::size_t i = 1; i < fieldExits.size(); ++i)
                intersectFieldFactsWith(fieldExits[i]);
        }
    } else if (auto* ws = dynamic_cast<const ast::WhileStmt*>(&s)) {
        if (ws->condition) {
            visitExpr(*ws->condition);
            checkConditionIsBool(*ws->condition, "while");
        }
        checkLoopLabelShadow(ws->label, s.span.begin);   // §21.5 (v0.201)
        loopLabelStack_.push_back(ws->label);
        ++loopDepth_;
        // §7.4 (#2118): a `while` body may execute zero times, so a narrowing
        // established INSIDE the body (`x := nonNull;`, `x ??= nonNull;`, or an
        // early-return null guard) must NOT leak past the loop — on the
        // zero-iteration path the variable was never proved non-null. Capture
        // the entry narrowing state, walk the body, then join to the
        // intersection of entry and body-exit (see intersectNarrowingSetsAfterLoop).
        // §7.4 (#2152): push a per-loop edge collector before the body walk so
        // each `break`/`continue` in the body can snapshot its state; the join
        // folds those edges in alongside the entry ∩ body-exit intersection.
        auto whileEntryNull = nullNarrowed_;
        auto whileEntryNarrowings = narrowings_;
        auto whileEntryField = fieldNarrowed_;  // §7.4 #2116
        loopEdgeNarrowing_.emplace_back();
        for (const auto& bs : ws->body) if (bs) visitStmt(*bs);
        intersectNarrowingSetsAfterLoop(whileEntryNull, whileEntryNarrowings,
                                        whileEntryField,
                                        loopEdgeNarrowing_.back());
        loopEdgeNarrowing_.pop_back();
        --loopDepth_;
        loopLabelStack_.pop_back();
    } else if (auto* fs = dynamic_cast<const ast::ForStmt*>(&s)) {
        if (fs->iterable) visitExpr(*fs->iterable);
        if (fs->withExpr) visitExpr(*fs->withExpr);
        // §21.4 / §29 (#1775): a synchronous `for var x in <source>` requires an
        // ITERABLE source — a value whose type implements `IEnumerable<T>`
        // (exposes `GetEnumerator`, §29), an inline array (`[N]T`, §15.1.2), a
        // string (§4.6), or a `Range<T>`. A non-iterable source (a bare
        // `int`/`bool`/`char`, or a plain class/record/enum that does not
        // implement `IEnumerable<T>`) otherwise slips past the type checker and
        // falls through every codegen for-shape handler to a "codegen for this
        // for statement" NYI. Diagnose it here instead.
        //
        // Conservative — only DEFINITE non-iterables are reported:
        //   • a non-`string` primitive is never iterable;
        //   • a LOCALLY-declared user type (`declaration->decl != null`) whose
        //     base closure has neither `GetEnumerator` nor an IEnumerable-family
        //     base is not iterable.
        // Everything else is left to the existing element-inference and codegen
        // for-shape handlers: builtin collections/interfaces and `Range<T>`
        // (synthetic `builtinClassSym` declarations carry `decl==null`),
        // imported types, the lazy-LINQ marker TypeRef, range/collection
        // literals (handled by AST shape below), and any source whose type did
        // not resolve. `await for` (§30.7) iterates an async source through a
        // separate path, so it is excluded here.
        if (!fs->isAwait && fs->iterable &&
            !dynamic_cast<const ast::RangeExpr*>(fs->iterable.get()) &&
            !dynamic_cast<const ast::CollectionLiteralExpr*>(
                fs->iterable.get())) {
            bool nonIterable = false;
            if (const TypeRef* ty = ctx_.exprType(*fs->iterable)) {
                if (ty->kind == TypeRef::Kind::Primitive) {
                    // §4.6: a string iterates its chars; every other primitive
                    // (int, bool, char, float, …) is not iterable.
                    nonIterable = ty->primitive != PrimitiveType::String;
                } else if (ty->kind == TypeRef::Kind::Named &&
                           ty->declaration && ty->declaration->decl) {
                    // Restrict the reject to genuinely USER-authored types. Some
                    // builtins carry a SYNTHESIZED `decl` (via
                    // builtinInterfaceSym): `IReadOnlySpan<T>` — which IS
                    // for-in iterable (§4.16/#1262, a direct indexed loop) —
                    // and the §2.19/§11.8 static-abstraction / comparison
                    // interfaces. Those must not be treated as plain
                    // non-iterable classes. (`Range`, the collections, and the
                    // value builtins carry `decl==null` and were already skipped
                    // by the guard above.)
                    std::string_view tn = ty->declaration->name;
                    bool builtin =
                        common::isHeapPointerReferenceBuiltin(tn) ||
                        isValueBuiltinByName(tn) ||
                        tn == "IReadOnlySpan" || tn == "INumber" ||
                        tn == "ISteppable" || tn == "IParsable" ||
                        tn == "IComparable" || tn == "IEquatable" ||
                        tn == "IEqualityComparer";
                    if (!builtin && !userTypeSupportsForIn(ty->declaration))
                        nonIterable = true;
                }
                // InlineArray → iterable; Named builtins / imported types → left
                // to the existing paths.
            }
            if (nonIterable) {
                ctx_.diagnostics().report(
                    Severity::Error, fs->span.begin,
                    "`for` requires an iterable source: a type implementing "
                    "`IEnumerable<T>`, an inline array, a string, or a "
                    "`Range<T>` (\xC2\xA7""21.4); this source is not iterable");
            }
        }
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
        checkLoopLabelShadow(fs->label, s.span.begin);   // §21.5 (v0.201)
        loopLabelStack_.push_back(fs->label);
        ++loopDepth_;
        // §7.4 (#2118): a `for` body may execute zero times (an empty range or
        // empty collection), so a narrowing established INSIDE the body must NOT
        // leak past the loop — same reasoning as the `while` handler above. Join
        // to the intersection of the entry and body-exit narrowing state.
        // §7.4 (#2152): push a per-loop edge collector before the body walk so
        // each `break`/`continue` in the body can snapshot its state; the join
        // folds those edges in alongside the entry ∩ body-exit intersection.
        auto forEntryNull = nullNarrowed_;
        auto forEntryNarrowings = narrowings_;
        auto forEntryField = fieldNarrowed_;  // §7.4 #2116
        loopEdgeNarrowing_.emplace_back();
        for (const auto& bs : fs->body) if (bs) visitStmt(*bs);
        intersectNarrowingSetsAfterLoop(forEntryNull, forEntryNarrowings,
                                        forEntryField,
                                        loopEdgeNarrowing_.back());
        loopEdgeNarrowing_.pop_back();
        --loopDepth_;
        loopLabelStack_.pop_back();
        currentScope_ = saveScope;
    } else if (auto* brk = dynamic_cast<const ast::BreakStmt*>(&s)) {
        // Slice S26 (§21.5): `break` is legal only inside a loop.
        if (loopDepth_ == 0) {
            ctx_.diagnostics().report(
                Severity::Error, s.span.begin,
                "`break` is not allowed outside a loop (§21.5)");
        } else {
            checkLoopJumpTarget(brk->label, /*isBreak=*/true,
                                const_cast<ast::BreakStmt*>(brk)->targetLoopOffset,
                                s.span.begin);
            // §7.4 (#2152): a `break` exits the loop, so the narrowing state at
            // the jump point is a possible after-loop state — snapshot it into
            // the target loop's collector for the after-loop join.
            recordLoopEdgeNarrowing(brk->targetLoopOffset);
        }
    } else if (auto* cont = dynamic_cast<const ast::ContinueStmt*>(&s)) {
        if (loopDepth_ == 0) {
            ctx_.diagnostics().report(
                Severity::Error, s.span.begin,
                "`continue` is not allowed outside a loop (§21.5)");
        } else {
            checkLoopJumpTarget(cont->label, /*isBreak=*/false,
                                const_cast<ast::ContinueStmt*>(cont)->targetLoopOffset,
                                s.span.begin);
            // §7.4 (#2152): a `continue` jumps to the cond re-check, which on
            // cond-false exits the loop, so its state is also a possible
            // after-loop state — snapshot it into the target loop's collector.
            recordLoopEdgeNarrowing(cont->targetLoopOffset);
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
        // §7.4 (v0.214, #2116): a catch arm runs when the try body was
        // interrupted at ANY point — possibly before a fact-establishing guard
        // executed — so a readonly-field fact established INSIDE the try body
        // must not be visible to the catch arms or (unless every catch exit
        // also carries it) past the try. Catch-arm entry state is
        // entry ∩ after-body (an entry fact the body INVALIDATED stays
        // dropped — the invalidating statement may have run before the
        // throw); the after-try state is after-body ∩ each catch arm's exit.
        //
        // fix #2164 / §7.4: the legacy `nullNarrowed_` set has the SAME
        // hygiene need — a guard inside the body (`if x = null then return 0;
        // end;`) established x non-null, and the catch arm `return x.Length;`
        // inherited that fact. But the catch arm runs when the body was
        // INTERRUPTED — possibly before the guard executed, when x is still
        // nullable — so the compile-ACCEPT turns into a null deref SIGSEGV.
        // Apply the same discipline to the local set: catch-arm entry is
        // entry ∩ after-body (no body-establishments, no body-drops
        // resurrected); after-try is after-body ∩ each catch exit. Set
        // algebra — no container-iteration dependence. // per §7.4, #2116,
        // #2164
        auto tryEntryField = fieldNarrowed_;
        auto tryEntryNull = nullNarrowed_;
        const std::uint64_t tryDropEpoch0 = fieldFactDropEpoch_;
        for (const auto& bs : trys->body) if (bs) visitStmt(*bs);
        auto afterBodyField = fieldNarrowed_;
        auto tryJoinField = afterBodyField;
        auto afterBodyNull = nullNarrowed_;
        // Catch-arm running base (#2116 pre-gate review, hardened): the body
        // can be interrupted at ANY point, and the exit-state intersection
        // entry ∩ after-body is BLIND to an interior drop that a later
        // re-establishment masks (`a := other; MayThrow(); if a._f = null
        // then return; end;` — the exit state carries the fact the
        // interrupted path lost; probe-confirmed SEGV). Entry facts are
        // therefore carried only when NO drop edge fired during the body
        // visit (fieldFactDropEpoch_ unchanged — then every entry fact is
        // valid at every interrupt point); otherwise only AMBIENT entry
        // facts (rootId 0 — never droppable) survive. Establishments inside
        // the body never enter (a throw may precede the establishing guard);
        // the running base is further shrunk by each earlier arm's exit (an
        // earlier arm's GUARD may evaluate at runtime before a later arm is
        // tried, so its drops must persist).
        auto catchRunningField = tryEntryField;
        if (fieldFactDropEpoch_ != tryDropEpoch0)
            catchRunningField.erase(catchRunningField.lower_bound({1u, 0u}),
                                    catchRunningField.end());
        for (auto it = catchRunningField.begin();
             it != catchRunningField.end();) {
            if (afterBodyField.count(*it)) ++it;
            else it = catchRunningField.erase(it);
        }
        // §7.4 #2164: the legacy local set has no drop-epoch tracking, so the
        // catch-arm entry is the conservative (entry ∩ after-body) intersection
        // — entry facts that the body KEPT (no interior drop on them — they
        // were present at entry and the body did not erase them). Body
        // establishments never enter (a throw may precede the establishing
        // guard; probe-confirmed SEGV in #2164). The legacy set only grows on
        // positive evidence and is erased only by explicit reassignment / mutable-
        // capture — the simpler intersection (no drop-edge carved-out) matches
        // the legacy semantics: a fact x in `tryEntryNull` AND `afterBodyNull`
        // holds at the catch-arm entry; anything established inside the body
        // (afterBodyNull − tryEntryNull) is filtered out.
        auto catchRunningNull = tryEntryNull;
        for (auto it = catchRunningNull.begin();
             it != catchRunningNull.end();) {
            if (afterBodyNull.count(*it)) ++it;
            else it = catchRunningNull.erase(it);
        }
        // §7.4 #2164: the after-try join is after-body ∩ every catch's
        // exit. Initialized to afterBodyNull once, then shrunken by each
        // arm's exit (the same shape tryJoinField has for the field set,
        // just without drop-edge carving).
        auto tryJoinNull = afterBodyNull;
        for (const auto& arm : trys->catches) {
            // §31.2.1/§12.9/§14.2 (#1939): the CATCH-surface copy of the
            // #2006/#2012/#2018 erasure guard. §31.2.1: an arm "matches any
            // thrown exception that is an instance of the type" — but a
            // constructed generic arm type whose argument is a still-ERASED
            // kind (a function type; appendDescKey mints no identity token)
            // reaches the COLLAPSED shared descriptor at dispatch, so the arm
            // would match a sibling instantiation and bind the exception at a
            // wrong static type (the #2001/#2006 confusion class). Reject the
            // ground erased-argument form; hold OFF a type-variable-mentioning
            // arm (`var ex: TEx`, `MyExc<T>` — #1505's admitted forms, resolved
            // per instantiation; the T-pinned-to-erased residual is the
            // design-gated #2012/#2069 scope), matching every other surface.
            for (const auto& armTy : arm.types) {
                if (!armTy) continue;
                TypeRef* rt = ctx_.resolvedType(*armTy);
                if (rt && rt->kind == TypeRef::Kind::Named &&
                    !rt->genericArgs.empty() &&
                    !refMentionsTypeVariable(rt) && !typeArgRuntimeReified(rt))
                    ctx_.diagnostics().report(
                        Severity::Error, armTy->span.begin,
                        "the type argument of this `catch` arm type is not "
                        "runtime-distinguishable — it is a still-erased type "
                        "kind (e.g. a function type), which the runtime type "
                        "descriptor collapses, so the arm could match a "
                        "different instantiation and bind the exception at a "
                        "wrong static type (§31.2.1/§12.9/§14.2)");
            }
            // #1939 catch-arm erasure guard (above) and #2116 narrowing-fact
            // setup (below) are independent: the guard is diagnostic-only over
            // const resolvedType reads; this assignment seeds the arm-body
            // running field facts. No ordering dependence.
            fieldNarrowed_ = catchRunningField;          // §7.4 #2116
            nullNarrowed_ = catchRunningNull;            // §7.4 #2164
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
                // per §31.2.1 (v0.182): when a multi-type BINDING arm's listed
                // types share no common base more specific than the implicit
                // root `Object` (catchArmNcb finds no shared ancestor — only
                // possible for the multi-type case, since a single listed type
                // always resolves via the size==1 shortcut), the binding form is
                // a compile error: `Object` gives the bound name no usable
                // surface and collides with §31.1's rejection of `Object`-typed
                // throw operands. The non-binding form (`T1 | T2` with no `var`)
                // needs no common type and is unrestricted — this caller is
                // already guarded by `arm.hasBinding`.
                if (!ncb && arm.types.size() > 1) {
                    ctx_.diagnostics().report(
                        Severity::Error, arm.types[0]->span.begin,
                        "a multi-type `catch` arm that binds a name must list "
                        "types sharing a common base class more specific than "
                        "`Object` (§31.2.1) — split into separate arms, or drop "
                        "the binding");
                }
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
            // §7.4 #2116: fold this catch arm's exit into the after-try join
            // AND the next arm's running base.
            for (auto it = tryJoinField.begin(); it != tryJoinField.end();) {
                if (fieldNarrowed_.count(*it)) ++it;
                else it = tryJoinField.erase(it);
            }
            for (auto it = catchRunningField.begin();
                 it != catchRunningField.end();) {
                if (fieldNarrowed_.count(*it)) ++it;
                else it = catchRunningField.erase(it);
            }
            // §7.4 #2164: same after-arm / next-arm intersection for the
            // local set. tryJoinNull is the after-try join (after-body ∩
            // every catch exit); catchRunningNull is the next-arm base
            // (each new arm starts from entry ∩ after-body ∩ earlier
            // arms' exits, shrinking as arms diverge).
            for (auto it = tryJoinNull.begin(); it != tryJoinNull.end();) {
                if (nullNarrowed_.count(*it)) ++it;
                else it = tryJoinNull.erase(it);
            }
            for (auto it = catchRunningNull.begin();
                 it != catchRunningNull.end();) {
                if (nullNarrowed_.count(*it)) ++it;
                else it = catchRunningNull.erase(it);
            }
        }
        // §7.4 (v0.214, #2116 pre-gate review): the FINALLY body runs on every
        // exit from the construct — the body's normal completion, its EARLY
        // RETURNS (a `return` inside the try runs the finally with the state
        // at THAT return point — e.g. the null branch of the body's own
        // guard; probe-confirmed SEGV), an unmatched-exception unwind, and
        // each catch exit. The only facts valid on ALL of those paths are the
        // try-ENTRY facts — all of them when no drop edge fired during the
        // body/arm visits, only the ambient (never-droppable) ones otherwise.
        // Never the after-body/arm-exit joins: those see establishments the
        // interrupted and early-exit paths never made.
        auto finallyEntryField = tryEntryField;
        if (fieldFactDropEpoch_ != tryDropEpoch0)
            finallyEntryField.erase(finallyEntryField.lower_bound({1u, 0u}),
                                    finallyEntryField.end());
        fieldNarrowed_ = std::move(finallyEntryField);
        const std::uint64_t finDropEpoch0 = fieldFactDropEpoch_;
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
        // §7.4 (v0.214, #2116 pre-gate review): after-try state. The paths
        // that REACH the after-try point are the completing ones — body
        // fall-through or a catch arm's fall-through — each carrying
        // tryJoinField (after-body ∩ every catch exit), then filtered through
        // the finally body's own drop edges (ambient facts are immune; local
        // facts survive only if the finally visit fired no drop edge). The
        // finally's exit facts join in: the finally runs to completion on
        // every path that reaches this point.
        if (fieldFactDropEpoch_ != finDropEpoch0)
            tryJoinField.erase(tryJoinField.lower_bound({1u, 0u}),
                               tryJoinField.end());
        tryJoinField.insert(fieldNarrowed_.begin(), fieldNarrowed_.end());
        fieldNarrowed_ = std::move(tryJoinField);
        // §7.4 #2164: the corresponding local-set after-try state — the
        // pre-finally running intersection (tryJoinNull as built per-arm
        // above), intersected with the finally body's exit. The finally runs
        // on every exit path, so its exits factor in: a fact the finally
        // established enters; a fact the finally erased drops. The legacy
        // local set has no drop-edge carving — apply the same
        // intersection-only discipline (matches the §7.4 join the if-stmt
        // and case-stmt handlers already apply).
        nullNarrowed_.insert(tryJoinNull.begin(), tryJoinNull.end());
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
        // §6.6 / A.6 (v0.184, #1690): `IncrementStmt ::= LValue ("++"|"--")`
        // consumes the same A.6 `LValue` production as AssignmentStmt — a
        // method-call result cannot serve as the chain's base here either.
        if (is->target) checkLValueChainNotCallBased(*is->target);
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
                        // §21.1 v0.201 (#2049): a `const` LOCAL — compile-time
                        // OR runtime — is single-assignment; `++`/`--` reassigns
                        // it and is a compile error naming the declaration.
                        if (loc->isConst)
                            ctx_.diagnostics().report(
                                Severity::Error, is->target->span.begin,
                                std::string("cannot increment/decrement const "
                                "local `") + std::string(id->name) +
                                "` — a const local is a single-assignment "
                                "binding (§21.1)");
                        // §17.15 (v0.216, #2098): `++`/`--` reassign; a
                        // binding-atom binding is not an assignable location
                        // (the §21.1 runtime-`const` mirror; reachable only
                        // through a `do`-bodied lambda inside the atom).
                        else if (loc->isBindAtomClause)
                            ctx_.diagnostics().report(
                                Severity::Error, is->target->span.begin,
                                std::string("cannot increment/decrement "
                                "binding-atom binding `") +
                                std::string(id->name) +
                                "` — a `(var …)` binding is a "
                                "single-assignment binding and not an "
                                "assignable location (§17.15, v0.216)");
                        else if (loc->isUsing)
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
        // §7.4 (v0.214, #2116 pre-gate review): `c++`/`c--` desugars to
        // `c := T.operator++(c)` (§17.6) — a reassignment rebinding `c` to the
        // operator's result, so any `c._f` readonly-field facts rode the OLD
        // referent and must drop (same edge as the compound-assign family).
        if (is->target) {
            if (auto* tid = dynamic_cast<const ast::IdentifierExpr*>(
                    is->target.get())) {
                if (tid->name != "this" && tid->name != "base")
                    if (Symbol* tsym = ctx_.exprSymbol(*is->target))
                        dropFieldFactsRooted(tsym);
            }
        }
    } else if (auto* cs = dynamic_cast<const ast::CaseStmt*>(&s)) {
        if (cs->scrutinee) {
            // §35.5 (v0.209/#2080 F4): the A.6 statement form's `case owned x
            // of` scrutinee marker — same routing as the expression atom's.
            const ast::Expr* savedScrutSlot = caseScrutineeSlot_;
            caseScrutineeSlot_ = cs->scrutinee.get();
            visitExpr(*cs->scrutinee);
            caseScrutineeSlot_ = savedScrutSlot;
        }
        validateCaseArms(cs->arms);  // §16.4 / §19.6 (#738)
        TypeRef* scrutTy =
            cs->scrutinee ? ctx_.exprType(*cs->scrutinee) : nullptr;
        scrutTy = recoverChoiceThisScrutTy(cs->scrutinee.get(), scrutTy);
        // §5.1 / §19 (#1153): reject an out-of-range integer pattern literal
        // (statement-position `case` is exempt from exhaustiveness but not from
        // the literal-range gate — codegen would otherwise truncate it).
        checkCasePatternConstantRanges(cs->arms, scrutTy);
        // §10.6 (#772): bare enum member names may be used throughout a `case`
        // over an enum scrutinee, including arm bodies. Inject the scrutinee
        // enum's constants for the arm bodies (the statement-position analogue
        // of the CaseExpr handler). // per §10.6
        // §35.5 (v0.200, #2045): the scrutinee-type prefix-omission rule
        // extends to a choice scrutinee. Inject the choice's PAYLOAD-FREE
        // case names (constants in the §10.6 sense — usable bare in arm
        // bodies and as bare patterns); payload-carrying cases are reached
        // only through DestructuringPattern arms and admissibility is
        // checked at the pattern, not injected as a name.
        Scope armEnumScope;
        Scope* preArmEnumScope = currentScope_;
        bool savedEnumArms = inEnumScrutineeCaseArms_;
        bool scrutIsEnum = scrutTy && scrutTy->kind == TypeRef::Kind::Named &&
            scrutTy->declaration &&
            scrutTy->declaration->kind == ast::TypeDeclKind::Enum;
        bool scrutIsChoice = scrutTy && scrutTy->kind == TypeRef::Kind::Named &&
            scrutTy->declaration &&
            scrutTy->declaration->kind == ast::TypeDeclKind::Choice;
        if (scrutIsEnum && currentScope_) {
            armEnumScope.parent = currentScope_;
            for (Symbol* m : scrutTy->declaration->members)
                if (dynamic_cast<EnumConstantSymbol*>(m))
                    armEnumScope.names[m->name] = m;
            currentScope_ = &armEnumScope;
            inEnumScrutineeCaseArms_ = true;
        } else if (scrutIsChoice && currentScope_) {
            armEnumScope.parent = currentScope_;
            for (Symbol* m : scrutTy->declaration->members)
                if (auto* cc = dynamic_cast<ChoiceCaseSymbol*>(m))
                    if (cc->caseDecl && cc->caseDecl->payload.empty())
                        armEnumScope.names[m->name] = m;
            currentScope_ = &armEnumScope;
        }
        // §7.4 (v0.214, #2116): case arms are ALTERNATIVES — a readonly-field
        // fact established inside one arm's body (e.g. an early-exit `if`
        // inside a `do … end` arm) must not be visible to the next arm's
        // analysis (the next arm executes only when this one did NOT), nor
        // survive past the case unless every arm's exit carries it. Each arm
        // starts from the RUNNING intersection of the case-entry state (taken
        // after the scrutinee visit, so scrutinee-evaluation drops are in)
        // with every earlier arm's exit: an earlier arm's ESTABLISHMENTS never
        // enter it (they are absent from the entry state), while an earlier
        // arm's DROPS (a guard's `ref` argument can rebind a fact's root, and
        // guards of non-selected arms do evaluate at runtime) stay dropped —
        // conservative and sound in both directions. The same running
        // intersection is the after-case state (it equals entry ∩ every arm
        // exit, which also covers the no-match fall-through path).
        //
        // fix #2164 / §7.4: the legacy `nullNarrowed_` set shares the same
        // arm-to-arm hygiene need. Without it, a guard inside arm 1's body
        // (`if x = null then return 0; end;`) established x as non-null and
        // the next arm's body (`return x.Length;`) inherited that fact — but
        // the next arm runs only when arm 1 did NOT (so x might be null there:
        // compile-accepted null deref, SIGSEGV at runtime). Apply the SAME
        // running-intersection discipline to the local set: each arm starts
        // from the running intersection, after-arm intersection folds the
        // exit. entryNullNarrowed is captured once at the top of the
        // construct; the running intersection is seeded with it and shrunken
        // by each arm's exit. Set algebra over symbol pointers — no
        // container-iteration dependence. // per §7.4, #2116, #2164
        auto caseRunningField = fieldNarrowed_;
        auto caseEntryNull = nullNarrowed_;
        auto caseRunningNull = caseEntryNull;
        // §7.4 (#2258): fold THIS arm's exit state into the running
        // intersections. Named (not inlined at the loop tail) because one arm
        // path — the choice-destructure arm below — `continue`s past the tail,
        // and skipping the fold there loses that arm's DROPS: the after-case
        // state would claim a fact the arm invalidated (`s := null;` inside a
        // `Some(var v) =>` arm), a silent ACCEPT of a null dereference.
        auto foldArmExitIntoRunning = [&] {
            for (auto it = caseRunningField.begin();
                 it != caseRunningField.end();) {
                if (fieldNarrowed_.count(*it)) ++it;
                else it = caseRunningField.erase(it);
            }
            // §7.4 #2164: the same running-intersection discipline for the
            // LOCAL set — every later arm starts from entry ∩ earlier-arm
            // exits, and the after-case state is the same intersection.
            for (auto it = caseRunningNull.begin();
                 it != caseRunningNull.end();) {
                if (nullNarrowed_.count(*it)) ++it;
                else it = caseRunningNull.erase(it);
            }
        };
        for (const auto& arm : cs->arms) {
            fieldNarrowed_ = caseRunningField;  // §7.4 #2116: per-arm base
            nullNarrowed_ = caseRunningNull;    // §7.4 #2164: per-arm base
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
            // §19.3 (#1847): a top-level bare-identifier pattern must be a type
            // test, a named constant, or an enum constant; reject one that
            // resolves to a runtime value or an unresolved name (codegen would
            // silently drop the arm — the mandatory `_` skipped, §19.6).
            if (auto* np =
                    dynamic_cast<const ast::NamedPattern*>(arm.pattern.get()))
                checkTopLevelNamedPattern(np, scrutTy);
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
                // §19.5 (#1203, #1936): a typed binding pattern `var n: T` is a
                // type-test + binding. If T is a reference type (class,
                // interface, string, or a BUILT-IN exception) but the scrutinee's
                // static type is a VALUE type (a primitive — non-nullable, not
                // string, not Object — or a record/enum), the test can NEVER
                // match: a value type is never a reference type. Reject it here
                // rather than miscompiling (an unconditional wrong-value match for
                // a string-typed binding, or — in codegen's emitIsA over a
                // non-pointer scrutinee — an ICmpInst assertion / malformed IR).
                // #1936 broadened #1203 two ways: (a) a BUILT-IN exception pattern
                // (`var x: IOException`) carries no user TypeSymbol so its bndTy
                // has a null `declaration` — the declaration-gated class/interface
                // test below missed it, and the arm reached codegen and emitted
                // malformed IR; (b) a RECORD or ENUM scrutinee is likewise a
                // sealed value type (§9/§10) — `case p of var q: C` over a record
                // `p` hit the same emitIsA crash. per §19.5 (#1203/#1936)
                if (bp->type && scrutTy && lsym->declType) {
                    TypeRef* bndTy = lsym->declType;
                    bool bndIsRefType =
                        (bndTy->kind == TypeRef::Kind::Primitive &&
                         bndTy->primitive == PrimitiveType::String) ||
                        (bndTy->kind == TypeRef::Kind::Named &&
                         bndTy->declaration &&
                         (bndTy->declaration->kind ==
                              ast::TypeDeclKind::Class ||
                          bndTy->declaration->kind ==
                              ast::TypeDeclKind::Interface)) ||
                        // #1936 / §31: a built-in exception (declaration-less
                        // Named), recognized by name via the shared registry —
                        // the sema counterpart of codegen isBuiltinExceptionTypeRef.
                        (bndTy->kind == TypeRef::Kind::Named &&
                         !bndTy->declaration && bp->type->name &&
                         !bp->type->name->parts.empty() &&
                         common::isBuiltinExceptionName(
                             bp->type->name->parts.back()));
                    bool scrutIsValuePrim =
                        scrutTy->kind == TypeRef::Kind::Primitive &&
                        !scrutTy->nullable &&
                        scrutTy->primitive != PrimitiveType::String &&
                        scrutTy->primitive != PrimitiveType::Object;
                    // #1936 / §9/§10: a record/enum scrutinee is a sealed value
                    // type — never a reference.
                    bool scrutIsValueRecEnum =
                        scrutTy->kind == TypeRef::Kind::Named &&
                        scrutTy->declaration && !scrutTy->nullable &&
                        (scrutTy->declaration->kind ==
                             ast::TypeDeclKind::Record ||
                         scrutTy->declaration->kind ==
                             ast::TypeDeclKind::Enum);
                    if (scrutIsValuePrim) {
                        if (bndIsRefType)
                            ctx_.diagnostics().report(
                                Severity::Error, bp->span.begin,
                                "binding pattern type is incompatible with "
                                "the scrutinee's static type — a value-type "
                                "scrutinee can never match a reference-typed "
                                "binding (§19.5)");
                    }
                    // #1936: a record/enum value scrutinee against a reference
                    // pattern — inserted as an else-if so a record scrutinee vs a
                    // non-reference (record/enum) pattern still falls through to
                    // the generic-instantiation checks below, unchanged.
                    else if (scrutIsValueRecEnum && bndIsRefType)
                        ctx_.diagnostics().report(
                            Severity::Error, bp->span.begin,
                            "binding pattern type is incompatible with "
                            "the scrutinee's static type — a value-type "
                            "scrutinee can never match a reference-typed "
                            "binding (§19.5)");
                    // §19.3/§19.5/§5.1/§12.9 (#1913): a typed binding whose
                    // generic INSTANTIATION is incompatible with the scrutinee
                    // (`var x: Ok<string>` at `Result<int>`) — the erased
                    // runtime test would match and bind at the wrong static
                    // type. Reject it, the admission counterpart of #1874's
                    // arg-sensitive §19.6 coverage.
                    else if (typeTestGenericInstantiationImpossible(
                                 scrutTy, lsym->declType))
                        ctx_.diagnostics().report(
                            Severity::Error, bp->span.begin,
                            "binding pattern type is incompatible with the "
                            "scrutinee's static type — its generic "
                            "instantiation can never match a value of the "
                            "scrutinee type (class hierarchies are invariant, "
                            "§12.9), so the pattern would bind at a wrong "
                            "static type (§19.3/§5.1)");
                    // §19.3/§19.5/§5.1/§12.9 (#1920): the interface-scrutinee
                    // counterpart — a FINAL class pattern (`var x: Box<string>`)
                    // whose implemented instantiations of the generic-interface
                    // scrutinee (`IBox<int>`) are all incompatible. Same erased-
                    // test wrong-bind hole as #1913, one level up through an
                    // interface.
                    else if (typeTestInterfaceInstantiationImpossible(
                                 scrutTy, lsym->declType))
                        ctx_.diagnostics().report(
                            Severity::Error, bp->span.begin,
                            "binding pattern type is incompatible with the "
                            "scrutinee's static type — its generic "
                            "instantiation can never match a value of the "
                            "scrutinee interface type (no implemented "
                            "instantiation is compatible under §12.9), so the "
                            "pattern would bind at a wrong static type "
                            "(§19.3/§5.1)");
                    // §19.3/§19.5/§5.1/§12.9 (#1927; narrowed/refined #1946): a
                    // generic class pattern against a generic-interface scrutinee.
                    // Unreachable = DEAD arm (cannot conform — #1920's non-final
                    // counterpart). ErasedAmbiguous = live but the differing type
                    // argument is a still-erased #1939 kind so the runtime test
                    // degrades and a sibling would mis-bind (the OLD #1927
                    // uniqueness rejection, kept only where #1843 does not reify).
                    else {
                        IfaceBindReject rej = typeTestInterfaceBindAmbiguous(
                            scrutTy, lsym->declType);
                        if (rej == IfaceBindReject::Unreachable)
                            ctx_.diagnostics().report(
                                Severity::Error, bp->span.begin,
                                "binding pattern type's generic instantiation can "
                                "never match a value of the scrutinee interface "
                                "type — no instantiation of the pattern class that "
                                "implements it is compatible under §12.9, so the "
                                "arm is unreachable (§19.3/§5.1)");
                        else if (rej == IfaceBindReject::ErasedAmbiguous)
                            ctx_.diagnostics().report(
                                Severity::Error, bp->span.begin,
                                "binding pattern type's generic argument is not "
                                "runtime-distinguishable — it is a still-erased "
                                "type kind (e.g. a function type or inline array), "
                                "and another instantiation of the pattern class "
                                "also implements the scrutinee interface type, so "
                                "the erased runtime type-test could bind a value "
                                "at a wrong static type (§19.3/§5.1/§12.9)");
                        // §12.9/§14.2/§17.4 (v0.197, #2012): the PLAIN-scrutinee
                        // analog of #2006's `is`/`as` erasure guard.
                        // typeTestInterfaceBindAmbiguous only reasons about a
                        // generic-INTERFACE scrutinee (its `None` there means
                        // "soundly admitted" — the unique conformer — so respect
                        // it and skip that domain below). But for a PLAIN
                        // scrutinee (Object, a class, a NON-generic interface) a
                        // `var x: T<erased-args>` reaches the COLLAPSED per-
                        // monomorphization descriptor with no guard at all:
                        // appendDescKey does not encode a function type's identity
                        // (Kind::Function → "F" at any depth), so
                        // `case o of var x: Wrap<(string)->string>` false-positive-
                        // MATCHES a `Wrap<(int)->int>` value and binds it at a wrong
                        // static type — the same memory-unsafe type confusion
                        // #2001/#2006 fixed. Bring this path to parity with #2006:
                        // reject a CONSTRUCTED generic target whose type argument is
                        // not runtime-reified (`typeArgRuntimeReified` is the
                        // oracle) and that carries no type variable (a bare
                        // parameter resolves to a concrete arg at monomorphization,
                        // #1843 — admit it, matching the conservative-accept posture
                        // above). Hold off a generic-INTERFACE scrutinee — that is
                        // typeTestInterfaceBindAmbiguous's domain (and any erased
                        // case there is already rejected by it or upstream by
                        // #1913/#1920), so re-deciding here would double-report or
                        // override its sound unique-conformer admission.
                        else if (lsym->declType->kind ==
                                     TypeRef::Kind::Named &&
                                 !lsym->declType->genericArgs.empty() &&
                                 !refMentionsTypeVariable(lsym->declType) &&
                                 !typeArgRuntimeReified(lsym->declType) &&
                                 !(scrutTy->kind == TypeRef::Kind::Named &&
                                   scrutTy->declaration &&
                                   scrutTy->declaration->kind ==
                                       ast::TypeDeclKind::Interface &&
                                   !scrutTy->genericArgs.empty()))
                            ctx_.diagnostics().report(
                                Severity::Error, bp->span.begin,
                                "binding pattern type's generic argument is not "
                                "runtime-distinguishable — it is a still-erased "
                                "type kind (e.g. a function type), which the "
                                "runtime type descriptor collapses, so the type-"
                                "test could match a different instantiation and "
                                "bind the value at a wrong static type "
                                "(§19.3/§12.9/§14.2/§17.4)");
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
                // §35.5 (v0.200, #2045): CHOICE case destructuring. When the
                // scrutinee is a choice and the pattern's head names one of
                // its cases, bind the case's payload fields as locals — the
                // §35.5 "Case patterns" positional destructuring production,
                // the choice analogue of record positional destructuring.
                // Payload bindings are copies (value-like choices) or borrows
                // (borrow-bearing/owning) per §35.5 "The borrowing match";
                // the owning-match transfer machinery lands in a follow-up.
                if (scrutIsChoice) {
                    auto* cc = const_cast<ChoiceCaseSymbol*>(
                        findChoiceCaseOnScrutinee(scrutTy, lastName));
                    if (cc && cc->caseDecl) {
                        // §35.5 "Case patterns": "a positional destructuring
                        // pattern `CaseName(slot, …)` with the case's exact
                        // payload arity." Reject an arity mismatch (mirrors
                        // the record-destructure arity check).
                        if (dp->subPatterns.size() !=
                            cc->caseDecl->payload.size()) {
                            ctx_.diagnostics().report(
                                Severity::Error, dp->span.begin,
                                "case-destructuring pattern `" +
                                std::string(lastName) + "(...)` binds " +
                                std::to_string(dp->subPatterns.size()) +
                                " slot(s) but case `" + std::string(lastName) +
                                "` has " +
                                std::to_string(cc->caseDecl->payload.size()) +
                                " payload field(s); the slot count must "
                                "match (\xC2\xA7""35.5)");
                        }
                        Scope armScope;
                        armScope.parent = currentScope_;
                        for (std::size_t i = 0; i < dp->subPatterns.size();
                             ++i) {
                            const auto& sp = dp->subPatterns[i];
                            // §14.1 / §35.4 (v0.226, #2208): a GENERIC-choice
                            // case payload's RESOLVED type is the BARE type
                            // parameter for a `Some(value: T)` declaration
                            // (TypeResolution's generic-param path leaves
                            // `decl == null`), not the instantiated payload
                            // type. Bind the bare T → `inner: T`, and every
                            // downstream `.Count`/method call on `inner`
                            // classifies against the unspecialized kind, so
                            // `inner.Count` falls through the Array<T>
                            // bz_array_len fast path and reads garbage
                            // (returns 0 for a valid Array<int>). Substitute
                            // the choice's instantiation through the payload
                            // type so `inner: Array<int>` at the binding site.
                            // Per-instantiation substitution — matches
                            // common::substitutedChoicePayloadType's contract
                            // (used by §35.4 classification and §35.3
                            // construction). A non-generic choice returns
                            // the resolved type unchanged (the helper's
                            // early-return path). // per §35.5, §14.1 (v0.226)
                            TypeRef* fieldTy = nullptr;
                            if (i < cc->caseDecl->payload.size() &&
                                cc->caseDecl->payload[i].type) {
                                fieldTy = ctx_.resolvedType(
                                    *cc->caseDecl->payload[i].type);
                                // #2219: a generic choice `Both<T,E>` at the
                                // scrutinee instantiates with concrete args
                                // (`Both<int,int>`); the case's bare-type-param
                                // payload types (`T`, `E`) must be substituted
                                // with the actual instantiation args so the
                                // destructured slot is bound at the right type
                                // (and both arms' common-type computation sees
                                // both at `int`, not at unrelated `T`/`E`).
                                // Substitute the field type here so every slot
                                // binding (below) sees the resolved form.
                                if (fieldTy) {
                                    fieldTy = substituteGenericCaseFieldType(
                                        fieldTy, scrutTy);
                                }
                            }
                            if (!sp.nakedBinding.empty()) {
                                auto* lsym = ctx_.makeSymbol<LocalSymbol>();
                                lsym->name = sp.nakedBinding;
                                lsym->decl = sp.nakedAnchor.get();
                                lsym->declType = fieldTy;
                                armScope.names[sp.nakedBinding] = lsym;
                            } else if (auto* vbp =
                                           dynamic_cast<const ast::BindingPattern*>(
                                               sp.pattern.get())) {
                                auto* lsym = ctx_.makeSymbol<LocalSymbol>();
                                lsym->name = vbp->name;
                                lsym->decl = vbp;
                                lsym->declType = vbp->type
                                    ? ctx_.resolvedType(*vbp->type) : fieldTy;
                                armScope.names[vbp->name] = lsym;
                            }
                            // §35.5: "Nested destructuring of an owned
                            // payload does not arise" (owned payloads are
                            // class/interface-typed). A nested destructure
                            // of a value-record payload is left to a follow-
                            // up slice.
                        }
                        Scope* saveScope = currentScope_;
                        currentScope_ = &armScope;
                        if (arm.guard) visitExpr(*arm.guard);
                        if (arm.exprBody) visitExpr(*arm.exprBody);
                        for (const auto& bs : arm.stmts)
                            if (bs) visitStmt(*bs);
                        currentScope_ = saveScope;
                        // §7.4 (#2258): this path leaves the arm loop early, so
                        // fold the arm's exit HERE — the loop-tail fold never
                        // runs for it, and a fact this arm DROPPED would be
                        // resurrected in the after-case state.
                        foldArmExitIntoRunning();
                        continue;  // skip the record-destructure path
                    }
                }
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
                // §19.3 (v0.226, #2178): a generic-record DESTRUCTURING pattern
                // against an ERASED scrutinee — the scrutinee's static type does
                // not determine the record's instantiation, so the pattern
                // would match any instantiation and bind the fields at the
                // record's TEMPLATE layout (a wrong-typed-read hazard, the same
                // family the #2001/#2006/#2012/#2018 erasure guards close).
                // Mirror the CaseExpr-side check below for the statement
                // position; the typed binding `var p: Pair<int, string>` remains
                // the precise spelling. See the ExprTypecheck_Exprs.cpp
                // counterpart for the full rationale.
                if (recTy && !recTy->genericParams.empty()) {
                    bool scrutErased =
                        !scrutTy ||
                        (scrutTy->kind == TypeRef::Kind::Primitive &&
                         scrutTy->primitive == PrimitiveType::Object) ||
                        (scrutTy->kind == TypeRef::Kind::Named &&
                         scrutTy->declaration &&
                         (scrutTy->declaration->kind == ast::TypeDeclKind::Class ||
                          (scrutTy->declaration->kind ==
                               ast::TypeDeclKind::Interface &&
                           scrutTy->genericArgs.empty()))) ||
                        (scrutTy->kind == TypeRef::Kind::Named &&
                         scrutTy->declaration != nullptr &&
                         scrutTy->declaration != recTy);
                    if (scrutErased) {
                        ctx_.diagnostics().report(
                            Severity::Error, dp->span.begin,
                            "record-destructuring pattern `" +
                            std::string(lastName) +
                            "(...)` is generic but names no type arguments, "
                            "and the scrutinee's static type does not "
                            "determine the record's instantiation — a box of "
                            "ANY instantiation would match and the arm's "
                            "bindings would read fields at the record's "
                            "TEMPLATE layout, the same wrong-typed-read "
                            "hazard #2001/#2006 fixed; use a typed binding "
                            "(`var p: " + std::string(lastName) +
                            "<…> => …`) so the type is statically known "
                            "(\xc2\xa7""19.3 / \xc2\xa7""12.7, v0.226, #2178)");
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
                // §7.4 (v0.210, #2105): a scrutinee-less `when` arm's
                // own boolean CONDITION narrows that arm's own body —
                // per §7.4 v0.210 first bullet: "a `when` guard … or a
                // `(when …)` arm condition (§19.1, narrowing that arm's
                // body)". For the statement-position form, the
                // condition is the ConstantPattern's first constant
                // (the parser wraps a single boolean expression in a
                // ConstantPattern; multiple constants are alternatives
                // for an enum scrutinee pattern, irrelevant here).
                // Save/restore mirrors the IfStmt and CaseExpr
                // scrutinee-less handlers (the per-arm running
                // intersection below #2116/#2164 is unaffected — only
                // this arm's BODY sees the condition's true-branch
                // facts, the exit intersection is unchanged).
                //
                // fix #2258 / §7.4: the un-seeding below is an
                // INTERSECTION, never an assignment. Restoring the arm's
                // ENTRY snapshot resurrected every fact the arm's BODY
                // legitimately invalidated (`x := null;`, a `ref`/`out`
                // argument, a call on a mutably-captured local — §7.4
                // "How narrowing is invalidated"), and because this
                // restore ran BEFORE the #2116/#2164 fold, the fold then
                // intersected `caseRunning*` with itself (a no-op) and
                // the arm's REAL exit state was discarded: a null flowed
                // into a slot the checker believed non-null (silent
                // ACCEPT, SIGSEGV). The intersection keeps the arm's
                // DROPS and still discards what the arm CONDITION seeded
                // (absent from the entry snapshot), so the fold below —
                // whose input is now armEntry ∩ armExit, and
                // armEntry ⊆ caseRunning* — computes exactly the
                // intended caseRunning* ∩ armExit. This branch is the
                // one every NON-binding, NON-destructuring arm takes
                // (constant, `_` discard, enum-constant, relational,
                // alt, and scrutinee-less boolean arms).
                auto armEntryNarrow = narrowings_;
                auto armEntryNull = nullNarrowed_;
                auto armEntryField = fieldNarrowed_;
                if (auto* cp = dynamic_cast<const ast::ConstantPattern*>(
                        arm.pattern.get())) {
                    if (!cp->constants.empty() && cp->constants[0]) {
                        nullNarrowed_ = armEntryNull;
                        fieldNarrowed_ = armEntryField;
                        collectNarrowings(cp->constants[0].get(),
                                          /*forTrueBranch=*/true);
                    }
                }
                if (arm.guard) visitExpr(*arm.guard);
                if (arm.exprBody) visitExpr(*arm.exprBody);
                for (const auto& bs : arm.stmts) if (bs) visitStmt(*bs);
                intersectNarrowingSetsWith(armEntryNull, armEntryNarrow,
                                           armEntryField);  // §7.4 #2258
            }
            // §7.4 #2116/#2164/#2258: fold this arm's exit into the running
            // intersections (see the lambda).
            foldArmExitIntoRunning();
        }
        fieldNarrowed_ = std::move(caseRunningField);  // §7.4 #2116
        nullNarrowed_ = std::move(caseRunningNull);    // §7.4 #2164
        currentScope_ = preArmEnumScope;  // §10.6 (#772): pop the enum scope
        inEnumScrutineeCaseArms_ = savedEnumArms;
    }
    // Break, Continue have no expressions.
}

bool typecheckExpressions(const ast::SourceFile& src, SemaContext& ctx) {
    ExprTC tc(ctx);
    tc.run(src);
    // §9.8 / #2325: the class half of serialization-eligibility checking
    // needs FieldSymbol::posParamBodyRef/posParamInitRef, which the walk
    // above just finished computing (ExprTypecheck_Exprs.cpp) — see
    // checkClassSerializationEligibilityLate's declaration comment
    // (TypeResolution_impl.hpp) for the full rationale. Run it here, inside
    // typecheckExpressions() itself, rather than as a separate call the
    // driver must remember to sequence after this function: resolveTypes()
    // and typecheckExpressions() are called together from many independent
    // pipeline constructions across the driver AND the test harness (fresh
    // SemaContext + collectSymbols + resolveTypes + typecheckExpressions,
    // repeated ad hoc in dozens of call sites) — a separately-invoked pass
    // would need every one of those threaded through by hand, and a single
    // missed call site would silently readmit an ineligible class (verified:
    // this happened — the codegen_serialization_t2_classes negative tests,
    // which go through the test harness's OWN independent pipeline rather
    // than Driver::compileFile, regressed when this was a separate call).
    // Folding it into typecheckExpressions() itself means every existing and
    // future caller gets it for free, with no separate wiring.
    checkClassSerializationEligibilityLate(src, ctx);
    return !ctx.diagnostics().hasErrors();
}

} // namespace bezel::sema
