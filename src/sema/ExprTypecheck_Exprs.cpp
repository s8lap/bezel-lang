#include "sema/ExprTypecheck_impl.hpp"

#include "common/BuiltinRegistry.hpp"
#include "common/OwnershipFacts.hpp"  // §35.7 (#2045): classifyChoiceType
#include "sema/EnumValues.hpp"  // enumHasZeroConstant — §17.11 default(E) (#3044)
#include "sema/TypeResolution.hpp"  // fix #2320: typeRefEqual for ctor overload re-resolution

#include <cmath>
#include <unordered_set>

// §4.3/§4.4 (v0.190, #1781): decimal literal range-check at formation reuses the
// runtime's compile-time decimal parser — the SAME entry codegen uses to
// materialize a decimal literal (LlvmBackend_Exprs.cpp). The runtime is linked
// into `bezelc`, so these symbols resolve at final link. `bz_decimal_parse_cstr`
// signals an out-of-range mantissa by storing an OverflowException object in the
// thread-local `bz_exc` slot (reachable via `bz_exc_addr()`) when handed a
// non-null OverflowException vtable; we probe that slot then restore it.
extern "C" void bz_decimal_parse_cstr(const char* s, void* out,
                                      void* ove_vtable, void* fe_vtable);
extern "C" void** bz_exc_addr();

namespace bezel::sema {

namespace {

// §5: parse an integer literal's magnitude (ignoring base prefix,
// underscores, and any type suffix). Returns true and the value via `out`
// when it parses; saturates large hex/decimal in the rare overflow case
// (which still exceeds int and is reported). Mirrors the codegen parser.
bool intLiteralMagnitude(std::string_view text, unsigned long long& out) {
    int base = 10;
    std::size_t i = 0;
    if (text.size() >= 2 && text[0] == '0') {
        char b = text[1];
        if (b == 'x' || b == 'X') { base = 16; i = 2; }
        else if (b == 'b' || b == 'B') { base = 2; i = 2; }
        else if (b == 'o' || b == 'O') { base = 8; i = 2; }
    }
    std::string digits;
    for (; i < text.size(); ++i) {
        char c = text[i];
        if (c == '_') continue;
        bool isDigit = (c >= '0' && c <= '9');
        bool isHex =
            isDigit || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (base == 16 ? !isHex : !isDigit) break;  // stop at suffix letters
        digits.push_back(c);
    }
    if (digits.empty()) return false;
    errno = 0;
    out = std::strtoull(digits.c_str(), nullptr, base);
    return true;
}

// §4.3/§4.4 (v0.190, #1781): true iff the `m`/`M`-suffixed decimal literal
// `text` denotes a value outside `decimal`'s range. Parses at compile time via
// the runtime's `bz_decimal_parse_cstr` — the identical parser codegen uses
// (LlvmBackend_Exprs.cpp) — passing a non-null sentinel OverflowException
// vtable so an out-of-range mantissa sets the thread-local `bz_exc` slot. The
// lexer already validated the literal's shape, so `fe_vtable` is null (only
// OverflowException can fire). The slot is saved and restored around the probe.
bool decimalLiteralOutOfRange(std::string_view text) {
    // Strip underscores and the m/M suffix, exactly as codegen does before its
    // own bz_decimal_parse_cstr call.
    std::string digits;
    for (char c : text) {
        if (c == '_' || c == 'm' || c == 'M') continue;
        digits.push_back(c);
    }
    unsigned char sentinelVtable = 0;   // any non-null address serves as the flag
    unsigned int dec[4] = {0, 0, 0, 0};
    void** excSlot = bz_exc_addr();
    void* saved = *excSlot;
    *excSlot = nullptr;
    bz_decimal_parse_cstr(digits.c_str(), dec, &sentinelVtable, nullptr);
    bool overflowed = (*excSlot != nullptr);
    // Restore the slot. On overflow the sentinel triggered a small runtime
    // allocation we intentionally drop — this is the compile-error path.
    *excSlot = saved;
    return overflowed;
}

// §6.2: a primitive that participates in mixed-type arithmetic promotion.
// These are the numeric types (§4.2/§4.3/§4.4) plus `char`, which §5.1
// admits implicit conversions from (to int and wider). `bool`, `string`,
// `Object`, and `None` do not promote.
bool isPromotablePrimitive(PrimitiveType p) {
    using P = PrimitiveType;
    switch (p) {
        case P::Byte:  case P::SByte: case P::Short: case P::UShort:
        case P::Int:   case P::UInt:  case P::Long:  case P::ULong:
        case P::Float: case P::Double: case P::Decimal: case P::Char:
            return true;
        default:
            return false;
    }
}

// §7.4 (v0.182, #1593): true iff ACCESSING this property/indexer can transfer
// control to user code — the "accessor call" trigger of the mutably-captured-
// narrowing invalidation rule (§7.4, #1566: while a mutably-captured local is
// narrowed non-null, any operation in the controlled block that can run user
// code — a method/constructor/accessor/operator call or an `await` — may reach
// the capturing closure and null the local, so it drops the narrowing). A
// compiler-generated backing-field load/store — a NON-virtual auto-property —
// cannot run user code, so it does NOT invalidate; every other form can:
//   - `=> e` expression body (a read-only getter) — user code;
//   - a Block body — a user-written `get`/`set` arm, or an abstract arm that
//     dispatches to a user override — user code;
//   - an auto body that is virtual/abstract/override — the runtime accessor may
//     be a user override with a computed body, so the access can dispatch to it.
//
// fix #3652 (§23.4 / §7.4, v0.286): §23.4 fixes an indexer's body forms to
// exactly three — expression-bodied (`=> e`), block-form (`get`/`set` arms),
// or BODY-LESS (an interface member, or an abstract-class indexer with no
// accessor list, "write `readonly` before `property` to expose only a
// getter"). There is no auto/backing-field indexer form in the grammar (a
// single scalar slot cannot answer per-index reads). The parser's fallback
// branch (Parser_Decls.cpp) nonetheless stamps the DEFAULT `bodyKind ==
// Auto` on that body-less form, because "no body" and "auto-property" share
// one parser arm for ordinary (non-indexer) properties. This function's
// `Auto` case, added for the ordinary-property auto-body form, was applied
// unchanged to indexers too: `readonly property [i: int]: int;` (no
// `abstract`/`virtual`/`override` token — an interface member is implicitly
// abstract, never spelled) fell to `Auto`'s "pure field access" answer and
// returned false — an interface/abstract indexer's dispatch to a REAL,
// user-written implementation was misclassified as a backing-field load, so
// a `T[i]` access through it never invalidated a mutably-captured narrowing.
// (The comment directly above this one, before this fix, asserted "Indexers
// are always Expression- or Block-bodied (never auto)" — that claim was
// false of the code beneath it; §23.4's body-less interface form is exactly
// the counterexample, and the SYMBOL overload below already encoded the
// correct answer for the imported side of the same declaration, which is
// how the #3652 imported-vs-local asymmetry was diagnosed.) An indexer is
// never a compiler-synthesized field regardless of `bodyKind`, so the
// isIndexer check is unconditional, ahead of the switch.
//
// ⚠ This overload answers from the AST ALONE and is therefore only half the
// question — an IMPORTED property has no `ast::PropertyDecl` (§2.5) and lands on
// the `!pd` arm. Nothing outside this file may call it; ask the SYMBOL overload
// below, which is the one that consults both sources. It is left non-`static`
// only because the symbol overload delegates to it.
bool propertyAccessRunsUserCode(const ast::PropertyDecl* pd) {
    if (!pd) return false;
    // fix #3652: an indexer's accessor always dispatches to a real
    // implementation (§23.4 admits no auto/backing-field indexer form), so a
    // `bodyKind == Auto` body-less indexer declaration (the interface /
    // abstract-class form) still runs user code — mirrors the SYMBOL
    // overload's own `if (ps->isIndexer) return true;` carve-out below.
    if (pd->isIndexer) return true;
    switch (pd->bodyKind) {
        case ast::PropertyBodyKind::Expression:
            return true;
        case ast::PropertyBodyKind::Block:
            return !pd->accessors.empty();  // any get/set arm can run user code
        case ast::PropertyBodyKind::Auto:
            for (auto m : pd->modifiers)
                if (m == "virtual" || m == "abstract" || m == "override")
                    return true;
            return false;  // a non-virtual auto-property is a pure field access
    }
    return false;
}

// fix #3616 (§7.4 / §2.8.3 rule 4, v0.286): the ONE both-sources spelling of the
// §7.4 accessor-call question, asked of the SYMBOL. Every call site takes this
// overload; the AST-only one above is a detail of this one's local arm.
//
// WHY A SECOND OVERLOAD RATHER THAN A CONSERVATIVE DEFAULT AT EACH SITE. The
// three §7.4 call sites had drifted into three different spellings of the
// null-`pd` case — two dropped the invalidation silently (`if (auto* pd = …)`),
// one answered `true` (`pd ? f(pd) : true`) with a comment explaining why
// unknown must be conservative. The prior revision of the AST-only comment above
// called leaving an imported property alone "conservative"; for an INVALIDATION
// rule it is the opposite — the narrowing survives an accessor that may reach
// the capturing closure and null the local, which §7.4 exists to forbid.
// MEASURED: the character-identical `expr_body_property_getter_invalidates_capture`
// program (tests/lit/sema/…_spec_7_4.bzl) is REJECTED when `Box` is local and
// ACCEPTED when `Box` is imported — the §2.8.3 rule 4 divergence in as many words.
//
// But a blunt `return true` for every decl-less property would create the
// divergence in the OTHER direction: its sibling lit test
// (`auto_property_read_keeps_capture_narrowing_spec_7_4`) pins that a
// non-virtual AUTO property read must NOT invalidate, and an imported one is
// still a plain backing-field load. So the imported arm asks the existing
// both-sources storage classification (`isAutoPropertyStorageSymbol`,
// sema/Symbol.hpp — the #2478 wire bit, `bodyKind == Auto && !static &&
// !abstract`) plus the wire-carried `modifiers`, and reproduces the AST answer
// rather than approximating it.
//
// Residual, deliberate over-conservatism: a `static` imported auto-property
// reads `true` here, because the #2478 wire bit is cleared for `static` and the
// import carries nothing else that separates a static auto body from a static
// expression body. That costs a narrowing optimization on a shape that must also
// be inside a mutating-capture block to be observable; it is never unsound.
bool propertyAccessRunsUserCode(const PropertySymbol* ps, const SemaContext& ctx) {
    // Unknown symbol ⇒ conservative. No current site can reach this (each
    // dynamic_casts first), and that is the point: the NEXT site cannot be
    // written unsoundly by omission.
    if (!ps) return true;
    if (auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl))
        return propertyAccessRunsUserCode(pd);
    // IMPORTED (§2.5): no ast::PropertyDecl ever crosses the boundary.
    // §23.4 — an indexer is never auto, so its accessor is always user code.
    if (ps->isIndexer) return true;
    for (std::string_view m : ps->modifiers)
        if (m == "virtual" || m == "abstract" || m == "override") return true;
    // A plain imported auto-property is a backing-field load, exactly as the
    // local one is. Anything else — computed, static, or unclassified — runs
    // user code as far as this rule is concerned.
    return !isAutoPropertyStorageSymbol(ps, ctx);
}

// §18.5 / stdlib §2.18 (v0.172, #812/#1339): the FORMAT portion of an encoded
// InterpPart::formatSpec (the codegen encoding is `[","WIDTH][":"FORMAT]`). A
// bare alignment (`,WIDTH` with no `:FORMAT`) carries no format and applies to
// any operand. Returns the format text (empty when there is none).
std::string_view interpFormatPortion(const std::string& spec) {
    std::string_view s = spec;
    if (s.empty()) return {};
    if (s.front() == ',') {
        auto colon = s.find(':');
        return (colon == std::string_view::npos) ? std::string_view()
                                                  : s.substr(colon + 1);
    }
    return s;
}

// §2.18: the intrinsically-formattable primitives (integers, floating, decimal).
// bool/char/string/Object are NOT formattable — a format spec on them is a
// compile error.
bool isIntrinsicallyFormattablePrim(PrimitiveType p) {
    using P = PrimitiveType;
    switch (p) {
        case P::Byte:  case P::SByte: case P::Short: case P::UShort:
        case P::Int:   case P::UInt:  case P::Long:  case P::ULong:
        case P::Float: case P::Double: case P::Decimal:
            return true;
        default:
            return false;  // Bool, Char, String, Object, None
    }
}

// The last name component of a Named TypeRef (declaration name, else the
// AST/imported spelling). Empty when unavailable.
std::string_view namedTypeName(const TypeRef* t) {
    if (!t || t->kind != TypeRef::Kind::Named) return {};
    if (t->declaration && !t->declaration->name.empty())
        return t->declaration->name;
    if (t->astNode && t->astNode->name && !t->astNode->name->parts.empty())
        return t->astNode->name->parts.back();
    if (!t->importedName.empty()) return t->importedName;
    return {};
}

// §2.18: the intrinsically-formattable temporal + Guid builtins.
bool isIntrinsicTemporalOrGuid(std::string_view n) {
    return n == "DateTime" || n == "DateOnly" || n == "TimeOnly" ||
           n == "TimeSpan" || n == "Guid";
}

// §18.5 (v0.172): does `ts` (transitively) declare `: IFormattable`? IFormattable
// is a null-declaration builtin interface, so it never lands in `baseTypes`;
// the walk recurses through resolved base classes / user interfaces for the
// `interface IMyFmt: IFormattable` case.
//
// fix #3139 (§18.5 / §2.5, family F1 of #3152): each level's own conformance
// came from its `ast::TypeDecl` base list, which an IMPORTED type does not
// have — so an imported `class Money: IFormattable` answered false and a
// format spec on it was rejected with "`Money` is neither" while the identical
// local declaration was accepted (measured). `sema::inheritsBuiltinInterface`
// is the same transitive walk over the symbol, on both sides.
bool declaresIFormattable(const TypeSymbol* ts) {
    return inheritsBuiltinInterface(ts, "IFormattable");
}

// §18.5 / §2.18 (v0.172): may a hole with a real format spec apply to an operand
// of type `t`? Nullable is transparent (null renders "null"; the spec applies to
// the payload). An unresolved generic type parameter is admitted conservatively
// (its concrete formattability is a bound-analysis question left to codegen).
bool isFormattableOperandType(const TypeRef* t) {
    if (!t) return true;  // already errored elsewhere; don't cascade
    if (t->kind == TypeRef::Kind::Primitive)
        return isIntrinsicallyFormattablePrim(t->primitive);
    if (t->kind == TypeRef::Kind::Named) {
        std::string_view n = namedTypeName(t);
        if (isIntrinsicTemporalOrGuid(n)) return true;
        if (t->declaration && declaresIFormattable(t->declaration)) return true;
        // Unresolved generic type parameter (no declaration): admit.
        if (!t->declaration) return true;
        return false;  // plain class/record/enum/interface, Object
    }
    return true;  // non-Named/non-Primitive (unit/function/array): not reached
                  // with a spec in practice; don't spuriously error.
}

// The source spelling of a primitive type, for diagnostics.
const char* primitiveSpelling(PrimitiveType p) {
    using P = PrimitiveType;
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
}

// §5.1/#1204: returns true when `e` is an "integer-literal-derived
// expression" — built entirely from INTEGER literals via unary/binary
// operators.  Such expressions have no declared signed/unsigned type of
// their own and are the §5.1 "constant narrowing" source: "AN EXPRESSION OF
// INTEGER TYPE that is a compile-time constant implicitly converts to any
// integer type whose range contains its value" (e.g. `ulong > 1000`,
// `ulong > 0 - 5`).  Only named variable references or non-integer-literal
// sub-exprs cause this to return false, triggering the signed↔unsigned
// rejection below.
//
// fix #4512 (§5.1, v0.306): narrowed from also matching `FloatLiteralExpr` —
// this predicate is the ONE gate for §5.1's constant-narrowing escape
// (reportNoImplicitConversionBetweenOperands, below), and that paragraph's
// own text is exact about its domain: an INTEGER-typed constant into an
// integer-typed target, never a float/decimal one. The old (over-broad)
// match let a FLOAT literal "adopt" a `decimal` operand it has no §5.1
// relation to (`decimal` and binary floats are on §5.1's "never implicit"
// list, and neither is an integer type, so nothing else in this rule would
// have caught it): measured on 67b79744 (docs v0.306), `var d: decimal :=
// 1m; if d > 5.0 then …` reached codegen and failed IR verification
// (`sext double 5.0 to i32`, "SExt only operates on integer"). No caller
// outside `reportNoImplicitConversionBetweenOperands` uses this predicate,
// so narrowing it is invisible everywhere else.
static bool isLiteralDerivedExpr(const ast::Expr* e) {
    if (!e) return false;
    if (dynamic_cast<const ast::IntLiteralExpr*>(e))   return true;
    if (auto* ue = dynamic_cast<const ast::UnaryExpr*>(e))
        return isLiteralDerivedExpr(ue->operand.get());
    if (auto* be = dynamic_cast<const ast::BinaryExpr*>(e))
        return isLiteralDerivedExpr(be->left.get()) &&
               isLiteralDerivedExpr(be->right.get());
    return false;
}

// fix #4330 (§9.5/§15.4, v0.298): simple (last-component) name of a Named
// TypeRef, or empty for non-Named. A file-local copy of ExprTypecheck.cpp's
// `namedTypeSimpleName`, which is `static` to its own translation unit —
// `ExprTypecheck_Calls.cpp` carries the identical copy under
// `callsNamedTypeSimpleName` for the same reason (needed by
// `reportCollectionLiteralInterfaceTarget`'s call site here, the `with`-
// expression admissibility gate below).
static std::string_view exprsNamedTypeSimpleName(const TypeRef* ref) {
    if (!ref || ref->kind != TypeRef::Kind::Named) return {};
    if (ref->declaration) return ref->declaration->name;
    if (!ref->importedName.empty()) return ref->importedName;
    if (ref->astNode && ref->astNode->name &&
        !ref->astNode->name->parts.empty())
        return ref->astNode->name->parts.back();
    return {};
}

// #625 / §12 / §31 / fix #2950 / fix #2971: the "does `ts` inherit a built-in
// exception" wrapper that used to live here is gone. It answered only half the
// question the unqualified-identifier resolver actually asks — the other half
// being WHICH §31.4 member the bare name denotes, which the resolver then
// hard-coded as `Message` alone. Both halves now come from
// `sema::builtinExceptionSurfaceMemberOf` (Symbol.hpp), the single statement of
// §31.4's surface; see the call site below.
//
// (What #2950 fixed here is preserved by that helper's own walk: this was a
// THIRD hand-rolled copy of the ancestor walk — AST base list, `b->decl`
// required on each ancestor step, and its own positive name list — so a LOCAL
// class whose base was an IMPORTED exception subclass got "unresolved
// identifier `Message`" where the identical hierarchy declared in ONE module
// resolved it.)

// §14.1 (#2006): REMOVED by fix #4229 — replaced by
// `ExprTC::typeTestTargetMentionsTypeVariable`, a member that threads
// `currentScopeGenericParamNames()` through `refOutParamTypeMentionsTypeParam`
// (the same ground truth `mentionsUnsubstitutedTypeParam` and #4199's
// `variantGenericAssignable` already use). The two static twins in
// ExprTypecheck.cpp and THIS file were structurally identical and BOTH
// misclassified the ~50 null-declaration stdlib builtins (`Task`,
// `Exception`, `IDisposable`, `Mutex`, `Semaphore`, `DateTime`, `TimeSpan`,
// `Guid`, `StringBuilder`, `Random`, `File`, `Path`, `IntPtr`, `CString`,
// ... — #4229) as "free type variables" — the structural guess returned
// TRUE on their shape alone (Named + null decl + no args + non-empty head),
// silently suppressing the §12.7 / §17.4 / §2.8.3 rule 4 erasure guards
// (#1939/#2006/#2012/#2018). The plain `is`/`as` (ExprTypecheck_Exprs.cpp
// ~2400) and value-form `case` binding (~10360) call sites both invoke
// `ctx_.typeTestTargetMentionsTypeVariable(...)` now.

}  // namespace

// §17.11 (#951): does `default(T)` have no well-defined zero value? See the
// header declaration. Recurses into a record's storage members and an inline
// array's element type.
//
// fix #3982/#3986/#3988/#3995 (§17.11/§9.7/§9.3/§3.6 rule 2, v0.294): this
// function's history — symbol-side (not `TypeSymbol::decl`, null for every
// imported type) field enumeration; generic-parameter substitution via
// `common::RecordSubstEnv`; full `recordStorageMembers` coverage (positional
// + body + auto-properties, not positional-only); and `incoming`-env
// composition so a DELEGATING generic record's inner frame resolves its own
// bare-parameter mentions. See git history / #3995's issue for the full
// per-fix rationale and measured repros — preserved there rather than
// duplicated here now that the implementation itself has moved.
//
// fix #4166 review follow-up (§14.2/§17.11/§35.10, v0.298): the
// implementation is now the SHARED free function `sema::typeHasNoDefault`
// (TypeResolution.hpp / TypeResolution_Checks.cpp), which
// `Resolver::argSatisfiesNew`'s Record arm also calls for the identically-
// shaped `where T: new()` question — see that declaration's comment for
// why the two needed unifying (they had drifted: `argSatisfiesNew`'s own
// port covered only Record-kind recursion, diverging from this function's
// full per-kind switch on Enum/Choice/InlineArray and the
// `isValueBuiltin`-aware Class/Interface test — issues #4219, #4223) and
// what the shared function gained in the process (a cycle guard THIS
// function itself never had — a self-referential record reached through
// `default(...)` SIGSEGV'd here identically to the `where T: new()` shape
// that review caught, on every revision before this one). This wrapper
// exists so every existing call site of `ExprTC::defaultIllFormed` — there
// are close to a dozen across this file and ExprTypecheck.cpp — keeps the
// same signature with the implementation hoisted.
//
// TWO deliberate behavior changes survive the hoist, both in
// `argSatisfiesNew`'s favor because its pre-hoist, categorize()-based leaf
// test was stricter than this function's own prior text on each:
//
//   * a non-nullable `Object`-typed component now ALSO reports
//     non-defaultable (previously excluded here — "left out... to keep
//     this consistent with the original #951 scope — a separate
//     consideration", this function's own prior text). §17.11's
//     "defaultable" definition is unambiguous regardless of which side
//     moved: "Not defaultable: non-nullable reference types" includes
//     `Object`.
//   * a null-declaration `Named` type naming a stdlib COLLECTION or `Task`
//     builtin (`Array`, `Dictionary`, `Set`, `OwningTask`, …) now ALSO
//     reports non-defaultable. This function's prior text returned
//     `false` unconditionally for the WHOLE null-declaration population
//     (collections and value builtins like `Guid`/`IntPtr`/`DateTime`
//     alike); `argSatisfiesNew`'s pre-hoist `categorize()`-based leaf test
//     inherited a coarser but directionally-correct distinction (every
//     builtin ⇒ reference) from its own fallback. Review caught the
//     regression this hoist created (`record Box(A: Array<int>)`: REJECT
//     on the parent, silent ACCEPT on the un-fixed hoist) — the THIRD
//     predicate this run caught misclassifying this same ~50-builtin
//     null-declaration population (issue #4229 tracks two more;
//     `fix #4199` a fourth). The fix here is narrower than
//     `categorize()`'s own fallback, BY NAME, scoped to the collection/
//     `Task` family only — see `typeHasNoDefaultWalk`'s Named-arm comment
//     (TypeResolution_Checks.cpp) for why the wholesale mirror was tried
//     and reverted (it broke `default(Guid)` / `default(IntPtr)`,
//     tests/lit/builtin_closed_ctor_surface_accept_2994.bzl, which this
//     function's OWN prior text had always accepted).
//
// Unifying toward this function's OWN narrower prior text on either would
// have silently reintroduced a false accept in `argSatisfiesNew` — caught
// by this fix's own broad regression sweeps. See `typeHasNoDefaultWalk`'s
// Primitive-arm and Named-arm comments (TypeResolution_Checks.cpp) for the
// full notes on each.
bool ExprTC::defaultIllFormed(const TypeRef* t,
                              const common::RecordSubstEnv* incoming) {
    return sema::typeHasNoDefault(t, ctx_, incoming);
}

// per §27.4 (v0.183, #1408) / A.7 LambdaDestructure. See declaration
// (ExprTypecheck_impl.hpp) for the contract. Mirrors the §19.3 case-pattern
// DestructuringPattern desugar (ExprTypecheck.cpp CaseStmt / ExprTypecheck_
// Exprs.cpp CaseExpr) but (a) checks the head against an exact target type
// instead of doing a runtime subtype test, and (b) binds straight into the
// lambda's own scope rather than a fresh per-arm scope.
TypeRef* ExprTC::bindLambdaDestructure(const ast::DestructuringPattern& dp,
                                       TypeRef* targetType, Scope& scope) {
    std::string_view lastName = dp.typeName && !dp.typeName->parts.empty()
        ? dp.typeName->parts.back() : std::string_view{};
    // Resolve the head to a record TypeSymbol. Prefer module-local types
    // (matches the CaseStmt/CaseExpr lookup), then the broader local symbol
    // table, then `importedSymbols()` — the imported record's TypeSymbol
    // lives there, not in `symbols()` (which is local-only per
    // SemaContext.hpp:90's `symbols_`). The pre-fix comment claimed
    // `ctx_.symbols()` covered "imported/builtin records"; that was
    // incorrect, and the lambda-destructure shape over an IMPORTED record
    // hit the §27.4 §"name does not name a known positional record type"
    // diagnostic instead of lowering. Same widening the #2844 fix
    // applied to patternMatches and bindLambdaDestructureParam in
    // LlvmBackend_Exprs2.cpp.
    TypeSymbol* recTy = nullptr;
    bool foundClass = false;
    auto considerSym = [&](Symbol* s) {
        auto* ts = dynamic_cast<TypeSymbol*>(s);
        if (!ts || ts->name != lastName) return;
        if (ts->kind == ast::TypeDeclKind::Record) { recTy = ts; return; }
        if (ts->kind == ast::TypeDeclKind::Class) foundClass = true;
    };
    if (ctx_.module()) {
        for (Symbol* child : ctx_.module()->children) {
            if (recTy) break;
            considerSym(child);
        }
    }
    if (!recTy) {
        for (const auto& sym : ctx_.symbols()) {
            if (recTy) break;
            considerSym(sym.get());
        }
    }
    if (!recTy) {
        for (Symbol* s : ctx_.importedSymbols()) {
            if (recTy) break;
            considerSym(s);
        }
    }
    if (!recTy) {
        // §27.4: "The head must name a positional record type." Classes have
        // no positional-destructuring surface (§19.3) — dedicated wording for
        // that common mistake; a name that resolves to nothing at all gets
        // the generic unknown-type wording.
        if (foundClass) {
            ctx_.diagnostics().report(
                Severity::Error, dp.span.begin,
                "destructuring lambda parameter `" + std::string(lastName) +
                "(...)` is not valid on a class type \xe2\x80\x94 classes do "
                "not have a positional-destructuring surface; the head of a "
                "destructuring lambda parameter must name a positional "
                "record type (\xc2\xa7""27.4)");
        } else {
            ctx_.diagnostics().report(
                Severity::Error, dp.span.begin,
                "`" + std::string(lastName) + "` in a destructuring lambda "
                "parameter does not name a known positional record type "
                "(\xc2\xa7""27.4)");
        }
        return nullptr;
    }
    // §27.4: "it must be exactly the parameter's type: when a target type
    // supplies the parameter type, a head naming any other type is a compile
    // error (no subtype test, nothing to fail at runtime)." Applies both at
    // the top level (targetType = the lambda's expected positional param
    // type) and recursively for a nested slot (targetType = the enclosing
    // field's resolved type) — a nested head naming a mismatched type is
    // exactly as wrong as a top-level one.
    if (targetType &&
        (targetType->kind != TypeRef::Kind::Named ||
         targetType->declaration != recTy || targetType->nullable)) {
        std::string targetDesc = targetType->nullable
            ? "a nullable type"
            : (targetType->kind == TypeRef::Kind::Named && targetType->declaration
                   ? "`" + std::string(targetType->declaration->name) + "`"
                   : "a non-record type");
        ctx_.diagnostics().report(
            Severity::Error, dp.span.begin,
            "destructuring lambda parameter head `" + std::string(lastName) +
            "` does not match the expected parameter type (" + targetDesc +
            ") \xe2\x80\x94 the head must name exactly the parameter's type, "
            "with no subtype test (\xc2\xa7""27.4)");
        return nullptr;
    }
    // Collect the record's non-static positional fields, in declaration
    // order (same filter the CaseStmt/CaseExpr handlers use).
    std::vector<FieldSymbol*> fields;
    for (Symbol* m : recTy->members) {
        auto* fs = dynamic_cast<FieldSymbol*>(m);
        if (!fs) continue;
        bool isStatic = false;
        for (auto& mod : fs->modifiers) if (mod == "static") { isStatic = true; break; }
        if (!isStatic) fields.push_back(fs);
    }
    if (dp.subPatterns.size() != fields.size()) {
        ctx_.diagnostics().report(
            Severity::Error, dp.span.begin,
            "destructuring lambda parameter `" + std::string(lastName) +
            "(...)` binds " + std::to_string(dp.subPatterns.size()) +
            " value(s) but record `" + std::string(lastName) + "` has " +
            std::to_string(fields.size()) +
            " positional field(s); the binding count must match "
            "(\xc2\xa7""27.4)");
        // Fall through and bind whatever slots line up with a real field —
        // still gives the body a chance to type-check the fields that do
        // exist rather than cascading "unresolved identifier" everywhere.
    }
    for (std::size_t i = 0; i < dp.subPatterns.size() && i < fields.size(); ++i) {
        const auto& sp = dp.subPatterns[i];
        // fix #3962 (§27.4/§7/§9.1, v0.294): read through
        // `storageComponentType` (sema/Symbol.hpp) rather than the bare
        // `posParam->type` — for an IMPORTED record's positional field, fix
        // #2516 documents the synthetic `posParam`'s `type` as deliberately
        // null (the real type lives in `ctx_.importedMemberType`, reached
        // only through this accessor). The bare read left `fieldTy` null for
        // every destructured slot over an imported record, which silently
        // disabled every check keyed on it — measured: a nullable field
        // (`X: int?`) destructured into a lambda parameter and passed to a
        // non-nullable-typed call argument compiled cleanly when `X`'s
        // record was imported, and was correctly rejected (§7) when local.
        TypeRef* fieldTy = const_cast<TypeRef*>(
            storageComponentType(fields[i], ctx_));
        if (!sp.nakedBinding.empty()) {
            // per §27.4: "each binding is an ordinary read of the
            // corresponding positional field at body entry" — a naked
            // identifier slot (`Entry(k, v)`).
            auto* lsym = ctx_.makeSymbol<LocalSymbol>();
            lsym->name = sp.nakedBinding;
            lsym->decl = sp.nakedAnchor.get();  // #1271 real Node anchor
            lsym->declType = fieldTy;
            scope.names[sp.nakedBinding] = lsym;
        } else if (dynamic_cast<const ast::DiscardPattern*>(sp.pattern.get())) {
            // `_`: binds nothing (parser already validated only `_`, a
            // naked binding, or a nested destructuring reach here).
        } else if (auto* nested = dynamic_cast<const ast::DestructuringPattern*>(
                       sp.pattern.get())) {
            // Nested LambdaDestructure — recurse, checked against this
            // slot's own field type.
            bindLambdaDestructure(*nested, fieldTy, scope);
        }
    }
    auto* rt = ctx_.makeTypeRef();
    rt->kind = TypeRef::Kind::Named;
    rt->declaration = recTy;
    return rt;
}

// §27.3 (v0.226, #2107): see ExprTypecheck_impl.hpp. Recursively collect the
// resolved type of every value-carrying `return e;` in a block-bodied lambda
// body. Descends through every statement wrapper that may lexically contain a
// `return` (do/end, if/elif/else, while, for, try/catch/finally, statement-
// position case block arms) but NOT into nested lambdas — a nested lambda is an
// expression value (LocalDecl initializer, AssignmentStmt value, ReturnStmt
// value, ExpressionStmt expr, …) and its returns belong to its own callable,
// not this one. A `return;` (no value) contributes nothing (procedure-exit).
void ExprTC::collectLambdaReturnTypeFromStmt(const ast::Stmt* s,
                                             std::vector<TypeRef*>& out) {
    if (!s) return;
    if (auto* rs = dynamic_cast<const ast::ReturnStmt*>(s)) {
        if (rs->value)
            if (auto* t = ctx_.exprType(*rs->value)) out.push_back(t);
        return;
    }
    if (auto* bs = dynamic_cast<const ast::DoEndBlock*>(s)) {
        collectLambdaReturnTypes(bs->body, out);
        return;
    }
    if (auto* ifs = dynamic_cast<const ast::IfStmt*>(s)) {
        collectLambdaReturnTypes(ifs->ifBranch.body, out);
        for (auto& el : ifs->elifBranches)
            collectLambdaReturnTypes(el.body, out);
        collectLambdaReturnTypes(ifs->elseBody, out);
        return;
    }
    if (auto* ws = dynamic_cast<const ast::WhileStmt*>(s)) {
        collectLambdaReturnTypes(ws->body, out);
        return;
    }
    if (auto* fs = dynamic_cast<const ast::ForStmt*>(s)) {
        collectLambdaReturnTypes(fs->body, out);
        return;
    }
    if (auto* ts = dynamic_cast<const ast::TryStmt*>(s)) {
        collectLambdaReturnTypes(ts->body, out);
        for (auto& arm : ts->catches)
            collectLambdaReturnTypeFromStmt(arm.body.get(), out);
        collectLambdaReturnTypes(ts->finallyBody, out);
        return;
    }
    if (auto* cs = dynamic_cast<const ast::CaseStmt*>(s)) {
        for (auto& arm : cs->arms)
            if (arm.bodyKind == ast::CaseArmBodyKind::Block)
                collectLambdaReturnTypes(arm.stmts, out);
        return;
    }
    // Every other statement (LocalDecl, AssignmentStmt, ExpressionStmt,
    // BreakStmt, ContinueStmt, ThrowStmt, YieldStmt, DiscardStmt, …) carries
    // expressions, not `return` statements; a nested lambda lives in those
    // expressions and is excluded by construction.
}

void ExprTC::collectLambdaReturnTypes(
        const std::vector<std::unique_ptr<ast::Stmt>>& stmts,
        std::vector<TypeRef*>& out) {
    for (const auto& s : stmts)
        collectLambdaReturnTypeFromStmt(s.get(), out);
}

// fix #2619 (§7.1 / §7.4 / §23.4, v0.240) — the §7 check on an INDEX argument.
//
// §23.4: "An indexer is a property whose name is replaced by `[parameters]`",
// so an index argument is a parameter pass, and §7.1: "The compiler enforces
// the distinction at every assignment, parameter pass, and return." Every
// indexer path in sema skipped that check (see the declaration's comment for
// the failure it produced); this runs it once per IndexExpr for every receiver
// family, builtin and user-declared alike.
//
// WHY THIS CANNOT FALSE-REJECT (the hazard for a widened rejection):
//
//  1. It fires only when the index argument's OWN static type is nullable.
//     Every non-nullable index spelling the compiler accepts today — a plain
//     `int`, a `const` local, an enum, a bare generic `T`, a `long`, a Range
//     literal `[a..b]`, a Range-typed value — never reaches the check at all.
//     (This is the gate #2613 chose, kept and generalized.)
//  2. The target it checks against is the index's own type with `nullable`
//     cleared — NOT the indexer's declared parameter type. So the only
//     diagnostic reachable from here is the §7 "…is nullable but the target
//     type is non-nullable" one; a type MISMATCH between an index argument and
//     a declared parameter is a different question this fix does not open
//     (nor does it touch the named/`ref`/`owned`/discard modifier axis, which
//     is #2758's unresolved design question — those forms are skipped).
//  3. The §7.4 narrowing exception is inherited by construction: it lives
//     inside `checkAssignable`, which this reuses, so `if i <> null then a[i]`,
//     an early-return narrowing, a `const` narrowing and `a[i!]` all stay
//     accepted (§7.4 deliberately does not rewrite `exprType`, so a naive
//     "is the argument type nullable" rejection would break them).
//  4. A declared index parameter that is ITSELF nullable legitimately accepts
//     a nullable argument (`property [i: int?]`, a `Dictionary<string?, V>`
//     key). Those are detected from the declared parameter type — substituted
//     against the receiver's concrete type arguments, so a `Box<int?>` whose
//     indexer is declared `[k: T]` reports `int?` — and skipped.
void ExprTC::checkIndexArgNullability(const ast::IndexExpr& ix,
                                      const ast::Expr& e, TypeRef* recvTy) {
    if (!recvTy || ix.indices.empty()) return;

    // The receiver's declared index parameters, where they are knowable:
    //   * a USER-declared indexer, whose resolved PropertySymbol the §23.4
    //     branch attached to this node (#2530). Its k-th parameter type is
    //     read through the ast::PropertyDecl for a local indexer and through
    //     the reconstructed ParameterSymbols for an IMPORTED one (#2414's
    //     dual read), then substituted like the result type is.
    //   * a builtin dictionary receiver, whose indexer is `[key: K]`
    //     (stdlib §4.8 / §4.9 / §4.17) — K is genericArgs[0].
    // Every other family (Array<T> and the list interfaces, the span/buffer
    // families, `string`, inline arrays, an indexer reached through a
    // type-parameter constraint) declares a NON-nullable index parameter, so
    // leaving `declared` null means exactly "non-nullable" here.
    PropertySymbol* ps = nullptr;
    if (auto* sym = dynamic_cast<PropertySymbol*>(ctx_.exprSymbol(e)))
        if (sym->isIndexer) ps = sym;
    TypeRef* dictKey = nullptr;
    if (!ps && recvTy->kind == TypeRef::Kind::Named && !recvTy->declaration &&
        !recvTy->genericArgs.empty() && recvTy->genericArgs[0]) {
        std::string_view cn = recvTy->headName();
        if (cn == "Dictionary" || cn == "OwningDictionary" ||
            cn == "SortedDictionary")
            dictKey = recvTy->genericArgs[0];
    }
    auto declaredIndexParam = [&](std::size_t k) -> TypeRef* {
        if (dictKey) return k == 0 ? dictKey : nullptr;
        if (!ps) return nullptr;
        TypeRef* p = nullptr;
        if (auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl)) {
            if (k < pd->indexerParams.size() && pd->indexerParams[k].type)
                p = ctx_.resolvedType(*pd->indexerParams[k].type);
        } else if (k < ps->indexerParams.size()) {
            if (auto* prm =
                    dynamic_cast<ParameterSymbol*>(ps->indexerParams[k]))
                p = prm->resolvedDeclType;
        }
        if (!p) return nullptr;
        return substituteMemberType(p, ix.target.get(), ps->parent);
    };

    // fix #4084 (§28.3 / §23.4 / §14.1, v0.294): cache the substituted
    // declared parameter types per indexer-access slot so the ownership pass
    // (`recordCallArgDirectLambdaTargetsFromIndexer`,
    // Ownership_Checks.cpp, fix #4075) can read them — pre-fix, that
    // resolver read the RAW declared `T` off `pd->indexerParams` /
    // `ps->indexerParams` and never called `substituteMemberType`, so a
    // generic indexer whose parameter is a bare type parameter `T`
    // (`property [mk: T]`) instantiated at a call site with a function
    // type (`Box<() -> owned Res>`) silently bypassed the
    // borrow-owned-return check (#4075's own defect class reopened through
    // generics). Populated here at the SAME point the §7 nullability check
    // computed each substitution — the `substituteMemberType` call
    // immediately above — so no work is duplicated and the cache cannot
    // disagree with the §7 verdict. Skipped for the dictionary / non-`ps`
    // cases: `recordCallArgDirectLambdaTargetsFromIndexer` only reads this
    // cache when `ps` (a user-defined indexer) is present, and a dictionary
    // never declares a function-typed index parameter anyway.
    if (ps) {
        std::vector<sema::TypeRef*> sub;
        sub.reserve(ix.indices.size());
        for (std::size_t k = 0; k < ix.indices.size(); ++k)
            sub.push_back(declaredIndexParam(k));
        ctx_.setIndexerSubstitutedParamTypes(&ix, std::move(sub));
    }

    // The diagnostic's subject. `<Receiver>[i]` (or `[key]` for a dictionary)
    // for the single-index case — the wording #2613 introduced — and an
    // ordinal for a multi-parameter indexer, where "the index argument" alone
    // would not say which one.
    std::string recvName;
    if (recvTy->kind == TypeRef::Kind::Named) {
        recvName = std::string(recvTy->headName());
        if (recvName.empty() && recvTy->declaration)
            recvName = std::string(recvTy->declaration->name);
    } else if (recvTy->kind == TypeRef::Kind::Primitive &&
               recvTy->primitive == PrimitiveType::String) {
        recvName = "string";
    }
    const std::size_t nargs = ix.indices.size();
    auto subject = [&](std::size_t k) -> std::string {
        if (nargs == 1) {
            if (recvName.empty()) return "index argument";
            return "index argument to `" + recvName +
                   (dictKey ? "[key]`" : "[i]`");
        }
        std::string s = "index argument " + std::to_string(k + 1);
        if (!recvName.empty()) s += " to the `" + recvName + "` indexer";
        return s;
    };

    for (std::size_t k = 0; k < nargs; ++k) {
        const auto& arg = ix.indices[k];
        // §22.3 / #2758: a named / `ref` / `out` / `owned` modifier or a `_`
        // discard on an index argument is a separate, unresolved question
        // (#2758). This governs the index VALUE only — the same guard #2613's
        // check carried.
        if (!arg.value || arg.isDiscard || !arg.modifier.empty()) continue;
        TypeRef* idxTy = ctx_.exprType(*arg.value);
        if (!idxTy || !idxTy->nullable) continue;      // gate (1)
        TypeRef* declared = declaredIndexParam(k);
        if (declared && declared->nullable) continue;  // gate (4)
        TypeRef* nonNull = ctx_.makeTypeRef(*idxTy);   // gate (2)
        nonNull->nullable = false;
        checkAssignable(idxTy, nonNull, arg.value->span.begin, subject(k),
                        arg.value.get());
    }
}

// fix #2758 (§23.5, v0.241) — INDEX ARGUMENTS ARE POSITIONAL-ONLY.
//
// §23.5, verbatim: "Index arguments are **positional-only**. The named-argument
// form (`Identifier :=`, §22.5) is not admitted at an index position — a
// compile error naming this rule. Indexer parameters may not declare default
// values, so §22.5's optional-parameter and named-binding machinery is never
// reached from an index position; an indexer's parameter names are
// declaration-internal, not caller-bindable API surface (consistent with
// §23.4's overloading by parameter *type* list)."
//
// WHY THE FORM EXISTS TO REJECT. A.7's `PrimarySuffix ::= "[" ArgList "]"`
// shares the CALL argument grammar, and the parser genuinely shares the
// production (`Parser::parseArgList`, Parser_Exprs.cpp) rather than merely the
// struct — so `d[key := "k"]` parses into a `CallArgument` with a non-empty
// `name`. A.4's Argument comment records exactly that over-admission: "Named
// arguments are additionally rejected at the index position (§23.5) — admitted
// by the shared grammar, rejected semantically."
//
// WHAT IT COST BEFORE. Indexer overload selection never consults `a.name`, and
// ~18 sema/codegen sites map `indices[k]` to `indexerParams[k]` positionally.
// Measured on 309356e against `property [key: string, fallback: int]: int`:
//
//   d[key := "abcd", fallback := 1]   accepted; the names are dropped and the
//                                     arguments bind by position (they happen
//                                     to agree, so it "works")
//   d[fallback := 1, key := "abcd"]   accepted by sema, then `1` binds to the
//                                     `string` slot and bezelc DIES IN THE LLVM
//                                     VERIFIER: "GEP base pointer is not a
//                                     vector or a vector of pointers", "Load
//                                     operand must be a pointer", "Call
//                                     parameter type does not match function
//                                     signature" — an internal compiler error
//                                     where §23.5 asks for a diagnostic.
//
// WHY IT CANNOT FALSE-REJECT. The predicate is `!a.name.empty()`, which the
// parser sets ONLY for the literal `Identifier ":="` prefix inside the
// brackets (Parser_Decls.cpp parseArgList) and never for a modifier, a
// discard, or any expression — including a bare index whose value is a local
// literally named `key` (`d[key, 1]`), which parses as an unmodified
// ArgExpression with an EMPTY name. Every positional spelling on every
// receiver family (user indexer, `Array<T>`, `Dictionary<K,V>`, `string`, an
// inline array, a span, `this[i]`, `base[i]`, an overloaded or generic
// indexer, the setter form, a trailing comma) leaves `name` empty and is not
// reached at all; the accept controls in tests/lit/index_positional_accept_2758
// .bzl and tests/sema/index_named_arg_positional_only_2758_test.cpp pin each.
// The named form in a CALL (`M(x := 1)`) and in a `new` (`new C(v := true)`)
// is a different node kind entirely and is untouched.
void ExprTC::checkIndexArgsPositional(const ast::IndexExpr& ix) {
    for (const auto& a : ix.indices) {
        if (!a.modifier.empty()) {
            // fix #3097 (§23.5, v0.249) — INDEX ARGUMENTS ARE NEVER MODIFIERED.
            //
            // §23.5, verbatim (v0.249): "`ref` and `out` parameters are not
            // allowed in indexer parameter lists." §28.1's closed-positions
            // list for `owned` (and `scoped`, §28.14) forbids either on an
            // argument whose position's parameter is determined by the
            // indexer's declaration. The accepted modifier spellings on a
            // `CallArgument` (Node.hpp `CallArgument::modifier`, §22.3 /
            // §28.14, #1439) are "ref", "out", "out var", "out using var",
            // "out owned", "out owned var", "owned", "scoped" — every one of
            // them is forbidden at an index position. Ownership of an index
            // argument is determined by the indexer's declared parameter
            // ownership, so a `ref`/`out`/`owned`/`scoped` modifier here
            // names nothing on either side: §23.5 declares the parameter
            // side closed, and #2758 declared the call-site side closed too
            // (the named-argument arm), leaving this modifier arm as the
            // remaining gap. #3049 closed `_` at the parser; `out _` falls
            // under this check too (modifier "out", `isDiscard == true`).
            //
            // Measured on the parent commit (c6a5355) the four modifiers
            // and `out _` all parsed and reached sema; `o[out _]` then
            // degraded at codegen behind an `internal: return-value
            // expression failed to lower … (degraded to a zero/null return
            // value) — #2491`, and the `o[out var i]` shape rejected for
            // the wrong reason ("unresolved identifier i") — a silent
            // accept surface and a wrong-reason surface, neither of which
            // the spec admits.
            //
            // The modifier carries no span of its own (CallArgument carries
            // only the value's), so report at the value — inside the
            // brackets, which is what a reader needs — falling back to the
            // whole index expression for the value-less `d[owned _]` shape.
            ctx_.diagnostics().report(
                Severity::Error,
                a.value ? a.value->span.begin : ix.span.begin,
                "an index argument may not carry a `" +
                std::string(a.modifier) +
                "` modifier — ownership of an index argument is determined "
                "by the indexer's declared parameter ownership, and \xC2\xA7""23.5 "
                "forbids `ref`/`out` parameters on indexer parameter lists "
                "so neither `ref`/`out` nor `owned`/`scoped` names anything "
                "here. Drop the `" + std::string(a.modifier) +
                "` prefix and pass the index argument as a plain "
                "expression (\xC2\xA7""23.5)");
            continue;
        }
        if (a.name.empty()) continue;
        // The name has no span of its own (CallArgument carries only the
        // value's), so report at the value — inside the brackets, which is
        // what a reader needs — falling back to the whole index expression
        // for the value-less `d[key := _]` shape (#3049 rejects that `_`
        // separately; this still names the §23.5 rule for the prefix).
        ctx_.diagnostics().report(
            Severity::Error,
            a.value ? a.value->span.begin : ix.span.begin,
            "the named-argument form `" + std::string(a.name) +
            " := \xE2\x80\xA6` is not admitted at an index position — index "
            "arguments are positional-only: an indexer's parameter names are "
            "declaration-internal, not caller-bindable API surface (indexer "
            "parameters may not declare default values, so \xC2\xA7""22.5's "
            "named-binding machinery is never reached here, and \xC2\xA7""23.4 "
            "overloads indexers by parameter TYPE list). Drop the `" +
            std::string(a.name) + " := ` prefix and pass the index "
            "positionally (\xC2\xA7""23.5)");
    }
}

// fix #2994 — see the declaration. A generic type PARAMETER and a reserved
// builtin have the SAME TypeRef shape (Named, `declaration == nullptr`), so
// the closed-surface arms below must be able to tell them apart by name.
// Mirrors the §14.1 (#992) detection in the NewExpr arm: a name is a type
// parameter when the enclosing method's, the enclosing type's, or (fix
// #3644, a direct twin of #3643's `ExprTC::isInScopeTypeParam` in
// ExprTypecheck.cpp) the enclosing EXTENSION's own §26.4 block-level
// parameter names it.
//
// The extension arm was missing. Inside an extension body `currentType_`
// names the EXTENDED type (`visitTopDecl`'s `ExtensionDecl` arm), so the
// `currentType_` arm above answers this function's question only by
// NAME-COINCIDENCE — it asks whether the EXTENDED type's own declared
// `genericParams` contains `name`, which matches the extension's true
// answer (§26.4: `extension Box<T>` binds a FRESH `T` belonging to the
// extension, not to `Box`'s own declaration; `ast::ExtensionDecl` carries no
// `genericParams` field of its own, #3083) only when an author reuses the
// same letter, and is a false negative whenever they do not — or whenever
// the extended type is IMPORTED, since `TypeSymbol::decl` is unconditionally
// null for an import (`ModuleLoader.cpp`'s Pass-3 generic-body reparse
// attaches a decl to individual MEMBER symbols only, never to the TYPE's own
// `decl`), so the `currentType_` arm never fires at all on that side. A
// generic extension writing `new T()` (or the builtin-named shadow this
// function exists to guard, `new Guid()` where `Guid` is the extension's own
// type parameter) was therefore reachable through
// `checkBuiltinClosedConstructorSurface`'s per-name builtin checks for an
// imported extended type while a same-named local one was correctly
// excluded — a false REJECT (or a categorically worse diagnostic), not a
// silent widening.
bool ExprTC::isEnclosingGenericTypeParamName(std::string_view name) const {
    if (name.empty()) return false;
    if (currentMethodDecl_)
        for (const auto& gp : currentMethodDecl_->genericParams)
            if (gp.name == name) return true;
    if (currentType_ && currentType_->decl)
        if (auto* td = dynamic_cast<const ast::TypeDecl*>(currentType_->decl))
            for (const auto& gp : td->genericParams)
                if (gp.name == name) return true;
    if (currentExtension_ && currentExtension_->decl)
        if (auto* ed = dynamic_cast<const ast::ExtensionDecl*>(
                currentExtension_->decl))
            if (ed->targetType &&
                isExtensionTargetTypeParam(*ed->targetType, name))
                return true;
    return false;
}

// fix #2994 (§17.7, v0.241) — STANDARD-LIBRARY BUILTINS HAVE A CLOSED
// CONSTRUCTOR SURFACE.
//
// §17.7, verbatim: "For a standard-library builtin type — any type the
// standard-library document declares — the constructor surface is exactly what
// that document declares — `new T(...)` resolves only against a declared
// constructor, never against a compiler-supplied fallback. A builtin declaring
// **no** constructor — `Guid` (stdlib §2.6), `IntPtr`/`UIntPtr` (stdlib §10.0)
// — admits no `new T(...)` at any argument list; the diagnostic names the
// declared producers … For a builtin record whose only declared constructor is
// its positional primary constructor (`ExceptionInfo`, §31.4;
// `ReadOnlyStringSpan`, stdlib §3.3), the undeclared parameterless form
// `new T()` is rejected the same way — exactly as for an identically-shaped
// user record."
//
// WHAT IT REPLACES. Measured on 309356e, all five reached the RIGHT-ANSWER-BY-
// ACCIDENT path #2994 filed: `lowerNew` has no arm for them, returns nullptr,
// and `lowerLocalDecl`'s #2434 fallback stores `getNullValue(slotTy)` behind an
// `internal: … (degraded to a zero-initialized value) — #2434` message. For
// `Guid`/`IntPtr` the all-zero pattern happens to BE the documented value; for
// `ExceptionInfo` and `ReadOnlyStringSpan` it is a record of NULL strings —
// the #2922 class, arriving through a silent fallback.
//
// WHY IT IS KEYED ON NAMES. The compiler has no table of declared builtin
// constructors to resolve against: a builtin's constructors are arms of
// codegen's `lowerNew`, not `ConstructorSymbol`s sema can see (§22.7's implicit
// default-constructor check is gated on a real `TypeSymbol`, so it never sees a
// builtin at all). The two registry predicates are exactly §17.7's two
// enumerated populations; the rest of `kNonHeapReservedBuiltins` is out.
//
// WHY §14.2 IS UNAFFECTED. §14.2's generic-body equivalence — "`new T()` for a
// value substitution is equivalent to `default(T)`" — is written on `T`, and a
// generic body writes `new T()`, whose head name is `T`. `Guid` still satisfies
// `where T: new()` (its `default` is well-defined, §17.11), so a generic
// factory instantiated at `Guid` still mints empty Guids. The one shape that
// could confuse the two — a type parameter that SHADOWS a builtin name — is
// excluded by `isEnclosingGenericTypeParamName`.
//
// WHY IT CANNOT FALSE-REJECT A USER TYPE. A user type carries a real `decl`,
// or (imported) a non-empty `moduleName`; both are excluded before the name is
// consulted, exactly as in codegen's `unconstructedBuiltinNewName` twin. A user
// `class Guid` therefore keeps constructing.
// fix #4170 (§35.1/§35.2/§35.3, v0.295; mirrors #4163's §5.4/§10 enum
// rejection): the single authority for "does `new` construct a value of
// this kind" — neither an enum nor a choice has a constructor. See the
// declaration in ExprTypecheck_impl.hpp for the full rationale; this is
// the same logic the explicit `new T(...)` NewExpr-typing arm used to
// spell out inline (moved here so the two §17.7 shorthand call sites in
// ExprTypecheck.cpp can share it instead of duplicating the predicate a
// third and fourth time, per this repo's own "duplicated classification
// predicates" caution, CLAUDE.md / LANE_RULES.md §6).
bool ExprTC::rejectNewOnEnumOrChoice(const ast::NewExpr& ne,
                                      const TypeRef* rt) {
    if (!(rt->kind == TypeRef::Kind::Named && rt->declaration)) return false;
    if (rt->declaration->kind == ast::TypeDeclKind::Enum) {
        ctx_.diagnostics().report(
            Severity::Error, ne.span.begin,
            "`new` cannot construct an enum — an enum value is "
            "produced by the cast `" +
            std::string(rt->declaration->name) +
            "(n)` from its underlying integer, not with `new` "
            "(§5.4/§10)");
        return true;
    }
    if (rt->declaration->kind == ast::TypeDeclKind::Choice) {
        ctx_.diagnostics().report(
            Severity::Error, ne.span.begin,
            "`new` cannot construct a choice — a choice has no "
            "constructors; a choice's construction is case "
            "selection, written `" +
            std::string(rt->declaration->name) +
            ".Case(args)` instead of `new` (§35.1/§35.2/§35.3)");
        return true;
    }
    return false;
}

// fix #4249 (§17.7, v0.306; third sibling of `rejectNewOnEnumOrChoice`
// above and `rejectNewOnNonInstantiableKind` below — same shape, one
// authority, per this repo's "duplicated classification predicates"
// caution, CLAUDE.md / LANE_RULES.md §6). See the declaration in
// ExprTypecheck_impl.hpp for the full rationale. In one line: §17.7
// grants the `new()` shorthand only positions (i), (ii), (iii) and the
// `return e;` STATEMENT of (v), and excludes this one verbatim — "It is
// **not** permitted at an expression body or at a lambda's ascribed
// return". #4208 held that carve-out by declining to TYPE the shorthand
// here (`!inLambda_`), but an untyped NewExpr is exactly what codegen's
// #2491 zero/null backstop swallows, so the position compiled and
// SEGV'd instead of being rejected. Both spellings of the excluded
// position route here.
void ExprTC::rejectNewShorthandAtLambdaReturn(const ast::NewExpr& ne,
                                               const TypeRef* rt) {
    std::string named = "new T(...)";
    if (rt && rt->kind == TypeRef::Kind::Named && rt->declaration)
        named = "new " + std::string(rt->declaration->name) + "(...)";
    ctx_.diagnostics().report(
        Severity::Error, ne.span.begin,
        "the `new()` shorthand is not permitted at a lambda's return. "
        "§17.7 grants the shorthand exactly four positions — field "
        "initializers, typed local-variable declarations, default "
        "parameter values, and a `return` statement of a method with a "
        "declared return type — and excludes this one: \"It is not "
        "permitted at an expression body or at a lambda's ascribed "
        "return\". (A lambda whose return type arrives only by target "
        "inference is outside the written-declared-type family "
        "altogether.) Name the type explicitly (`" + named + "`) (§17.7)");
}

// fix #4176 (§3.1/§3.2, §11.7, §12.5, §17.7, v0.298; sibling to
// `rejectNewOnEnumOrChoice` above, same shape as #4170's own fix): the
// single authority for "does `new` construct a value of this NAMED
// CLASS-SHAPED kind" — an abstract class, a `closed` class, a `static`
// class, and an interface all have no instance for `new` to mint. This
// logic ("Slice S11") already existed, inline, in the explicit
// `new T(...)` NewExpr-typing arm below; #4176 found it was NEVER reached
// from the two §17.7 typeless `new()` shorthand call sites in
// ExprTypecheck.cpp — their own `rt`-resolution routed straight to
// `checkNewExprArgs` for `TypeDeclKind::Class` and nothing else — so
// `using var w: ISomeInterface := new();` with no implementing class in
// scope reached codegen with no vtable to install and SEGFAULTed on the
// first virtual dispatch through it (measured, exit 139): the same
// "unconstructable kind reaches codegen with nothing to construct" defect
// class #4163/#4170 closed for Enum/Choice, but for the four class-shaped
// non-instantiable kinds. `new AbstractThing()` through the SAME shorthand
// path crashes identically (measured, exit 139) — abstract classes are
// `TypeDeclKind::Class`, not Interface, so #4170's own enum/choice
// predicate could never have caught either gap. `closed`/`static` class
// shorthand do not crash the same way (both are otherwise fully concrete
// — no unresolved vtable slot or missing override — so the mis-
// construction is silently ACCEPTED rather than fatal), but are folded
// into this one shared predicate anyway rather than hand-picked out of
// the block they have always lived in together, per CLAUDE.md /
// LANE_RULES.md §6 ("prefer feeding the existing authority" over a
// narrower, second spelling of the same four-way classification).
// Diagnostic text and citations are copied verbatim from the pre-existing
// explicit-`new` arm this replaces — not a wording change, a pure
// factoring, exactly as #4170's own commit did for enum/choice.
bool ExprTC::rejectNewOnNonInstantiableKind(const ast::NewExpr& ne,
                                             const TypeRef* rt) {
    if (!(rt->kind == TypeRef::Kind::Named && rt->declaration)) return false;
    TypeSymbol* t = rt->declaration;
    bool isAbstract = false;
    bool isStatic = false;
    for (auto mod : t->modifiers) {
        if (mod == "abstract") isAbstract = true;
        if (mod == "static") isStatic = true;
    }
    // §12.5 (v0.242, #3052), verbatim: "`new` on an abstract
    // or `closed` class is a compile error." The two spellings
    // are named separately so the diagnostic points at the
    // modifier the author actually wrote.
    const bool isClosed = typeIsClosed(t);
    if (isAbstract || isClosed) {
        ctx_.diagnostics().report(
            Severity::Error, ne.span.begin,
            std::string("cannot instantiate ") +
            (isAbstract ? "abstract" : "`closed`") +
            " class `" +
            std::string(t->name) + "` with `new` "
            "(§12.5)");
        return true;
    }
    if (isStatic) {
        ctx_.diagnostics().report(
            Severity::Error, ne.span.begin,
            std::string("cannot instantiate static "
                        "class `") +
            std::string(t->name) + "` with `new` "
            "(§11.7)");
        return true;
    }
    if (t->kind == ast::TypeDeclKind::Interface) {
        ctx_.diagnostics().report(
            Severity::Error, ne.span.begin,
            std::string("cannot instantiate interface `") +
            std::string(t->name) +
            "` with `new` (§3.1 / §3.2 / §17.7)");
        return true;
    }
    return false;
}

void ExprTC::checkBuiltinClosedConstructorSurface(const ast::NewExpr& ne) {
    // Resolved exactly as `lowerNew` resolves it (and as codegen's #2974 twin
    // does): the explicit `ne.type` when present, else the §17.7 `new(...)`
    // shorthand's recorded exprType, which the field-initializer / local-decl
    // sites publish from the declared target type before calling this.
    const TypeRef* rt =
        ne.type ? ctx_.resolvedType(*ne.type) : ctx_.exprType(ne);
    if (!rt || rt->kind != TypeRef::Kind::Named) return;
    const TypeSymbol* rd = rt->declaration;
    // fix #3543 (§2.8.3 rule 3, v0.286) follow-up: the discriminator the
    // rest of the front-end adopted after #2995/#2997 was `moduleName.empty()`
    // — pre-#3543 a builtinClassSym left it empty, SymbolCollection (local)
    // and ModuleLoader (imported) populated it on every real TypeSymbol.
    // #3543 inverted that discriminator for the synthesized iface by
    // populating moduleName=`System`/`System.Collections.Generic` on the
    // synthesized iface TypeSymbol; `isSynthesizedBuiltinInterface` (#3139)
    // is the by-identity flag set on the same synthesized iface just BEFORE
    // the #3543 moduleName assignment, and survives the inversion. After
    // this fix a USER type still returns (`decl != nullptr` OR
    // `!isSynthesizedBuiltinInterface`); a synthesized builtin iface
    // (`decl == nullptr` AND `isSynthesizedBuiltinInterface`) falls through
    // to the per-name checks, which silently no-op for it (none of
    // `kNoConstructorBuiltins` / `kPositionalCtorOnlyBuiltinRecords` /
    // `kBuiltinCtorLoweredByNameHeads` name a synthesized iface head), so
    // the only behavior delta is that a synthesized iface is now processed
    // (silently) instead of being skipped — user-observable behavior is
    // identical (no diagnostic is produced either way).
    if (rd && (rd->decl || !rd->isSynthesizedBuiltinInterface)) return;   // a USER type
    std::string n = rd ? std::string(rd->name) : std::string(rt->headName());
    if (n.empty()) return;
    if (isEnclosingGenericTypeParamName(n)) return;            // §14.2 shape

    if (common::isNoConstructorBuiltin(n)) {
        // §17.7 names the declared producers, per builtin. Anything else would
        // be the #2935 re-wording lesson paid twice: a diagnostic that says
        // only "no constructor" leaves the reader to find the spelling that
        // does exist.
        std::string producers =
            n == "Guid"
                ? "`Guid.Empty`, `Guid.NewGuid()`, or `default(Guid)`"
                : "`" + n + ".Zero` or `default(" + n + ")`";
        ctx_.diagnostics().report(
            Severity::Error, ne.span.begin,
            "`new " + n + "(\xE2\x80\xA6)` is not admitted at any argument "
            "list — `" + n + "` is a standard-library builtin and declares no "
            "constructor, and a builtin's constructor surface is exactly what "
            "the standard library documents for it (never a compiler-supplied "
            "fallback). Use " + producers + " (\xC2\xA7""17.7)");
        return;
    }
    if (common::isPositionalCtorOnlyBuiltinRecord(n)) {
        // fix #3136 (language spec §17.7 "Standard-library builtins have a
        // closed constructor surface", v0.241; §31.4; stdlib §3.3): the
        // declared arity of the record's only constructor — three for both
        // `ExceptionInfo` and `ReadOnlyStringSpan`. §17.7 reads, verbatim,
        // "the constructor surface is exactly what that document declares —
        // `new T(...)` resolves only against a declared constructor, never
        // against a compiler-supplied fallback." The pre-fix arm only checked
        // the parameterless form (#2994), so `new ExceptionInfo("a", "b")`
        // (2 args) and `new ReadOnlyStringSpan("s", 1, 3, 5)` (4 args) sailed
        // past sema and reached codegen, which had no arm for them — they
        // fell through to `lowerLocalDecl`'s #2434 zero-init fallback, a
        // record of NULL strings, the #2922 class. Reading the result through
        // a concatenation (a `+ e.TypeName + …`) crashed with SIGSEGV. This
        // arm grows the same wording: ANY argument list that does not match
        // the declared arity is rejected here with the §17.7 diagnostic, so
        // the codegen arm #3098 grew is reached only by the spelling the doc
        // declares.
        //
        // The diagnostic keeps the parameterless wording VERBATIM (the "exactly
        // as for an identically-shaped user record" rule #2994 introduced —
        // the #2935 lesson: a diagnostic that only says "no constructor" leaves
        // the reader to find the spelling that does exist) so the
        // tests/lit/builtin_closed_ctor_surface_reject_2994.bzl CHECK-DAG
        // pins keep matching, and adds an arity-equal sentence for the
        // non-zero wrong-arity case so the reader sees the expected count.
        const std::size_t arity =
            common::positionalCtorArityOfBuiltinRecord(n);
        if (ne.args.size() != arity) {
            const std::string arityWord = arity == 1
                ? "1 positional field"
                : std::to_string(arity) + " positional fields";
            std::string detail;
            if (ne.args.empty()) {
                // Parameterless: keep #2994's wording VERBATIM so the lit
                // FileCheck pin keeps matching. (Sub-string search is
                // sufficient because the lit CHECK-DAG uses `{{...}}` regex
                // for the middle, but the leading ``new T()`` and trailing
                // ``(§17.7)`` are anchored literals.)
                ctx_.diagnostics().report(
                    Severity::Error, ne.span.begin,
                    "`new " + n + "()` is not a declared constructor of `" +
                    n + "` — its only declared constructor is its positional "
                    "primary constructor, so the parameterless form is "
                    "rejected exactly as it would be for an "
                    "identically-shaped user record; supply every "
                    "positional field, or obtain the value from the "
                    "standard-library producer that yields it (\xC2\xA7""17.7)");
            } else {
                ctx_.diagnostics().report(
                    Severity::Error, ne.span.begin,
                    "`new " + n + "(...)` is not a declared constructor of `" +
                    n + "` — its only declared constructor is its positional "
                    "primary constructor, which takes " + arityWord +
                    ", so the supplied " + std::to_string(ne.args.size()) +
                    (ne.args.size() == 1 ? " argument" : " arguments") +
                    " do not match it (rejected exactly as it would be for "
                    "an identically-shaped user record; supply every "
                    "positional field, or obtain the value from the "
                    "standard-library producer that yields it) (\xC2\xA7"
                    "17.7)");
            }
            // fix #3425: the arity is already wrong, so binding names against
            // a declared list would only cascade. One diagnostic per `new`.
            return;
        }
    }

    // fix #3425 (§22.5 / §17.7, v0.283): §22.5 binding for a NAMED argument on
    // this built-in constructor. Reached with `n` already established as a
    // built-in head that is NOT a user type, NOT a generic type parameter, and
    // whose §17.7 closed-surface arms above did not already reject the call.
    checkBuiltinCtorNamedArgs(ne, n);
}

// fix #3425 (spec §22.5 "Optional parameters and named arguments" and §17.7
// "The `new` operator", v0.283): see the header for the hole this closes, the
// measured wrong values, and why the two #3398 limitation-rejections hold here
// too.
void ExprTC::checkBuiltinCtorNamedArgs(const ast::NewExpr& ne,
                                       const std::string& head) {
    // Positional-only constructions are entirely unchanged — this check exists
    // for the named form and claims nothing else.
    bool anyNamed = false;
    for (const auto& a : ne.args)
        if (!a.name.empty()) { anyNamed = true; break; }
    if (!anyNamed) return;

    // A POSITIVE name test, exactly as `checkBuiltinNamedArgs` uses: the
    // caller's gate proves `n` is not a user type, but a declaration-less
    // single-part Named TypeRef is "a reserved builtin, or a type parameter"
    // (BuiltinRegistry.hpp), and an UNRESOLVED type name reaches here too.
    // Claiming only names the registry recognizes keeps a missed built-in
    // costing a rejection that does not fire, never a false one — the same
    // fail-safe direction every other positive table here takes.
    if (!common::isReservedBuiltinTypeName(head) &&
        !common::isBuiltinExceptionName(head))
        return;

    const std::string desc = "constructor of built-in `" + head + "`";
    std::vector<common::BuiltinFormals> cands =
        common::builtinCtorParamNameCandidates(head, ne.args.size());
    if (cands.empty()) {
        // Limitation 1 (the #3398 discipline): no complete parameter-name list
        // for this constructor, so the name can be neither matched nor
        // rejected and the binding cannot be trusted. Reject loudly rather
        // than bind positionally behind the caller's back.
        ctx_.diagnostics().report(
            Severity::Error, ne.span.begin,
            desc + ": named arguments are not supported on this built-in "
                   "constructor — the compiler carries no parameter names for "
                   "it at " + std::to_string(ne.args.size()) +
                " argument(s), so a name can be neither matched nor rejected "
                "(\xC2\xA7""22.5, \xC2\xA7""17.7, #3425). Pass the arguments "
                "positionally.");
        return;
    }

    BuiltinBind bb;
    if (!bindBuiltinCandidates(cands, ne.args, ne.span.begin, desc, bb))
        return;  // the helper reported; no cascade

    // Limitation 2 (the #3398 discipline): codegen's `lowerNew` lowers a
    // built-in construction by argument POSITION (`ne.args[i]`), so a binding
    // that is not the identity permutation would be honoured by sema and
    // ignored by codegen — the silent field permutation this fix exists to
    // remove. §22.5's *Evaluation order* rules out buying the reorder by
    // permuting the argument list here ("A call's supplied argument
    // expressions are evaluated in call-site textual order"), so the call is
    // rejected until codegen can consume the binding (#3423).
    //
    // The exception is the two record-shaped heads whose `lowerNew` arm
    // already resolves each argument to its field slot BY NAME (#3098): for
    // those the reorder is honoured end to end and must stay admitted.
    if (common::isBuiltinCtorLoweredByName(head)) return;
    const std::vector<std::string_view>& names = bb.names;
    for (std::size_t j = 0; j < names.size(); ++j) {
        if (bb.bind.paramToArg[j] == static_cast<int>(j)) continue;
        if (bb.bind.paramToArg[j] < 0) {
            // A §22.5 optional parameter left to its default. Beyond the
            // supplied count that is exactly what positional lowering does
            // with a trailing omitted argument; WITHIN it, it is a genuine
            // permutation codegen would get wrong.
            if (j >= ne.args.size()) continue;
            ctx_.diagnostics().report(
                Severity::Error, ne.span.begin,
                desc + ": a named argument that binds to a parameter other "
                       "than its own position is not supported on a built-in "
                       "constructor — `" + std::string(names[j]) +
                    "` is parameter " + std::to_string(j + 1) +
                    " but is left to its default while a later argument is "
                    "supplied (\xC2\xA7""22.5, #3425). Pass the arguments in "
                    "declaration order, or positionally.");
            return;
        }
        const std::size_t ai = static_cast<std::size_t>(bb.bind.paramToArg[j]);
        ctx_.diagnostics().report(
            Severity::Error, ne.span.begin,
            desc + ": a named argument that binds to a parameter other than "
                   "its own position is not supported on a built-in "
                   "constructor — `" + std::string(names[j]) +
                "` is parameter " + std::to_string(j + 1) +
                " but is written as argument " + std::to_string(ai + 1) +
                " (\xC2\xA7""22.5, #3425). Pass the arguments in declaration "
                "order, or positionally.");
        return;
    }
}

void ExprTC::visitExpr(const ast::Expr& e) {
    // §17.15 (v0.216, #2098): the binding atom `(var x := E; body)`. Handled
    // before visitChildren — the clauses are §21.1 declaration statements
    // typed verbatim by visitStmt's LocalDecl arm inside a child Scope, so
    // each binding scopes over the later clauses' initializers and the body
    // only (its own initializer does not see it: visitStmt evaluates the
    // initializer before registering the name); nothing outside the atom
    // ever sees a binding (the scope is restored on exit). The atom's type
    // is the body's, so member access on the atom result resolves.
    if (auto* bx = dynamic_cast<const ast::BindExpr*>(&e)) {
        Scope atomScope;
        atomScope.parent = currentScope_;
        Scope* savedScope = currentScope_;
        currentScope_ = &atomScope;
        bool savedInBindAtom = inBindAtom_;
        inBindAtom_ = true;  // §17.15/§30.5 await-in-atom targeted rejection
        for (const auto& cl : bx->clauses) {
            if (!cl) continue;
            visitStmt(*cl);
            // §17.15 decision (2) / §21.1: a binding is NOT an assignable
            // location — flag the clause's LocalSymbol so the assignment /
            // increment / ref-out rejection sites (the runtime-`const`
            // mirrors) fire on it, including through a `do`-bodied lambda
            // inside the atom that captures the binding.
            auto it = atomScope.names.find(cl->name);
            if (it != atomScope.names.end())
                if (auto* ls = dynamic_cast<LocalSymbol*>(it->second))
                    ls->isBindAtomClause = true;
        }
        if (bx->body) {
            visitExpr(*bx->body);
            if (TypeRef* bt = ctx_.exprType(*bx->body))
                ctx_.setExprType(&e, bt);
        }
        inBindAtom_ = savedInBindAtom;
        currentScope_ = savedScope;
        return;
    }
    // §28.3 K3.4 / A7 (v0.209, #2080): the arm-position transfer marker
    // `owned x` (A.7 MarkedTransfer). Positional admission is enforced HERE
    // (the grammar is additive): the node is admitted exactly as an
    // if-expression branch or an expression-`case`/`when` arm result
    // (markedArmSlot_, set by those constructs' child visits) — everywhere
    // else it is a compile error naming axiom A7's narrowed bound. A CASE
    // SCRUTINEE marker (`case owned x of`, caseScrutineeSlot_) is the
    // separate §35.5 owning-match production — its consuming lowering is not
    // implemented yet, so it draws its own targeted rejection (#2124). The
    // marker's TYPE is the named binding's (needed for the construct's §20
    // common-arm-type computation); the ownership semantics —
    // consume-if-selected, the K5.1 freeze, the K8.3 scrutinee-arm reject —
    // are the ownership pass's.
    if (auto* mt = dynamic_cast<const ast::MarkedTransferExpr*>(&e)) {
        if (&e == caseScrutineeSlot_) {
            ctx_.diagnostics().report(
                Severity::Error, e.span.begin,
                "the owning-match consuming scrutinee (`case owned " +
                std::string(mt->name) +
                " of \xe2\x80\xa6`) is not implemented in this version "
                "\xe2\x80\x94 match on a borrow (`case " +
                std::string(mt->name) +
                " of \xe2\x80\xa6`) and transfer per-arm instead "
                "(\xc2\xa7""35.5, v0.200; #2124)");
        } else if (&e != markedArmSlot_) {
            ctx_.diagnostics().report(
                Severity::Error, e.span.begin,
                "the transfer marker `owned " + std::string(mt->name) +
                "` is admitted only as an if-expression branch, an "
                "expression-`case`/`when` arm result, or the entire "
                "right-hand side of a binding or assignment \xe2\x80\x94 "
                "in every other expression position (`??` operands, "
                "operator operands, guards, argument subexpressions) it is "
                "a compile error (axiom A7, narrowed v0.209; "
                "\xc2\xa7""28.3 / A.7 MarkedTransfer)");
        }
        // Type the marker as the named binding, whatever the admission
        // verdict, so downstream typing does not cascade.
        if (!mt->name.empty()) {
            if (Symbol* sym = lookupName(mt->name)) {
                ctx_.setExprSymbol(&e, sym);
                if (auto* loc = dynamic_cast<LocalSymbol*>(sym)) {
                    if (loc->declType) ctx_.setExprType(&e, loc->declType);
                } else if (auto* par = dynamic_cast<ParameterSymbol*>(sym)) {
                    if (par->declType) {
                        if (auto* rt = ctx_.resolvedType(*par->declType))
                            ctx_.setExprType(&e, rt);
                    } else if (par->resolvedDeclType) {
                        ctx_.setExprType(&e, par->resolvedDeclType);
                    }
                }
            } else {
                ctx_.diagnostics().report(
                    Severity::Error, e.span.begin,
                    "unknown identifier `" + std::string(mt->name) +
                    "` in the `owned` transfer marker (\xc2\xa7""28.3, "
                    "v0.209)");
            }
        }
        return;
    }
    // Slice 3b: IdentifierExpr name lookup. `this` / `base` are
    // implicit-meaning identifiers — leave them unresolved
    // (typecheck handles their semantics in a future slice).
    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(&e)) {
        if (id->name == "this" || id->name == "base") {
            if (inStaticMember_) {
                ctx_.diagnostics().report(
                    Severity::Error, id->span.begin,
                    std::string("`") + std::string(id->name) +
                    "` is not available in a static method or "
                    "static constructor — it requires an instance "
                    "receiver (§12.3)");
            }
            // Slice S29 (§22.8): a field initializer may not
            // reference `this` / `base`.
            if (inFieldInitializer_) {
                ctx_.diagnostics().report(
                    Severity::Error, id->span.begin,
                    std::string("a field initializer may not "
                                "reference `") +
                    std::string(id->name) + "` (§22.8)");
            }
            // per §26.1 / fix #456: inside a PRIMITIVE/builtin extension body
            // (currentType_ is null — no TypeSymbol for the extended type), set
            // `this`'s exprType to the extension's target TypeRef so that
            // resolveMemberAccess can call ctx_.exprType(*ma.target) and then
            // lookupExtensionMember on it. Without this, `this.OtherExtMethod()`
            // from inside `extension int` leaves `this` untyped → the extension
            // member lookup is never reached → codegen emits ret i32 0.
            // Named-type extensions (class/record/enum) already work because
            // resolveMemberAccess reads currentType_ directly — not exprType.
            if (id->name == "this" && !currentType_ && currentExtension_) {
                if (auto* ed = dynamic_cast<const ast::ExtensionDecl*>(
                        currentExtension_->decl)) {
                    if (ed->targetType) {
                        if (TypeRef* extTy = ctx_.resolvedType(*ed->targetType))
                            ctx_.setExprType(&e, extTy);
                    }
                }
            }
            return;
        }
        // §2.7 rule 1 (#2097): in VALUE position — a bare name that is NOT the
        // head of a dotted access (`&e != maTargetHint_`, which is set only
        // while visiting a MemberAccessExpr's target) — an enclosing-type
        // member outranks a same-named top-level type (the implicit-`this`
        // reading). A dotted head keeps the historical type-first order so
        // `Color.Red` / `Data.M()`-style static access still resolves its head
        // to the top-level type.
        Symbol* sym =
            lookupName(id->name,
                       /*memberBeforeModuleType=*/&e != maTargetHint_);
        if (sym) {
            ctx_.setExprSymbol(&e, sym);
            // §10.6 (#772): a bare enum constant `Green` resolved via the
            // case-scrutinee enum scope injection carries the enum's type, so an
            // arm body that yields it has a determinable common arm type (§20).
            // GATED on `inEnumScrutineeCaseArms_` so it applies ONLY inside such
            // a case's arms — NOT in an enum-constant initializer like
            // `C := A + 100`, where a bare sibling `A` is its integer value for
            // arithmetic (typing it as the enum there breaks the fold). The
            // qualified `Color.Green` form gets its type via MemberAccess.
            // // per §10.6
            if (inEnumScrutineeCaseArms_) {
                if (auto* ec = dynamic_cast<EnumConstantSymbol*>(sym)) {
                    if (ec->parent) {
                        auto* et = ctx_.makeTypeRef();
                        et->kind = TypeRef::Kind::Named;
                        et->declaration = ec->parent;
                        ctx_.setExprType(&e, et);
                    }
                }
            }
            // Slice S29 (§22.8): a field initializer may not observe
            // an instance member (implicit-`this` reference).
            if (inFieldInitializer_) checkNotInstanceMember(*id, sym);
            // per §22.10 (v0.180): record where a CLASS primary-constructor
            // parameter is referenced. A parameter resolves to a FieldSymbol
            // whose `posParam` is non-null and whose parent is a class. Its
            // reference is in an *initializer* position when inFieldInitializer_
            // is set (a field/property initializer, or the inheritance clause's
            // base call — all visited with that flag), otherwise in a *body*
            // position (method / property-accessor / constructor / destructor
            // body). An `owned` such parameter used only in bodies is captured
            // as an owned field disposed by the generated Dispose (§28.4); this
            // classification drives that decision in codegen.
            //
            // fix #3227 (§22.10 / §22.5, v0.254): the "later default" case —
            // a §22.5 default-value expression of one parameter that reads an
            // EARLIER parameter on the same list (`B := A + 1`). The default
            // is visited with `inFieldInitializer_=true` so the §22.8 instance-
            // member gate still fires, BUT the earlier parameter is NOT consumed
            // by an init field or base-call argument — it is read by the
            // default's evaluation, which runs against the same object instance
            // (`currentThis_ = obj` in codegen) and sees the parameter through
            // its captured field slot. Capture it: set `posParamBodyRef` (NOT
            // `posParamInitRef`), matching the body-position shape. Without
            // this, an earlier parameter used ONLY in a later default gets
            // `posParamInitRef=true` / `posParamBodyRef=false`, so:
            //   * `isCapturedOwnedPrimaryParam` (`!posParamInitRef` arm) reports
            //     false → the `owned` half is treated as init-consumed and the
            //     generated Dispose skips the slot → a LEAK for `owned` earlier
            //     params;
            //   * `isInstanceFieldStorageSymbol` reports false → the §9.8
            //     serializer's captured-component census silently drops the
            //     parameter;
            //   * the §22.10 "Referenced only in field initializers" enumeration
            //     case fires where the spec's body-position rule should.
            if (auto* pfs = dynamic_cast<FieldSymbol*>(sym)) {
                if (pfs->posParam && pfs->parent &&
                    pfs->parent->kind == ast::TypeDeclKind::Class) {
                    if (inFieldInitializer_ && !inPrimaryCtorDefault_)
                        pfs->posParamInitRef = true;
                    else
                        pfs->posParamBodyRef = true;
                }
            }
            // per §14.1 / §22.12 (issue #520): when the identifier is a
            // generic class name with explicit type args (`Registry<int>`),
            // build an instantiated TypeRef (kind=Named, declaration=TypeSymbol,
            // genericArgs populated with the resolved type args) and attach it
            // as the exprType.  The subsequent `.member` MemberAccessExpr then
            // finds a Named TypeRef with a declaration and resolves the static
            // member against the concretely instantiated class.
            // When typeArgs is empty (the common case) this block is skipped.
            //
            // fix #3160 (§14.2, v0.256): the `Ferry<Bad>.Count()` receiver was
            // accepted pre-#3160 — this IdentifierExpr-as-type-name path built
            // the instantiated TypeRef inline and never reached
            // `Resolver::checkGenericConstraints`, so a failed `where T:
            // sendable` (and every other §14.2 constraint form) was silently
            // skipped at every static-member-access instantiation site. Run
            // the constraint check on the just-built TypeRef via the
            // free-function entry point, so the same diagnostic
            // Resolver::checkGenericConstraints would have produced fires
            // uniformly. The IdentifierExpr's span is the well-located
            // diagnostic site (the wrapping `ast::Type` node path prints 0:0
            // — the new-route location issue noted in #3160's "Note").
            if (!id->typeArgs.empty()) {
                if (auto* ts = dynamic_cast<TypeSymbol*>(sym)) {
                    auto* instTy = ctx_.makeTypeRef();
                    instTy->kind = TypeRef::Kind::Named;
                    instTy->declaration = ts;
                    std::vector<const ast::Type*> argAsts;
                    argAsts.reserve(id->typeArgs.size());
                    for (const auto& ta : id->typeArgs) {
                        if (ta) {
                            TypeRef* resolved = ctx_.resolvedType(*ta);
                            instTy->genericArgs.push_back(resolved);
                            argAsts.push_back(ta.get());
                        } else {
                            instTy->genericArgs.push_back(nullptr);
                            argAsts.push_back(nullptr);
                        }
                    }
                    ctx_.setExprType(&e, instTy);
                    // §14.2 constraint check at this instantiation site —
                    // same dispatch as every other syntactic position that
                    // names a generic type. Free-function entry point wraps a
                    // fresh Resolver over the same ctx, so no ExprTC-
                    // Resolver plumbing is needed; the deferred-nested /
                    // deferred-type-bound paths (which require Resolver state
                    // local to resolveTypes) are intentionally skipped here —
                    // see Resolver::checkGenericConstraintsAtUseSite's
                    // function-level comment.
                    checkGenericConstraintsAtExpr(ctx_, e.span.begin, argAsts,
                                                   instTy->genericArgs, ts);
                    // Return early — no further local/param/field type
                    // attachment is needed; the exprType is now set.
                    return;
                }
            }
            // Slice 3d: also attach the symbol's resolved type to
            // the identifier expr so MemberAccessExpr can pick it
            // up when this identifier is the target.
            if (auto* loc = dynamic_cast<LocalSymbol*>(sym)) {
                // Slice 3g: prefer the LocalSymbol's resolved type
                // (which already accounts for inference) over re-
                // querying the LocalDecl's ascription.
                if (loc->declType) {
                    ctx_.setExprType(&e, loc->declType);
                }
            } else if (auto* par = dynamic_cast<ParameterSymbol*>(sym)) {
                // Slice 3e: ParameterSymbol::declType points at the
                // parameter's Type ast (set by symbol collection).
                // Look up its resolved TypeRef and attach.
                if (par->declType) {
                    if (auto* rt = ctx_.resolvedType(*par->declType)) {
                        ctx_.setExprType(&e, rt);
                    }
                } else if (par->resolvedDeclType) {
                    // per §24: a context-typed lambda param (event handler).
                    ctx_.setExprType(&e, par->resolvedDeclType);
                }
            } else if (auto* fs = dynamic_cast<FieldSymbol*>(sym)) {
                // Implicit `this.<field>` form: bare identifier
                // resolving to a class field. Same field-type rule
                // as MemberAccessExpr below.
                // §14 / §12 (#989): when the field is INHERITED from a generic
                // base (fs->parent has generic params), call substituteMemberType
                // with a null target (= implicit `this`) so the base's T-arg is
                // resolved from currentType_'s base-chain — exactly as the
                // MemberAccessExpr path does for explicit `this.field` access.
                if (auto* fd =
                        dynamic_cast<const ast::FieldDecl*>(fs->decl)) {
                    if (fd->type) {
                        if (auto* rt = ctx_.resolvedType(*fd->type)) {
                            ctx_.setExprType(
                                &e, substituteMemberType(rt, nullptr,
                                                         fs->parent));
                        }
                    }
                } else if (fs->posParam && fs->posParam->type) {
                    // §9.1: a positional record field's type comes from the
                    // Param node, not a FieldDecl (fs->decl is the TypeDecl).
                    // Without this, a bare identifier `Data` inside a record
                    // method body (implicit this.Data where Data: Inner) has no
                    // exprType, so `Data.Val` — a MemberAccessExpr on Data —
                    // cannot resolve targetType and `Val`'s symbol is never set.
                    // Codegen's fieldAddr then finds no symbol → returns nullptr
                    // → the method body emits `ret i32 0` (wrong result).
                    if (auto* rt = ctx_.resolvedType(*fs->posParam->type)) {
                        ctx_.setExprType(
                            &e, substituteMemberType(rt, nullptr, fs->parent));
                    }
                } else if (ctx_.isImported(fs)) {
                    // fix #2532 (§2.7/§2.8.2): an IMPORTED field inherited
                    // from an imported base and reached via a BARE identifier
                    // (`field := v;`, implicit this) has no ast::FieldDecl
                    // (the loader's makeImportedSymbol carries no AST), so its
                    // type rides the importedMemberType side table — the
                    // FieldSymbol sibling of the #2504 property fix in the
                    // adjacent branch, with the IDENTICAL heap-corruption
                    // repro under ASan (confirmed live: SEGV in
                    // bz_string_release, double-free-shaped). The
                    // resolveMemberAccess path (explicit `this.field` /
                    // `h.field`) already publishes the type this way; the
                    // bare-identifier path simply missed it.
                    if (TypeRef* rt = ctx_.importedMemberType(fs))
                        ctx_.setExprType(&e,
                            substituteMemberType(rt, nullptr, fs->parent));
                }
            } else if (auto* ps = dynamic_cast<PropertySymbol*>(sym)) {
                // §12.1 / §23: implicit `this.<auto-property>` form: a bare
                // identifier resolving to a class or record auto-property
                // (bodyKind == Auto).  The property's declared type gives the
                // expression type so that downstream checks (isStringExprType,
                // isStringFieldLvalue, etc.) see the correct type.
                // Without this, `Name := name` in a ctor body where `Name` is
                // a string auto-property has exprType=null → lowerStringFieldStore
                // skips the retain → the ctor stores the string pointer without
                // incrementing the rc, causing a reference-count imbalance when
                // the synthesized deserializer attempts to release the binder's
                // reference after the ctor has "consumed" it via plain store.
                if (!ps->isIndexer) {
                    if (auto* pd =
                            dynamic_cast<const ast::PropertyDecl*>(ps->decl)) {
                        if (pd->type) {
                            if (auto* rt = ctx_.resolvedType(*pd->type)) {
                                ctx_.setExprType(&e, rt);
                            }
                        }
                    } else if (ctx_.isImported(ps)) {
                        // fix #2504 (§23/§2.8.2): an IMPORTED auto-property
                        // inherited from an imported base and reached via a
                        // BARE identifier (`Name := v;`, implicit this) has no
                        // ast::PropertyDecl (the module loader's
                        // makeImportedSymbol carries no AST), so its type rides
                        // the importedMemberType side table — mirroring the
                        // MemberAccess path (resolveMemberAccess's imported-
                        // Property branch, i.e. the `this.Name` form that
                        // already works). Without this the bare-identifier
                        // assignment target carries no exprType, so codegen's
                        // isStringExprType/isStringFieldLvalue bail at their
                        // first check and lowerStringFieldStore is skipped →
                        // the setter stores the string pointer without
                        // retain/release → heap corruption (double-free in
                        // bz_string_release, confirmed under ASan).
                        if (TypeRef* rt = ctx_.importedMemberType(ps))
                            ctx_.setExprType(&e,
                                substituteMemberType(rt, nullptr, ps->parent));
                    }
                }
            } else if (auto* cms = dynamic_cast<ConstMemberSymbol*>(sym)) {
                // fix #3079 (§22.12 / §2.7 / §8.5, v0.249): the BARE-identifier
                // sibling of resolveMemberAccess's #609 `ConstMemberSymbol`
                // arm. §22.12 makes a class-level `const` implicitly static and
                // gives it "a primitive value type or `string`"; §2.7 rule 1
                // reaches it unqualified from inside the declaring type (the
                // chain-head member fallback), so `K` and `C.K` name the same
                // constant. Only the QUALIFIED spelling was typed: the
                // MemberAccess path sets the exprType (#609), the bare path had
                // no arm at all, so `K` carried no type.
                //
                // A missing exprType on a RECEIVER is not inert — it silently
                // deletes code. MEASURED on 309356e with `public const K: int
                // := 7;`: `Console.WriteLine(K)` printed `7` (the console path
                // folds the constant itself) while `Console.WriteLine(
                // K.ToString())` printed NOTHING AT ALL — no newline, no
                // diagnostic at any severity, exit 0 — because CG27c
                // (LlvmBackend_Exprs.cpp, "ToString() on any primitive") is
                // gated on `ctx_.exprType(*ma->target)` being a Primitive, the
                // untyped receiver skipped every primitive-method arm, and
                // lowerCall's `if (!ms)` chain fell through to its unconditional
                // `return nullptr`, which `tryLowerConsoleCall` then read as
                // "not a console call". `K.GetHashCode()` and `K.CompareTo(3)`
                // dropped identically, and `var h: int := K.GetHashCode();`
                // degraded to the #2434 zero-init slot. The type is the one
                // fact all of those arms ask for.
                //
                // Read exactly as the #609 arm reads it: the explicit `: Type`
                // annotation when present, else the initializer's own exprType
                // (already set by visitMember → visitExpr) for an inferred
                // `const K := 7;`.
                if (auto* cd = dynamic_cast<const ast::ConstDecl*>(cms->decl)) {
                    if (cd->type) {
                        if (auto* rt = ctx_.resolvedType(*cd->type))
                            ctx_.setExprType(&e, rt);
                    } else if (cd->value) {
                        if (auto* rt = ctx_.exprType(*cd->value))
                            ctx_.setExprType(&e, rt);
                    }
                } else if (ctx_.isImported(cms)) {
                    // fix #3177 (§22.12 / §2.7 / §2.5, v0.254, family M4): the
                    // IMPORTED arm #3079 did not grow. Both adjacent branches
                    // in this same `else if` chain got theirs — the FieldSymbol
                    // one (#2532, 60 lines up) and the PropertySymbol one
                    // (#2504, 25 lines up) — while a `const` INHERITED FROM AN
                    // IMPORTED BASE, reached by the §2.7 rule-1 bare spelling,
                    // still had `cms->decl == nullptr` and so carried no type.
                    //
                    // MEASURED on ec6a02f: `Console.WriteLine("k=" +
                    // K.ToString())` in a class deriving from an imported base
                    // that declares `public const K: int := 7;` is REJECTED
                    // with "`+` requires both operands to be `string`" (§4.6) —
                    // the untyped receiver never reaches the primitive-ToString
                    // arm — while the character-identical single-module twin
                    // compiles and prints `k=7`. A false REJECT of a legal
                    // program, with a diagnostic describing something the user
                    // did not write.
                    //
                    // `ctx_.importedMemberType` is deliberately read directly
                    // rather than through `sema::storageComponentType`: a
                    // `const` is not §9.1/§9.2 STORAGE, and that accessor feeds
                    // record-storage walks (`anyRecordStorageField`,
                    // Sendability's member census) whose member set must not
                    // grow. This mirrors the two sibling arms exactly.
                    if (TypeRef* rt = ctx_.importedMemberType(cms))
                        ctx_.setExprType(&e, rt);
                }
            }
            // §12.7: flow narrowing. Inside the controlled block of `if x is T`
            // (or the else of `if x is not T`), a reference to the narrowed
            // variable takes the narrowed type `T` instead of its declared
            // base type — overriding whatever exprType the declared-type rules
            // above attached. Member access on this identifier (`x.M`) then
            // resolves against `T` (resolveMemberAccess reads the target's
            // exprType first). Narrowing applies only to simple variables
            // (locals and parameters), keyed by their resolved symbol.
            if (auto nt = narrowings_.find(sym); nt != narrowings_.end()) {
                ctx_.setExprType(&e, nt->second);
            }
        } else if (primitiveFromName(id->name) != PrimitiveType::None) {
            // §5.2: a primitive type name used in expression position is
            // a conversion callee (`int(x)`). It has no value symbol;
            // the enclosing call resolves the conversion (typeCall).
        } else if (id->name == "IntPtr" || id->name == "UIntPtr") {
            // §32.7 / stdlib §10 (#1512): IntPtr/UIntPtr are compiler-intrinsic
            // value types (pointer-sized integers). In expression position the
            // bare type name is the head of either the static const `.Zero`
            // (`IntPtr.Zero`) or the explicit conversion `IntPtr(longValue)`
            // (§5.2). Like a primitive type name, it carries NO value symbol;
            // the enclosing member-access (resolveMemberAccess) or call
            // (typeCall) resolves it. Silently pass so it is not reported as an
            // "unresolved identifier" — the spec's own §32.6/§32.7 null-handle
            // examples depend on `handle = IntPtr.Zero`.
        } else if (id->name == "Console" || id->name == "Math" ||
                   id->name == "Convert" || id->name == "Environment" ||
                   id->name == "Random" ||
                   // §3 stdlib collection and helper classes:
                   id->name == "Array" || id->name == "OwningArray" ||
                   id->name == "Dictionary" || id->name == "OwningDictionary" ||
                   id->name == "Stack" || id->name == "OwningStack" ||
                   id->name == "Queue" || id->name == "OwningQueue" ||
                   id->name == "Set" || id->name == "OwningSet" ||
                   id->name == "LinkedList" || id->name == "OwningLinkedList" ||
                   id->name == "StringBuilder" || id->name == "Encoding" ||
                   id->name == "DateTime" || id->name == "TimeSpan" ||
                   // stdlib §2.5 / #1357: DateOnly and TimeOnly are auto-imported
                   // value-type builtins with static Parse/TryParse factory methods;
                   // recognized here so DateOnly.Parse(…)/TimeOnly.Parse(…) resolve.
                   id->name == "DateOnly" || id->name == "TimeOnly" ||
                   // stdlib §2.6: Guid is auto-imported (a builtin value type).
                   id->name == "Guid" ||
                   // stdlib §9.4 / issue #654: Boolean is auto-imported (a builtin
                   // record type, alias `bool`).  Needed for Boolean.TryParse.
                   id->name == "Boolean" ||
                   // stdlib §2.12 / issue #654: BitConverter — static class for
                   // IEEE 754 bit-reinterpretation (FloatToInt32Bits etc.).
                   id->name == "BitConverter" ||
                   // stdlib §9.3: Decimal is auto-imported (a builtin value type).
                   // fix #521: needed for Decimal.Parse, Decimal.Zero, etc.
                   id->name == "Decimal" || id->name == "decimal" ||
                   // §7 System.IO:
                   id->name == "File" || id->name == "Directory" ||
                   id->name == "Path" ||
                   // §4 Linq:
                   id->name == "Linq" ||
                   // §6.1 / §30.8 (stdlib §6.1): CancellationToken is auto-imported
                   // (§2.3); used in expression position for `CancellationToken.None`.
                   // CancellationTokenSource requires `import System.Threading` per
                   // spec; import-gating not yet enforced (same stance as §4.8
                   // KeyNotFoundException).
                   id->name == "CancellationToken" ||
                   id->name == "CancellationTokenSource" ||
                   // §30.10 / stdlib §6.7: the `Task` static-helper class is
                   // auto-imported (Task.Delay, Task.FromResult, …). Only used in
                   // expression position as the receiver of a static helper call.
                   id->name == "Task" ||
                   // §30.14 (#600): Domain is a static class auto-imported for
                   // Domain.Spawn; recognized here so Domain.Spawn(…) can resolve
                   // as a call on a known-class receiver.
                   id->name == "Domain" ||
                   // §8.1 / issue #646: Stopwatch — static factory StartNew() and
                   // instance methods. Recognized here so Stopwatch.StartNew() and
                   // sw.Start()/Stop()/… resolve as calls on a known-class receiver.
                   id->name == "Stopwatch" ||
                   // §8.2 / issue #646: Debug — static class for Assert/WriteLine.
                   // Recognized here so Debug.Assert(…) and Debug.WriteLine(…)
                   // resolve as calls on a known-class receiver.
                   id->name == "Debug" ||
                   // stdlib §10.1/§10.2/§10.3 / §32.6 / fix #977: CString, WString,
                   // RawBytes are FFI-boundary classes with static factory methods
                   // (CString.From(s), WString.From(s), RawBytes.From(buffer)).
                   // They resolve as builtin class receivers so member access on
                   // the type name (CString.From) is accepted in expression position.
                   id->name == "CString" ||
                   id->name == "WString" ||
                   id->name == "RawBytes" ||
                   // fix #3249 (§4.19, v0.186): FrozenArray<T> — the §4.19
                   // shared-read primitive has a static factory
                   // (`FrozenArray<T>.From(items: Array<T>)` — the spec's
                   // example at §10.4). Recognized here so the bare
                   // `FrozenArray<T>.From` member-access shape compiles and
                   // the IdentifierExpr's typeArgs path (line ~1211 below)
                   // builds the instantiated TypeRef against the
                   // builtinClassSym declaration.
                   id->name == "FrozenArray") {
            // §2.3: these System classes are auto-imported and always
            // available without an explicit import. Give them a synthetic
            // TypeSymbol (null decl) so member access can proceed; the
            // codegen handles known methods (Write/WriteLine, etc.)
            // as intrinsics.
            auto* ts = builtinClassSym(id->name);
            ctx_.setExprSymbol(&e, ts);
        } else if (currentType_ && !inStaticMember_ &&
                   builtinExceptionSurfaceMember(currentType_, id->name) !=
                       BuiltinExcMember::None) {
            // #625 / §12 / §31: an unqualified §31.4 surface name inside an
            // INSTANCE member body of a class that inherits (directly or via a
            // user-class chain) from a built-in exception type resolves as the
            // inherited property — a read of the corresponding slot in the
            // §31.4 header the object carries. No user TypeSymbol or
            // FieldSymbol exists for it; codegen's fixed-byte-offset path
            // handles the read (same as `this.X` — see the MemberAccessExpr
            // case in LlvmBackend_Exprs.cpp CG22+).
            // Static methods are excluded: there is no `this` in a static
            // context so the name would have no meaningful receiver.
            // Do NOT set a symbol — there is none; just set the type so
            // downstream (type-assignability, .Length, .Count, …) can use it.
            //
            // fix #2971 (§31.4 / §12.1 / §11.3, v0.240): this arm read
            // `id->name == "Message"`, so the bare spelling of the other three
            // §31.4 members was a THIRD rejection of the same inherited
            // surface, with a THIRD diagnostic from a THIRD site — measured on
            // 0dac23f inside `class ExS: Exception`:
            //     bare.bzl:5:38: unresolved identifier `StackTrace`
            //     bare.bzl:6:38: unresolved identifier `CauseChain`
            //     bare.bzl:10:38: unresolved identifier `ParamName`
            // while bare `Message` in the same bodies resolved. #2971 named the
            // receiver-typed and `this.`-qualified spellings (both the member-
            // lookup arm in ExprTypecheck_Calls.cpp); this site is the one it
            // did not measure, and leaving it would have shipped the same
            // reachable-but-inconsistent shape the issue is about.
            switch (builtinExceptionSurfaceMember(currentType_, id->name)) {
                case BuiltinExcMember::ParamName:
                    // §31.4: `string?` — "null when not supplied".
                    ctx_.setExprType(&e,
                        ctx_.primitiveTypeRef(PrimitiveType::String,
                                              /*nullable=*/true));
                    break;
                case BuiltinExcMember::CauseChain:
                    ctx_.setExprType(&e, excCauseChainTypeRef());
                    break;
                default:
                    // Message / StackTrace — §31.4 declares both `string`.
                    ctx_.setExprType(&e,
                        ctx_.primitiveTypeRef(PrimitiveType::String));
                    break;
            }
        } else if (
            // §2.2 v0.164 / §2.7 / #1365: an import alias (e.g. `ML` in
            // `import MathLib as ML;`) or a module-name prefix segment
            // (e.g. `MathLib` in `MathLib.Calc.Square(4)`) is NOT a type
            // symbol in scope, but is a valid qualifier in a dotted member-
            // access chain.  Suppress "unresolved identifier" so the
            // enclosing MemberAccessExpr's resolveMemberAccess can handle
            // the full qualifier correctly (§2.7 rule 1 for aliases, rule 2
            // for module-qualified names).
            !importAliases_.empty() &&
            importAliases_.count(std::string(id->name))) {
            // Import alias: silently pass — resolveMemberAccess will handle
            // the enclosing `Alias.TypeName` pattern.
        } else if (
            !sourceImports_.empty() && [&]() {
                const std::string name(id->name);
                // Exact match: `MathLib` in `import MathLib;`.
                if (sourceImports_.count(name)) return true;
                // Prefix match: `BankingApp` when `BankingApp.Txn` is imported.
                for (const std::string& mod : sourceImports_) {
                    if (mod.size() > name.size() &&
                        mod.substr(0, name.size()) == name &&
                        mod[name.size()] == '.')
                        return true;
                }
                return false;
            }()) {
            // Module-name prefix: silently pass — resolveMemberAccess will
            // handle the enclosing `ModuleName.TypeName` pattern.
        } else if (&e == maTargetHint_ && isInScopeTypeParam(id->name)) {
            // §11.8 (v0.176, #807): `T.Member(...)` — the receiver of a
            // member access names an in-scope generic type parameter. Mark it
            // with a GenericParamSymbol so resolveMemberAccess dispatches
            // through the parameter's static-interface constraint. A bare `T`
            // in any OTHER expression position (not a member-access target)
            // keeps today's "unresolved identifier" error.
            ctx_.setExprSymbol(&e, typeParamMarker(id->name));
        } else if (currentScope_) {
            // §22.16 (v0.250, #3038): `result` and `old` are contextual
            // keywords (§22.16 / Appendix B.2), valid only inside an
            // `ensures` clause — `result` denotes the method's return
            // value (typed as the declared return type, read as a borrow);
            // `old(E)` denotes the value `E` had at the method's entry.
            // Option B (the binding + capture form) is now implemented:
            //   * `result` is bound in the `ensures` scope only — a
            //     synthetic LocalSymbol typed as the method's declared
            //     return type. The IdentifierExpr's exprSymbol and exprType
            //     are set so the rest of the typecheck (member access on
            //     the borrow, comparisons) sees an ordinary identifier.
            //     Codegen's IdentifierExpr path recognises the synthetic
            //     symbol and loads from the `ensuresResultSlot_` frame slot.
            //   * `old(E)` is the callee of a CallExpr. The IdentifierExpr
            //     path is silent for that one position — the CallExpr
            //     handler (typeCall in ExprTypecheck_Calls.cpp) takes over,
            //     visiting the operand and setting the CallExpr's type
            //     (with the §22.16 ownership restriction — class-typed
            //     operands are rejected with a diagnostic).
            // Anywhere else (method body, `requires`, `invariant`, void
            // method, constructor) `result`/`old` are still rejected as
            // contextual keywords reserved for `ensures`.
            if (id->name == "result") {
                if (currentClauseKind_ == ContractClauseKind::Ensures &&
                    currentReturnType_ &&
                    returnCtx_ == ReturnCtx::ValueMethod) {
                    // Per §22.16: `result` denotes the returned value,
                    // typed as the declared return type, in a read-only
                    // borrow position. Bind a synthetic LocalSymbol so
                    // downstream typecheck sees a normal identifier.
                    LocalSymbol* rsym = ctx_.syntheticResultLocal();
                    rsym->declType = currentReturnType_;
                    ctx_.setExprSymbol(&e, rsym);
                    ctx_.setExprType(&e, currentReturnType_);
                } else {
                    ctx_.diagnostics().report(
                        Severity::Error, id->span.begin,
                        std::string("`result` is a contextual keyword "
                                    "reserved for `ensures` clauses on a "
                                    "value-returning method (\xc2\xa7"
                                    "22.16); it is not an identifier and "
                                    "cannot be referenced from a method "
                                    "body, a `requires` clause, an "
                                    "`invariant` member, a void method, "
                                    "or a constructor"));
                }
            } else if (id->name == "old") {
                if (currentClauseKind_ == ContractClauseKind::Ensures &&
                    &e == calleeOfCallCtx_) {
                    // `old(E)` — the CallExpr handler (typeCall) takes
                    // over from here: it visits the operand, enforces
                    // the §22.16 ownership restriction, and sets the
                    // CallExpr's exprType. Suppress the contextual-
                    // keyword diagnostic so the handler runs cleanly.
                } else {
                    ctx_.diagnostics().report(
                        Severity::Error, id->span.begin,
                        std::string("`old` is a contextual keyword "
                                    "reserved for `ensures` clauses "
                                    "(\xc2\xa7""22.16); it is not an "
                                    "identifier and cannot be referenced "
                                    "from a method body, a `requires` "
                                    "clause, or an `invariant` member"));
                }
            } else {
                ctx_.diagnostics().report(
                    Severity::Error, id->span.begin,
                    std::string("unresolved identifier `") +
                    std::string(id->name) + "`");
            }
        }
        return;
    }
    // Slice 3 main contract: every literal gets a type.
    if (auto* il = dynamic_cast<const ast::IntLiteralExpr*>(&e)) {
        // §5: an integer literal's type comes from its suffix when
        // present (`L` → long, `u` → uint, `uL`/`Lu` → ulong), else
        // int. Context (target) typing of unsuffixed literals is a
        // separate, broader change (tracked in the literal-typing
        // issue); this covers the explicit-suffix forms.
        bool hasL = false, hasU = false;
        for (char c : il->text) {
            if (c == 'l' || c == 'L') hasL = true;
            if (c == 'u' || c == 'U') hasU = true;
        }
        PrimitiveType pt = PrimitiveType::Int;
        if (hasL && hasU) pt = PrimitiveType::ULong;
        else if (hasL) pt = PrimitiveType::Long;
        else if (hasU) pt = PrimitiveType::UInt;
        // §5: an unsuffixed integer literal must fit in `int`; if it does
        // not, it either requires a suffix OR the declared target type
        // (long/ulong/uint) allows implicit widening (issue #184).
        if (pt == PrimitiveType::Int) {
            unsigned long long mag = 0;
            if (intLiteralMagnitude(il->text, mag) &&
                (errno == ERANGE || mag > 2147483647ull)) {
                using P = PrimitiveType;
                bool widened = false;
                // §4.2 Boundary carve-out (v0.159, #1828): the unsuffixed
                // decimal literal of magnitude 2^31 is admitted ONLY as the
                // immediate operand of a unary `-`, denoting int.MinValue
                // (-2147483648). The long-side analog (2^63 under `L`) is
                // honored in the suffixed branch below via the same
                // `literalUnderUnaryMinus_` flag; without this int-side check
                // the magnitude 2^31 was rejected here before the negation
                // could apply, so int.MinValue was unwritable as a bare
                // literal (asymmetric with the working long.MinValue path).
                // Anything strictly above 2^31 still falls through to
                // widen-or-reject below.
                if (literalUnderUnaryMinus_ && errno != ERANGE &&
                    mag == 2147483648ull) {
                    widened = true;  // keep pt == Int; this is int.MinValue.
                }
                // Widen to the declared type when in a typed variable context.
                else if (currentLiteralWideningHint_ != P::None) {
                    bool fits = false;
                    switch (currentLiteralWideningHint_) {
                        case P::Long:
                            fits = (errno != ERANGE && mag <= 9223372036854775807ULL);
                            break;
                        case P::ULong:
                            fits = (errno != ERANGE);  // any positive value fits
                            break;
                        case P::UInt:
                            fits = (errno != ERANGE && mag <= 4294967295ULL);
                            break;
                        default: break;
                    }
                    if (fits) {
                        pt = currentLiteralWideningHint_;
                        widened = true;
                    }
                }
                if (!widened) {
                    ctx_.diagnostics().report(
                        Severity::Error, il->span.begin,
                        "integer literal does not fit in `int`; add a suffix "
                        "(`L` for long, `U`/`UL` for unsigned) to widen it "
                        "(§5)");
                }
            }
        } else {
            // §4.2 / §5 (#749, #1155): an explicitly L/U/UL-suffixed literal
            // names its type, and its value must be representable in that
            // type's range. A magnitude that overflows 64 bits is
            // unrepresentable in any integer type (e.g. `99999999999999999999999UL`,
            // which once silently miscompiled an enum member to 0, #749). A
            // magnitude that fits 64 bits but exceeds the *named* suffix's range
            // — an `L` literal in (2^63, 2^64), or a `U` literal in (2^32, 2^64)
            // — does not fit the type the suffix selects and once silently
            // wrapped (`9223372036854775808L` → long.MinValue, `5000000000U` →
            // wrapped uint, #1155). Reject both.
            unsigned long long mag = 0;
            if (intLiteralMagnitude(il->text, mag)) {
                if (errno == ERANGE) {
                    ctx_.diagnostics().report(
                        Severity::Error, il->span.begin,
                        "integer literal is too large to represent (exceeds the "
                        "64-bit range of any integer type) (§5)");
                } else {
                    // §4.2: per-type maximum representable magnitude. `long`
                    // admits |long.MinValue| = 2^63 only as the direct operand
                    // of a unary minus (#1155); a bare `long` literal caps at
                    // long.MaxValue = 2^63 − 1.
                    using P = PrimitiveType;
                    unsigned long long maxMag;
                    switch (pt) {
                        case P::Long:
                            maxMag = literalUnderUnaryMinus_
                                         ? 9223372036854775808ULL   // 2^63
                                         : 9223372036854775807ULL;  // 2^63 − 1
                            break;
                        case P::UInt:
                            maxMag = 4294967295ULL;  // 2^32 − 1
                            break;
                        case P::ULong:
                        default:
                            maxMag = 18446744073709551615ULL;  // 2^64 − 1
                            break;
                    }
                    if (mag > maxMag) {
                        ctx_.diagnostics().report(
                            Severity::Error, il->span.begin,
                            std::string("integer literal does not fit in `") +
                            primitiveSpelling(pt) + "` (§4.2)");
                    }
                }
            }
        }
        ctx_.setExprType(&e, ctx_.primitiveTypeRef(pt));
        return;
    }
    if (auto* fl = dynamic_cast<const ast::FloatLiteralExpr*>(&e)) {
        // §18.1 / §4.4: the `f`/`F` suffix gives type `float` (f32);
        // `d`/`D` or no suffix gives `double` (f64); `m`/`M` gives `decimal`.
        // per §4.4 (fix #521): decimal is fully implemented as a 128-bit type
        // with 96-bit mantissa + scale + sign (.NET-compatible layout).
        PrimitiveType pt = PrimitiveType::Double;
        if (!fl->text.empty()) {
            char last = fl->text.back();
            if (last == 'f' || last == 'F') {
                pt = PrimitiveType::Float;
            } else if (last == 'm' || last == 'M') {
                // §4.4: `m`/`M` suffix → decimal literal.
                pt = PrimitiveType::Decimal;
            }
            // `d`/`D` → double (already default).
        }
        // §4.3 (v0.190, #1781): a floating-point literal denotes the nearest
        // representable value of its type; a literal whose magnitude exceeds the
        // type's largest finite value (`1e999` for double, `1e40f` for float) is
        // a compile error AT FORMATION — it never rounds to ±Infinity (§6.3's
        // overflow-to-Infinity governs arithmetic operations, not literals).
        // Range-check here, uniformly with the integer literals of §4.2 above.
        // The same rule bounds `decimal` (§4.4) against decimal's range.
        if (pt == PrimitiveType::Decimal) {
            if (decimalLiteralOutOfRange(fl->text)) {
                ctx_.diagnostics().report(
                    Severity::Error, fl->span.begin,
                    "decimal literal is out of range for `decimal` "
                    "(magnitude exceeds `decimal.MaxValue`) (\xC2\xA7""4.3)");
            }
        } else {
            // Float/Double: parse the magnitude exactly as codegen does (strip
            // underscores and the f/F/d/D suffix, then strtod). The value
            // materialized for `float` is `(float)v`, so check the overflow
            // against the target type's own largest finite value.
            std::string digits;
            for (char c : fl->text) {
                if (c == '_' || c == 'f' || c == 'F' || c == 'd' || c == 'D')
                    continue;
                digits.push_back(c);
            }
            if (!digits.empty()) {
                double v = std::strtod(digits.c_str(), nullptr);
                bool overflow = (pt == PrimitiveType::Float)
                                    ? std::isinf(static_cast<float>(v))
                                    : std::isinf(v);
                if (overflow) {
                    ctx_.diagnostics().report(
                        Severity::Error, fl->span.begin,
                        std::string("floating-point literal is out of range for "
                                    "`") + primitiveSpelling(pt) +
                            "` (magnitude exceeds the largest finite `" +
                            primitiveSpelling(pt) + "`) (\xC2\xA7""4.3)");
                }
            }
        }
        ctx_.setExprType(&e, ctx_.primitiveTypeRef(pt));
        return;
    }
    if (dynamic_cast<const ast::CharLiteralExpr*>(&e)) {
        ctx_.setExprType(&e, ctx_.primitiveTypeRef(PrimitiveType::Char));
        return;
    }
    if (dynamic_cast<const ast::StringLiteralExpr*>(&e) ||
        dynamic_cast<const ast::VerbatimStringExpr*>(&e)) {
        ctx_.setExprType(
            &e, ctx_.primitiveTypeRef(PrimitiveType::String));
        return;
    }
    if (auto* is = dynamic_cast<const ast::InterpolatedStringExpr*>(&e)) {
        // §18.5: visit embedded expression parts BEFORE returning, so
        // their identifiers resolve and their types are set for codegen.
        for (const auto& part : is->parts)
            if (part.expr) visitExpr(*part.expr);
        // §18.5 dispatch rule (v0.172, #812/#1339): a hole WITH a real format
        // spec (not a bare `,`alignment) requires an operand that is either
        // intrinsically formattable (a numeric/temporal/Guid) or implements the
        // auto-imported `IFormattable` interface (stdlib §2.18). A format spec on
        // any other operand — bool/char/string/Object or a plain user type — is a
        // compile error naming this rule. Alignment-only holes are unrestricted.
        for (const auto& part : is->parts) {
            if (!part.expr) continue;
            if (interpFormatPortion(part.formatSpec).empty()) continue;
            const TypeRef* t = ctx_.exprType(*part.expr);
            if (t && t->kind != TypeRef::Kind::Error &&
                !isFormattableOperandType(t)) {
                std::string what;
                if (t->kind == TypeRef::Kind::Primitive)
                    what = std::string("`") + primitiveSpelling(t->primitive) + "`";
                else if (!namedTypeName(t).empty())
                    what = std::string("`") + std::string(namedTypeName(t)) + "`";
                else
                    what = "this operand";
                ctx_.diagnostics().report(
                    Severity::Error, e.span.begin,
                    "a format spec requires an intrinsically-formattable operand "
                    "(a numeric/temporal/Guid) or one implementing `IFormattable`; "
                    + what + " is neither (\xC2\xA7""18.5)");
            }
        }
        ctx_.setExprType(
            &e, ctx_.primitiveTypeRef(PrimitiveType::String));
        return;
    }
    if (dynamic_cast<const ast::BoolLiteralExpr*>(&e)) {
        ctx_.setExprType(&e, ctx_.primitiveTypeRef(PrimitiveType::Bool));
        return;
    }
    if (dynamic_cast<const ast::NullLiteralExpr*>(&e)) {
        // null is context-typed; absent the context we annotate with
        // nullable Object. Later passes refine when there's a target
        // type to constrain against.
        ctx_.setExprType(
            &e, ctx_.primitiveTypeRef(PrimitiveType::Object,
                                      /*nullable=*/true));
        return;
    }
    if (dynamic_cast<const ast::UnitExpr*>(&e)) {
        // Build a unit TypeRef on first use (no primitive entry).
        auto* unit = ctx_.makeTypeRef();
        unit->kind = TypeRef::Kind::Unit;
        ctx_.setExprType(&e, unit);
        return;
    }
    // For non-literal expressions, recurse into children so their
    // own literals get typed. The expression itself may also get a
    // type from the operator's result rules (slice 3c).
    // §4.2 (#1155): flag a unary-minus's direct integer-literal operand so
    // its range-check admits the negative-boundary magnitude (long.MinValue =
    // -2^63, written as `-9223372036854775808L`).
    bool savedNegLit = literalUnderUnaryMinus_;
    if (auto* ue = dynamic_cast<const ast::UnaryExpr*>(&e)) {
        literalUnderUnaryMinus_ =
            (ue->op == "-" && ue->operand &&
             dynamic_cast<const ast::IntLiteralExpr*>(ue->operand.get()) !=
                 nullptr);
    } else {
        literalUnderUnaryMinus_ = false;
    }
    visitChildren(e);
    literalUnderUnaryMinus_ = savedNegLit;
    // per §7.4 (v0.182, #1566): a call, a function-value invocation (`f?(…)`), or
    // a constructor can transfer control to user code, which may run a closure
    // that nulls a mutably-captured local — so after visiting one (its arguments
    // are already checked under the pre-call narrowing), invalidate the narrowing
    // of every mutably-captured local. (`await` is handled in typeUnary, and
    // user-defined operators in typeBinary/typeUnary; the spec's remaining
    // "accessor call" trigger — a user-defined property/indexer getter — is
    // tracked separately as #1593.) A no-op unless the body has a lambda that
    // mutates a captured local.
    if (dynamic_cast<const ast::CallExpr*>(&e) ||
        dynamic_cast<const ast::NullCondCallExpr*>(&e) ||
        dynamic_cast<const ast::NewExpr*>(&e)) {
        invalidateCapturedNarrowings();
    }
    // Slice 3c: binary / unary operator result typing.
    if (auto* be = dynamic_cast<const ast::BinaryExpr*>(&e)) {
        typeBinary(*be);
    }
    if (auto* ue = dynamic_cast<const ast::UnaryExpr*>(&e)) {
        typeUnary(*ue);
    }
    // §17.12 / #2553: the postfix `!` (null-forgiving) operator yields
    // the non-nullable form of its operand's type. Recording it lets
    // member access through `x!` (and any other consumer) resolve —
    // without it, `x!.member` reads/writes silently fail (#206).
    if (auto* bg = dynamic_cast<const ast::PostfixBangExpr*>(&e)) {
        if (bg->operand) {
            if (TypeRef* ot = ctx_.exprType(*bg->operand)) {
                if (ot->nullable) {
                    TypeRef* nn = ctx_.makeTypeRef(*ot);
                    nn->nullable = false;
                    ctx_.setExprType(&e, nn);
                } else {
                    ctx_.setExprType(&e, ot);  // already non-nullable
                }
            }
        }
    }
    // §20: the `if` expression `x if c else y` has the COMMON type of its
    // two branches (the type the other branch widens/subtypes into). A
    // bottom-typed (`throw`) branch imposes no constraint; incompatible
    // branches (e.g. the `throw E if c else x` shape, which parses as
    // `throw (E if c else x)`) get a clean §20 error rather than crashing
    // codegen's PHI. typeConditional records the result type on `e` so
    // member access on the result (`(a if c else b).M`) resolves.
    if (auto* cnd = dynamic_cast<const ast::ConditionalExpr*>(&e)) {
        typeConditional(*cnd, e);
    }
    // §17.4: `is` / `is not` yield bool; `as` yields the nullable form
    // of the target type (a failed cast is null). Recording it lets
    // `(x as T)` — and member access on its result — resolve.
    if (auto* te = dynamic_cast<const ast::TypeTestExpr*>(&e)) {
        // §12.7/§2.8.3 rule 4/§17.4 (v0.197, #2006): erasure guard for the plain
        // `is`/`as` expression path — parity with the case-binding path
        // (#1927/#1946). A runtime type-test against a CONSTRUCTED generic
        // whose type argument is a runtime-ERASED kind reaches a COLLAPSED
        // per-monomorphization descriptor: appendDescKey does not encode a
        // function type's identity (Kind::Function → "F" at any depth), so
        // `Wrap<(int)->int> is Wrap<(string)->string>` returns a false-positive
        // TRUE and the follow-on `as` reinterprets one signature as another —
        // the same memory-unsafe type confusion #2001 fixed for inline-array
        // sizes. Reject such a test at compile time. `typeArgRuntimeReified`
        // (the codegen descArgsConcrete mirror) is the oracle: reject iff the
        // target is a constructed generic that carries no type variable (a bare
        // parameter resolves to a concrete arg at monomorphization — admit it,
        // #1843's `o is Box<Box<T>>`) yet is NOT runtime-reified. Post-#2001 a
        // folded inline-array arg IS reified, so `o is Wrap<[3]int>` is admitted.
        if (te->testType) {
            if (TypeRef* tt = ctx_.resolvedType(*te->testType)) {
                if (tt->kind == TypeRef::Kind::Named && !tt->genericArgs.empty() &&
                    !typeTestTargetMentionsTypeVariable(tt) &&
                    !typeArgRuntimeReified(tt))
                    ctx_.diagnostics().report(
                        Severity::Error, e.span.begin,
                        "the type argument of this `is`/`as` target is not "
                        "runtime-distinguishable — it is a still-erased type "
                        "kind (e.g. a function type), which the runtime type "
                        "descriptor collapses, so the test would confuse "
                        "distinct instantiations and bind a value at a wrong "
                        "static type (§12.7/§2.8.3 rule 4/§17.4)");
            }
        }
        if (te->op == "is" || te->op == "is not") {
            ctx_.setExprType(&e,
                             ctx_.primitiveTypeRef(PrimitiveType::Bool));
        } else if (te->op == "as" && te->testType) {
            if (TypeRef* tt = ctx_.resolvedType(*te->testType)) {
                // fix #2911 + fix #2962 (§17.4 "Degenerate value-type
                // targets", v0.241): the DIRECT arm — a concretely-spelled
                // inline-array or unit target, the two halves of ONE spec
                // paragraph, rejected through ONE predicate
                // (degenerateAsTargetReason, TypeResolution_Checks.cpp).
                //
                // MEASURED on 309356e, which is what makes this a rejection
                // rather than a tidy-up. `var o: Object := a; var miss := o as
                // [4]int;` compiled silently and produced an ALL-ZERO array
                // indistinguishable from a legitimately zero-valued one — the
                // `null` §17.4 promises had nowhere to live, because §15.1
                // admits no `([4]int)?` for it to live in (#2911). `o as ()`
                // and `o as ()?` likewise compiled silently, falling through
                // codegen's reference-target arm to a bare pointer typed
                // `()?`, a value of undefined shape (#2962; #2915 had already
                // excluded `Kind::Unit` from the unbox arm because the unit
                // type lowers to LLVM `void` and `load void` is not
                // constructible).
                //
                // The existing typing below is left running after the report:
                // downstream passes then see EXACTLY the types they saw before
                // this fix, so a rejected program cannot reach a shape they
                // have never handled. The error stops the compile before
                // codegen either way.
                if (std::string degenerate = degenerateAsTargetReason(tt);
                    !degenerate.empty()) {
                    ctx_.diagnostics().report(Severity::Error, e.span.begin,
                                              degenerate);
                } else if (tt->kind == TypeRef::Kind::Named &&
                           !tt->declaration && tt->genericArgs.empty() &&
                           currentMethodDecl_) {
                    // §17.4's generic clause: "When the target is a generic
                    // type parameter, the check applies per instantiation
                    // (§3.6 rule 2)". A bare parameter cannot be judged here —
                    // "a generic body type-checks under the weakest assumption
                    // its constraints establish" — so record the obligation and
                    // discharge it against each instantiation at the end of the
                    // pass, the #3044 `default(T)` shape reused verbatim.
                    //
                    // ONLY a parameter the enclosing METHOD binds is recorded:
                    // this pass sees a generic method's instantiations (they
                    // are CALLS) but not a generic TYPE's (they are `Named<…>`
                    // annotations the Resolver walks), so the type-parameter
                    // arm lives in Resolver::checkAsTargetInstantiations
                    // against the same shared predicate. Matching the name
                    // against the method's own list is also what makes
                    // SHADOWING correct: inside `class Box<T>`'s `method M<T>`
                    // the name denotes the METHOD's parameter, whose bindings
                    // `Box`'s instantiations know nothing about (#2713's
                    // misattribution guard, mirrored).
                    for (const auto& gp : currentMethodDecl_->genericParams) {
                        if (gp.name != tt->headName()) continue;
                        recordAsTargetTypeParamUse(currentMethodDecl_, gp.name,
                                                   e.span.begin);
                        break;
                    }
                }
                // fix #4332 (§15.4, v0.298): thread the `as` target type onto
                // a collection-literal operand. The operand was already visited
                // by visitChildren before this handler ran, without any target —
                // so the literal has no exprType and its elements were visited
                // in an untyped context. Set the target now and re-visit the
                // operand so the §15.4 admissibility gate and per-element
                // assignability check run with the right context; without this,
                // codegen's #2434 backstop silently degrades the literal to a
                // zero-initialised value.
                if (auto* cl = dynamic_cast<const ast::CollectionLiteralExpr*>(
                        te->operand.get())) {
                    if (!ctx_.exprType(*cl)) {
                        ctx_.setExprType(te->operand.get(), tt);
                        const bool isEmpty =
                            !cl->isDict && cl->elements.empty();
                        const bool admitted = isEmpty
                            ? isCollectionLiteralEmptyAdmissibleTarget(tt)
                            : isCollectionLiteralAdmissibleTarget(tt,
                                                                  cl->isDict);
                        if (tt->kind != TypeRef::Kind::Error && !admitted) {
                            if (isCollectionLiteralInterfaceTarget(tt))
                                reportCollectionLiteralInterfaceTarget(
                                    ctx_.diagnostics(),
                                    te->operand->span.begin,
                                    exprsNamedTypeSimpleName(tt));
                            else if (cl->isDict &&
                                     isCollectionLiteralAdmissibleTarget(
                                         tt, /*isDict=*/false))
                                reportCollectionLiteralDictFormBadTarget(
                                    ctx_.diagnostics(),
                                    te->operand->span.begin, tt);
                            else
                                reportCollectionLiteralBadTarget(
                                    ctx_.diagnostics(),
                                    te->operand->span.begin, tt);
                        }
                        checkBorrowingFamilyLiteralElements(*cl, tt);
                        checkCollectionLiteralElementAssignability(*cl, tt);
                        // Re-visit the operand now that the target is set, so
                        // its elements are properly typed under the target
                        // context (visitChildren already visited them untyped).
                        visitExpr(*te->operand);
                    }
                }
                if (tt->nullable) {
                    ctx_.setExprType(&e, tt);
                } else {
                    TypeRef* nn = ctx_.makeTypeRef(*tt);
                    nn->nullable = true;
                    ctx_.setExprType(&e, nn);
                }
            }
        }
    }
    // §19 + §20: a `case` expression's type is the COMMON type of its
    // (non-diverging) arm bodies — folded through the same common-type rule
    // as the `if` expression. A bottom-typed (`throw`/diverging) arm imposes
    // no constraint; arms with no common type get a clean §20 error rather
    // than crashing codegen's result PHI. Recorded on `e` so member access on
    // a case result (`(case … end).M`) resolves.
    if (auto* cse = dynamic_cast<const ast::CaseExpr*>(&e)) {
        typeCase(*cse, e);
    }
    // §15.1: IndexExpr `a[i]` — the expression type is the array's
    // element type, so member access on array elements (`a[i].field`)
    // can resolve. The element type comes from the target's InlineArray
    // TypeRef, or from an indexer-property return type (deferred).
    // §4.6: string indexer `s[i]` → char (code point).
    if (auto* ix = dynamic_cast<const ast::IndexExpr*>(&e)) {
        if (ix->target) {
            TypeRef* tt = ctx_.exprType(*ix->target);
            // fix #2795 (§23.4 / §7.1 / §7.4, v0.240): the receiver type of a
            // `this`-rooted index. §23.4: "Inside a member of a class with its
            // own indexer, `this[i]` is required (the brackets need a left-hand
            // side)" — so this is the MANDATED spelling, not an exotic one.
            // But the IdentifierExpr arm above deliberately leaves `this`
            // WITHOUT an exprType (only #456's primitive/builtin-`extension`
            // case sets one), so `ctx_.exprType(*ix->target)` was null and this
            // ENTIRE block was skipped for `this[i]`. Everything the block does
            // was therefore missing on the one spelling §23.4 requires: the
            // indexer RESULT TYPE (`var s := this[i].ToString()` degraded to a
            // zero-initialized value, #2434, and emitted malformed IR — "ret
            // i32 %4 / ptr"), the #2530 PropertySymbol attachment, the #2543
            // write-only-read gate, and the #2619 §7 index check (so
            // `this[NI()]` with an `int?` index compiled with NO diagnostic and
            // returned the WRONG VALUE — the null index became 0). A bare
            // `return this[i];` happened to work because the return coercion
            // supplies the type, which is why this went unnoticed.
            //
            // Resolved LOCALLY here rather than by publishing an exprType for
            // `this` in the IdentifierExpr arm (the wider of the two shapes the
            // issue proposed): that would turn on every exprType(`this`) reader
            // at once, including codegen's `indexReceiverDecl`
            // (LlvmBackend_Assign.cpp), whose #747 R42 branch deliberately
            // recovers the owner TypeSymbol from `methodFns_` /
            // `currentSpecMethodSymbol_` — a specialization-aware path that a
            // sema-published TypeRef would silently short-circuit. Confining
            // the resolution to this expression form changes nothing outside
            // `this[i]`.
            //
            // GENERICS: `substituteMemberType` already handles a `this` target
            // (its own #747 R42 branch seeds the base-chain walk from
            // `currentType_` when the receiver carries no exprType), so passing
            // `ix->target` through unchanged below is correct at both shapes —
            // an INHERITED generic-base indexer resolves `T` to the enclosing
            // instantiation's binding (`Derived: Base<int>` → `int`), while the
            // declaring generic class's own indexer stays its rigid parameter
            // (inside `Box<T>`, `this[i]` is `T`), which is exactly what a body
            // in that class sees. The synthesized TypeRef therefore carries NO
            // genericArgs — it denotes the enclosing declaration, not an
            // instantiation of it.
            //
            // fix #2832 (§12.3 / §23.4, v0.240): `base[i]` — the OTHER
            // keyword-rooted spelling — is covered too, now that its
            // miscompile is fixed in the same change. #2795 deliberately left
            // it out on the grounds that typing it alone would only widen the
            // set of spellings reaching a known silent wrong value (`base[i]`
            // on a `virtual` base indexer overridden in the derived class
            // returned the OVERRIDE's value: 202 where 102 is correct), and
            // that the receiver-resolution and non-virtual-dispatch halves
            // had to land together. They do here: codegen's
            // `indexAccessReceiverDecl` takes the §12.3 step to the class base
            // and the two #1080 vtable branches now decline a `base` receiver
            // (LlvmBackend_Assign.cpp), so this typing feeds a correct
            // lowering rather than a wider one.
            //
            // The receiver is the CLASS BASE, mirroring `memberTargetType`'s
            // own `base` arm (ExprTypecheck_Calls.cpp: "§12.3: `base.X`
            // resolves X against the base class, so an override in the current
            // type is bypassed and the base member is found"). Resolving it
            // here rather than to `currentType_` is what makes the attached
            // #2530 PropertySymbol the BASE's indexer — which is in turn what
            // codegen's #2833 fallback reads.
            //
            // §12.3 also bounds where `base` is legal at all: "`base` is legal
            // only inside instance members of a derived class." A class with
            // no class base has `Object` as its base, and `Object` declares no
            // indexer — so `base[i]` there names nothing. It used to compile
            // and silently read the class's OWN indexer (MEASURED: 301, the
            // derived value). Reject it, mirroring the named form's #1106
            // defect-6 rejection in ExprTypecheck_Calls.cpp.
            //
            // A STATIC member is excluded so the existing "`this`/`base` is
            // not available in a static method or static constructor (§12.3)"
            // rejection stays the only diagnostic there.
            if (!tt && currentType_ && !inStaticMember_) {
                if (auto* tid = dynamic_cast<const ast::IdentifierExpr*>(
                        ix->target.get())) {
                    TypeSymbol* recv = nullptr;
                    if (tid->name == "this") {
                        recv = currentType_;
                    } else if (tid->name == "base") {
                        for (TypeSymbol* b : currentType_->baseTypes)
                            if (b && b->kind == ast::TypeDeclKind::Class) {
                                recv = b;
                                break;
                            }
                        if (!recv) {
                            // fix #2983 (§12.3 / §31.4, v0.240): the REJECT is
                            // right — §31.4 declares no indexer on any built-in
                            // exception, so `base[i]` names nothing either way
                            // — but the CLAIM was not. This loop scans
                            // `TypeSymbol::baseTypes`, which never holds a
                            // built-in exception base (a built-in has no
                            // TypeSymbol), so a `class ExB: Exception` was told
                            // "`ExB` has no class base", a falsehood about the
                            // user's own declaration. A diagnostic can be right
                            // to fire and wrong in what it says; name the real
                            // base. `sema::builtinExceptionBaseOf` is the ONE
                            // walk that knows it (#2950).
                            std::string_view bex =
                                builtinExceptionBaseOf(currentType_);
                            ctx_.diagnostics().report(
                                Severity::Error, e.span.begin,
                                "`base[...]` does not name a base-class "
                                "indexer: `" +
                                    std::string(currentType_->name) +
                                    (bex.empty()
                                         ? std::string(
                                               "` has no class base (its base "
                                               "is `Object`, which declares no "
                                               "indexer)")
                                         : ("`'s class base is the built-in `" +
                                            std::string(bex) +
                                            "` (\xC2\xA7""31.4), which declares "
                                            "no indexer")) +
                                    " (§12.3/§23.4)");
                        }
                    }
                    if (recv) {
                        TypeRef* rt = ctx_.makeTypeRef();
                        rt->kind = TypeRef::Kind::Named;
                        rt->declaration = recv;
                        tt = rt;
                    }
                }
            }
            if (tt) {
                // fix #2800 (§7.4, v0.240): a NULLABLE indexer receiver is a
                // compile error unless the compiler has narrowed it to
                // non-null — §7.4 names "indexing" verbatim alongside member
                // access and method calls. The IndexExpr typing block read
                // `ctx_.exprType(*ix->target)` and never consulted
                // `tt->nullable`, so a nullable receiver typed exactly like its
                // non-nullable form and the program compiled (the program ran
                // the null array as empty for Array<int>?, faulting at
                // runtime). Mirror the §7.4 member-access gate in
                // ExprTypecheck_Calls.cpp:13664 — a SIMPLE VARIABLE
                // IdentifierExpr is gated by `isKnownNonNull`; everything
                // else (fields, properties, method results) is never
                // narrowable — TOCTOU, copy to a local first. `tt->nullable`
                // is the receiver-type flag, distinct from the IndexExpr
                // RESULT type (an `Array<T?>` element read is still
                // nullable-per-§7.1 even when the array is non-nullable, and
                // that case is handled by the existing #1161 gate in
                // ExprTypecheck_Calls.cpp:13792, not here). The diagnostic
                // fires but does NOT return — codegen is gated on
                // `hasErrors()` (Driver.cpp), so the rest of this block's
                // typing is harmless when the program will not compile.
                if (tt->nullable) {
                    bool receiverNarrowed = false;
                    if (auto* tid = dynamic_cast<const ast::IdentifierExpr*>(
                            ix->target.get())) {
                        if (tid->name != "this" && tid->name != "base") {
                            Symbol* tsym = lookupName(tid->name);
                            if (tsym && (dynamic_cast<LocalSymbol*>(tsym) ||
                                          dynamic_cast<ParameterSymbol*>(tsym)))
                                receiverNarrowed = isKnownNonNull(tsym);
                        }
                    }
                    if (!receiverNarrowed) {
                        std::string recvName;
                        if (auto* tid = dynamic_cast<const ast::IdentifierExpr*>(
                                ix->target.get()))
                            recvName = std::string(tid->name);
                        // Distinguish a SIMPLE-VAR gate from the TOCTOU /
                        // non-narrowable form so the user gets a remedy
                        // that matches the receiver's category — copy to a
                        // local is the standard fix for the field case.
                        std::string msg =
                            recvName.empty()
                                ? std::string("indexing on a nullable value "
                                              "requires narrowing it to "
                                              "non-null first (§7.4): guard "
                                              "with `if x <> null`, force-"
                                              "unwrap with `x!`, or copy to "
                                              "a local first")
                                : (std::string("indexing on nullable `") +
                                   recvName +
                                   "` requires narrowing it to non-null "
                                   "first (§7.4): guard with `if " +
                                   recvName +
                                   " <> null`, force-unwrap with `" +
                                   recvName + "!, or copy to a local first");
                        ctx_.diagnostics().report(
                            Severity::Error, e.span.begin, msg);
                    }
                }
                if (tt->kind == TypeRef::Kind::InlineArray && tt->arrayElement) {
                    // §15.6 (#746 R41-E): "Slicing is not supported on inline
                    // arrays in this version." A range index on an inline array
                    // otherwise reached codegen and emitted a `%bz.range` struct
                    // as a GEP index (malformed IR); reject it cleanly here.
                    if (!ix->indices.empty() && ix->indices[0].value &&
                        dynamic_cast<const ast::RangeExpr*>(
                            ix->indices[0].value.get()))
                        ctx_.diagnostics().report(
                            Severity::Error, e.span.begin,
                            "range slicing `[a..b]` is not supported on inline "
                            "array types `[N]T` (§15.6)");
                    ctx_.setExprType(&e, tt->arrayElement);
                }
                else if (tt->kind == TypeRef::Kind::Primitive &&
                         tt->primitive == PrimitiveType::String) {
                    // §3.4 / §4.6 / issue #474: a string indexer distinguishes
                    // the index kind. `s[i]` (a single int) returns the i-th
                    // `char` (code point); `s[r]` for a `Range<int>` value `r`
                    // returns the SUBSTRING spanning `r` — a fresh `string`.
                    // Detect the Range form by the index being a range literal
                    // (`s[a..b]`, `s[..b]`, `s[a..]`, `s[..]`) so the access is
                    // typed `string`; otherwise it is `char`.
                    bool rangeIndex =
                        !ix->indices.empty() && ix->indices[0].value &&
                        dynamic_cast<const ast::RangeExpr*>(
                            ix->indices[0].value.get()) != nullptr;
                    // R33: also a Range-typed VALUE — `var r := 0..2; s[r]` —
                    // where the index is a Range<int> variable / call result,
                    // not a syntactic range literal. Without this the result is
                    // mistyped `char`, the slice value is coerced away, and the
                    // codegen range-slice produces an empty string.
                    if (!rangeIndex && !ix->indices.empty() &&
                        ix->indices[0].value) {
                        if (auto* it = ctx_.exprType(*ix->indices[0].value))
                            if (it->kind == TypeRef::Kind::Named &&
                                it->declaration == builtinClassSym("Range"))
                                rangeIndex = true;
                    }
                    ctx_.setExprType(
                        &e, rangeIndex
                                ? ctx_.primitiveTypeRef(PrimitiveType::String)
                                : ctx_.primitiveTypeRef(PrimitiveType::Char));
                }
                // fix #4502 (§23.4, v0.306): every OTHER primitive — `bool`,
                // `char`, and every numeric type (`byte` .. `decimal`),
                // `Object` — declares no indexer. §23.4: "An indexer is a
                // property whose name is replaced by `[parameters]`" —
                // i.e. an indexer is a DECLARED member, and no primitive
                // declares one; `string`'s `[i]`/`[range]` form (typed by
                // the branch immediately above, §3.4/§4.6) is the sole
                // built-in exception. Before this branch, `bool`/`int`/
                // `decimal` (and every other non-`string` primitive) fell
                // through this entire if/else-if chain with NO diagnostic:
                // the IndexExpr was left untyped, exactly the hole the
                // #2800 fix closed for a Named-declaration receiver with no
                // indexer member (see the `!anyIndexer` branch ~900 lines
                // below) — codegen's #2434 backstop degraded the read to a
                // zero-initialized value and the program COMPILED AND RAN.
                // Measured on d3b36b1c (docs v0.306): `var v := b[0];` on a
                // `bool` local exited 0 (only the #2434 warning on stderr)
                // and the program printed "ok". Reject cleanly instead,
                // mirroring the #2800 wording so the message family reads
                // the same regardless of receiver kind.
                else if (tt->kind == TypeRef::Kind::Primitive) {
                    ctx_.diagnostics().report(
                        Severity::Error, e.span.begin,
                        std::string("type `") + primitiveSpelling(tt->primitive) +
                            "` does not declare an indexer — `a[i]` requires "
                            "an indexer (§23.4)");
                }
                // issue #388: heap-collection indexer `a[i]` / `dict[key]`. The
                // built-in collections resolve to a Named TypeRef with a null
                // declaration (TypeResolution_Names) and carry their type
                // arguments in genericArgs. Type the index access as the
                // collection's element/result type so codegen can lower the
                // read and the setter against it (mirrors the inline-array path
                // above):
                //   * Array<T> / OwningArray<T> (stdlib §4.4 / §4.5): the
                //     indexer `[i: int]: T` result is T = genericArgs[0].
                //   * Dictionary<K,V> / OwningDictionary<K,V> (§4.8 / §4.9): the
                //     indexer `[key: K]: V` result is V = genericArgs[1].
                //   * issue #401 / §4.1 / §4.2: the int-indexed list INTERFACES
                //     IReadOnlyList<T> (get-only), IList<T> (get/set borrowing),
                //     and IOwningList<T> (get/owned-set) each declare an indexer
                //     `[i: int]: T`; the element type is T = genericArgs[0], the
                //     same position as Array<T>. (No collection interface
                //     declares a key-indexer, so dictionaries are concrete-only.)
                else if (tt->kind == TypeRef::Kind::Named && !tt->declaration &&
                         tt->astNode && tt->astNode->name &&
                         !tt->astNode->name->parts.empty()) {
                    std::string_view cn = tt->headName();
                    // issue #545: Buffer<T>[i] → T (genericArgs[0]).
                    if (cn == "Buffer" && !tt->genericArgs.empty() &&
                        tt->genericArgs[0]) {
                        ctx_.setExprType(&e, tt->genericArgs[0]);
                    }
                    bool isArrLike =
                        cn == "Array" || cn == "OwningArray" ||
                        // §4.14 (#743/f11): OwningBuffer<T>[i] is element-indexed
                        // exactly like OwningArray<T> — its result type is the
                        // element T = genericArgs[0]. Without this it fell to the
                        // untyped path, where the ownership checker assumed a
                        // borrow and falsely rejected `return buf[0]` for a
                        // value-type T (§28.3 borrow-lifetime).
                        cn == "OwningBuffer" ||
                        // fix #3249 (§4.19, v0.240): FrozenArray<T>[i]'s
                        // result type is the element T (a sendable value
                        // type or string per §4.19). Mirrors Array<T>'s
                        // shape; codegen lowers the indexer get against the
                        // shared bz_array_get path. The bare-annotation
                        // shape (`var f: FrozenArray<T> := ...` produces a
                        // declaration-less Named TypeRef with an astNode,
                        // so reaches THIS branch via `cn == "FrozenArray"`;
                        // the From(...) static-factory result is caught by
                        // the builtinClassSym branch added immediately
                        // before the user-defined indexer fallback below.
                        cn == "FrozenArray" ||
                        // issue #401: list interfaces are array-backed; their
                        // indexer result type is the element T = genericArgs[0].
                        cn == "IReadOnlyList" || cn == "IList" ||
                        cn == "IOwningList";
                    // §4.15 (#549) / §4.14 (#779): ArraySpan/ReadOnlyArraySpan
                    // (borrowing) and Span/ReadOnlySpan (owning) indexer returns
                    // T = genericArgs[0].
                    bool isSpan =
                        cn == "ArraySpan" || cn == "ReadOnlyArraySpan" ||
                        cn == "Span" || cn == "ReadOnlySpan";
                    // §4.8/§4.9 + §4.17 (#1354/#1764): a key-indexed dictionary
                    // read `dict[key]` returns V = genericArgs[1]. SortedDictionary
                    // <K, V> shares the same indexer contract as Dictionary<K, V>;
                    // it was omitted here, so `d[key]` was left untyped and the
                    // ownership pass conservatively rejected `return d[key]` for a
                    // value-type V (§28.3 borrow-lifetime) — the same failure the
                    // OwningBuffer note above describes.
                    bool isDict =
                        cn == "Dictionary" || cn == "OwningDictionary" ||
                        cn == "SortedDictionary";
                    if ((isArrLike || isSpan) && !tt->genericArgs.empty() &&
                        tt->genericArgs[0]) {
                        // §15.6 / fix #690: `arr[range]` on Array<T> produces
                        // a new Array<T> — the result type is the ARRAY type
                        // (tt), not the element type (T). Only Array<T> supports
                        // range slicing; OwningArray<T>, span types, and list
                        // interfaces use the scalar-element path.
                        bool isRangeIdx =
                            !ix->indices.empty() && ix->indices[0].value &&
                            dynamic_cast<const ast::RangeExpr*>(
                                ix->indices[0].value.get()) != nullptr;
                        // §15.6 (#1079): also a Range-typed VALUE — `var r := 0..2;
                        // arr[r]` — not a syntactic range literal. Mirrors the
                        // string-slice path above. Without this `arr[r]` was typed
                        // as the element T (not Array<T>), so `.Count` read garbage
                        // and `for var x in arr[r]` found no collection-typed
                        // iterable ("codegen for this for statement" reject). The
                        // codegen indexer-get already lowers a Range-variable
                        // subscript via bz_array_slice; only the type was missing.
                        if (!isRangeIdx && !ix->indices.empty() &&
                            ix->indices[0].value) {
                            if (auto* it = ctx_.exprType(*ix->indices[0].value))
                                if (it->kind == TypeRef::Kind::Named &&
                                    it->declaration == builtinClassSym("Range"))
                                    isRangeIdx = true;
                        }
                        // §15.6 (#746 R41-E): range slicing produces a NEW Array<T>
                        // of copied references; it is admitted only on Array<T>.
                        // "Slicing is not admitted on OwningArray<T>" — the
                        // copy-returning semantics would manufacture a second
                        // owner for each owned element. Reject it instead of
                        // silently typing the (miscompiled) result as the element.
                        if (isRangeIdx && cn == "OwningArray")
                            ctx_.diagnostics().report(
                                Severity::Error, e.span.begin,
                                "range slicing `[a..b]` is not admitted on "
                                "`OwningArray<T>` — it would manufacture a second "
                                "owner for each owned element (§15.6)");
                        if (isRangeIdx && cn == "Array")
                            ctx_.setExprType(&e, tt);
                        else {
                            TypeRef* elem = tt->genericArgs[0];
                            // §4.14 (#1157): for class-typed T, an
                            // `OwningBuffer<T>` stores `T?` in each slot
                            // (nullable references, all null until set). Its
                            // indexer is declared `[i: int]: T?`, so the
                            // indexer-get must be typed `T?` — an empty-slot
                            // read then requires a null guard / `?.` (a bare
                            // `buf[i].member` would null-deref → SIGSEGV) and
                            // `buf[i] := null` (the natural §28.8.1 restoration)
                            // is accepted. Value-typed T (primitive / record /
                            // enum, incl. the value-builtins DateTime/Range/…)
                            // stores `T` directly with value defaults, so its
                            // indexer stays non-nullable `T`. A bare generic
                            // param (no declaration) is left non-nullable —
                            // its category is unknown until instantiation.
                            // per §7.4 / #1157 (#1208): Span<T> and
                            // ReadOnlySpan<T> (owning spans over OwningBuffer<T>)
                            // store `T?` in each slot for class/interface T —
                            // identical semantics to OwningBuffer<T> itself.
                            // ArraySpan / ReadOnlyArraySpan (borrowing views of
                            // Array<T>) store `T` directly (non-nullable) and
                            // are excluded.
                            if ((cn == "OwningBuffer" ||
                                 cn == "Span" || cn == "ReadOnlySpan") &&
                                elem && !elem->nullable &&
                                elem->kind == TypeRef::Kind::Named &&
                                elem->declaration &&
                                (elem->declaration->kind ==
                                     ast::TypeDeclKind::Class ||
                                 elem->declaration->kind ==
                                     ast::TypeDeclKind::Interface) &&
                                !elem->declaration->isValueBuiltin) {
                                TypeRef* nn = ctx_.makeTypeRef(*elem);
                                nn->nullable = true;
                                elem = nn;
                            }
                            ctx_.setExprType(&e, elem);
                        }
                    } else if (isDict && tt->genericArgs.size() > 1 &&
                             tt->genericArgs[1])
                        ctx_.setExprType(&e, tt->genericArgs[1]);
                    // §3.3 (#883): ReadOnlyStringSpan[i] → char (the i-th code
                    // point of the viewed range). Non-generic, so its element
                    // type is the fixed `char` primitive (not genericArgs[0]).
                    else if (cn == "ReadOnlyStringSpan") {
                        // §7 / stdlib §3.3 (#2613): stdlib §3.3 line 1321
                        // declares the indexer `public property [i: int]: char`
                        // — a single NON-nullable `int` index, and the only
                        // indexer form the record declares (no Range overload).
                        // The §7 check on that index used to live here, gated to
                        // this one head name; #2619 found the SAME hole on every
                        // other indexer path (including USER-declared indexers,
                        // which do have a PropertySymbol, so the "builtin members
                        // have no MethodSymbol" explanation never covered them)
                        // and moved the check to checkIndexArgNullability, run
                        // once for every receiver family at the end of this
                        // block. Keeping a second copy here would double-report.
                        ctx_.setExprType(
                            &e, ctx_.primitiveTypeRef(PrimitiveType::Char));
                    }
                    // #794 / §14.2 + §23.4: `x[i]` where `x` has the type of a
                    // bare generic parameter `T` constrained `where T: IIndexable`
                    // and the constraint interface declares an indexer. The stdlib
                    // branches above don't match (cn names a type parameter, not a
                    // collection), so the IndexExpr was left untyped → the
                    // ownership pass assumed a borrow and the result fell to 0.
                    // Type it as the constraint interface's indexer return type
                    // (codegen then devirtualizes through genericSubstSemaTy_).
                    // Mirrors the #484 method-via-constraint resolver
                    // (ExprTypecheck_Calls.cpp): walk the type parameter's
                    // where-constraints on the enclosing method, then the class.
                    if (!ctx_.exprType(e)) {
                        // fix #3644 (§26.4 / §14.2, v0.286): `isParam` used to
                        // be computed INSIDE the lambda from a `gps` list
                        // (currentMethodDecl_'s or currentType_'s own
                        // `genericParams`) — but an extension's own §26.4
                        // block-level type parameter binds via its
                        // `targetType`'s argument positions
                        // (`isExtensionTargetTypeParam`), never through a
                        // `genericParams` list (`ast::ExtensionDecl` has no
                        // such field), so there was no `gps` to pass for a
                        // third, extension arm. Hoisting the "is `cn` a
                        // declared param here" computation to each call site
                        // and taking a plain `bool` lets the extension arm
                        // below reuse the same gc-scan instead of a second
                        // hand-rolled copy of it.
                        auto tryIndexerConstraint =
                            [&](bool isParam,
                                const std::vector<ast::GenericConstraint>& gcs)
                            -> bool {
                            if (!isParam) return false;
                            for (const auto& gc : gcs) {
                                if (gc.typeParam != cn) continue;
                                for (const auto& item : gc.items) {
                                    if (item.kind != "type" || !item.typeArg)
                                        continue;
                                    TypeRef* ctr =
                                        ctx_.resolvedType(*item.typeArg);
                                    if (!ctr ||
                                        ctr->kind != TypeRef::Kind::Named ||
                                        !ctr->declaration)
                                        continue;
                                    auto* ps = dynamic_cast<PropertySymbol*>(
                                        lookupMember(ctr->declaration, ".this"));
                                    if (!ps || !ps->isIndexer) continue;
                                    TypeRef* rt = nullptr;
                                    // fix #3620 (§14.2 / §23.4 / §2.5,
                                    // v0.286): an IMPORTED constraint
                                    // interface's indexer has no
                                    // `ast::PropertyDecl` (ModuleLoader
                                    // reconstructs only the PropertySymbol +
                                    // `indexerParams` ParameterSymbols, no AST
                                    // crosses the artifact boundary — §2.5),
                                    // so `ps->decl == nullptr` and the prior
                                    // `if (!pd || !pd->type) continue` guard
                                    // abandoned the whole constraint path for
                                    // any imported interface. Fall back to
                                    // `ctx_.importedMemberType(ps)` (the same
                                    // side table the IndexExpr sibling site
                                    // ~500 lines below reads from in its
                                    // own `if (pd) … else importedMemberType`
                                    // arm), so a constraint indexer is typed
                                    // for an imported interface exactly the
                                    // same way as for a local one.
                                    if (auto* pd =
                                            dynamic_cast<const ast::PropertyDecl*>(
                                                ps->decl)) {
                                        if (pd->type)
                                            rt = ctx_.resolvedType(*pd->type);
                                    } else {
                                        rt = ctx_.importedMemberType(ps);
                                    }
                                    if (rt) {
                                        ctx_.setExprType(
                                            &e, substituteMemberType(
                                                    rt, ix->target.get(),
                                                    ps->parent));
                                        // per §7.4 (v0.182, #1593): a `T[i]`
                                        // indexer access resolved through a
                                        // type-parameter constraint is an
                                        // accessor call too — invalidate a
                                        // mutably-captured local's narrowing
                                        // (#1566). fix #3616: ask the symbol,
                                        // not the decl. Mirrors the IndexExpr
                                        // sibling site (§7.4 path now also
                                        // fires for the imported constraint
                                        // interface case the `if (!pd ||
                                        // !pd->type) continue` guard used to
                                        // silently skip).
                                        if (propertyAccessRunsUserCode(ps, ctx_))
                                            invalidateCapturedNarrowings();
                                        return true;
                                    }
                                }
                            }
                            return false;
                        };
                        bool done = false;
                        if (currentMethodDecl_ &&
                            !currentMethodDecl_->genericParams.empty()) {
                            bool isParam = false;
                            for (const auto& gp :
                                 currentMethodDecl_->genericParams)
                                if (gp.name == cn) { isParam = true; break; }
                            done = tryIndexerConstraint(
                                isParam,
                                currentMethodDecl_->genericConstraints);
                        }
                        if (!done && currentType_ && currentType_->decl)
                            if (auto* td = dynamic_cast<const ast::TypeDecl*>(
                                    currentType_->decl))
                                if (!td->genericParams.empty()) {
                                    bool isParam = false;
                                    for (const auto& gp : td->genericParams)
                                        if (gp.name == cn) {
                                            isParam = true;
                                            break;
                                        }
                                    done = tryIndexerConstraint(
                                        isParam, td->genericConstraints);
                                }
                        // fix #3644 (§26.4, v0.286): the third arm #3643 gave
                        // `isInScopeTypeParam`/`forEachConstraintBound` in
                        // ExprTypecheck.cpp was missing here — an extension
                        // over an IMPORTED generic type (`currentType_->decl`
                        // unconditionally null for an import, per #3643's
                        // measurement) never reached the `currentType_` arm
                        // above at all, so `x[i]` on the extension's own
                        // constrained type parameter fell through to
                        // "unresolved identifier" instead of resolving
                        // through the constraint interface's indexer.
                        if (!done && currentExtension_ &&
                            currentExtension_->decl)
                            if (auto* ed =
                                    dynamic_cast<const ast::ExtensionDecl*>(
                                        currentExtension_->decl))
                                if (ed->targetType) {
                                    bool isParam = isExtensionTargetTypeParam(
                                        *ed->targetType, cn);
                                    tryIndexerConstraint(
                                        isParam, ed->genericConstraints);
                                }
                    }
                }
                // fix #3249 (§23.4 / §4.19, v0.240): FrozenArray<T>'s indexer
                // result type is its element T (a sendable value type or
                // string per §4.19). The receiver arrives as a Named TypeRef
                // whose declaration is the synthesized builtinClassSym
                // ("FrozenArray") (stamped by ExprTypecheck_Calls.cpp for the
                // `FrozenArray<T>.From(...)` static-factory result; the bare
                // annotation `var f: FrozenArray<T> := ...` produces a
                // declaration-less Named TypeRef with an astNode instead —
                // caught by the broader branch at line 2378 once we teach it
                // to admit `FrozenArray` as an array-like head, as is done
                // for the buffer/span/list family below). Codegen's
                // lowerCollectionIndexerGet already lowers the indexer get
                // against `isCollectionTypeName(recvTy)` (BuiltinRegistry.cpp's
                // kArrayBackedCollections admits "FrozenArray"), so once the
                // type is set the runtime path produces the bz_array_get
                // load. Without this branch the indexer fell through to the
                // user-defined indexer path below, which walks
                // `tt->declaration->members` looking for an `isIndexer`
                // PropertySymbol; builtinClassSym's TypeSymbol declares no
                // members, so `anyIndexer` stayed false and the §23.4 reject
                // "type `FrozenArray` does not declare an indexer" fired
                // (the regression #3249 captures). REJECTED upstream by the
                // §4.19 / §3.6 element-type restriction (a non-sendable T
                // never reaches this site — ExprTypecheck_Calls.cpp enforces
                // it at the From(...) call site).
                else if (tt->kind == TypeRef::Kind::Named && tt->declaration ==
                         builtinClassSym("FrozenArray") &&
                         !tt->genericArgs.empty() && tt->genericArgs[0]) {
                    // Range slicing is not admitted on FrozenArray<T> — it is
                    // fixed-size, deeply-immutable, shared-read (§4.19). The
                    // §15.6 Array<T>/OwningArray<T> range-slicing branch
                    // above intentionally excludes other array-backed
                    // receivers ("Slicing is not admitted on
                    // OwningArray<T>"); the same exclusion applies here, so
                    // a range subscript falls through and is rejected by the
                    // typecheck's bounds-check path or codegen — not by this
                    // site. We simply type the result as the element T.
                    ctx_.setExprType(&e, tt->genericArgs[0]);
                }
                // §23.4: USER-DEFINED indexer `obj[i]` on a class/record/
                // interface type. The result type is the indexer property's
                // declared type. The ordinary member-lookup path types named
                // properties (resolveMemberAccess) but never reaches an indexer
                // (it has no member name — `obj[i]` is an IndexExpr, not a
                // MemberAccess), so the IndexExpr was left untyped. Type it here
                // by finding the indexer property (stored under `.this`, walked
                // transitively over base types) and resolving its declared type,
                // substituting generic-param result types against the receiver's
                // concrete args (#361 / §14.1) — exactly as resolveMemberAccess
                // does for a named property. This matters downstream: the
                // ownership pass's borrow-vs-copy predicate (§28.3) consults the
                // IndexExpr's type — a VALUE-typed indexer result is a copy, not
                // a borrow, so returning it from an owned `using var` local is
                // not a borrow escape (a CLASS/borrow-typed result still is).
                else if (tt->kind == TypeRef::Kind::Named && tt->declaration) {
                    // §23.4 (#1172): resolve the indexer OVERLOAD for
                    // `obj[a, b, ...]` by its FULL parameter list — the
                    // candidate's parameter COUNT must equal the number of index
                    // arguments AND every parameter type must match the
                    // corresponding argument (an exact structural match is
                    // preferred; a sole arity-matching candidate is accepted with
                    // implicit conversion, as the single-indexer path always
                    // has). Candidates are gathered over the receiver type and
                    // its bases (an inherited indexer is a valid candidate),
                    // mirroring lookupMember's frontier walk and the codegen
                    // selector (Emitter::selectIndexer) so codegen lowers exactly
                    // the overload typed here. This replaces the prior
                    // first-parameter-only match (#774), which ignored arity and
                    // later parameters: it selected the wrong overload (`[r,c]`
                    // vs `[r,s]` → IR_FAIL), or a 1-parameter overload for a
                    // 2-argument call (dropping the extra index → WRONG_VALUE),
                    // and accepted an arity-mismatched call outright.
                    const std::size_t nargs = ix->indices.size();
                    std::vector<const TypeRef*> argTys;
                    argTys.reserve(nargs);
                    for (const auto& idxArg : ix->indices)
                        argTys.push_back(idxArg.value
                                             ? ctx_.exprType(*idxArg.value)
                                             : nullptr);
                    // fix #2414 (§23.4 / §2.8.2 clause 1, v0.235): an IMPORTED
                    // indexer's candidate has no ast::PropertyDecl (mirrors
                    // Property's own #503 limitation — ModuleLoader.cpp's
                    // Indexer case reconstructs `indexerParams` as
                    // ParameterSymbols carrying `resolvedDeclType` instead of
                    // an AST node). Read a candidate's k-th index-parameter
                    // type and its arity through either source uniformly, so
                    // the overload-matching logic below works unchanged for a
                    // local OR an imported candidate — without this, every
                    // imported indexer candidate had `cpd == nullptr` and was
                    // skipped outright, `anyIndexer` still true (set before
                    // the decl check) but neither `ps` nor `arityOnly` ever
                    // assigned, falling into the "no overload matches"
                    // rejection below for a perfectly well-typed access
                    // (confirmed: `no indexer overload matches the 1
                    // argument (§23.4)` for the issue's own 1-arg repro).
                    auto candParamCount =
                        [](const PropertySymbol* cand) -> std::size_t {
                        if (auto* cpd = dynamic_cast<const ast::PropertyDecl*>(
                                cand->decl))
                            return cpd->indexerParams.size();
                        return cand->indexerParams.size();
                    };
                    auto candParamResolvedType =
                        [this](const PropertySymbol* cand,
                              std::size_t k) -> const TypeRef* {
                        if (auto* cpd = dynamic_cast<const ast::PropertyDecl*>(
                                cand->decl)) {
                            return (k < cpd->indexerParams.size() &&
                                    cpd->indexerParams[k].type)
                                       ? ctx_.resolvedType(
                                             *cpd->indexerParams[k].type)
                                       : nullptr;
                        }
                        if (k < cand->indexerParams.size()) {
                            if (auto* p = dynamic_cast<ParameterSymbol*>(
                                    cand->indexerParams[k]))
                                return p->resolvedDeclType;
                        }
                        return nullptr;
                    };
                    // fix #2834 (§23.4 / §7.1, v0.240): an indexer is
                    // OVERLOADED by its parameter TYPE LIST (§23.4), and §7.1
                    // says `T` and `T?` are distinct types. The prior
                    // structural check (kinds + primitive/declaration) did
                    // NOT compare `nullable` — so an `int?` argument
                    // structurally matched `[i: int]` and the resolver
                    // picked whichever candidate the `t->members` walk
                    // reached first (typically `[i: int]`), bypassing the
                    // exact-match `[n: int?]` overload and then triggering
                    // §7's "nullable argument to non-nullable target"
                    // reject (#2619's gate (4) would have exempted `[n:
                    // int?]` entirely). Compare `nullable` on both the
                    // Primitive and the Named arm so a nullable-parameter
                    // candidate is preferred for a nullable argument; the
                    // arity-only fallback below is preserved untouched so
                    // implicit-conversion spellings like `a[someLong]`
                    // continue to work and a nullable argument still falls
                    // to §7 only when NO nullable-parameter overload
                    // exists.
                    auto allParamsMatch =
                        [&](const PropertySymbol* cand) -> bool {
                        for (std::size_t k = 0; k < nargs; ++k) {
                            const TypeRef* a = argTys[k];
                            const TypeRef* p = candParamResolvedType(cand, k);
                            if (!p || !a || p->kind != a->kind) return false;
                            if (p->nullable != a->nullable) return false;
                            if (p->kind == TypeRef::Kind::Primitive) {
                                if (p->primitive != a->primitive) return false;
                            } else if (p->kind == TypeRef::Kind::Named) {
                                if (p->declaration != a->declaration)
                                    return false;
                            } else {
                                return false;
                            }
                        }
                        return true;
                    };
                    PropertySymbol* ps = nullptr;        // exact full-list match
                    PropertySymbol* arityOnly = nullptr;  // first arity match
                    bool anyIndexer = false;
                    std::vector<TypeSymbol*> frontier{tt->declaration};
                    for (std::size_t fi = 0; fi < frontier.size() && !ps; ++fi) {
                        TypeSymbol* t = frontier[fi];
                        for (Symbol* m : t->members) {
                            auto* cand = dynamic_cast<PropertySymbol*>(m);
                            if (!cand || !cand->isIndexer) continue;
                            anyIndexer = true;
                            if (candParamCount(cand) != nargs) continue;
                            if (!arityOnly) arityOnly = cand;
                            if (allParamsMatch(cand)) { ps = cand; break; }
                        }
                        for (TypeSymbol* b : t->baseTypes)
                            if (b) frontier.push_back(b);
                    }
                    // No exact full-list match: a sole arity-matching candidate
                    // is the selection (the arguments are implicitly converted,
                    // exactly as the single-indexer path always did).
                    if (!ps) ps = arityOnly;
                    if (ps && ps->isIndexer) {
                        // fix #2955 (§23.4 / §13 / §14.1 / §2.8.3 rule 4,
                        // v0.240): an INDEXER whose INDEX PARAMETER is typed
                        // by the RECEIVER DECLARATION's own generic
                        // parameter (e.g. `interface I<T> { property [i: T]:
                        // int }` reached as `r[i]` over `r: I<string>`) has no
                        // working lower path in the current ABI — the
                        // exported cross-module stub is keyed by the open
                        // type and erases `T -> i32`, while the call-site
                        // argument is the concrete-substituted type (a `ptr`
                        // for `string`), so the emitted indirect call carries
                        // the wrong `i32 (ptr, i32)` signature for an `i32
                        // (ptr, ptr)` argument and aborts at LLVM IR
                        // verification with no located diagnostic. The named-
                        // property twin is #2944 (return type) and the
                        // property-side twin is #2928; this is the INDEX
                        // PARAMETER form of the family.
                        //
                        // MEASURED (issue's own repro, two-module
                        // `bezelc -c lib.bzl` then `bezelc --import lib.o
                        // app.bzl`):
                        //   * `r["k"]` on `var r: IAtU5<string> := g` →
                        //     `Call parameter type does not match function
                        //     signature! ptr @.bz.str.50` (raw verifier
                        //     abort, no source location, exit non-zero).
                        //   * the SAME shape reached DIRECTLY through the
                        //     implementing class (`g["k"]` on `var g :=
                        //     new GArrU5<string>()`) → byte-identical
                        //     verifier abort, because the imported-stub
                        //     dispatch goes through the same `bz$<Class>$
                        //     idxget$<arity>` symbol whose signature is the
                        //     same erasure.
                        //
                        // Decline with a sourced diagnostic, in the style of
                        // #2884 (the property twin) and #2832 / #1943's
                        // imported-generic shapes — and exactly as
                        // `accessorAbiFits` (LlvmBackend_Setup.cpp, added in
                        // #2946) refuses to install an itable slot whose ABI
                        // doesn't fit the per-instantiation substitution
                        // here refuses to TYPE an indexer access whose
                        // declared ABI can't be made to fit the substitution
                        // at the call site. The Driver gates codegen on
                        // `!diagnostics.hasErrors()` (Driver.cpp's `if
                        // (!objectPath.empty() && !diagnostics.hasErrors())`
                        // before emitModule), so once this diagnostic fires
                        // codegen is skipped and the verifier never sees the
                        // broken module. The full fix is importer-side
                        // accessor monomorphization per instantiation, the
                        // way `methodBodyText` / `ctorBodyText` already
                        // carry method/ctor bodies across the artifact
                        // boundary (#428 family).
                        //
                        // GATED: only the shape the issue's own diagnosis
                        // names, and only on the dispatch paths the ABI
                        // mismatch actually breaks. The receiver must be a
                        // generic Interface (any interface — local or
                        // imported — because the itable slot is filled
                        // with a function whose ABI is fixed at the erasure)
                        // OR an IMPORTED generic Class/Record (because the
                        // cross-module stub `bz$<Class>$idxget$<arity>` is
                        // the same erasure and there is no per-instantiation
                        // re-emission). The LOCAL generic Class/Record case
                        // is OUT OF GATE — codegen inlines the body into
                        // the caller (the `getterExpr` / `getterBlock`
                        // paths in `lowerIndexerGet`/`-Set`), and the body
                        // runs in the caller's substitution context, so a
                        // body that doesn't touch the index parameter
                        // works at runtime even when the indexer's
                        // declared parameter type is the generic param.
                        // PROBED: `GCtrl4<string>` accessed directly as
                        // `g["k"]` (local generic class) returns the
                        // correct 42, no verifier abort — the body is
                        // `_w + 2`, never reads `i`. The same indexer
                        // reached through a generic INTERFACE
                        // (`r["k"]` on `var r: ICtrl3<string>`) DOES abort
                        // the verifier — same root cause, but the dispatch
                        // is itable-mediated.
                        //
                        // Receiver's index parameter type must additionally
                        // be a bare TypeRef::Kind::Named whose `headName`
                        // matches one of those generic params' names — a
                        // Name-only reference, not a declaration-resolved
                        // one (mirrors `interfaceIndexerElementType`'s
                        // predicate shape). Out-of-gate shapes:
                        //   * a NON-generic receiver — no substitution to
                        //     erase, the stub ABI IS the call ABI.
                        //   * an indexer parameter typed by a
                        //     declaration-resolved class/record (e.g. `[i:
                        //     SomeNamedClass]`) — `lowerType` lowers it to
                        //     the same `ptr` the call site passes.
                        //   * a builtin collection (Array<T>, dictionary,
                        //     string, span/buffer) — has no PropertySymbol
                        //     here and was already a builtin path; the issue
                        //     is specific to a USER-declared indexer.
                        //   * a `base`-rooted indexer — gated separately
                        //     (#2832's `rejectImportedVirtualBaseIndexer` in
                        //     codegen; this decline would also fire on it,
                        //     which is correct — the same family).
                        if (tt && tt->kind == TypeRef::Kind::Named &&
                            tt->declaration &&
                            !tt->declaration->genericParams.empty() &&
                            (tt->declaration->kind ==
                                 ast::TypeDeclKind::Interface ||
                             ctx_.isImported(tt->declaration))) {
                            // Map generic-param NAME → its positional index in
                            // the receiver declaration's genericParams list,
                            // so the per-parameter substitution check below
                            // can walk `tt->genericArgs` in the same order.
                            std::unordered_map<std::string_view, std::size_t>
                                gpIndex;
                            for (std::size_t gi = 0;
                                 gi < tt->declaration->genericParams.size();
                                 ++gi) {
                                if (auto* gp =
                                        dynamic_cast<const GenericParamSymbol*>(
                                            tt->declaration->genericParams[gi]))
                                    gpIndex[gp->name] = gi;
                            }
                            auto paramIsBareReceiverTypeParam =
                                [&](const TypeRef* pty) -> bool {
                                if (!pty || pty->kind != TypeRef::Kind::Named)
                                    return false;
                                if (pty->declaration) return false;
                                std::string_view hn = pty->headName();
                                if (hn.empty()) return false;
                                return gpIndex.count(hn) != 0;
                            };
                            // For an index parameter typed by a generic
                            // param of the receiver, the substitution MOVES
                            // the index parameter's ABI iff the substituted
                            // arg's lowered LLVM type is not the i32
                            // erasure placeholder. The only TypeRef that
                            // lowers to i32 is the int primitive — every
                            // other kind (any other primitive, any named
                            // type, any nested generic) lowers to a
                            // different type and would abort the verifier on
                            // the call. This is the same population the
                            // call-site ABI (`gFnTy`/`sFnTy` in
                            // `lowerIndexerGet`/`-Set`,
                            // LlvmBackend_Assign.cpp) needs to match;
                            // checking it here keeps the decline to the
                            // shape that ACTUALLY breaks, instead of
                            // catching the T=int case (where the
                            // substitution is the same `i32` erasure and
                            // the call type matches the argument type —
                            // measured to compile clean and run clean).
                            auto substitutedArgMovesAbi =
                                [&](const TypeRef* pty) -> bool {
                                if (!pty || pty->kind != TypeRef::Kind::Named)
                                    return true;  // unknown / non-named:
                                                  // be conservative
                                if (pty->declaration) return true;
                                std::string_view hn = pty->headName();
                                auto it = gpIndex.find(hn);
                                if (it == gpIndex.end()) return true;
                                const std::size_t gi = it->second;
                                if (gi >= tt->genericArgs.size()) return true;
                                const TypeRef* arg = tt->genericArgs[gi];
                                if (!arg) return true;
                                // The ONLY shape that does NOT move the ABI:
                                // the int primitive. Every other kind
                                // (string, bool, byte, char, any Named
                                // declaration, any nested generic, any
                                // non-int primitive) lowers to a different
                                // LLVM type.
                                if (arg->kind == TypeRef::Kind::Primitive &&
                                    arg->primitive == PrimitiveType::Int)
                                    return false;
                                return true;
                            };
                            bool paramHitsGenericParam = false;
                            if (auto* pd2 =
                                    dynamic_cast<const ast::PropertyDecl*>(
                                        ps->decl)) {
                                for (const auto& p : pd2->indexerParams) {
                                    if (p.type) {
                                        const TypeRef* pt =
                                            ctx_.resolvedType(*p.type);
                                        if (paramIsBareReceiverTypeParam(pt) &&
                                            substitutedArgMovesAbi(pt)) {
                                            paramHitsGenericParam = true;
                                            break;
                                        }
                                    }
                                }
                            }
                            if (!paramHitsGenericParam) {
                                for (const Symbol* pSym :
                                     ps->indexerParams) {
                                    if (auto* prm =
                                            dynamic_cast<const ParameterSymbol*>(
                                                pSym)) {
                                        if (paramIsBareReceiverTypeParam(
                                                prm->resolvedDeclType) &&
                                            substitutedArgMovesAbi(
                                                prm->resolvedDeclType)) {
                                            paramHitsGenericParam = true;
                                            break;
                                        }
                                    }
                                }
                            }
                            if (paramHitsGenericParam) {
                                auto loc = e.span.begin;
                                ctx_.diagnostics().report(
                                    Severity::Error, loc,
                                    "indexer `" + std::string(ps->name) +
                                    "` on the generic type `" +
                                    std::string(tt->declaration->name) +
                                    "` is not yet monomorphized "
                                    "(\xC2\xA7""23.4 / \xC2\xA7""14.1 / "
                                    "\xC2\xA7""2.8.3 rule 4 / #2955): a "
                                    "generic type's indexer accessors are "
                                    "not exported per instantiation, so an "
                                    "index parameter typed by a type "
                                    "parameter of the receiver's "
                                    "declaration has no correct target — "
                                    "it aborts LLVM IR verification with "
                                    "an `i32 (ptr, …)` call against a "
                                    "substituted `ptr` argument. Access a "
                                    "field directly, or route through a "
                                    "method, until importer-side indexer "
                                    "monomorphization lands");
                            }
                        }
                        auto* pd = dynamic_cast<const ast::PropertyDecl*>(
                            ps->decl);
                        TypeRef* rt = nullptr;
                        // fix #2414: an imported indexer's declared element
                        // type rides `ctx_.importedMemberType` (the same
                        // side table Property reads, ModuleLoader.cpp
                        // `fieldTypes_`), mirroring the param-type fallback
                        // above — there is no `pd->type` to resolve.
                        if (pd) {
                            if (pd->type) rt = ctx_.resolvedType(*pd->type);
                        } else {
                            rt = ctx_.importedMemberType(ps);
                        }
                        if (rt)
                            ctx_.setExprType(
                                &e, substituteMemberType(
                                        rt, ix->target.get(), ps->parent));
                        // fix #2530 (§22.3 / §23, v0.236): attach the resolved
                        // indexer PropertySymbol to the IndexExpr. The ref/out
                        // operand pass (`checkRefOutOperandCategory`,
                        // ExprTypecheck.cpp) dispatches `obj[i]` through the
                        // exact `exprSymbol(target)` -> PropertySymbol ->
                        // `checkRefOutPropertyOperand` site that the named-
                        // member `obj.P` path already uses, but the indexer
                        // branch was missing this attachment — so a
                        // write-only indexer as `ref` fell through to codegen
                        // with no write-only gate (#2461's hasGetter bit is
                        // published for indexers too, but the IndexExpr
                        // operand path never consulted it). Without this,
                        // LOCAL write-only indexer-as-`ref` ICEs at
                        // "lowerExpr returned null ... #1386" in
                        // `buildOrderedArgs` (lowerIndexerGet's no-getter path
                        // returns null) and IMPORTED write-only indexer-as-`ref`
                        // emits a call to the externally-declared-but-undefined
                        // `bz$<Class>$idxget$<arity>` stub → "undefined
                        // reference" linker error. With the attachment, both
                        // shapes reach `checkRefOutPropertyOperand`, whose
                        // imported (!pd) branch (already covers #2461) and
                        // whose local branch (this commit's hasGetter twin)
                        // reject write-only-as-`ref` with a clean §22.3/§23
                        // message. `kind == "ref"` is enforced downstream of
                        // this check, so `out` (where §22.3: "no getter call
                        // occurs") is correctly admitted — issue's case 3.
                        // Set ONLY for the user-defined indexer branch (an
                        // Array<T>/OwningDictionary<>/etc. collection returns
                        // no PropertySymbol here, and the inline-array path is
                        // a GEP-load with no slot semantics to gate).
                        ctx_.setExprSymbol(&e, ps);
                        // fix #2543 (§23, v0.236): reject a BARE READ of a
                        // write-only (set-only block) INDEXER. Symmetric to
                        // `propertyDeclIsSetOnly`'s read-of-write-only gate
                        // for the named-property case (ExprTypecheck_Calls.cpp
                        // ~line 14327), which deliberately excludes
                        // `pd->isIndexer` because IndexExpr reaches this site,
                        // not a MemberAccessExpr. The local branch mirrors
                        // #2530's ref/out operand check shape:
                        //   - Expression / Auto bodyKinds always have a getter.
                        //   - Block bodyKind has a getter iff any accessor is
                        //     a `get` arm (loop on `!acc.isSet`, mirroring the
                        //     hasSetter loop in checkRefOutPropertyOperand).
                        // The IMPORTED case (no `pd`) consults
                        // `ctx_.importedPropertyHasGetter` — published by
                        // #2461 for indexers too — so an imported set-only
                        // indexer is rejected with the same diagnostic. The
                        // plain `:=` assignment target is exempted (a write
                        // dispatches to the setter and is the LEGAL escape
                        // hatch, exactly like `c.Val := 7` for #1192); a
                        // `ref`/`out` argument operand is exempted (the
                        // ref/out operand pass `checkRefOutPropertyOperand`
                        // #2530 is the specialized gate and correctly
                        // rejects `ref` and admits `out` per §22.3); a
                        // compound `+=` is NOT exempted because it reads
                        // first. Without this gate, a bare READ of a
                        // write-only indexer silently degraded to a zero-
                        // initialized value via the #2434 fallback at codegen.
                        if (&e != plainSetAssignTargetNode_ &&
                            &e != refOutArgTargetNode_) {
                            bool idxHasGetter = false;
                            if (pd) {
                                if (pd->bodyKind == ast::PropertyBodyKind::
                                        Expression ||
                                    pd->bodyKind == ast::PropertyBodyKind::
                                        Auto) {
                                    idxHasGetter = true;
                                } else if (pd->bodyKind ==
                                           ast::PropertyBodyKind::Block) {
                                    for (const auto& acc : pd->accessors)
                                        if (!acc.isSet) {
                                            idxHasGetter = true;
                                            break;
                                        }
                                }
                            } else {
                                // imported: rely on the published
                                // importedPropertyHasGetter bit (default
                                // `true` for stale-dep compatibility — see
                                // #2461's rationale in SemaContext.hpp).
                                idxHasGetter =
                                    ctx_.importedPropertyHasGetter(ps);
                            }
                            if (!idxHasGetter) {
                                // Mirror the propertyDeclIsSetOnly NAMED-
                                // property message shape ("property `W` is
                                // write-only ...") so a user greps for the
                                // same substring. An INDEXER has no usable
                                // `pd->name` (the parser leaves it empty for
                                // the `property this[i: T]: U` form, and
                                // `ps->name` is the metadata handle ".this"
                                // for the IMPORTED case — neither makes a
                                // useful human label), so use "the
                                // `<DeclaringType>` indexer" mirroring
                                // #2530's indexer label in
                                // checkRefOutPropertyOperand.
                                std::string typeName = ps->parent
                                    ? std::string(ps->parent->name)
                                    : std::string();
                                std::string label = typeName.empty()
                                    ? std::string("the indexer")
                                    : ("the `" + typeName + "` indexer");
                                ctx_.diagnostics().report(
                                    Severity::Error, e.span.begin,
                                    std::string("a read of a write-only "
                                                "indexer is not allowed — ")
                                    + label +
                                    " is write-only (it declares only a "
                                    "`set` accessor) and cannot be read "
                                    "(§23)");
                            }
                        }
                        // per §7.4 (v0.182, #1593): a user-defined indexer
                        // access `obj[i]` invokes an "accessor" (get/set)
                        // that can run user code — invalidate a mutably-
                        // captured local's narrowing exactly as a named-
                        // property access or a call does (#1566). Indexers
                        // are never auto, so any user indexer qualifies. An
                        // imported indexer (no `pd`) is conservatively treated
                        // as running user code too — the safe default (it
                        // forgoes a narrowing optimization it might have kept,
                        // never keeps one it shouldn't). fix #3616: that
                        // reasoning now lives ONCE, inside the symbol overload,
                        // instead of in this site's own `pd ? … : true`; this
                        // was the only one of the three sites that got it
                        // right, and having it right here is why the other two
                        // being wrong went unnoticed.
                        if (propertyAccessRunsUserCode(ps, ctx_))
                            invalidateCapturedNarrowings();
                    } else if (anyIndexer && nargs > 0) {
                        // §23.4: the receiver declares indexer(s) but none has
                        // the argument count — e.g. `g[i, j]` against a
                        // 1-parameter indexer (previously accepted, silently
                        // dropping the extra index). Reject cleanly.
                        ctx_.diagnostics().report(
                            Severity::Error, e.span.begin,
                            "no indexer overload matches the " +
                                std::to_string(nargs) +
                                (nargs == 1 ? " argument" : " arguments") +
                                " (§23.4)");
                    } else if (!anyIndexer) {
                        // fix #2800 (§23.4, v0.240): the receiver declares NO
                        // indexer at all (no `isIndexer` member on the type
                        // or any of its bases — `tt->declaration` is set so
                        // the receiver is a user-defined class/record/
                        // interface, not a builtin collection which has its
                        // own above-branch). The `anyIndexer && nargs > 0`
                        // branch fires only when indexers exist but none
                        // matches the argument count; the `!anyIndexer`
                        // shape previously fell through with no diagnostic,
                        // leaving the IndexExpr untyped and codegen
                        // degrading to a zero value (#2434's `internal:
                        // ... zero-initialized value` note). Reject cleanly
                        // with a §23.4 "no indexer" message so the user
                        // gets a sourced error instead of a silent zero.
                        std::string tn = (tt->declaration &&
                                          !tt->declaration->name.empty())
                                              ? std::string(tt->declaration->name)
                                              : std::string();
                        ctx_.diagnostics().report(
                            Severity::Error, e.span.begin,
                            std::string("type `") +
                                (tn.empty() ? std::string("?") : tn) +
                                "` does not declare an indexer — "
                                "`a[i]` requires an indexer (§23.4)");
                    }
                }
                // fix #2619 (§7.1 / §7.4 / §23.4, v0.240): run the §7
                // index-argument check ONCE, after every receiver family above
                // has been typed — a user-declared indexer's resolved
                // PropertySymbol (#2530) is attached by then, which is what
                // lets the check read its declared parameter types. Placed
                // here rather than per-branch because the hole was in EVERY
                // branch, and a single site cannot be forgotten by the next
                // receiver family added above.
                checkIndexArgNullability(*ix, e, tt);
                // fix #2832 (§12.3 / §12.5 / §23.4, v0.240): `base[i]` where
                // the resolved base indexer is ABSTRACT has no implementation
                // to reach — §12.3's whole contract is "calls the base class's
                // implementation", and §12.5 is what says an abstract member
                // has none. The named form already rejects this ("cannot call
                // abstract base method `M` via `base` — it has no
                // implementation (§12.3/§12.5)", ExprTypecheck_Calls.cpp
                // #874); the indexer form silently ran the DERIVED override
                // instead (the #2832 miscompile), and with that fixed it would
                // instead degrade to a zero/null value in codegen — the static
                // accessor-body path finds no body. Reject it here so neither
                // wrong value is reachable. Reads the resolved PropertySymbol
                // the #2530 attachment just published, so it covers every
                // arity and overload without re-doing the lookup. An IMPORTED
                // abstract indexer has no ast::PropertyDecl to read modifiers
                // from and is left alone (the same `!pd` scope limit every
                // other AST-reading indexer check in this block carries).
                if (auto* btid = dynamic_cast<const ast::IdentifierExpr*>(
                        ix->target.get())) {
                    if (btid->name == "base") {
                        auto* bps = dynamic_cast<PropertySymbol*>(
                            ctx_.exprSymbol(e));
                        auto* bpd = bps ? dynamic_cast<const ast::PropertyDecl*>(
                                              bps->decl)
                                        : nullptr;
                        bool bAbstract = false;
                        // Mirror the named-method form L2373-2379 (#874) and
                        // the named-property sibling above: read the SYMBOL's
                        // modifiers FIRST (populated on import at
                        // ModuleLoader.cpp:851), then fall back to the AST
                        // decl's modifiers for the local case. The pure
                        // decl-only scan missed every IMPORTED abstract
                        // indexer — the imported PropertySymbol has no `decl`
                        // — so `base[i]` on an imported abstract base
                        // degraded to the codegen-time #2837 cross-module
                        // reject instead of this clean §12.5 message. fix
                        // #3238.
                        if (bps)
                            for (auto mod : bps->modifiers)
                                if (mod == "abstract") {
                                    bAbstract = true;
                                    break;
                                }
                        if (!bAbstract && bpd)
                            for (auto mod : bpd->modifiers)
                                if (mod == "abstract") {
                                    bAbstract = true;
                                    break;
                                }
                        if (bAbstract)
                            ctx_.diagnostics().report(
                                Severity::Error, e.span.begin,
                                "cannot access an abstract base indexer via "
                                "`base` — it has no implementation "
                                "(§12.3/§12.5)");
                    }
                }
            }
        }
    }
    // Slice 3d: MemberAccessExpr resolution. Runs after the target
    // is visited (so its TypeRef is available), looks the member up
    // on the target's TypeSymbol, and annotates the access with the
    // resolved member symbol and field/property type.
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        resolveMemberAccess(*ma);
        // per §7.4 (v0.182, #1593): a named-property access whose accessor can
        // run user code (a user-defined getter/setter, or a virtual/abstract
        // dispatch) is an "accessor call" — the control transfer may reach a
        // closure that nulls a mutably-captured local, so it invalidates that
        // local's narrowing exactly as a method/constructor call does (#1566;
        // the target was already visited under the pre-access narrowing). A
        // non-virtual auto-property is a pure field load and does NOT invalidate
        // (propertyAccessRunsUserCode); indexers are handled at the IndexExpr
        // sites. A no-op unless the body has a lambda that mutates a captured
        // local.
        // fix #3616: the inner `if (auto* pd = …)` this replaces dropped the
        // invalidation for EVERY imported property (§2.5 leaves `ps->decl`
        // null), so the same program was rejected with a local `Box` and
        // accepted with an imported one — the §2.8.3 rule 4 divergence. The
        // outer PropertySymbol gate stays: a builtin `.Length`/`.Count` access
        // sets no PropertySymbol at all and is not this rule's business.
        if (auto* ps = dynamic_cast<PropertySymbol*>(ctx_.exprSymbol(e))) {
            if (propertyAccessRunsUserCode(ps, ctx_))
                invalidateCapturedNarrowings();
        }
        // §22.12 (cf. C# CS0176): a static member must be accessed through the
        // type name (`C._x`), not through an instance (`c._x`). The latter was
        // silently accepted (and previously crashed codegen, #747). Reject it.
        if (ma->target) {
            sema::Symbol* mem = ctx_.exprSymbol(e);
            auto isStaticMod = [](const sema::MemberSymbol* m) {
                for (auto md : m->modifiers)
                    if (md == "static") return true;
                return false;
            };
            bool memStatic = false;
            if (auto* fs = dynamic_cast<sema::FieldSymbol*>(mem))
                memStatic = isStaticMod(fs);
            else if (auto* ms = dynamic_cast<sema::MethodSymbol*>(mem))
                memStatic = isStaticMod(ms) && !ms->isOperator &&
                            ms->extensionDecl == nullptr;
            else if (auto* ps = dynamic_cast<sema::PropertySymbol*>(mem))
                memStatic = isStaticMod(ps);
            if (memStatic) {
                // A type-name receiver (`C._x`) or primitive-keyword receiver
                // (`int.MaxValue`) is fine; an instance value receiver is not.
                sema::Symbol* recv = ctx_.exprSymbol(*ma->target);
                bool recvIsType =
                    dynamic_cast<sema::TypeSymbol*>(recv) != nullptr ||
                    // §11.8 (v0.176, #807): a type-parameter receiver
                    // (`T.Zero()`) IS a type-name receiver — the static
                    // dispatches through the constraint at instantiation.
                    dynamic_cast<sema::GenericParamSymbol*>(recv) != nullptr ||
                    // fix #4038 (§27.8, v0.294): a module-level `type` alias
                    // receiver (`AliasOfFoo.Bar`) IS a type-name receiver too
                    // — §27.8 sees the alias through to the aliased type for
                    // "identity, equality, method dispatch" alike. Without
                    // this arm, resolveMemberAccess's own #4038 fix (which now
                    // sets `ma->target`'s exprType to the aliased Named
                    // TypeRef so codegen sees through the alias) newly tripped
                    // THIS check: `recv` is a TypeAliasSymbol (not a
                    // TypeSymbol), so `recvIsType` was false, and the now-
                    // non-null `ctx_.exprType(*ma->target)` made the
                    // instance-receiver rejection fire on a perfectly legal
                    // type-name access.
                    dynamic_cast<sema::TypeAliasSymbol*>(recv) != nullptr;
                bool recvIsPrim = false;
                if (auto* rid = dynamic_cast<const ast::IdentifierExpr*>(
                        ma->target.get()))
                    recvIsPrim =
                        primitiveFromName(rid->name) != PrimitiveType::None;
                if (!recvIsType && !recvIsPrim && ctx_.exprType(*ma->target)) {
                    ctx_.diagnostics().report(
                        Severity::Error, ma->span.begin,
                        "static member `" + std::string(ma->member) +
                        "` must be accessed through the type name, not an "
                        "instance (§22.12)");
                }
            }
        }
    }
    // §17.5 null-conditional member access `x?.M`.
    if (auto* nm = dynamic_cast<const ast::NullCondMemberExpr*>(&e)) {
        resolveNullCondMember(*nm);
    }
    // Slice 3f: NewExpr is typed as its constructed type. `default`
    // and `typeof` follow the same pattern (default(T) → T,
    // typeof(T) → Type per §17.1).
    if (auto* ne = dynamic_cast<const ast::NewExpr*>(&e)) {
        if (ne->type) {
            if (auto* rt = ctx_.resolvedType(*ne->type)) {
                ctx_.setExprType(&e, rt);
                // fix #2994 (§17.7, v0.241): a standard-library builtin's
                // constructor surface is exactly what the standard library
                // documents for it. Runs on the EXPLICIT `new T(...)` form
                // here; the §17.7 `new()` shorthand is checked at the two
                // sites that give it its target type (ExprTypecheck.cpp).
                checkBuiltinClosedConstructorSurface(*ne);
                // §5.4/§10 (#960) and §35.1/§35.2/§35.3 (#4163): `new`
                // cannot construct an enum (an enum value comes from the
                // int→enum CAST `E(n)`, §5.4 — an enum has no constructor,
                // §10) or a choice (§35.1 "Declaration": "Choices cannot
                // have destructors, constructors, instance fields,
                // block-form or storage-backed properties, indexers, or
                // nested types"; §35.2 "Members": "a choice's construction
                // *is* case selection", produced only by the qualified
                // case call `Type.Case(args)`, §35.3). `new E(...)` /
                // `new ChoiceType(args)` were silently accepted; the
                // choice case additionally reached codegen with no case
                // discriminant and crashed the compiler on the first
                // `case` statement over the result (SIGSEGV, #4163). Both
                // the qualified case call and the enum cast are CallExprs,
                // not NewExprs, and are unaffected. fix #4170 (v0.295)
                // factored this out to `rejectNewOnEnumOrChoice` so the
                // §17.7 typeless `new()` shorthand sites in
                // ExprTypecheck.cpp (which reach the same `rt`-resolution
                // but never ran through this NewExpr-typing block) share
                // this one authority instead of a third/fourth spelling
                // of the same two checks.
                rejectNewOnEnumOrChoice(*ne, rt);
                // §14.5 Scope (v0.184, #1687): "Class-, record-, and
                // interface-level type arguments are never inferred from
                // constructor arguments: `new Pair(1, "x")` against `record
                // Pair<TF, TS>(...)` is a compile error directing to explicit
                // arguments (`new Pair<int, string>(1, "x")`) or, where a
                // target type exists, the `new()` shorthand." This is the
                // explicit-name form (`ne->type` is present — the `new()`
                // shorthand takes the type name from the DECLARED TARGET TYPE
                // via a separate code path, `ne->type == nullptr`, and is
                // unaffected). A NON-empty but mismatched explicit list
                // (`new Pair<int>(...)`) is already rejected generically by
                // the type resolver's generic-arity check; this catches the
                // case that check does not — an EMPTY list on a generic
                // user type, which previously left the constructor's field
                // types as unresolved type parameters and either silently
                // miscompiled or crashed the backend (issue #1687 q19).
                if (rt->kind == TypeRef::Kind::Named && rt->declaration &&
                    rt->genericArgs.empty() &&
                    !rt->declaration->genericParams.empty()) {
                    ctx_.diagnostics().report(
                        Severity::Error, ne->span.begin,
                        "`new " + std::string(rt->declaration->name) +
                        "(...)` supplies no type arguments, but `" +
                        std::string(rt->declaration->name) + "` declares " +
                        std::to_string(rt->declaration->genericParams.size()) +
                        " type parameter(s) — class-, record-, and "
                        "interface-level type arguments are never inferred "
                        "from constructor arguments; supply the full "
                        "explicit list (`new " +
                        std::string(rt->declaration->name) +
                        "<...>(...)`) or, where a target type exists, use "
                        "the `new()` shorthand (§14.5, §17.7)");
                }
                // §14.1 (#992): `new T()` where T is a generic type parameter
                // requires `where T: new()` on the enclosing generic declaration.
                // Without that constraint the body type-checks under the weakest
                // assumption its constraints establish, and construction cannot
                // be assumed available.  `where T: class`, `where T: value`, a
                // type-bound, or no constraint at all are NOT sufficient.
                // Detection: a generic type parameter resolves to a Named TypeRef
                // with declaration==nullptr; we then verify the name matches one
                // of the enclosing method or class generic params and that at
                // least one `new` constraint item is listed for it.
                if (rt->kind == TypeRef::Kind::Named &&
                    rt->declaration == nullptr &&
                    ne->type && ne->type->name &&
                    !ne->type->name->parts.empty()) {
                    std::string_view paramName = ne->type->name->parts.front();
                    // Determine whether paramName is a generic type parameter
                    // in the enclosing method or class, and whether a `new()`
                    // constraint is present for it.
                    bool isTypeParam = false;
                    bool hasNewConstraint = false;
                    // fix #3644 (§26.4 / §14.1, v0.286): `checkGPs` used to
                    // take a `gps` list and derive `isParamHere` itself
                    // (mirroring `tryIndexerConstraint`'s identical shape a
                    // few hundred lines up, see its own comment) — but an
                    // extension's own §26.4 block-level parameter has no
                    // `genericParams` list to pass (it binds through
                    // `isExtensionTargetTypeParam`'s walk of the extension's
                    // `targetType`). Taking `isParamHere` as a plain `bool`,
                    // computed per call site, lets the extension arm below
                    // reuse the same `hasNewConstraint` scan.
                    auto checkGPs =
                        [&](bool isParamHere,
                            const std::vector<ast::GenericConstraint>& gcs) {
                        if (!isParamHere) return;
                        isTypeParam = true;
                        for (const auto& gc : gcs) {
                            if (gc.typeParam != paramName) continue;
                            for (const auto& item : gc.items)
                                if (item.kind == "new") { hasNewConstraint = true; return; }
                        }
                    };
                    bool isMethodParam = false;
                    if (currentMethodDecl_ &&
                        !currentMethodDecl_->genericParams.empty()) {
                        for (const auto& gp : currentMethodDecl_->genericParams)
                            if (gp.name == paramName) { isMethodParam = true; break; }
                        if (isMethodParam)
                            checkGPs(true,
                                     currentMethodDecl_->genericConstraints);
                    }
                    if (!isMethodParam && currentType_ && currentType_->decl)
                        if (auto* td = dynamic_cast<const ast::TypeDecl*>(
                                currentType_->decl))
                            if (!td->genericParams.empty()) {
                                bool isTd = false;
                                for (const auto& gp : td->genericParams)
                                    if (gp.name == paramName) {
                                        isTd = true;
                                        break;
                                    }
                                checkGPs(isTd, td->genericConstraints);
                            }
                    // fix #3644: the extension arm #3643 gave
                    // `isInScopeTypeParam`/`forEachConstraintBound` in
                    // ExprTypecheck.cpp was missing here — an extension's
                    // own §26.4 type parameter (never in `currentType_`'s
                    // `genericParams`, and unreachable at all when the
                    // extended type is imported — `currentType_->decl` is
                    // unconditionally null then, per #3643's measurement)
                    // silently escaped the `where T: new()` obligation
                    // §14.1 requires: `new T()` inside a generic extension
                    // method type-checked whether or not the extension's own
                    // `T` was constrained `new()`, because this predicate
                    // never saw the extension's parameter or its
                    // constraints at all. Only tried once neither the method
                    // nor the (name-coincident) class arm already matched —
                    // same method > type > extension precedence #3643 used.
                    if (!isMethodParam && !isTypeParam && currentExtension_ &&
                        currentExtension_->decl)
                        if (auto* ed = dynamic_cast<const ast::ExtensionDecl*>(
                                currentExtension_->decl))
                            if (ed->targetType)
                                checkGPs(
                                    isExtensionTargetTypeParam(
                                        *ed->targetType, paramName),
                                    ed->genericConstraints);
                    if (isTypeParam && !hasNewConstraint) {
                        ctx_.diagnostics().report(
                            Severity::Error, ne->span.begin,
                            "`new " + std::string(paramName) + "()` requires "
                            "the type parameter `" + std::string(paramName) +
                            "` to be constrained `where " +
                            std::string(paramName) + ": new()` — the body "
                            "type-checks under the weakest assumption its "
                            "constraints establish, and construction is not "
                            "guaranteed without the `new()` constraint (§14.1, "
                            "#992)");
                    }
                }
                // per §15.4 / §22.7 (#599, #2725): a collection literal `[..]`
                // in a `new` argument position is a compile error when the
                // constructor parameter is not a collection type.  The
                // stdlib builtin collection types (Array<T>, Stack<T>, etc.)
                // are represented as Named TypeRefs with a null declaration
                // and expose only constructors whose parameter shapes do NOT
                // admit a collection literal — §15.4 requires a
                // collection-typed target context for `[..]`, and a `new`
                // argument position supplies no target.  Codegen silently
                // ignores all `new Array<T>(...)` arguments and always emits
                // an empty-capacity allocation, so the misuse produces a
                // silently empty collection (#599).  Detect and reject here
                // before codegen sees the node.
                //
                // fix #2725: the rationale was previously one string for the
                // whole branch ("the constructor takes an optional `int`
                // capacity"), which is false for every type in the branch
                // except the capacity-bearing ones — Stack/OwningStack/Queue/
                // OwningQueue/OwningLinkedList/Shared/Weak take no `int
                // capacity` form at all; LinkedList takes `IEnumerable<T>`,
                // not `int`; StringBuilder also takes `(initial: string)`,
                // which the message hid.  A diagnostic that names a
                // constructor the type does not have is worse than one that
                // names none. The shape per type below mirrors the stdlib
                // declarations (v0.282: §4.4 Array, §4.5 OwningArray, §4.10
                // Set, §4.10 OwningSet, §4.8 Dictionary, §4.8 OwningDictionary,
                // §4.6 LinkedList, §4.7 OwningLinkedList, §4.11 Stack/Queue,
                // §3.1 StringBuilder, §2.16 Shared/Weak).
                if (rt->kind == TypeRef::Kind::Named && !rt->declaration &&
                    rt->astNode && rt->astNode->name &&
                    !rt->astNode->name->parts.empty()) {
                    std::string_view colName = rt->headName();
                    bool isBuiltinColl =
                        colName == "Array"     || colName == "OwningArray"  ||
                        colName == "Stack"     || colName == "OwningStack"  ||
                        colName == "Queue"     || colName == "OwningQueue"  ||
                        colName == "Set"       || colName == "OwningSet"    ||
                        colName == "LinkedList"|| colName == "OwningLinkedList" ||
                        colName == "Dictionary"|| colName == "OwningDictionary" ||
                        colName == "StringBuilder"  ||
                        // §28.13 / stdlib §2.16: Shared<T>/Weak<T> constructors
                        // take an owned value, not a collection literal.
                        colName == "Shared"    || colName == "Weak";
                    if (isBuiltinColl) {
                        // Per-type constructor-shape rationale (#2725). One
                        // string per stdlib type, paired with the matching
                        // declaration in `docs/Bezel_Standard_Library.md`.
                        // The WHY-REJECTED half (`§15.4 requires a
                        // collection-typed target`) and the remedy (bind to a
                        // named typed binding first) are the same for every
                        // type — only the WHAT-IS-TAKEN half varies.
                        std::string_view rationale;
                        if (colName == "Array") {
                            rationale = "takes an optional `int` capacity "
                                        "(or a `(size: int, fill: T)` sized-fill pair)";
                        } else if (colName == "OwningArray") {
                            rationale = "takes an optional `int` capacity";
                        } else if (colName == "Stack" || colName == "OwningStack" ||
                                   colName == "Queue" || colName == "OwningQueue" ||
                                   colName == "OwningLinkedList") {
                            rationale = "takes no arguments";
                        } else if (colName == "Set" || colName == "OwningSet" ||
                                   colName == "Dictionary" ||
                                   colName == "OwningDictionary") {
                            rationale = "takes an optional `int` capacity "
                                        "(and/or an `IEqualityComparer<K>`)";
                        } else if (colName == "LinkedList") {
                            // §15.4: a collection literal has no standalone
                            // type, so it cannot be passed where the expected
                            // type is `IEnumerable<T>` either — the literal
                            // needs a target-typed position. Bind the literal
                            // to a typed binding first, then transfer through
                            // `Add`/`AddLast`/etc., or pass an Array/span.
                            rationale = "takes an `IEnumerable<T>` source "
                                        "(a collection literal has no standalone "
                                        "type to coerce to `IEnumerable<T>` — "
                                        "bind the literal to a typed binding "
                                        "first and transfer through `AddLast`/"
                                        "`AddFirst`/etc., or pass an Array/span)";
                        } else if (colName == "StringBuilder") {
                            rationale = "takes an `int` capacity or a `string` "
                                        "initial value";
                        } else if (colName == "Shared") {
                            // stdlib §2.16: `constructor(owned value: T)`.
                            rationale = "takes a single `owned` value of `T`";
                        } else {  // "Weak"
                            // stdlib §2.16 declares no constructors on Weak<T>;
                            // `new Weak<T>(...)` is already rejected upstream
                            // by constructor-lookup, but if the lookup ever
                            // relaxes this branch is the safety net.
                            rationale = "declares no constructors and cannot be "
                                        "constructed with `new`";
                        }
                        for (const auto& a : ne->args) {
                            if (a.value && dynamic_cast<const ast::CollectionLiteralExpr*>(
                                               a.value.get())) {
                                // Report on the literal's position.
                                ctx_.diagnostics().report(
                                    Severity::Error, a.value->span.begin,
                                    std::string("collection literal `[..]` cannot be "
                                        "passed as an argument to `new ") +
                                    std::string(colName) +
                                    "<...>` — the constructor " +
                                    std::string(rationale) +
                                    ", not a collection literal; bind the literal "
                                    "to a named typed binding first (`var xs: " +
                                    std::string(colName) +
                                    "<T> := [...]`) and transfer its elements "
                                    "through `Add`/`AddLast`/etc. "
                                    "(\xC2\xA7""15.4, #2725)");
                            }
                        }
                    }
                }
                // stdlib §4.4 v0.165 (#1375): the sized-fill constructor
                // `Array<T>(size: int, fill: T)` requires `fill` to be
                // assignable to the element type T (and `size` to be int-like).
                // Codegen fills each slot with `fill` via a raw element-width
                // store, so a type-mismatched fill (`new Array<int>(4, "x")`)
                // would silently write a wrong-width value into every slot —
                // reject it here, matching every other typed constructor arg.
                if (rt->kind == TypeRef::Kind::Named && !rt->declaration &&
                    rt->astNode && rt->astNode->name &&
                    !rt->astNode->name->parts.empty() &&
                    rt->headName() == "Array" &&
                    ne->args.size() == 2 &&
                    ne->args[0].value && ne->args[1].value &&
                    !rt->genericArgs.empty() && rt->genericArgs[0]) {
                    // size (arg 0): must be int-like.
                    checkAssignable(
                        ctx_.exprType(*ne->args[0].value),
                        ctx_.primitiveTypeRef(PrimitiveType::Int),
                        ne->args[0].value->span.begin,
                        "size argument of the sized `Array<T>` constructor",
                        ne->args[0].value.get());
                    // fill (arg 1): must be assignable to the element type T.
                    // §4.2 typed-context narrowing: a bare in-range int literal
                    // fill bound to a narrower integer element type is admitted at
                    // that type (the doc's canonical read-loop is
                    // `new Array<byte>(64 * 1024, 0)`, §7.1) — retype the literal
                    // before the assignability check, mirroring the record-arg and
                    // local-decl narrowing paths.
                    {
                        TypeRef* fillTy = ctx_.exprType(*ne->args[1].value);
                        TypeRef* elemTy = rt->genericArgs[0];
                        if (fillTy && elemTy &&
                            fillTy->kind == TypeRef::Kind::Primitive &&
                            fillTy->primitive == PrimitiveType::Int &&
                            elemTy->kind == TypeRef::Kind::Primitive) {
                            PrimitiveType tgt = elemTy->primitive;
                            bool narrower =
                                tgt == PrimitiveType::Byte ||
                                tgt == PrimitiveType::SByte ||
                                tgt == PrimitiveType::Short ||
                                tgt == PrimitiveType::UShort ||
                                tgt == PrimitiveType::UInt ||
                                tgt == PrimitiveType::Long ||
                                tgt == PrimitiveType::ULong;
                            if (narrower) {
                                auto cv = foldIntConstExprTC(*ne->args[1].value);
                                auto fits = [](long long v, PrimitiveType pt) {
                                    switch (pt) {
                                        case PrimitiveType::SByte:
                                            return v >= -128 && v <= 127;
                                        case PrimitiveType::Byte:
                                            return v >= 0 && v <= 255;
                                        case PrimitiveType::Short:
                                            return v >= -32768 && v <= 32767;
                                        case PrimitiveType::UShort:
                                            return v >= 0 && v <= 65535;
                                        case PrimitiveType::UInt:
                                            return v >= 0 && v <= 4294967295LL;
                                        case PrimitiveType::ULong:
                                            return v >= 0;
                                        default:
                                            return true;  // Long fits any int
                                    }
                                };
                                if (cv && fits(*cv, tgt))
                                    ctx_.setExprType(ne->args[1].value.get(),
                                                     elemTy);
                            }
                        }
                    }
                    checkAssignable(
                        ctx_.exprType(*ne->args[1].value),
                        rt->genericArgs[0],
                        ne->args[1].value->span.begin,
                        "fill value of the sized `Array<T>` constructor",
                        ne->args[1].value.get());
                }
                // §28.13 / stdlib §2.16 (#603): Shared<T> / Weak<T> require T to
                // be a proper class type.  Runtime-managed (string, function),
                // value (int/record/enum), and interface types have no disposal
                // obligation to share and are rejected at instantiation, mirroring
                // the §14.4 / OwningArray<T: class> constraint.
                if (rt->kind == TypeRef::Kind::Named && !rt->declaration &&
                    rt->astNode && rt->astNode->name &&
                    !rt->astNode->name->parts.empty()) {
                    std::string_view shName = rt->headName();
                    if (shName == "Shared" || shName == "Weak") {
                        // Check the first generic argument for a proper class type.
                        bool argIsClass = false;
                        std::string badTypeName;
                        if (!rt->genericArgs.empty() && rt->genericArgs[0]) {
                            const TypeRef* ta = rt->genericArgs[0];
                            if (ta->kind == TypeRef::Kind::Named &&
                                ta->declaration &&
                                ta->declaration->kind == ast::TypeDeclKind::Class) {
                                argIsClass = true;
                            } else if (ta->kind == TypeRef::Kind::Named &&
                                       !ta->declaration &&
                                       ta->astNode && ta->astNode->name &&
                                       !ta->astNode->name->parts.empty()) {
                                // §28.13 v0.140 / #689: compiler-known stdlib class types
                                // (Mutex, Atomic<T>, AtomicInteger<T>, Semaphore, Channel<T>,
                                // ChannelSender<T>, and other compiler-known classes like Lock,
                                // OwningArray<T>, etc.) have no user TypeDecl but are classes.
                                // Accept all builtin named types here; the Shared<T> sendability
                                // rule in isSendable (ExprTypecheck_Calls.cpp) further restricts
                                // which of these are sendable across domains.
                                std::string_view taname = ta->headName();
                                // The full set of compiler-known class types (not value types,
                                // not records/enums/interfaces): Mutex, Lock, Atomic<T>,
                                // AtomicInteger<T>, Semaphore, Channel<T>, ChannelSender<T>,
                                // Task, OwningTask, OwningArray, OwningStack, OwningQueue,
                                // OwningSet, OwningLinkedList, OwningDictionary, Array, Stack,
                                // Queue, Set, LinkedList, Dictionary, EventSource,
                                // AsyncEventSource, Shared, Weak.
                                // Excluded: string (primitive), Object (primitive), int etc.
                                // For simplicity: if the type arg is a Named builtin (no decl)
                                // and is not a primitive, treat it as a class type.
                                // The "T must be a class" guard's purpose is to exclude
                                // primitives, records, enums, interfaces, and functions —
                                // all of which either have no heap-allocated class disposal
                                // obligation or are represented differently.
                                // Builtin named types without a user decl are all class-like
                                // (heap-allocated, runtime-managed, disposable).
                                argIsClass = true;
                            } else {
                                // Produce a human-readable type name for the diagnostic.
                                if (ta->kind == TypeRef::Kind::Primitive) {
                                    // Map PrimitiveType → name for the diagnostic.
                                    using P = sema::PrimitiveType;
                                    switch (ta->primitive) {
                                        case P::Bool:    badTypeName = "bool"; break;
                                        case P::Char:    badTypeName = "char"; break;
                                        case P::String:  badTypeName = "string"; break;
                                        case P::Byte:    badTypeName = "byte"; break;
                                        case P::SByte:   badTypeName = "sbyte"; break;
                                        case P::Short:   badTypeName = "short"; break;
                                        case P::UShort:  badTypeName = "ushort"; break;
                                        case P::Int:     badTypeName = "int"; break;
                                        case P::UInt:    badTypeName = "uint"; break;
                                        case P::Long:    badTypeName = "long"; break;
                                        case P::ULong:   badTypeName = "ulong"; break;
                                        case P::Float:   badTypeName = "float"; break;
                                        case P::Double:  badTypeName = "double"; break;
                                        case P::Decimal: badTypeName = "decimal"; break;
                                        case P::Object:  badTypeName = "Object"; break;
                                        default:         badTypeName = "primitive type"; break;
                                    }
                                }
                                else if (ta->kind == TypeRef::Kind::Function)
                                    badTypeName = "function type";
                                else if (ta->kind == TypeRef::Kind::Named) {
                                    if (ta->declaration) {
                                        badTypeName = std::string(ta->declaration->name);
                                        if (ta->declaration->kind ==
                                            ast::TypeDeclKind::Interface)
                                            badTypeName += " (interface)";
                                        else if (ta->declaration->kind ==
                                                 ast::TypeDeclKind::Record)
                                            badTypeName += " (record/value type)";
                                        else if (ta->declaration->kind ==
                                                 ast::TypeDeclKind::Enum)
                                            badTypeName += " (enum/value type)";
                                    } else if (ta->astNode && ta->astNode->name &&
                                               !ta->astNode->name->parts.empty()) {
                                        badTypeName = std::string(
                                            ta->astNode->name->parts.back());
                                        badTypeName += " (runtime-managed or value type)";
                                    }
                                }
                                if (badTypeName.empty())
                                    badTypeName = "<unknown>";
                            }
                        } else {
                            badTypeName = "<no type argument>";
                        }
                        if (!argIsClass && !badTypeName.empty()) {
                            ctx_.diagnostics().report(
                                Severity::Error, ne->span.begin,
                                std::string(shName) +
                                "<T> requires T to be a proper class type: "
                                "`" + badTypeName + "` is not a class — "
                                "runtime-managed and value types have no "
                                "disposal obligation to share "
                                "(\xC2\xA7""28.13 / stdlib \xC2\xA7""2.16, #603)");
                        }
                    }
                }
                // §30.14 / §30.15 (#600): Channel<T> and ChannelSender<T>
                // instantiation — element type T must be sendable per §3.6.
                // Checked at instantiation so the error is at the `new` site.
                if (rt->kind == TypeRef::Kind::Named && !rt->declaration &&
                    rt->astNode && rt->astNode->name &&
                    !rt->astNode->name->parts.empty()) {
                    std::string_view chName = rt->headName();
                    if (chName == "Channel" || chName == "ChannelSender") {
                        if (rt->genericArgs.empty() || !rt->genericArgs[0]) {
                            ctx_.diagnostics().report(
                                Severity::Error, ne->span.begin,
                                std::string(chName) +
                                "<T> requires a type argument (§30.15, #600)");
                        } else {
                            const TypeRef* elemTy = rt->genericArgs[0];
                            if (!isSendable(elemTy)) {
                                // Produce a human-readable name for the type arg.
                                std::string tName;
                                if (elemTy->kind == TypeRef::Kind::Named) {
                                    if (elemTy->declaration)
                                        tName = std::string(elemTy->declaration->name);
                                    else if (elemTy->astNode && elemTy->astNode->name &&
                                             !elemTy->astNode->name->parts.empty())
                                        tName = std::string(elemTy->headName());
                                } else if (elemTy->kind == TypeRef::Kind::Primitive) {
                                    using P = sema::PrimitiveType;
                                    switch (elemTy->primitive) {
                                        case P::Bool:    tName = "bool"; break;
                                        case P::Int:     tName = "int"; break;
                                        case P::String:  tName = "string"; break;
                                        default:         tName = "primitive type"; break;
                                    }
                                } else if (elemTy->kind == TypeRef::Kind::Function) {
                                    tName = "function type";
                                } else if (elemTy->kind == TypeRef::Kind::InlineArray) {
                                    tName = elemTy->arrayOwned ? "[N owned]T" : "[N]T";
                                }
                                if (tName.empty()) tName = "<type>";
                                ctx_.diagnostics().report(
                                    Severity::Error, ne->span.begin,
                                    std::string("type argument `") + tName +
                                    "` of `" + std::string(chName) + "<T>` is not "
                                    "sendable: the element type must satisfy the §30.14 "
                                    "sendable-type rules (not borrow-bearing, not "
                                    "move-only, not function-typed, not domain-local; "
                                    "class types must be transfer-closed/sealed) "
                                    "(\xC2\xA7""30.14/\xC2\xA7""30.15, #600)");
                            }
                        }
                    }
                }
                // §6.2 / issue #647: `new Lock()` is a compile error.
                // Lock has NO public constructor — instances are obtained only
                // from Mutex.Acquire() / TryAcquire().  Reject `new Lock(...)` here.
                if (rt->kind == TypeRef::Kind::Named && !rt->declaration &&
                    rt->astNode && rt->astNode->name &&
                    !rt->astNode->name->parts.empty() &&
                    rt->headName() == "Lock") {
                    ctx_.diagnostics().report(
                        Severity::Error, ne->span.begin,
                        "`Lock` has no public constructor: obtain a Lock via "
                        "`mutex.Acquire()` or `mutex.TryAcquire(timeout)` "
                        "(stdlib \xC2\xA7""6.2, #647)");
                }
                // §6.3 / issue #647: `new SemaphoreToken()` is a compile error.
                // SemaphoreToken has NO public constructor — instances are obtained
                // only from Semaphore.Acquire() / TryAcquire().
                if (rt->kind == TypeRef::Kind::Named && !rt->declaration &&
                    rt->astNode && rt->astNode->name &&
                    !rt->astNode->name->parts.empty() &&
                    rt->headName() == "SemaphoreToken") {
                    ctx_.diagnostics().report(
                        Severity::Error, ne->span.begin,
                        "`SemaphoreToken` has no public constructor: obtain a "
                        "SemaphoreToken via `semaphore.Acquire()` or "
                        "`semaphore.TryAcquire(timeout)` "
                        "(stdlib \xC2\xA7""6.3, #647)");
                }
                // §6.4 / issue #647: `new Atomic<T>(initial)` — T-restriction check.
                // Atomic<T> requires T to be a cell-sized primitive type per §6.4:
                // {bool, char, byte, sbyte, short, ushort, int, uint, long, ulong,
                //  IntPtr, UIntPtr}.  Any other type argument is a compile error.
                // Verbatim diagnostic from §6.4: "Atomic<T> requires a cell-sized
                // primitive type; T is not eligible. For floating-point atomics,
                // use Atomic<int> with a bit-cast; for record-typed atomics, use
                // a Mutex."
                if (rt->kind == TypeRef::Kind::Named && !rt->declaration &&
                    rt->astNode && rt->astNode->name &&
                    !rt->astNode->name->parts.empty() &&
                    rt->headName() == "Atomic") {
                    bool tOk = false;
                    if (!rt->genericArgs.empty() && rt->genericArgs[0]) {
                        const TypeRef* ta = rt->genericArgs[0];
                        if (ta->kind == TypeRef::Kind::Primitive) {
                            using P = sema::PrimitiveType;
                            auto p = ta->primitive;
                            tOk = (p == P::Bool || p == P::Char ||
                                   p == P::Byte || p == P::SByte ||
                                   p == P::Short || p == P::UShort ||
                                   p == P::Int || p == P::UInt ||
                                   p == P::Long || p == P::ULong);
                        } else if (ta->kind == TypeRef::Kind::Named &&
                                   !ta->declaration && ta->astNode &&
                                   ta->astNode->name &&
                                   !ta->astNode->name->parts.empty()) {
                            // IntPtr and UIntPtr are registered as nullary builtin
                            // Named types (not PrimitiveType); accept them here.
                            auto tn = ta->headName();
                            tOk = (tn == "IntPtr" || tn == "UIntPtr");
                        }
                    } else {
                        // No type argument provided — sema will report "wrong arity";
                        // avoid a second diagnostic.
                        tOk = true;
                    }
                    if (!tOk) {
                        ctx_.diagnostics().report(
                            Severity::Error, ne->span.begin,
                            "Atomic<T> requires a cell-sized primitive type; T is "
                            "not eligible. For floating-point atomics, use "
                            "Atomic<int> with a bit-cast; for record-typed atomics, "
                            "use a Mutex. (stdlib \xC2\xA7""6.4, #647)");
                    }
                }
                // §6.4 / issue #647: `new AtomicInteger<T>(initial)` — T-restriction.
                // AtomicInteger<T> requires T to be a primitive integer type:
                // {byte, sbyte, short, ushort, int, uint, long, ulong}.
                // bool, char, IntPtr, UIntPtr are NOT eligible (use Atomic<T>
                // and CompareExchange directly per §6.4).
                // Diagnostic NOT verbatim (§6.4 has none for AtomicInteger);
                // wording mirrors §6.4 §AtomicInteger restriction prose.
                if (rt->kind == TypeRef::Kind::Named && !rt->declaration &&
                    rt->astNode && rt->astNode->name &&
                    !rt->astNode->name->parts.empty() &&
                    rt->headName() == "AtomicInteger") {
                    bool tOk = false;
                    if (!rt->genericArgs.empty() && rt->genericArgs[0]) {
                        const TypeRef* ta = rt->genericArgs[0];
                        if (ta->kind == TypeRef::Kind::Primitive) {
                            using P = sema::PrimitiveType;
                            auto p = ta->primitive;
                            tOk = (p == P::Byte || p == P::SByte ||
                                   p == P::Short || p == P::UShort ||
                                   p == P::Int || p == P::UInt ||
                                   p == P::Long || p == P::ULong);
                        }
                        // bool, char, IntPtr, UIntPtr are NOT eligible.
                    } else {
                        tOk = true;  // arity error reported elsewhere
                    }
                    if (!tOk) {
                        ctx_.diagnostics().report(
                            Severity::Error, ne->span.begin,
                            "AtomicInteger<T> requires a primitive integer type "
                            "(byte, sbyte, short, ushort, int, uint, long, ulong); "
                            "T is not eligible. For bool, char, IntPtr, or UIntPtr "
                            "atomics, use Atomic<T> and CompareExchange directly. "
                            "(stdlib \xC2\xA7""6.4, #647)");
                    }
                }
                // §7 / stdlib §6.4 (#2618): `new Atomic<T>(initial)` /
                // `new AtomicInteger<T>(initial)` — check the ARGUMENT against
                // the declared NON-nullable `initial: T`.
                //
                // #2602 gated the atomic INSTANCE methods, but that gate lives
                // in checkBuiltinCollectionCallArgs and runs only on the
                // CallExpr path; a constructor is a NewExpr, whose only sema
                // check here was the T-restriction above. So an un-narrowed
                // `T?` initial passed sema entirely unchecked and reached
                // codegen, where it was silently discarded and the cell
                // initialized to 0 (#2618's own report says this). With the
                // #2618 codegen unwrap in place it would instead read the
                // payload out of a possibly-null box — either way a SILENT
                // WRONG VALUE. This closes the pair the way #2601/#2602 do:
                // sema REJECTS the un-narrowed form, codegen UNWRAPS the
                // narrowed one. checkAssignable is narrowing-aware (§7.4), so
                // a null-checked `T?` still compiles.
                //
                // SCOPE: this gate checks NULLABILITY ONLY — it fires solely
                // when the argument's own type is a NULLABLE primitive and T is
                // not — and it checks against the non-nullable form of THAT
                // type, not against T. It deliberately does NOT re-implement
                // constructor argument typing: a first draft compared the
                // argument to T directly and false-rejected the legal
                // `new Atomic<byte>(0)`, because the literal `0` is typed `int`
                // and the int->byte constant conversion is not a checkAssignable
                // conversion. A false reject breaks working code and is worse
                // than the bug being closed, so the check is confined to the one
                // question this issue is about: did a possibly-null value reach
                // a non-nullable slot? Everything else about the argument keeps
                // its existing (unchecked) behavior — widening, literal
                // narrowing, and the T-restriction above are untouched.
                if (rt->kind == TypeRef::Kind::Named && !rt->declaration &&
                    rt->astNode && rt->astNode->name &&
                    !rt->astNode->name->parts.empty() &&
                    (rt->headName() == "Atomic" ||
                     rt->headName() == "AtomicInteger") &&
                    !rt->genericArgs.empty() && rt->genericArgs[0] &&
                    !rt->genericArgs[0]->nullable &&
                    ne->args.size() == 1 && ne->args[0].value &&
                    !ne->args[0].isDiscard && ne->args[0].modifier.empty()) {
                    TypeRef* argTy = ctx_.exprType(*ne->args[0].value);
                    if (argTy && argTy->kind == TypeRef::Kind::Primitive &&
                        argTy->nullable) {
                        // checkAssignable is narrowing-aware (§7.4 / #1191), so
                        // a null-checked `T?` still compiles and only the
                        // genuinely un-narrowed argument is reported.
                        TypeRef* nonNull = ctx_.primitiveTypeRef(
                            argTy->primitive, /*nullable=*/false);
                        checkAssignable(
                            argTy, nonNull, ne->args[0].value->span.begin,
                            std::string("argument to `") +
                                std::string(rt->headName()) + "` constructor",
                            ne->args[0].value.get());
                    }
                }
                // issue #545: `new Buffer<T>(n)` — T-restriction check.
                // Buffer<T> requires T to be a value-type primitive in this revision:
                // {bool, char, byte, sbyte, short, ushort, int, uint, long, ulong,
                //  float, double, decimal}.  Class, record, string, and interface
                // elements re-open the §5.20 borrow-storing / zeroed-slot-disposal
                // questions; they are explicitly out of scope for this slice.
                // Diagnostic: "Buffer<T> requires a value-type element in this
                // revision; class elements await the §5.20 design (#545)."
                if (rt->kind == TypeRef::Kind::Named && !rt->declaration &&
                    rt->astNode && rt->astNode->name &&
                    !rt->astNode->name->parts.empty() &&
                    rt->headName() == "Buffer") {
                    bool tOk = false;
                    if (!rt->genericArgs.empty() && rt->genericArgs[0]) {
                        const TypeRef* ta = rt->genericArgs[0];
                        // Accept only primitive value types.
                        tOk = (ta->kind == TypeRef::Kind::Primitive);
                        // string is PrimitiveType::String — REJECT (runtime-managed).
                        if (tOk && ta->primitive == sema::PrimitiveType::String)
                            tOk = false;
                    } else {
                        // No type arg — arity error reported elsewhere; suppress
                        // the redundant restriction diagnostic.
                        tOk = true;
                    }
                    if (!tOk) {
                        ctx_.diagnostics().report(
                            Severity::Error, ne->span.begin,
                            "Buffer<T> requires a value-type element in this "
                            "revision; class elements await the \xC2\xA7""5.20 "
                            "design (#545)");
                    }
                }
                // §2.15 / issue #631: `new BinaryDeserializer(arg)` requires
                // `arg` to be a `ReadOnlySpan<byte>` (the result of calling
                // `.AsReadOnlySpan()` on an `OwningBuffer<byte>`). Passing the
                // owned buffer directly (`new BinaryDeserializer(bytes)` where
                // `bytes: OwningBuffer<byte>`) is a type error — the constructor
                // signature is `BinaryDeserializer(input: ReadOnlySpan<byte>)`
                // (stdlib §2.15). Without this check the wrong-arg form compiled
                // silently and segfaulted at runtime.
                if (rt->kind == TypeRef::Kind::Named && !rt->declaration &&
                    rt->astNode && rt->astNode->name &&
                    !rt->astNode->name->parts.empty() &&
                    rt->headName() == "BinaryDeserializer" &&
                    !ne->args.empty() && ne->args[0].value) {
                    const TypeRef* arg0 = ctx_.exprType(*ne->args[0].value);
                    if (arg0 && arg0->kind == TypeRef::Kind::Named &&
                        !arg0->declaration && arg0->astNode &&
                        arg0->astNode->name &&
                        !arg0->astNode->name->parts.empty() &&
                        arg0->headName() == "OwningBuffer") {
                        ctx_.diagnostics().report(
                            Severity::Error, ne->args[0].value->span.begin,
                            "`new BinaryDeserializer(...)` expects "
                            "`ReadOnlySpan<byte>` as its first argument "
                            "(stdlib \xC2\xA7""2.15); got `OwningBuffer<byte>` — "
                            "call `.AsReadOnlySpan()` first "
                            "(\xC2\xA7""2.15, #631)");
                    }
                }
                // Slice S11: forbid `new` on abstract or static
                // class types. Interfaces and (purely declarative)
                // static-only types are also non-instantiable.
                //
                // fix #4176 (v0.298): the four-way abstract/closed/static/
                // interface classification below now lives in
                // `rejectNewOnNonInstantiableKind` (defined above, beside
                // `rejectNewOnEnumOrChoice`) so the two §17.7 shorthand
                // call sites in ExprTypecheck.cpp can share it instead of
                // leaving it unreachable from the shorthand path entirely.
                // Diagnostic text/citations are unchanged — pure factoring.
                if (rt->kind == TypeRef::Kind::Named && rt->declaration) {
                    TypeSymbol* t = rt->declaration;
                    if (!rejectNewOnNonInstantiableKind(*ne, rt)) {
                        if (t->kind == ast::TypeDeclKind::Class) {
                            // Slice T7: constructor arity / arg types.
                            checkNewExprArgs(*ne, t);
                        } else if (t->kind == ast::TypeDeclKind::Record) {
                            // #1105 (§9.1): validate a POSITIONAL record's primary-
                            // constructor `new` args (count + per-field type). Record
                            // construction previously skipped all arg checking, so
                            // too-few / too-many / wrong-typed args were silently
                            // accepted and miscompiled. Only positional records are
                            // routed here; checkNewExprArgs validates the positional
                            // fields and returns (generic records defer to #428).
                            //
                            // fix #3141 (family F3 of umbrella #3152, §2.8.3 rule 4):
                            // this gate used to ask the record's `ast::TypeDecl`.
                            // `ModuleLoader` never assigns `Symbol::decl` for an
                            // imported type, so the gate was false for EVERY import
                            // and #1105's arg check — the one that stopped a
                            // wrong-typed positional argument from being stored into
                            // a differently-sized field — never ran across a module
                            // boundary. `hasPositionalFields` is mirrored onto the
                            // TypeSymbol on both sides (#501), so read it there.
                            //
                            // fix #4059 (§9.1 / §9.2 / §22.7, v0.295): the
                            // pre-existing `hasPositionalFields` gate ALSO skipped
                            // any arg check for a body-only record (positional-field
                            // count is zero), where §9.1 has nothing to validate
                            // and §9.2 declares no implicit default ctor — until
                            // #4059 fix's `ctors.empty()` arm rejects the silent
                            // zero-fill (`new Foo()` type-checked clean and minted
                            // a 0-valued record; `return f.Bar;` returned 0). The
                            // fix opens this gate so every record — positional,
                            // body-only, or combined-form — runs through
                            // `checkNewExprArgs`. The function already has the
                            // right arms: `t->hasPositionalFields` routes the
                            // positional-record binder; the ctors-collection arm
                            // handles body records with explicit ctors; and the
                            // new body-record reject (added in the same fix's
                            // parallel edit to `checkNewExprArgs`) closes the
                            // zero-fill hole for body records with no ctors.
                            checkNewExprArgs(*ne, t);
                        }
                    }
                    // fix #3719 (§17.11 / §3.6 rule 2, v0.242): this
                    // construction IS the instantiation site (§3.6 rule 2)
                    // for any class-level or extension-level `default(T)`
                    // obligation recorded on one of `t`'s own generic
                    // parameters — see
                    // recordDefaultObligationsForConstruction's own comment
                    // for why neither half was ever checked before this fix.
                    // Applies to both Class and Record (both take type
                    // arguments here); Interface is never constructed with
                    // `new` (rejected above), and an abstract/static/closed
                    // class's own rejection above does not need this
                    // recording to also run. It is harmless when it does:
                    // measured at this fix's review, an `abstract class
                    // Box<K> where K: value` illegally `new`'d with a
                    // non-defaultable K emits BOTH the abstract-instantiation
                    // rejection AND this constraint-violation diagnostic.
                    // Both are true and the program is rejected either way,
                    // so the redundancy is cosmetic — it is NOT the case
                    // (as an earlier draft of this comment claimed) that a
                    // rejected-as-non-constructible decl can never also be a
                    // `default(T)` join key.
                    if (t->kind == ast::TypeDeclKind::Class ||
                        t->kind == ast::TypeDeclKind::Record) {
                        recordDefaultObligationsForConstruction(*ne, t, rt);
                    }
                    // §14.1 / §2.5 (#428 phase B): imported generic RECORD
                    // construction is now supported — codegen performs per-
                    // instantiation specialization in the importing module using
                    // the same local recordStruct path, with the substitution
                    // built from TypeSymbol::genericParams (populated by the
                    // ModuleLoader).  The prior §14 guard that rejected this
                    // case is removed here; the residual gate for imported
                    // generic CLASS is still in checkImportedNewExprArgs.
                }
            }
        }
    }
    // §17.10: typeof(T) → Type.  The `Type` sealed class is defined in the
    // standard library (stdlib §2.13) and is a builtin Named type (arity 0,
    // no user TypeDecl). The sema type of a TypeOfExpr is a Named TypeRef
    // backed by a builtinClassSym("Type") declaration. This prevents codegen
    // from seeing an untyped expression and emitting silent garbage (#487).
    //
    // #511: emit the "not yet implemented" diagnostic HERE in sema so that
    // chained uses such as `typeof(T).Name` fire the error unconditionally
    // regardless of context.  The expression is still TYPED as `Type` so that
    // no secondary "untyped expression" cascades are emitted and the
    // future-implementation path (System.Reflection §11) is preserved.
    if (auto* to = dynamic_cast<const ast::TypeOfExpr*>(&e)) {
        (void)to; // the queriedType is already resolved by TypeResolution
        auto* typeTy = ctx_.makeTypeRef();
        typeTy->kind = TypeRef::Kind::Named;
        typeTy->declaration = builtinClassSym("Type");
        ctx_.setExprType(&e, typeTy);
        ctx_.diagnostics().report(
            Severity::Error, e.span.begin,
            "reflection is not yet implemented: 'typeof' "
            "(§17.10, #487); deferred pending System.Reflection §11");
    }
    // §17.9: nameof(QualifiedName) → string. The result is the rightmost
    // identifier of the operand, as a compile-time string constant. No
    // runtime resolution is performed; sema just types the expression as
    // string (codegen emits the name text as a string literal global).
    if (auto* no = dynamic_cast<const ast::NameOfExpr*>(&e)) {
        (void)no; // name is resolved at codegen from the AST node
        ctx_.setExprType(&e, ctx_.primitiveTypeRef(PrimitiveType::String));
    }
    if (auto* de = dynamic_cast<const ast::DefaultExpr*>(&e)) {
        if (de->type) {
            if (auto* rt = ctx_.resolvedType(*de->type)) {
                ctx_.setExprType(&e, rt);
                // §17.11 (#951): `default(T)` is a compile error when T has no
                // well-defined zero value — a non-nullable REFERENCE type
                // (string/class/interface/function), or recursively a record
                // with a non-nullable-ref field or an inline array with
                // non-nullable-ref elements. The null such a default would
                // produce violates the non-null contract (§7.9, the same defect
                // the type system prevents for an uninitialized non-nullable
                // reference field). `default(T?)` (nullable) and value types
                // (value-builtins/enums/records of value fields) stay accepted.
                // (The generic-parameter constrained case is the #864 check
                // below; defaultIllFormed returns false for a bare type param.)
                // fix #3044 (§17.11, v0.242): the enum arm gets its OWN
                // diagnostic — its remedy is "declaring a zero-valued
                // constant", not the `default(T?)` the shared reference-type
                // message prescribes.
                if (defaultIllFormed(rt) && rt->kind == TypeRef::Kind::Named &&
                    rt->declaration &&
                    rt->declaration->kind == ast::TypeDeclKind::Enum) {
                    ctx_.diagnostics().report(
                        Severity::Error, de->span.begin,
                        "`default(T)` is a compile error for enum `" +
                        std::string(rt->declaration->name) +
                        "` — it declares no constant of underlying value 0, so "
                        "it has no well-defined zero value; declare a "
                        "zero-valued constant (§17.11)");
                } else if (defaultIllFormed(rt) && rt->kind == TypeRef::Kind::Named &&
                           rt->declaration == ctx_.builtinClassSym("Range")) {
                    // fix #3108 (§16.3/§16.4, v0.242): `default(Range<T>)` is a
                    // compile error — the all-zeroes `Range<T>` would have step=0,
                    // falsifying §16.4's "no `Range<T>` value with a zero step can
                    // exist" invariant. The empty range is spelled `0..<0`; the
                    // closed-constructor-surface rule (§17.7) bars `new Range<T>`.
                    ctx_.diagnostics().report(
                        Severity::Error, de->span.begin,
                        std::string("`default(Range<T>)` is a compile error — "
                            "the all-zeroes value would have step=0, falsifying "
                            "§16.4's no-zero-step invariant; the empty range is "
                            "spelled `0..<0` (§17.11)"));
                } else if (defaultIllFormed(rt)) {
                    std::string detail;
                    if (rt->kind == TypeRef::Kind::Named && rt->declaration &&
                        rt->declaration->kind == ast::TypeDeclKind::Record)
                        detail = "a record any of whose fields is a "
                                 "non-nullable reference type";
                    else if (rt->kind == TypeRef::Kind::Named &&
                             rt->declaration &&
                             rt->declaration->kind ==
                                 ast::TypeDeclKind::Choice)
                        // §35.10 (v0.200, #2044): a sum has no principled
                        // zero. The remedy is `default(C?)` (the nullable
                        // form) or an explicit case construction.
                        detail = "a choice type (no case is distinguished; "
                                 "§35.10)";
                    else if (rt->kind == TypeRef::Kind::InlineArray)
                        detail = "an inline array whose element type is a "
                                 "non-nullable reference type";
                    else
                        detail = "a non-nullable reference type (a class, "
                                 "interface, `string`, or function type)";
                    ctx_.diagnostics().report(
                        Severity::Error, de->span.begin,
                        "`default(T)` is a compile error for " + detail +
                        " — it has no well-defined zero value; write "
                        "`default(T?)` for the nullable counterpart where "
                        "applicable (§17.11)");
                }
                // §17.11 (#864 part 1): `default(T)` under `where T: class` or
                // a type-bound constraint is a compile error — the constraint
                // guarantees T is non-nullable, and `default(T)` would produce
                // null, which does not fit T's contract. `default(T?)` (nullable)
                // stays accepted; only the non-nullable form triggers this error.
                // `where T: value` and unconstrained T are OK (value types have
                // a well-defined zero; unconstrained T may be a value type).
                if (rt->kind == TypeRef::Kind::Named &&
                    rt->declaration == nullptr &&   // generic type parameter
                    !rt->nullable &&                // non-nullable `T`, not `T?`
                    de->type->name && !de->type->name->parts.empty()) {
                    std::string_view paramName = de->type->name->parts.front();
                    // Walk the enclosing method's constraints first, then the
                    // enclosing class's, then (fix #3644) the enclosing
                    // extension's — returning true iff it reported, so a
                    // later scope does not RE-REPORT the identical
                    // diagnostic once an earlier scope already found a
                    // matching non-null-kind constraint. REVIEW FINDING
                    // (#3644): the first cut of the extension arm below was
                    // gated only on `!isMethodParam`, with no tracking of
                    // whether an earlier arm already reported — measured to
                    // double-fire the identical `default(T)` diagnostic, same
                    // span, for `class Box<T> where T: class` extended by
                    // `extension Box<T> where T: class` (both the type arm
                    // and the extension arm matched the coincidentally
                    // same-named `T`). `checkGPs`, fixed in this same commit
                    // a few hundred lines up, already had the right shape
                    // (`!isTypeParam` gates its own extension arm); this
                    // function now mirrors it via a `bool` return instead of
                    // drifting from it.
                    auto checkConstraintsFor =
                        [&](const std::vector<ast::GenericConstraint>& gcs)
                        -> bool {
                        for (const auto& gc : gcs) {
                            if (gc.typeParam != paramName) continue;
                            for (const auto& item : gc.items) {
                                if (item.kind == "class" || item.kind == "type") {
                                    ctx_.diagnostics().report(
                                        Severity::Error, de->span.begin,
                                        "`default(T)` is a compile error when T "
                                        "is constrained non-null (`where " +
                                        std::string(paramName) + ": " +
                                        (item.kind == "class"
                                             ? "class"
                                             : (item.typeArg && item.typeArg->name &&
                                                !item.typeArg->name->parts.empty()
                                                    ? std::string(item.typeArg->name->parts.front())
                                                    : "type")) +
                                        "`) — the null value `default(T)` does not "
                                        "fit T's non-null contract (§17.11); "
                                        "write `default(T?)` to produce null of the "
                                        "nullable counterpart");
                                    return true;
                                }
                            }
                        }
                        return false;
                    };
                    // Check method-level constraints first; fall through to
                    // class-level constraints only when the name is not a
                    // method-level generic param (avoids false positives when
                    // the class has a same-named param with looser constraints).
                    bool isMethodParam = false;
                    bool constraintReported = false;
                    if (currentMethodDecl_) {
                        for (const auto& gp : currentMethodDecl_->genericParams)
                            if (gp.name == paramName) { isMethodParam = true; break; }
                        if (isMethodParam)
                            constraintReported = checkConstraintsFor(
                                currentMethodDecl_->genericConstraints);
                    }
                    if (!isMethodParam && !constraintReported && currentType_ &&
                        currentType_->decl)
                        if (auto* td = dynamic_cast<const ast::TypeDecl*>(
                                currentType_->decl))
                            constraintReported =
                                checkConstraintsFor(td->genericConstraints);
                    // fix #3644 (§17.11 / §26.4, v0.286): the extension arm
                    // was missing — the identical gap `forEachConstraintBound`
                    // (ExprTypecheck.cpp, #3643) had for the same "walk the
                    // enclosing scope's `where` clauses" question. An
                    // extension's own §26.4 type parameter's `where T: class`
                    // / `where T: <type>` constraint lives on the EXTENSION,
                    // not on `currentType_` (the extended type — read here
                    // only by name-coincidence, and not at all when the
                    // extended type is imported, since `currentType_->decl`
                    // is unconditionally null then). Gated on
                    // `!constraintReported` (REVIEW FINDING fix, #3644) so
                    // this arm runs only when NEITHER an earlier scope
                    // already reported — mirroring `forEachConstraintBound`'s
                    // own SHORT-CIRCUITING per-scope scan (method, then type,
                    // then extension, each tried only once the prior one did
                    // NOT already succeed, via its `return true` as soon as
                    // one scope's `scan` accepts). An earlier revision of
                    // this comment called that scan "unconditional", which
                    // was inaccurate and is corrected here:
                    // `forEachConstraintBound` stops at the first successful
                    // scope exactly like this fixed version now does — it
                    // does not scan every scope regardless of an earlier
                    // match, and neither does this one any more.
                    if (!isMethodParam && !constraintReported &&
                        currentExtension_ && currentExtension_->decl)
                        if (auto* ed = dynamic_cast<const ast::ExtensionDecl*>(
                                currentExtension_->decl))
                            checkConstraintsFor(ed->genericConstraints);
                    // fix #3044 (§17.11 / §3.6 rule 2, v0.242): this body takes
                    // `default(T)` of a bare type parameter, so an instantiation
                    // that supplies a type whose `default` is NOT well-defined
                    // must be rejected AT THE INSTANTIATION SITE. Which types
                    // those are is not knowable here; record the obligation and
                    // discharge it against each instantiation at the end of the
                    // pass. The owner is the METHOD when the parameter is
                    // method-level (its instantiation site is the call), else
                    // the enclosing type.
                    //
                    // Gated on `where T: value`, and deliberately no wider.
                    // §17.11's generic-body list makes a guarantee ONLY there —
                    // "Under `where T: value`, `default(T)` is always non-null"
                    // — and it is that guarantee a not-well-defined default
                    // contradicts. For an UNCONSTRAINED `T` the same list says
                    // "`default(T)` is permitted but its result is treated as
                    // possibly null", so `Make<string>()` over an unconstrained
                    // body is legal and must stay legal; rejecting it here would
                    // be over-broad. (`where T: class` is already an error at the
                    // `default(T)` itself, just above.)
                    auto paramHasValueConstraint =
                        [&](const std::vector<ast::GenericConstraint>& gcs) {
                        for (const auto& gc : gcs) {
                            if (gc.typeParam != paramName) continue;
                            for (const auto& item : gc.items)
                                if (item.kind == "value") return true;
                        }
                        return false;
                    };
                    if (isMethodParam) {
                        if (paramHasValueConstraint(
                                currentMethodDecl_->genericConstraints))
                            recordDefaultTypeParamUse(
                                currentMethodDecl_, paramName, de->span.begin);
                    } else {
                        // fix #3719 (§17.11 / §3.6 rule 2 / §26.4, v0.294):
                        // these two arms used to be `else if`-chained on
                        // `currentType_`'s mere TRUTHINESS, so — inside an
                        // extension body over a LOCAL target type (where
                        // `currentType_` is always non-null: `currentType_ =
                        // targetSym` alongside `currentExtension_ = extSym`
                        // when visiting an extension member, ExprTypecheck.cpp)
                        // — the class-level branch below unconditionally
                        // consumed the branch and the `currentExtension_` arm
                        // #3644 added was UNREACHABLE whenever `paramName` was
                        // NOT (by name-coincidence) also one of the class's
                        // own constrained parameters, which is the common
                        // case (an extension's own type-parameter name need
                        // not, and usually does not, match the class's).
                        // `checkConstraintsFor` a few lines up (also #3644)
                        // already has the correct shape — try BOTH scopes
                        // independently, each gated on its OWN constraint
                        // list actually naming `paramName`, not on which
                        // scope pointer happens to be non-null — mirrored
                        // here. Trying both is safe or the identical
                        // reason `checkConstraintsFor`'s double-report REVIEW
                        // FINDING isn't a problem here: unlike that function,
                        // this one does not itself emit a diagnostic — it only
                        // RECORDS a deferred obligation, and
                        // `recordDefaultTypeParamUse` already dedupes per
                        // (owner, param) pair. When a class-level and an
                        // extension-level parameter are genuinely
                        // name-coincident AND both declare `where <name>:
                        // value`, that is two independent, both-real
                        // obligations against two different owners — both
                        // SHOULD be recorded, each to be discharged against
                        // its own instantiation sites.
                        if (currentType_ && currentType_->decl)
                            if (auto* td = dynamic_cast<const ast::TypeDecl*>(
                                    currentType_->decl))
                                if (paramHasValueConstraint(
                                        td->genericConstraints))
                                    recordDefaultTypeParamUse(
                                        currentType_->decl, paramName,
                                        de->span.begin);
                        // fix #3644 (§17.11 / §3.6 rule 2 / §26.4, v0.286):
                        // the extension arm was missing — same gap as
                        // `checkConstraintsFor` a few lines up. An
                        // extension's own §26.4 `where T: value` obligation
                        // was never recorded (`currentType_` names the
                        // EXTENDED type, whose `decl` is unconditionally
                        // null for an imported extended type, and only a
                        // name-coincidence otherwise), so a generic
                        // extension method's `default(T)` under `where T:
                        // value` never got its instantiation-site obligation
                        // discharged — silently admitting a type argument
                        // whose `default` is NOT well-defined instead of
                        // rejecting it at the instantiation site (§17.11's
                        // closing rule). `currentExtension_->decl` (the
                        // ExtensionDecl AST node) is the owner key, mirroring
                        // `currentType_->decl` above.
                        if (currentExtension_ && currentExtension_->decl)
                            if (auto* ed = dynamic_cast<const ast::ExtensionDecl*>(
                                    currentExtension_->decl))
                                if (paramHasValueConstraint(ed->genericConstraints))
                                    recordDefaultTypeParamUse(
                                        currentExtension_->decl, paramName,
                                        de->span.begin);
                    }
                }
            }
        }
    }
    // Slice 3h: CallExpr return-type typing. When the callee
    // resolves to a MethodSymbol (via slice 3d's MemberAccessExpr
    // resolver or slice 3b's identifier resolver), the call's
    // exprType is the method's resolved return type. Overload
    // picking is not yet implemented — the first matching member
    // wins, which can give the wrong result when overloads have
    // distinct return types. Tightened with the overload-resolution
    // slice.
    if (auto* ce = dynamic_cast<const ast::CallExpr*>(&e)) {
        typeCall(*ce);
    }
}

// §17.1 result-type table for binary operators. Slice 3c covers the
// common cases with no operator-overload resolution: equality and
// relational ops produce bool; arithmetic / shift / bitwise produce
// the left operand's type (when the operands type-match); logical
// ops `and` / `or` produce bool; `??` produces the non-nullable
// result type of the LHS; range ops produce a Range value (slice 3c
// leaves Range as Error).
// §25.2 / §12.2 (v0.181, #1562): every `operator <token>` overload of the
// given arity visible on `ts` — its own declarations plus those inherited from
// its base classes. §25.2's resolution rule reaches inherited operators ("a
// derived-typed operand reaches its base's operators, as in C#"); no user-facing
// upcast exists that could otherwise expose them. §12.2's per-token no-hiding
// rule guarantees a single inheritance chain contributes at most one declaring
// type per token, so this walk yields no intra-chain duplicates for a
// well-formed program. `baseTypes` mixes the base class and interfaces; both are
// walked, but an interface's static-abstract operator (recurrence-parameter
// typed) never matches a concrete operand at the call site (operandMatchesParam
// discriminates by declaration), so including them is harmless.
std::vector<MethodSymbol*>
ExprTC::collectInheritedOperators(TypeSymbol* ts, std::string_view token,
                                  std::size_t arity) {
    std::vector<MethodSymbol*> out;
    if (!ts) return out;
    std::vector<TypeSymbol*> work{ts};
    std::unordered_set<TypeSymbol*> seen;
    while (!work.empty()) {
        TypeSymbol* cur = work.back();
        work.pop_back();
        if (!cur || !seen.insert(cur).second) continue;
        for (Symbol* m : cur->members) {
            auto* ms = dynamic_cast<MethodSymbol*>(m);
            if (ms && ms->isOperator && ms->operatorToken == token &&
                ms->params.size() == arity)
                out.push_back(ms);
        }
        for (TypeSymbol* b : cur->baseTypes) work.push_back(b);
    }
    return out;
}

// §25: the user-defined `static operator <op>` method that applies to a
// binary expression, if either operand's type (class or record) — OR a base
// class of that type (§25.2, v0.181, #1562) — declares one. `=`/`<>` on
// classes are excluded (they invoke Equals, §25.2).
MethodSymbol* ExprTC::findBinaryOperator(const ast::BinaryExpr& be) {
    // §22.6 / §25 (#1131): collect ALL binary-operator overloads of `be.op`
    // declared on the operand's type. A type may declare several overloads of the
    // same operator symbol (e.g. `operator +(V, int)` and `operator +(V, V)`);
    // returning only the FIRST and validating just that one made every later
    // overload unreachable (the matching overload was wrongly rejected). The
    // caller (`tryOperand`) picks the candidate whose parameters match the
    // operands in order.
    auto candidatesOn = [&](const ast::Expr* operand) -> std::vector<MethodSymbol*> {
        if (!operand) return {};
        TypeSymbol* ts = memberTargetType(operand);
        if (!ts) return {};
        // §25.2 (v0.181, #1562): the candidate set is the operator overloads
        // declared on the operand's type AND those inherited from its base
        // classes. §25: user-defined `operator =` and `<>` on class types take
        // precedence over built-in reference equality — only skip when no
        // user-defined operator exists (handled by falling through).
        return collectInheritedOperators(ts, be.op, 2);
    };
    // §25 (#1096): the operator's operands must match its declared parameters
    // IN ORDER (left → param 0, right → param 1). Bezel does not auto-commute
    // operator overloads — §25 defines no commutation, and silently swapping
    // would be unsound for non-commutative operators (`4 - v` ≠ `v - 4`). The
    // matched type is found on either operand, but the operand at each position
    // must be the type the corresponding parameter declares; otherwise the
    // operator does not apply. (Previously a reversed-operand match, e.g.
    // `4 * v` against `operator *(Vec, int)`, was accepted, and codegen emitted
    // the args in source order → an IR-verification crash.)
    auto operandMatchesParam = [&](const ast::Expr* operand,
                                   Symbol* paramSym) -> bool {
        auto* ps = dynamic_cast<ParameterSymbol*>(paramSym);
        if (!ps) return false;
        TypeRef* pt = ps->declType ? ctx_.resolvedType(*ps->declType)
                                   : ps->resolvedDeclType;
        TypeRef* at = operand ? ctx_.exprType(*operand) : nullptr;
        if (!pt || !at) return false;
        // A named-type (record/class/enum) parameter must receive an operand of
        // that exact declared type — the discriminating position for the
        // reversed-operand (commutation) case.
        // §7.11 (#1132): user-defined operator lifting over nullable operands is
        // DEFERRED — a `Vec?` operand does NOT match a non-nullable `Vec`
        // parameter. Without the nullability gate the operator resolved and
        // codegen passed the `{i8, Vec}` wrapper struct to a function expecting a
        // bare `Vec` (malformed IR). Requiring nullability agreement makes the
        // operator not resolve, so the use is cleanly rejected below.
        // §8.4 (#1248): `=`/`<>` are exempt from the nullability gate — a `T?`
        // operand IS allowed to match a non-nullable `T` parameter for these two
        // operators because codegen wraps the call in the null-safe equality
        // protocol (lowerNullSafeRecordEquality), which unwraps the `{i8, T}`
        // payload to bare `T` before invoking the user operator. All other
        // operators remain gated (§7.11: lifting over `T?` is deferred).
        if (pt->kind == TypeRef::Kind::Named) {
            if (at->kind != TypeRef::Kind::Named) return false;
            // §25.2 (v0.181, #1562): "a derived-typed operand satisfies a
            // base-typed parameter by the ordinary argument rules." Accept an
            // exact declaration match OR a derived operand whose declaration
            // derives from the parameter's declaration — this is what makes an
            // operator inherited from a base class applicable to a derived-typed
            // operand (the params are declared with the *base* type). The
            // covariant direction is preserved (operand is-a parameter), so no
            // spurious commutation is introduced. Note: isSubtype compares
            // declarations only; generic-instantiation consistency for a generic
            // declaring type stays with genericArgsConsistent below.
            bool declOk = pt->declaration == at->declaration ||
                          (pt->declaration && at->declaration &&
                           isSubtype(at->declaration, pt->declaration));
            return declOk &&
                   (pt->nullable || !at->nullable ||
                    be.op == "=" || be.op == "<>");
        }
        // A primitive parameter accepts a primitive operand (numeric widening /
        // coercion is handled downstream); the kinds must still agree.
        if (pt->kind == TypeRef::Kind::Primitive)
            return at->kind == TypeRef::Kind::Primitive;
        return false;
    };
    // #1270 (§25 / §14.1): `operandMatchesParam` matches a Named operand against a
    // Named parameter by DECLARATION only, ignoring generic type args — so a binary
    // operator on a generic type (`operator +(a,b: Box<T>)`) wrongly accepted
    // MISMATCHED instantiations (`Box<int> + Box<bool>`, where the single param `T`
    // cannot unify to both). When the operator's declaring type is generic, unify
    // both parameter types against their operands into ONE binding map: a
    // type-parameter name bound to two conflicting concrete types fails
    // (unifyReturnAgainstTarget checks consistency), so the operator does not apply.
    // Non-generic operators (empty genericParams) and consistent instantiations are
    // unaffected.
    auto genericArgsConsistent = [&](MethodSymbol* m) -> bool {
        if (!m->parent || m->parent->genericParams.empty()) return true;
        std::unordered_set<std::string_view> paramNames;
        for (Symbol* gp : m->parent->genericParams)
            if (gp) paramNames.insert(gp->name);
        auto declTypeOf = [&](Symbol* paramSym) -> TypeRef* {
            auto* ps = dynamic_cast<ParameterSymbol*>(paramSym);
            if (!ps) return nullptr;
            return ps->declType ? ctx_.resolvedType(*ps->declType)
                                : ps->resolvedDeclType;
        };
        std::unordered_map<std::string_view, TypeRef*> solved;
        TypeRef* p0 = declTypeOf(m->params[0]);
        TypeRef* p1 = declTypeOf(m->params[1]);
        TypeRef* a0 = be.left ? ctx_.exprType(*be.left) : nullptr;
        TypeRef* a1 = be.right ? ctx_.exprType(*be.right) : nullptr;
        if (p0 && a0 && !unifyReturnAgainstTarget(p0, a0, paramNames, solved))
            return false;
        if (p1 && a1 && !unifyReturnAgainstTarget(p1, a1, paramNames, solved))
            return false;
        return true;
    };
    auto tryOperand = [&](const ast::Expr* operand) -> MethodSymbol* {
        // #1131: examine EVERY overload of the operator declared on `operand`'s
        // type and return the first whose parameters match the operands in order.
        for (MethodSymbol* m : candidatesOn(operand)) {
            if (operandMatchesParam(be.left.get(), m->params[0]) &&
                operandMatchesParam(be.right.get(), m->params[1]) &&
                genericArgsConsistent(m))
                return m;
        }
        return nullptr;
    };
    MethodSymbol* fromLeft = tryOperand(be.left.get());
    MethodSymbol* fromRight = tryOperand(be.right.get());
    // §25.2 (#1347, v0.160): if distinct applicable overloads are found on
    // BOTH operand types, the expression is a compile error. The compiler
    // declines to guess which is intended (mirroring §22.6's overload-
    // resolution stance). Operands of the same type find the same symbol —
    // `fromLeft == fromRight` — which is unambiguous and not an error.
    if (fromLeft && fromRight && fromLeft != fromRight) {
        // Collect the declaring-type names for the error message.
        std::string leftTypeName, rightTypeName;
        if (fromLeft->parent)  leftTypeName  = std::string(fromLeft->parent->name);
        if (fromRight->parent) rightTypeName = std::string(fromRight->parent->name);
        ctx_.diagnostics().report(
            Severity::Error, be.span.begin,
            "ambiguous operator `" + std::string(be.op) + "`: distinct "
            "applicable overloads found on both operand types "
            "(candidates: `" + leftTypeName + "::operator " +
            std::string(be.op) + "` and `" + rightTypeName +
            "::operator " + std::string(be.op) + "`) — "
            "remove a conflicting import or qualify the call (§25.2)");
        return nullptr;
    }
    return fromLeft ? fromLeft : fromRight;
}

// fix #4501 (§5.1 / §6.2 / §17.3 / §8.4, v0.306): the ONE place that answers
// "does §5.1 admit an implicit conversion between these two operand types?"
//
// WHY IT IS ONE PLACE NOW. `typeBinary` carried TWO copies of this question:
// one in the equality/relational arm (#1204) and one in the §6.2
// arithmetic-promotion arm. The two agreed on the numeric-vs-numeric case and
// both had the SAME hole — each made `isPromotablePrimitive(lt) &&
// isPromotablePrimitive(rt)` a PRECONDITION OF REJECTING, so a pair in which
// either operand was not a promotable numeric fell through the check entirely
// and was silently admitted. That inverted the rule: §5.1 lists the pairs that
// DO convert, and §6.2 makes a pair with no conversion a compile error, so
// "not on the promotion list" is a reason to reject, never a reason to skip.
//
// Measured on 16e992f0 (docs v0.306), one compile + one run each:
//   `string = 0`      -> reached codegen; `icmp eq ptr %2, i32 0` (+ five more
//                        malformed instructions) -> "IR verification failed";
//                        on an assertions-enabled LLVM the same IR aborts the
//                        compiler in `ICmpInst::AssertOK` (SIGABRT, exit 134,
//                        the symptom #4501 reports).
//   `bool = "x"`      -> the same.
//   `decimal = "x"`   -> "SExt only operates on integer".
//   `bool = 0`        -> compiled with NO diagnostic and the program printed
//                        `True`: a fabricated answer, the worst of the four.
//
// §17.3 fixes the authority for the equality arm: "`=` and `<>` invoke
// `Equals` with a null-safe wrapper (§8.4)". `string` has no `Equals(int)` and
// `bool` has no `Equals(int)`; the candidate set is empty, and §5.1 supplies
// no conversion that would make one applicable. For the relational and
// arithmetic arms the authority is §6.2 directly.
//
// WHAT IS NOT REJECTED, deliberately:
//   * `Object` on either side. §5.1's reference conversions include "any type
//     to `Object`" (boxing, §8.6), so `o = 5` IS admissible under §8.4 —
//     `Object.Equals(Object?)` (§8.1) is the applicable candidate. (That row
//     currently fails in LLVM's IR verifier; it is a CODEGEN defect with its
//     own issue, and rejecting it here would be a false reject with no
//     authority behind it.)
//   * A nullable operand on either side: `s = null`, `x = null` and the
//     lifted forms are §8.4/§7.11 questions answered elsewhere.
//   * Identical primitives, and §5.1 widening pairs in either direction.
//   * Named/unit/inline-array/function operands — the record, choice,
//     interface, enum, `Object` and unit gates above own those, each with its
//     own wording.
//
// THE LITERAL ESCAPE IS NOW KEYED ON THE OTHER OPERAND. §5.1's constant
// narrowing lets an untyped INTEGER constant adopt the other operand's
// INTEGER type, which is why `uint > 1000` must keep compiling (#1204). The
// escape is exactly as narrow as the paragraph's own text — "an expression
// of integer type … converts to any integer type whose range contains its
// value" — never a float/decimal constant, never a float/decimal/`bool`/
// `string` target (fix #4512, §5.1, v0.306: an earlier spelling here matched
// a FLOAT literal too and accepted any `isPromotablePrimitive` target
// including `decimal`/`float`/`double`, which let a float constant "adopt"
// `decimal` — a pair §5.1 never relates — and reach codegen; see
// `isLiteralDerivedExpr`'s header comment for the measured crash).
// `isTrueIntegerType` below is the same 8-type "any integer type" set §4.2
// enumerates (`isTrueInt` in the compound-assignment §6.6 check,
// ExprTypecheck.cpp, is the identical set under a local name — `char` is a
// distinct §4.5 kind and is excluded from both). The old spelling asked only
// "is either side literal-derived?" with no target-type gate at all, so the
// `0` in `string = 0` and in `bool = 0` exempted the pair all by itself — a
// second, independent reason those two rows were admitted, and the reason
// fixing the `isPromotablePrimitive` precondition alone would not have
// closed them.
bool ExprTC::reportNoImplicitConversionBetweenOperands(
    const ast::BinaryExpr& be, std::string_view op, const TypeRef* lt,
    const TypeRef* rt, bool allowLiteralAdoption) {
    if (!lt || !rt) return false;
    if (lt->kind != TypeRef::Kind::Primitive ||
        rt->kind != TypeRef::Kind::Primitive)
        return false;
    if (lt->nullable || rt->nullable) return false;
    if (lt->primitive == rt->primitive) return false;
    if (lt->primitive == PrimitiveType::None ||
        rt->primitive == PrimitiveType::None)
        return false;
    // §5.1 reference conversions: "any type to `Object`" (boxing, §8.6).
    if (lt->primitive == PrimitiveType::Object ||
        rt->primitive == PrimitiveType::Object)
        return false;
    // §5.1 numeric/char widening, either direction.
    if (primitiveWidens(lt->primitive, rt->primitive) ||
        primitiveWidens(rt->primitive, lt->primitive))
        return false;
    if (allowLiteralAdoption) {
        // §5.1 constant narrowing — see the header comment: the constant
        // adopts the OTHER operand's type, so the escape requires that type
        // to be one an INTEGER constant can have — the 8 sized integer
        // types, per §4.2/§5.1's own "any integer type" wording (fix #4512:
        // narrower than `isPromotablePrimitive`, which also admits
        // `float`/`double`/`decimal`/`char` — types a §5.1 constant can be
        // ASSIGNED to via ordinary widening, but not "adopt" via this
        // constant-only escape).
        auto isTrueIntegerType = [](PrimitiveType p) {
            switch (p) {
                case PrimitiveType::Byte:  case PrimitiveType::SByte:
                case PrimitiveType::Short: case PrimitiveType::UShort:
                case PrimitiveType::Int:   case PrimitiveType::UInt:
                case PrimitiveType::Long:  case PrimitiveType::ULong:
                    return true;
                default:
                    return false;
            }
        };
        if (isLiteralDerivedExpr(be.left.get()) &&
            isTrueIntegerType(rt->primitive))
            return false;
        if (isLiteralDerivedExpr(be.right.get()) &&
            isTrueIntegerType(lt->primitive))
            return false;
    }
    const bool bothNumeric = isPromotablePrimitive(lt->primitive) &&
                             isPromotablePrimitive(rt->primitive);
    const std::string head =
        std::string("operator `") + std::string(op) +
        "` has no implicit conversion between operand types `" +
        primitiveSpelling(lt->primitive) + "` and `" +
        primitiveSpelling(rt->primitive) + "`";
    if (bothNumeric) {
        // The pre-existing §6.2 mixed-numeric wording, byte-for-byte: several
        // lit rows pin it (`sema_mixed_numeric__*`, `sema_521_incompatible__*`).
        ctx_.diagnostics().report(
            Severity::Error, be.span.begin,
            head + " (§6.2); add an explicit cast (§5.2) to convert"
                   " one operand to the other's type.");
    } else if (op == "=" || op == "<>") {
        ctx_.diagnostics().report(
            Severity::Error, be.span.begin,
            head + " (§5.1) — `=`/`<>` invoke `Equals` "
                   "(§17.3/§8.4) and §5.1 admits no conversion "
                   "between these operand types, so there is no applicable "
                   "`Equals` for the pair. Compare values of the same type, or "
                   "convert one operand explicitly (§5.2) — for a flag, "
                   "`int(b)`; for a rendering, `.ToString()` (§8.5).");
    } else {
        ctx_.diagnostics().report(
            Severity::Error, be.span.begin,
            head + " (§5.1) — §5.1's table has no row between "
                   "them, so the operation is a compile error (§6.2); "
                   "convert one operand explicitly (§5.2).");
    }
    return true;
}

void ExprTC::typeBinary(const ast::BinaryExpr& be) {
    const auto& op = be.op;
    // §25: a user-defined operator overload takes precedence over the
    // built-in result-type rules; the expression is that method's call.
    if (op != "and" && op != "or" && op != "in") {
        if (MethodSymbol* opm = findBinaryOperator(be)) {
            ctx_.setExprSymbol(&be, opm);
            // per §7.4 (v0.182, #1566): a user-defined binary operator is a call
            // that can transfer control to user code — invalidate mutably-
            // captured narrowings (both operands were already visited under the
            // prior narrowing). Built-in primitive operators do not reach here.
            invalidateCapturedNarrowings();
            if (auto* od = dynamic_cast<const ast::OperatorDecl*>(opm->decl)) {
                if (od->returnType)
                    if (auto* rt = ctx_.resolvedType(*od->returnType)) {
                        // §25 / §14.1 (#1260): the operator is declared inside a
                        // GENERIC record/class (`operator +(a,b: Box<T>): Box<T>`).
                        // Its return type embeds the declaring type's params (`T`),
                        // which `resolvedType` leaves unsubstituted. Substitute them
                        // from the concrete operand instantiation (`Box<int>`) so
                        // the result is `Box<int>` — otherwise `r := a + b` was typed
                        // `Box<T>` and a downstream `r.value` resolved to the opaque
                        // param `T` (a later `r.value + 1` then wrongly reported
                        // "operator + not defined for `?`"). Reuses the #597 member-
                        // return substitution (substituteMemberType → classSubst).
                        if (opm->parent &&
                            !opm->parent->genericParams.empty()) {
                            auto isParentInst = [&](const ast::Expr* e) -> bool {
                                TypeRef* t = e ? ctx_.exprType(*e) : nullptr;
                                return t && t->kind == TypeRef::Kind::Named &&
                                       t->declaration == opm->parent &&
                                       !t->genericArgs.empty();
                            };
                            const ast::Expr* recv =
                                isParentInst(be.left.get())  ? be.left.get()
                                : isParentInst(be.right.get()) ? be.right.get()
                                                               : nullptr;
                            if (recv)
                                rt = substituteMemberType(rt, recv, opm->parent);
                        }
                        ctx_.setExprType(&be, rt);
                    }
            } else if (ctx_.isImported(opm)) {
                // §25 / §2.5 (#1485): an imported user-defined operator overload
                // carries no ast::OperatorDecl (imported symbols have no AST
                // node — decl == nullptr), so the dynamic_cast above yields null
                // and the BinaryExpr's result type would never be set. Without
                // it, `r := a + b` infers null, and a downstream `r.field`
                // fails member resolution → codegen silently drops the whole
                // expression (fieldAddr() returns null) or reads garbage. The
                // loader stashed the operator's resolved return type keyed on
                // the symbol; use it. Mirrors the imported-regular-method
                // fallback in ExprTypecheck_Calls.cpp.
                if (auto* rt = ctx_.importedReturnType(opm)) {
                    // §25 / §14.1 (#3159, v0.254): the GENERIC arm above
                    // substitutes the parent's type parameters from a concrete
                    // operand instantiation; the IMPORTED arm here was returning
                    // the loader's bare-parametric `Vec<T>` shape unchanged. With
                    // the result type left at `Vec<T>`, the BinaryExpr is typed
                    // as a different specialization than the operands
                    // (`Vec<string>`), so:
                    //   * `r := p + q` infers `r: Vec<T>` and `r.a` is the bare
                    //     parametric — the subsequent `string + r.a` falls into
                    //     the §4.6 mixed-string diagnostic on a perfectly legal
                    //     `+` overload, not the "operator not defined" diagnostic
                    //     the user actually violated;
                    //   * when no downstream string is involved, codegen
                    //     instantiates the call against `Vec<T>` while
                    //     `p`/`q` are `Vec<string>`, producing two distinct
                    //     record layouts ($spec0 vs $spec1) at the LLVM call site
                    //     and a "Call parameter type does not match function
                    //     signature" IR-verification failure (§3.2).
                    // Reapply the same parent-param substitution as the generic
                    // local arm so the imported operator's result is the
                    // concrete operand's `Vec<string>`, not the bare-parametric
                    // `Vec<T>`. Mirrors substituteMemberType's #597/#361 reader.
                    if (opm->parent &&
                        !opm->parent->genericParams.empty()) {
                        auto isParentInst = [&](const ast::Expr* e) -> bool {
                            TypeRef* t = e ? ctx_.exprType(*e) : nullptr;
                            return t && t->kind == TypeRef::Kind::Named &&
                                   t->declaration == opm->parent &&
                                   !t->genericArgs.empty();
                        };
                        const ast::Expr* recv =
                            isParentInst(be.left.get())  ? be.left.get()
                            : isParentInst(be.right.get()) ? be.right.get()
                                                           : nullptr;
                        if (recv)
                            rt = substituteMemberType(rt, recv, opm->parent);
                    }
                    ctx_.setExprType(&be, rt);
                }
            }
            return;
        }
        // §11.8 (v0.176, #807): constraint-operator dispatch — a binary
        // operator on TYPE-PARAMETER-typed operands resolves through a static
        // operator member of the parameter's `where T: I<T>` constraint when
        // no other resolution applies (findBinaryOperator found nothing above:
        // a type-parameter TypeRef has no declaration to search). Both
        // operands must be the SAME parameter; the interface member's shape is
        // the §2.19 recurrence form `static operator <op>(a: T, b: T): R`.
        // Resolution to the concrete type's operator (or the primitive
        // intrinsic op) happens at instantiation in codegen.
        {
            // §2.8.2/§11.8 (#2359 bug 2b): the SAME imported-TypeRef gap as
            // bug 2a — `total`'s exprType (from `var total := T.Zero();`,
            // resolved through an imported constraint interface) carries no
            // `astNode`. Read the bare-parameter name via `headName()`
            // (alias-transparent, falls back to `importedName`) instead of
            // requiring `astNode` directly, so `total + x` is recognized as
            // a type-parameter-typed BinaryExpr for a cross-module `T`.
            auto typeParamNameOf = [&](const ast::Expr* e) -> std::string_view {
                if (!e) return {};
                const TypeRef* t = ctx_.exprType(*e);
                if (!t || t->kind != TypeRef::Kind::Named || t->declaration ||
                    t->nullable || t->headName().empty())
                    return {};
                std::string_view n = t->headName();
                return isInScopeTypeParam(n) ? n : std::string_view{};
            };
            std::string_view lp = typeParamNameOf(be.left.get());
            if (!lp.empty() && lp == typeParamNameOf(be.right.get())) {
                TypeRef* bound = nullptr;
                if (MethodSymbol* iop =
                        findConstraintStaticOperator(lp, op, &bound)) {
                    ctx_.setExprSymbol(&be, iop);
                    // Result type: the operator's declared return with the
                    // interface's recurrence parameter substituted by the
                    // call-site parameter (i.e. the operand's own TypeRef);
                    // a concrete return (bool for `<`) passes through.
                    TypeRef* resTy = nullptr;
                    if (auto* od = dynamic_cast<const ast::OperatorDecl*>(
                            iop->decl)) {
                        if (od->returnType)
                            resTy = ctx_.resolvedType(*od->returnType);
                    } else if (ctx_.isImported(iop)) {
                        // §2.5/§11.8 (#2359 bug 2b): the constraint interface
                        // itself is imported, so `iop` (its static operator
                        // member) carries no AST at all (`decl == nullptr`)
                        // — mirror the `isImported(opm)` fallback above for
                        // the NAMED-type operator case: read the loader's
                        // stashed resolved return type keyed on the symbol.
                        resTy = ctx_.importedReturnType(iop);
                    }
                    if (resTy && resTy->kind == TypeRef::Kind::Named &&
                        !resTy->declaration) {
                        // Bare recurrence-parameter return → the operand type.
                        resTy = be.left ? ctx_.exprType(*be.left) : nullptr;
                    }
                    if (resTy) ctx_.setExprType(&be, resTy);
                    return;
                }
                // §25.2 (v0.181, #1498): the candidate set for an operator on a
                // type parameter is drawn SOLELY from its static-interface
                // constraints (§11.8) — a type parameter declares no operators
                // of its own. When an ORDERING operator does not resolve through
                // any constraint (none declares a comparison-group operator, so
                // the complete-group entitlement above found nothing), the
                // candidate set is empty: reject cleanly here. Falling through to
                // the built-in relational result-type rule below would type the
                // expression `bool` with no operator symbol, and codegen would
                // then emit a primitive `icmp`/`fcmp` on the instantiated
                // operands — a crash on any non-primitive instantiation (the
                // LLVM ICmp assertion of #1498). `=`/`<>` are NOT rejected here:
                // reference/structural equality is always available (§8.3).
                if (op == "<" || op == ">" || op == "<=" || op == ">=") {
                    ctx_.diagnostics().report(
                        Severity::Error, be.span.begin,
                        "operator `" + std::string(op) + "` is not available on "
                        "type parameter `" + std::string(lp) + "` — a type "
                        "parameter declares no operators of its own, and none of "
                        "its constraints declares a comparison operator; add a "
                        "`where " + std::string(lp) + ": INumber<" +
                        std::string(lp) + ">` (or `ISteppable<" +
                        std::string(lp) + ">`) bound (\xC2\xA7""25.2/\xC2\xA7"
                        "11.8)");
                    ctx_.setExprType(
                        &be, ctx_.primitiveTypeRef(PrimitiveType::Bool));
                    return;
                }
            }
        }
        // §7.11 (#1132) / proposal 0114 D8 (v0.267): user-defined operator
        // lifting over nullable operands is REJECTED — the same compile
        // error as the primitive gate above (mirrored per §7.11 v0.277: "such an
        // operand at a user-defined-operator position is the same compile
        // error, naming the same remedies"). The operator did NOT resolve above
        // (the nullability gate in operandMatchesParam), but it IS declared on
        // the operand's underlying type and an operand is nullable. Emit the
        // same-wording diagnostic rather than the generic "operator not
        // defined" message below — the operator exists; only its lifting
        // over `T?` is unsupported. (`=`/`<>` are excluded: they are already
        // null-safe per §8.4.)
        if (op != "=" && op != "<>") {
            auto nullableUserOperand = [&](const ast::Expr* operand) -> bool {
                if (!operand) return false;
                const TypeRef* t = ctx_.exprType(*operand);
                if (!t || !t->nullable) return false;
                TypeSymbol* ts = memberTargetType(operand);
                if (!ts || (ts->kind != ast::TypeDeclKind::Record &&
                            ts->kind != ast::TypeDeclKind::Class))
                    return false;
                for (Symbol* m : ts->members) {
                    auto* ms = dynamic_cast<MethodSymbol*>(m);
                    if (ms && ms->isOperator && ms->operatorToken == op &&
                        ms->params.size() == 2)
                        return true;
                }
                return false;
            };
            if (nullableUserOperand(be.left.get()) ||
                nullableUserOperand(be.right.get())) {
                ctx_.diagnostics().report(
                    Severity::Error, be.span.begin,
                    std::string("operator `") + std::string(op) +
                    "` is not defined on a nullable value type — narrow first "
                    "(`x <> null and …` (\xC2\xA7""7.4), `??` (\xC2\xA7""7.7), "
                    "or `!` (\xC2\xA7""7.8)); user-defined operators do not "
                    "lift over `T?` (\xC2\xA7""7.11, v0.267)");
                // Error recovery: type the result as the left operand's type so
                // downstream uses do not cascade an "untyped expression" error.
                if (TypeRef* lt = be.left ? ctx_.exprType(*be.left) : nullptr)
                    ctx_.setExprType(&be, lt);
                return;
            }
        }
    }
    // fix #2819 (§3.5 *Operators on primitive types*, §6.2 *Mixed-type
    // arithmetic*, §5.1 *Implicit conversions*, v0.240): an arithmetic,
    // bitwise, shift or ordering operator with a UNIT (`()`) operand is
    // ill-formed and must be rejected here.
    //
    // §3.5 enumerates each intrinsic operator group BY OPERAND TYPE —
    // "**Arithmetic** on `byte`, `sbyte`, `short`, `ushort`, `int`, `uint`,
    // `long`, `ulong`, `float`, `double`, `decimal`", bitwise and shift on the
    // integer types, "**Ordering** … on every primitive **except `bool`**" —
    // and `()` is in none of those lists (it is not even a primitive: §3.1
    // lists the unit type separately). §3.5 closes the door explicitly: "The
    // set of admitted combinations is fixed; primitives do not declare
    // cross-type `operator +` methods that user code could inspect." §6.2 then
    // gives the disposition for an operand pair with no shared type: "If no
    // implicit conversion exists between the operand types … the operation is a
    // compile error and an explicit cast is required" — and §5.1's table has no
    // row converting `()` to anything numeric (its only conversion out of unit
    // is boxing to `Object`). Nor can a user supply one: §25.2 draws the
    // candidate set for non-primitive operands from `operator` overloads
    // declared on the operand types, and the unit type declares none and cannot
    // be extended to (§26.2).
    //
    // Before this the expression was ACCEPTED, with three different wrong
    // outcomes depending only on where the unit came from: `total + t.Wait()`
    // compiled and contributed 0 on every iteration (the #2819 repro — and the
    // reason #2807's original bug report carried a false exit-code claim that
    // nobody, including the compiler, contradicted); `total + C.Nop()` on a
    // void-returning call emitted malformed IR ("Call parameter type does not
    // match function signature", an `sadd.with.overflow` fed a `void`); and
    // `total + u` on a `()`-typed local degraded through the #2434 fallback to
    // 0. One clean rejection replaces all three.
    //
    // CATEGORY-BLIND by construction, as #2819 asked: the gate is "this
    // operand's resolved type is `Kind::Unit`", not an enumeration of the
    // expressions that can produce unit (`Task.Wait()`, a void-returning call,
    // a `()` literal, a `()`-typed local/param/field, a unit-returning lambda
    // invocation) — the same hole is reachable from each, and enumerating
    // source categories is how the §14.2 predicate family accreted four
    // separate bug reports.
    //
    // Nullability is deliberately IGNORED, so a `()?` operand is rejected too:
    // §7.11 lifts an operator "only where it is defined on the underlying type
    // `T`", and it is defined nowhere on `()`. Same posture as the `bool`
    // ordering check below, which notes it covers `bool?` for this reason.
    //
    // NOT rejected — the accept-controls this deliberately leaves alone:
    //   * `=` and `<>`. §8.4 defines `a = b` as `a.Equals(b)` with a null-safe
    //     wrapper, and unit reaches `Object` (§8.6 boxing; §8.5 even specifies
    //     its `ToString` as `"()"`), so `u = v` is well-formed and always true.
    //     No spec text withdraws equality from unit, so rejecting it would be a
    //     false reject with no authority behind it.
    //   * `and`/`or`/`in` — not in the token set below.
    //   * Every non-operator position: a bare `t.Wait();` statement, a
    //     unit-returning method called for effect, `_ := f();`, `var u: () :=
    //     ();`, `u := ();`, `var o: Object := ();` (§8.6 *Boxing of the unit
    //     value* — note a BOXED unit's static type is `Object`, not `()`, so it
    //     never reaches this gate), `$"u={u}"` interpolation, and a lambda
    //     whose body is a unit call. All verified still accepted.
    if (op == "+" || op == "-" || op == "*" || op == "/" || op == "mod" ||
        op == "&" || op == "|" || op == "^" || op == "<<" || op == ">>" ||
        op == "<" || op == ">" || op == "<=" || op == ">=") {
        auto isUnit = [](const TypeRef* t) {
            return t && t->kind == TypeRef::Kind::Unit;
        };
        TypeRef* lt = be.left ? ctx_.exprType(*be.left) : nullptr;
        TypeRef* rt = be.right ? ctx_.exprType(*be.right) : nullptr;
        if (isUnit(lt) || isUnit(rt)) {
            bool relational =
                (op == "<" || op == ">" || op == "<=" || op == ">=");
            ctx_.diagnostics().report(
                Severity::Error, be.span.begin,
                "operator `" + std::string(op) + "` is not defined for a unit "
                "(`()`) operand (\xC2\xA7""3.5) \xE2\x80\x94 the unit type has "
                "exactly one value and no arithmetic, comparison or bitwise "
                "operations, and \xC2\xA7""5.1 provides no conversion from `()` "
                "to a numeric type, so the operation is a compile error "
                "(\xC2\xA7""6.2). A unit-valued expression \xE2\x80\x94 a method "
                "declared without a return type, or `Task.Wait()` "
                "(\xC2\xA7""30.3), whose result is read from `Result`, not from "
                "`Wait()` \xE2\x80\x94 is a statement, not a value: call it on "
                "its own line.");
            // Error recovery: type the result so downstream uses do not cascade
            // an "untyped expression" error. A relational yields `bool`; an
            // arithmetic/bitwise/shift yields the OTHER (non-unit) operand's
            // type when there is one, else `int`.
            if (relational) {
                ctx_.setExprType(&be, ctx_.primitiveTypeRef(PrimitiveType::Bool));
            } else if (!isUnit(lt) && lt) {
                ctx_.setExprType(&be, lt);
            } else if (!isUnit(rt) && rt) {
                ctx_.setExprType(&be, rt);
            } else {
                ctx_.setExprType(&be, ctx_.primitiveTypeRef(PrimitiveType::Int));
            }
            return;
        }
    }
    // fix #3903 (§3.1/§3.4/§3.5/§8.1/§25/§26.3, v0.294): `Object` is a class
    // (§3.1 "Every type derives from `Object`" lists it separately from the
    // primitive VALUE types; §3.4 "`Object` is a class"; §8.1 gives its fixed
    // member surface), but the implementation represents an `Object`-typed
    // operand as `TypeRef::Kind::Primitive`/`PrimitiveType::Object` (so boxing,
    // `is`/`as`, etc. can treat it uniformly with the true primitives). Every
    // "operator requires a declared overload" gate above and below keys off
    // `TypeRef::Kind::Named` (the record/class/interface/choice checks) or an
    // enumerated primitive kind (the bitwise/mod/nullable checks) — `Object`
    // matches neither, so it fell through every one of them and reached the
    // generic result-type rules typed as an ordinary primitive.
    //
    // §3.5 defines the intrinsic arithmetic/bitwise/shift/ordering operators
    // over an enumerated list of primitive value types that does not include
    // `Object`. Per §25, `Object` could only acquire one of these operators by
    // declaring a `static method operator` overload in its own body — but §8.1
    // fixes that body to exactly `GetType`/`Equals`/`GetHashCode`/`ToString`/
    // `ReferenceEquals` ("User code cannot redeclare or replace `Object`"),
    // and an extension block cannot add one either (§26.3 "No operator
    // overloads"). So `Object` has no way to acquire any of these operators —
    // the same "no applicable overload" verdict a record/class/interface/
    // choice with no matching declaration gets elsewhere in this function,
    // just reached because there is structurally no path to declare one.
    // `=`/`<>` are excluded: §8.4 gives every type `=`/`<>` via `Equals`, and
    // `Object`'s inherited (reference-equality, §8.3) `Equals` already answers
    // them correctly through the ordinary dispatch path — this gate does not
    // apply to them.
    //
    // Measured on HEAD before this fix: `Object + int`/`Object - Object`/
    // `Object * int`/`Object / int`/`Object mod int` were typed `Object` (the
    // left operand's own type, via the "result follows lt" rule far below) and
    // died in LLVM's IR verifier ("Both operands to a binary operator are not
    // of the same type!", an add/sub/mul/sdiv/srem between a `ptr` and an
    // `i32`). `Object & int`/`Object | int`/`Object ^ int` didn't even crash —
    // they hit the unrelated #2434 lower-to-zero backstop and silently
    // compiled to a WRONG value (a zero-initialized slot, with only an
    // "internal:" stderr note and exit 0). `Object < int` reached the generic
    // relational fallback (typed `bool`) and died in IR verification the same
    // way as arithmetic; `Object > Object` reached the SAME fallback but
    // compiled cleanly to a raw pointer `icmp` — an order over heap addresses
    // that violates §30.13's determinism guarantee, with no crash and no
    // diagnostic at all. One clean rejection replaces all of these.
    if (op == "+" || op == "-" || op == "*" || op == "/" || op == "mod" ||
        op == "&" || op == "|" || op == "^" || op == "<<" || op == ">>" ||
        op == "<" || op == ">" || op == "<=" || op == ">=") {
        auto isObjectPrim = [](const TypeRef* t) {
            return t && t->kind == TypeRef::Kind::Primitive &&
                   t->primitive == PrimitiveType::Object;
        };
        TypeRef* objLt = be.left  ? ctx_.exprType(*be.left)  : nullptr;
        TypeRef* objRt = be.right ? ctx_.exprType(*be.right) : nullptr;
        if (isObjectPrim(objLt) || isObjectPrim(objRt)) {
            bool relational =
                (op == "<" || op == ">" || op == "<=" || op == ">=");
            ctx_.diagnostics().report(
                Severity::Error, be.span.begin,
                "operator `" + std::string(op) + "` is not defined for type "
                "`Object` (\xC2\xA7""25) \xE2\x80\x94 `Object`'s declared "
                "surface is fixed (\xC2\xA7""8.1) and an extension block "
                "cannot add an operator overload (\xC2\xA7""26.3), so no `" +
                std::string(op) + "` overload can ever be declared for it; "
                "only `=`/`<>` are defined on `Object` (\xC2\xA7""8.4, via "
                "`Equals`). Narrow to a concrete type first (`is`/`as`, "
                "\xC2\xA7""17.4).");
            if (relational) {
                ctx_.setExprType(&be,
                                 ctx_.primitiveTypeRef(PrimitiveType::Bool));
            } else if (!isObjectPrim(objLt) && objLt) {
                ctx_.setExprType(&be, objLt);
            } else if (!isObjectPrim(objRt) && objRt) {
                ctx_.setExprType(&be, objRt);
            } else {
                ctx_.setExprType(&be, ctx_.primitiveTypeRef(PrimitiveType::Int));
            }
            return;
        }
    }
    // §25.2 (#746 R41-C): a relational operator (`<`, `>`, `<=`, `>=`) on a
    // record/class operand requires a matching user-defined overload — none is
    // auto-generated. A partial comparison group (e.g. only `<`/`>` declared,
    // then `<=` used) otherwise fell through to the built-in result-type rule,
    // and codegen emitted a struct-level `icmp` on the aggregate (malformed IR).
    // Reject the undefined use cleanly. (Primitive operands have no
    // memberTargetType and keep the built-in relational.)
    //
    // fix #3929 (§25/§25.2/§11.8, v0.294): widened to also cover Choice, and
    // — as its OWN diagnostic, not this one — Interface (see the dedicated
    // block immediately below). A choice CAN overload comparison operators
    // (§25, §35.2), but `findBinaryOperator` (via `collectInheritedOperators`,
    // which is kind-blind) already tried that above and returns before
    // reaching here when a match exists — so arriving here with a choice
    // operand means no matching overload was declared, the same "no
    // applicable overload" verdict a record/class operand gets. Before this
    // fix a choice operand fell through this record/class-only filter and
    // reached codegen typed `bool` by the generic relational fallback far
    // below (no backstop there checks for a Named type at all, unlike the
    // arithmetic path a few lines below, which at least catches a Named LEFT
    // operand) — measured on HEAD before this fix: `Shape < int` died in
    // LLVM's IR verifier ("Both operands to ICmp instruction are not of the
    // same type!", an `icmp` between a `%choice.Shape` aggregate and `i32`),
    // in EITHER operand order (this loop already walks both operands, so
    // widening the kind filter fixes both orders in one place rather than
    // adding a second, order-specific check).
    if (op == "<" || op == ">" || op == "<=" || op == ">=") {
        for (const ast::Expr* operand : {be.left.get(), be.right.get()}) {
            TypeSymbol* ts = memberTargetType(operand);
            // Exclude the value-builtins (DateTime/TimeSpan/Guid/…): they carry
            // a kind==Class TypeSymbol but support a built-in relational compare
            // with no user overload (§2.5).
            if (ts && !ts->isValueBuiltin &&
                (ts->kind == ast::TypeDeclKind::Record ||
                 ts->kind == ast::TypeDeclKind::Class ||
                 ts->kind == ast::TypeDeclKind::Choice)) {
                ctx_.diagnostics().report(
                    Severity::Error, be.span.begin,
                    "operator `" + std::string(op) +
                    "` is not defined for type `" + std::string(ts->name) +
                    "` — comparison operators must be explicitly overloaded "
                    "(§25.2)");
                ctx_.setExprType(&be, ctx_.primitiveTypeRef(PrimitiveType::Bool));
                return;
            }
        }
    }
    // fix #3929 (§25/§11.8, v0.294): an Interface operand gets a DEDICATED
    // diagnostic rather than reusing the record/class/choice "declare a
    // matching operator" wording above (issue #3952, caught in review): that
    // wording is actionable for a choice (it CAN declare an operator; see the
    // block above) but is actively wrong for an interface. §25's opening
    // sentence lists exactly who "may overload operators" — "Records, enums,
    // choices ... and classes" — interfaces are not on that list, and §11.8
    // states plainly that "through an interface-typed value only instance
    // members are reachable" and that "`T.Member(...)` dispatch requires a
    // generic type parameter constrained by the interface: a value's static
    // type names no single conforming type." So even where the interface (or
    // a conforming type) DOES declare a matching static operator, it can
    // never be reached through a plain interface-typed operand — reviewer-
    // verified: giving `IFoo` a matching `static operator +` and a
    // conforming `Box` still leaves `b + 1` (`b: IFoo`) rejected. Telling the
    // user to "declare a matching operator" would send them down a path that
    // cannot fix their code; this diagnostic instead names the real
    // remedies — narrow to the concrete conforming type (whose OWN operator,
    // if it declares one, IS then reachable — this is a record/class-typed
    // operand at that point, handled by the block above), or constrain a
    // generic type parameter by the interface (§11.8's `Sum<T>` pattern).
    //
    // This is deliberately a THIRD spelling, not a merge into the record/
    // class/choice block above: `Object` (block earlier in this function, can
    // never declare an operator at all), record/class/choice (can declare,
    // just didn't here), and interface (can be declared on, but never
    // reachable through a value) are three different reasons a "no applicable
    // overload" verdict applies, and collapsing interface into the
    // record/class wording would be a drifting predicate in the
    // UNDER-differentiation direction — the same defect class this project's
    // classification-predicate discipline warns against, just inverted.
    if (op == "<" || op == ">" || op == "<=" || op == ">=") {
        for (const ast::Expr* operand : {be.left.get(), be.right.get()}) {
            TypeSymbol* ts = memberTargetType(operand);
            if (ts && ts->kind == ast::TypeDeclKind::Interface) {
                ctx_.diagnostics().report(
                    Severity::Error, be.span.begin,
                    "operator `" + std::string(op) +
                    "` is not defined for type `" + std::string(ts->name) +
                    "` (\xC2\xA7""25) \xE2\x80\x94 even where `" +
                    std::string(ts->name) +
                    "` or a conforming type declares a matching static "
                    "operator, it is reachable only through a generic type "
                    "parameter constrained by `" + std::string(ts->name) +
                    "`, never through a plain interface-typed value "
                    "(\xC2\xA7""11.8: \xE2\x80\x9Cthrough an interface-typed "
                    "value only instance members are reachable\xE2\x80\x9D; "
                    "\xE2\x80\x9CT.Member(...) dispatch requires a generic "
                    "type parameter constrained by the "
                    "interface\xE2\x80\x9D). Narrow to the concrete "
                    "conforming type first (`is`/`as`, \xC2\xA7""17.4), or "
                    "constrain a generic parameter by `" +
                    std::string(ts->name) + "` instead.");
                ctx_.setExprType(&be, ctx_.primitiveTypeRef(PrimitiveType::Bool));
                return;
            }
        }
    }
    // per §3.5 (v0.182, #1574): a relational operator (`<`, `<=`, `>`, `>=`)
    // is not defined on `bool` operands, and per §4.6 (v0.182, #1575) it is
    // not defined on a nullable-reference (`string?`) operand either — both
    // fall through to the generic primitive result-type rule below (which
    // types every relational as `bool` regardless of operand type) and were
    // wrong: `true < false` compiled to `true`, and `string? < string?`
    // segfaulted on a null operand. Reject both here. Only `=`/`<>` are valid
    // on bool / `string?` (the general equality path below is unaffected).
    //
    // fix #4500 (§3.5 / §6.2 / §5.1, v0.306): the ARITHMETIC operators
    // `+ - * /` join this gate's operator set rather than growing a second,
    // parallel `bool`-operand check further down. §3.5 answers ONE question
    // for both families — "does the intrinsic-operator table admit an operand
    // of this type?" — by enumerating each group BY OPERAND TYPE:
    // "**Arithmetic** on `byte`, `sbyte`, `short`, `ushort`, `int`, `uint`,
    // `long`, `ulong`, `float`, `double`, `decimal`: `+`, `-`, `*`, `/` …"
    // (no `bool`) beside "**Ordering** … on every primitive **except
    // `bool`**". §5.1's table has no row out of `bool` ("Between integers and
    // `bool`" is on its never-implicit list), so §6.2's disposition applies:
    // "If no implicit conversion exists between the operand types … the
    // operation is a compile error and an explicit cast is required". The
    // remedy is the same one the ordering arm already names — `int(b)`.
    //
    // Measured on HEAD before this fix (bezelc @ 8fd7cabf, docs v0.306), one
    // compile + one run each: `decimal * bool` and `double * bool` compiled
    // with NO diagnostic and the program printed `-2`; `bool + bool` printed
    // `True` and `bool * bool` printed `False` (arithmetic answered in the
    // boolean domain); `int * bool` and `int + bool` emitted malformed IR
    // ("Call parameter type does not match function signature",
    // `@llvm.smul.with.overflow.i32(i32, i1)`) and died in LLVM's verifier.
    // One clean rejection replaces all four outcomes, in either operand order
    // and at a same-typed `bool op bool` pair alike — the gate is per-OPERAND
    // (§3.5's table is read per operand type), not per pair, so it does not
    // depend on the other operand being numeric.
    //
    // NOT folded in, deliberately: `mod` (§6.1), and the bitwise/shift set
    // `& | ^ << >>` (§6.5) already reject a `bool` operand further down, each
    // under its OWN spec rule with its own remedy ("boolean operations use
    // `and`/`or`/`not`" for bitwise; "`mod` is defined on the integer types and
    // `decimal`" for mod). Those are not copies of this rule that drifted —
    // they are different sections answering different questions, they already
    // agree with §3.5's table, and their wording is pinned by lit rows. What
    // was missing was §3.5's ARITHMETIC bullet, and it is added here, at the
    // one site that already answered the identical question for ordering.
    if (op == "<" || op == ">" || op == "<=" || op == ">=" ||
        op == "+" || op == "-" || op == "*" || op == "/") {
        // #1574: bool ordering. §7.11's relational-lifting excludes `bool?`
        // for the same reason, so checking the primitive kind here (ignoring
        // nullability) covers a `bool?` operand too. The same reading covers
        // the arithmetic arm (#4500): §7.11 lifts an operator "only where it
        // is defined on the underlying type `T`", and `+ - * /` are defined
        // nowhere on `bool`.
        auto isBool = [](const TypeRef* t) {
            return t && t->kind == TypeRef::Kind::Primitive &&
                   t->primitive == PrimitiveType::Bool;
        };
        auto isStringPrim = [](const TypeRef* t) {
            return t && t->kind == TypeRef::Kind::Primitive &&
                   t->primitive == PrimitiveType::String;
        };
        TypeRef* lt = be.left  ? ctx_.exprType(*be.left)  : nullptr;
        TypeRef* rt = be.right ? ctx_.exprType(*be.right) : nullptr;
        const bool ordering =
            (op == "<" || op == ">" || op == "<=" || op == ">=");
        // §4.6 is the MORE SPECIFIC authority for `string + <anything>`:
        // `+` on a string operand is concatenation (a declared `String.operator
        // +`, §3.5), not §3.5's numeric-arithmetic bullet, and its remedy is
        // interpolation or `.ToString()` — not `int(b)`. That check lives
        // below and already covers every non-string right operand, `bool`
        // included, so this gate stands aside for exactly that pair rather
        // than answering it with advice that does not fit.
        const bool deferToStringConcat =
            (op == "+") && (isStringPrim(lt) || isStringPrim(rt));
        if ((isBool(lt) || isBool(rt)) && !deferToStringConcat) {
            ctx_.diagnostics().report(
                Severity::Error, be.span.begin,
                "operator `" + std::string(op) + "` is not defined for `bool` "
                "operands (\xC2\xA7""3.5) \xE2\x80\x94 " +
                (ordering
                     ? std::string(
                           "no ordering of `false`/`true` is "
                           "defined; only `=`/`<>` are valid on bool. To order "
                           "by a flag, convert explicitly (`int(b)`).")
                     : std::string(
                           "\xC2\xA7""3.5 defines arithmetic (`+`, `-`, `*`, `/`) "
                           "on the numeric types only, and \xC2\xA7""5.1 admits no "
                           "implicit conversion out of `bool`, so the operation "
                           "is a compile error (\xC2\xA7""6.2). To compute with a "
                           "flag, convert explicitly (`int(b)`).")));
            // Error recovery, mirroring the unit-operand gate above: an
            // ordering yields `bool`; an arithmetic yields the OTHER
            // (non-bool) operand's type when there is one, else `int`.
            if (ordering) {
                ctx_.setExprType(&be,
                                 ctx_.primitiveTypeRef(PrimitiveType::Bool));
            } else if (!isBool(lt) && lt) {
                ctx_.setExprType(&be, lt);
            } else if (!isBool(rt) && rt) {
                ctx_.setExprType(&be, rt);
            } else {
                ctx_.setExprType(&be, ctx_.primitiveTypeRef(PrimitiveType::Int));
            }
            return;
        }
        // The #1575 `string?` rule below is ORDERING-only: §4.6 defines
        // `<`/`<=`/`>`/`>=` on `string`, while `+` on a string operand is the
        // §4.6 CONCATENATION rule, checked below with its own nullability
        // handling. Arithmetic therefore stops here.
        if (ordering) {
            // #1575: ordering on `string` requires non-nullable operands; a
            // `string?` operand is a compile error (narrow first, §7.4) — the same
            // use-requires-narrowing discipline as member access on a nullable
            // reference. Distinct from §7.11 lifting, which is confined to nullable
            // VALUE types and does NOT extend to `string`. Previously a `string?`
            // operand fell through to ordinal-compare codegen and segfaulted on a
            // null operand; `=`/`<>` stay null-safe (§8.4) and are unaffected.
            auto nullableStringOperand = [&](const ast::Expr* operand) -> bool {
                if (!operand) return false;
                const TypeRef* t = ctx_.exprType(*operand);
                if (!t || t->kind != TypeRef::Kind::Primitive ||
                    t->primitive != PrimitiveType::String || !t->nullable)
                    return false;
                // §7.4: a null-narrowed (`x <> null`) simple local/parameter is
                // known non-null even though exprType keeps the nullable flag
                // (null-narrowing does not rewrite exprType). This is the same
                // question §7.11's own gates ask, so it is the same predicate
                // (#3298) rather than a fifth copy of the lookup.
                return !isNarrowedNonNullRead(operand);
            };
            if (nullableStringOperand(be.left.get()) ||
                nullableStringOperand(be.right.get())) {
                ctx_.diagnostics().report(
                    Severity::Error, be.span.begin,
                    "operator `" + std::string(op) + "` requires non-nullable "
                    "`string` operands (\xC2\xA7""4.6); a `string?` operand is a "
                    "compile error \xE2\x80\x94 narrow first (\xC2\xA7""7.4), the same "
                    "use-requires-narrowing discipline as member access on a "
                    "nullable reference. Equality (`=`/`<>`) stays null-safe "
                    "(\xC2\xA7""8.4) and is unaffected.");
                ctx_.setExprType(&be, ctx_.primitiveTypeRef(PrimitiveType::Bool));
                return;
            }
        }
    }
    // §25.1 (#1096): an arithmetic/bitwise/shift operator on a record/class
    // operand requires a matching user-defined overload whose parameters match
    // the operand ORDER (findBinaryOperator enforces the order; Bezel does not
    // auto-commute). An undeclared operator, or a reversed-operand use like
    // `4 * v` against `operator *(Vec, int)`, otherwise fell through to the
    // built-in numeric rule and codegen emitted a struct-level arithmetic op →
    // an IR-verification crash. Reject cleanly. (Primitive and value-builtin
    // operands — e.g. string `+`, decimal arithmetic, DateTime — keep their
    // built-in operators: they have no record/class memberTargetType here.)
    //
    // fix #3929 (§25/§25.2/§11.8, v0.294): widened to also cover Choice, and
    // — as its OWN diagnostic just below, not this one (issue #3952) — cover
    // Interface too. See the relational (§25.2) sibling above for the full
    // rationale on why Interface needs a different message than Choice (a
    // choice CAN declare an operator; an interface's operator, even where
    // one is declared, is never reachable through a plain interface-typed
    // value, §11.8) — a choice reaching here already failed
    // `findBinaryOperator`'s search of its own declarations. This loop
    // already walks BOTH operands, so widening the filter (rather than
    // patching the order-asymmetric backstops below, which only ever inspect
    // the LEFT operand's type) closes the gap in both operand orders at
    // once: measured on HEAD before this fix, `s + 1` (`s: Shape`, choice)
    // was already rejected by the generic Named-type backstop a few lines
    // down (which only checks `lt`), but the swapped `1 + s` fell through it
    // (backstop checks only the left operand) and died in the IR verifier;
    // `1 << s`/`1 & s` swapped the same way but didn't even crash — the
    // isBitwise backstop below also only checks `lt`, so they hit the
    // unrelated #2434 lower-to-zero path and silently degraded to a
    // zero-initialized value (exit 0, no diagnostic). This gate now catches
    // every one of those rows on either operand, with the same wording the
    // record/class case already gets. (Interface exhibits the identical
    // order-asymmetric leak — `b: IFoo`, `1 + b`/`1 << b` swapped the same
    // way — closed by the dedicated Interface check below instead.)
    if (op == "+" || op == "-" || op == "*" || op == "/" || op == "mod" ||
        op == "&" || op == "|" || op == "^" || op == "<<" || op == ">>") {
        for (const ast::Expr* operand : {be.left.get(), be.right.get()}) {
            TypeSymbol* ts = memberTargetType(operand);
            if (ts && !ts->isValueBuiltin &&
                (ts->kind == ast::TypeDeclKind::Record ||
                 ts->kind == ast::TypeDeclKind::Class ||
                 ts->kind == ast::TypeDeclKind::Choice)) {
                ctx_.diagnostics().report(
                    Severity::Error, be.span.begin,
                    "operator `" + std::string(op) +
                    "` is not defined for type `" + std::string(ts->name) +
                    "` with these operands — declare a matching `operator " +
                    std::string(op) +
                    "` whose parameters match the operand order (Bezel does not "
                    "auto-commute operator overloads) (§25)");
                if (auto* opTy = ctx_.exprType(*operand))
                    ctx_.setExprType(&be, opTy);
                return;
            }
        }
    }
    // fix #3929 (§25/§11.8, v0.294): an Interface operand on an
    // arithmetic/bitwise/shift operator gets the SAME dedicated diagnostic
    // as the relational case above (issue #3952) — see that block's comment
    // for the full rationale (three genuinely different "no applicable
    // overload" reasons — Object/record-class-choice/interface — get three
    // messages, not a merge). This loop walks both operands, closing the
    // order-asymmetric gap the arithmetic/bitwise backstops below have (they
    // only ever inspect the LEFT operand) for interface operands specifically
    // (the Choice case above already has the same both-operand coverage).
    if (op == "+" || op == "-" || op == "*" || op == "/" || op == "mod" ||
        op == "&" || op == "|" || op == "^" || op == "<<" || op == ">>") {
        for (const ast::Expr* operand : {be.left.get(), be.right.get()}) {
            TypeSymbol* ts = memberTargetType(operand);
            if (ts && ts->kind == ast::TypeDeclKind::Interface) {
                ctx_.diagnostics().report(
                    Severity::Error, be.span.begin,
                    "operator `" + std::string(op) +
                    "` is not defined for type `" + std::string(ts->name) +
                    "` (\xC2\xA7""25) \xE2\x80\x94 even where `" +
                    std::string(ts->name) +
                    "` or a conforming type declares a matching static "
                    "operator, it is reachable only through a generic type "
                    "parameter constrained by `" + std::string(ts->name) +
                    "`, never through a plain interface-typed value "
                    "(\xC2\xA7""11.8: \xE2\x80\x9Cthrough an interface-typed "
                    "value only instance members are reachable\xE2\x80\x9D; "
                    "\xE2\x80\x9CT.Member(...) dispatch requires a generic "
                    "type parameter constrained by the "
                    "interface\xE2\x80\x9D). Narrow to the concrete "
                    "conforming type first (`is`/`as`, \xC2\xA7""17.4), or "
                    "constrain a generic parameter by `" +
                    std::string(ts->name) + "` instead.");
                if (auto* opTy = ctx_.exprType(*operand))
                    ctx_.setExprType(&be, opTy);
                return;
            }
        }
    }
    // §8.4 / §10 (#1137): enum comparison (`=`/`<>`/`<`/`<=`/`>`/`>=`) is an
    // underlying-integer comparison between operands of the SAME enum type (spec
    // §8.4 table: "Enum | Underlying integer comparison"). The operands are
    // reduced to their underlying integers before any type check, so an
    // `enum = int` or `enumA = enumB` (different enums) silently compiled as a
    // raw int comparison. Require both operands to be the same enum: if either
    // operand is an enum, the other must be the same enum (a non-enum or a
    // different enum is a compile error). A `null` literal operand is exempt —
    // null comparison of a nullable enum (`c = null`) is governed by §8.4's
    // null-safe equality, not this rule.
    if (op == "=" || op == "<>" || op == "<" || op == ">" ||
        op == "<=" || op == ">=") {
        const TypeRef* lt = be.left ? ctx_.exprType(*be.left) : nullptr;
        const TypeRef* rt = be.right ? ctx_.exprType(*be.right) : nullptr;
        auto enumOf = [&](const TypeRef* t) -> const TypeSymbol* {
            if (t && t->kind == TypeRef::Kind::Named && t->declaration &&
                t->declaration->kind == ast::TypeDeclKind::Enum)
                return t->declaration;
            return nullptr;
        };
        bool leftNull = be.left &&
            dynamic_cast<const ast::NullLiteralExpr*>(be.left.get());
        bool rightNull = be.right &&
            dynamic_cast<const ast::NullLiteralExpr*>(be.right.get());
        const TypeSymbol* le = enumOf(lt);
        const TypeSymbol* re = enumOf(rt);
        // Only enforce when BOTH operands have a RESOLVED type — an unresolved
        // operand (e.g. `this` inside an enum method, or an error expression) is
        // left to other diagnostics rather than risk a false positive.
        if (lt && rt && !leftNull && !rightNull && (le || re) && le != re) {
            std::string other = le ? (re ? std::string(re->name) : "a non-enum")
                                   : "a non-enum";
            ctx_.diagnostics().report(
                Severity::Error, be.span.begin,
                "operator `" + std::string(op) + "` compares enum `" +
                std::string(le ? le->name : re->name) + "` with " +
                (le && re ? ("a different enum `" + other + "`")
                          : std::string("a non-enum operand")) +
                " — an enum may be compared only with a value of the same enum "
                "type (\xC2\xA7""8.4 / \xC2\xA7""10).");
            ctx_.setExprType(&be, ctx_.primitiveTypeRef(PrimitiveType::Bool));
            return;
        }
    }
    // §9.2 (v0.156)/§8.3: `=` / `<>` on a body record that is not auto-equality-
    // bearing and that declares no explicit operator is a compile error at the
    // use site.  Disqualifiers include a non-static explicit field and — per
    // §9.2 v0.156 — an `override` of `Equals` or `GetHashCode`.  (v0.182,
    // #1572: a computed-property body and a constructor body — however
    // non-trivial — are NOT disqualifiers; equality/hashing/ToString are
    // defined over storage only, so no member's body can affect them.)  A user
    // `=`/`<>` overload would already have been selected by findBinaryOperator
    // above (so we never reach here for those); recordHasUserDeclaredEquality
    // only returns true for operator overloads now (not bare Equals/GetHashCode
    // — see #1249).  Positional records, enums, and eligible body records are
    // unaffected.
    if (op == "=" || op == "<>") {
        for (const ast::Expr* operand : {be.left.get(), be.right.get()}) {
            TypeSymbol* ts = memberTargetType(operand);
            if (ts && ts->kind == ast::TypeDeclKind::Record &&
                !isAutoEqualityBearingBodyRecord(ts, ctx_) &&
                !recordHasUserDeclaredEquality(ts)) {
                // §9.3 (v0.158, #1383): positional/combined-form records are no
                // longer unconditionally skipped. A positional record whose
                // fields fall outside the admissible-component rule gets the same
                // use-site error as a non-auto-hashable body record (§9.3
                // "use-site-error discipline"). For LOCAL records the flag lives
                // on the ast::TypeDecl; for IMPORTED records (no ast node) it
                // lives on TypeSymbol itself (issue #501 fix). fix #3600: the
                // shared both-sources accessor.
                bool positional = recordIsPositional(ts);
                bool isEq = positional
                                ? isPositionalRecordEqualityBearing(ts, ctx_)
                                : false;
                if (!isEq) {
                    ctx_.diagnostics().report(
                        Severity::Error, be.span.begin,
                        "`" + std::string(op) + "` is not available on record `" +
                        std::string(ts->name) +
                        "`: it is not auto-hashable (§9.2/§9.3). Declare"
                        " `=` and `<>` overloads (with `Equals`/`GetHashCode`),"
                        " or simplify the record to an auto-hashable form.");
                    break;
                }
            }
            // §35.7 (v0.200, #2045; v0.226, #2110): a choice draws §9.2's
            // use-site compile error on `=`/`<>` exactly when it is NOT
            // auto-hashable — i.e. any borrow-bearing or owning choice,
            // AND any value-like choice whose case payload fails the §9.2
            // admissible-component rule (§35.7's recursive clause: "any
            // value-like choice with an inadmissible payload"). The predicate
            // is the SAME isAutoEqualityBearingChoice that drives
            // isEqualityBearing, the §14.2 `where T: hashable` constraint, and
            // checkHashContainerKeys, so all consumers move together — the
            // former classification-only "recursion is deferred" note is
            // retired. (A function-typed or `Object` payload is
            // classification-neutral per §35.4 — still ValueLike — yet outside
            // the §9.2 admissible-component list, so it reaches here.)
            if (ts && ts->kind == ast::TypeDeclKind::Choice) {
                // §35.7 / §28.1 (#2047): classify at the operand's INSTANTIATION
                // so `Result<int, string>` is value-like (the `owned` markers
                // erase over int/string) — hence auto-hashable — while
                // `Result<Connection, string>` stays owning. The operand's
                // resolved type carries the concrete generic args; fall back to a
                // bare declaration ref for a non-generic choice / missing type.
                sema::TypeRef bare;
                const sema::TypeRef* opTy = ctx_.exprType(*operand);
                if (!opTy || opTy->kind != sema::TypeRef::Kind::Named ||
                    opTy->declaration != ts) {
                    bare.kind = sema::TypeRef::Kind::Named;
                    bare.declaration = ts;
                    opTy = &bare;
                }
                // §35.7 (v0.226, #2110): the recursive admissibility check
                // lives inside isAutoEqualityBearingChoice; the classification
                // is consulted only to phrase the diagnostic (owning /
                // borrow-bearing vs value-like-with-inadmissible-payload).
                //
                // v0.276 / #3333 widened isAutoEqualityBearingChoice to ALSO
                // return false on a value-like choice with a homogeneous
                // `=`/`<>` overload, but the binary `=`/`<>` use-site path
                // is unreachable on such a choice: findBinaryOperator above
                // already dispatched to the user's overload when one was
                // declared (a choice with `=`/`<>` overload and no
                // co-declared pair gets the §25.2 co-declaration error
                // separately, not this one). The rescue clause
                // (choiceHasUserDeclaredEquality) therefore matters ONLY on
                // the direct-call path — see ExprTypecheck_Calls.cpp's
                // checkEqualityMemberUseSite, which is OR'd with the
                // choice-side predicate. The binary expression site keeps
                // its original single-predicate shape.
                if (!isAutoEqualityBearingChoice(ts, ctx_, opTy)) {
                    auto verdict = common::classifyChoiceType(opTy, ctx_);
                    std::string reason;
                    if (verdict &&
                        *verdict != common::ChoiceClassification::ValueLike) {
                        reason =
                            std::string("it is not auto-hashable "
                                        "(§35.7) — the choice is ") +
                            (*verdict == common::ChoiceClassification::Owning
                                 ? "owning"
                                 : "borrow-bearing") +
                            ", and a non-value-like choice carries no "
                            "value equality";
                    } else {
                        // Value-like yet still not auto-hashable:
                        // a case payload failed the §9.2 admissible-component
                        // rule (§35.7's recursive clause). The choice
                        // cannot have a homogeneous `=`/`<>` overload here
                        // — findBinaryOperator dispatched to it above when
                        // one was declared — so the cause is uniquely the
                        // per-payload rule.
                        reason =
                            "it is not auto-hashable (§35.7): the "
                            "choice is value-like but a case payload is not "
                            "an admissible equality component (§9.2)";
                    }
                    ctx_.diagnostics().report(
                        Severity::Error, be.span.begin,
                        "`" + std::string(op) + "` is not available on choice `" +
                        std::string(ts->name) + "`: " + reason +
                        ". Declare `=` and `<>` overloads (with "
                        "`Equals`/`GetHashCode`), or restructure the choice "
                        "payload to an admissible component type.");
                }
            }
        }
    }
    // §7.11 "No three-valued logic": `and` / `or` require both operands to be
    // non-nullable `bool`. A `bool?` operand is a compile error.
    if (op == "and" || op == "or") {
        auto isBoolNullable = [&](const ast::Expr* e) -> bool {
            if (!e) return false;
            const TypeRef* t = ctx_.exprType(*e);
            return t && t->kind == TypeRef::Kind::Primitive &&
                   t->primitive == PrimitiveType::Bool && t->nullable;
        };
        if (isBoolNullable(be.left.get()) || isBoolNullable(be.right.get())) {
            ctx_.diagnostics().report(
                Severity::Error, be.span.begin,
                "operator `" + std::string(op) +
                "` requires non-nullable `bool` operands (§7.11); a `bool?`"
                " operand is not permitted — there is no three-valued logic."
                " Use `??` to coalesce the `bool?` to a `bool` first, or"
                " narrow with an explicit null check.");
            ctx_.setExprType(&be, ctx_.primitiveTypeRef(PrimitiveType::Bool));
            return;
        }
        // §6 (#1040): `and`/`or`/`not` have boolean-only semantics, so a
        // NON-bool operand (int/uint/char/string/class/...) is a compile error.
        // Without this it slipped through here — the result was typed `bool`
        // below — and crashed codegen, which builds the short-circuit as a PHI
        // and hit an LLVM PHINode "operands must be the same type" assertion on
        // the non-bool operand values (`int(u and 255)` / `int(5 and 3)`).
        // (The nullable-`bool` case is handled above; here a KNOWN type that is
        // not a `bool` primitive is rejected. Untyped/error operands are left to
        // their own diagnostics to avoid cascading.) Bitwise integer ops use
        // `&`/`|`/`^` (§6.5), which are distinct operators.
        auto isNonBool = [&](const ast::Expr* e) -> bool {
            if (!e) return false;
            const TypeRef* t = ctx_.exprType(*e);
            return t && !(t->kind == TypeRef::Kind::Primitive &&
                          t->primitive == PrimitiveType::Bool);
        };
        if (isNonBool(be.left.get()) || isNonBool(be.right.get())) {
            ctx_.diagnostics().report(
                Severity::Error, be.span.begin,
                "operator `" + std::string(op) +
                "` requires `bool` operands (§6: `and`/`or`/`not` have"
                " boolean-only semantics). Use the bitwise operators"
                " `&`/`|`/`^` for integer bit operations.");
            ctx_.setExprType(&be, ctx_.primitiveTypeRef(PrimitiveType::Bool));
            return;
        }
    }
    if (op == "=" || op == "<>" || op == "<" || op == ">" ||
        op == "<=" || op == ">=" || op == "and" || op == "or") {
        // §7.11 / proposal 0114 / v0.267 (#3298): relational operators do not
        // lift over a nullable value type — reject BEFORE the §8.4 relational
        // result-type rule below, which would silently type it `bool` on the
        // old read and is the spec's "fail-open guard" defect §18.5 names. A
        // nullable primitive operand (`int?`, `byte?`, `double?`, …) read as
        // non-nullable by §7.4 flow narrowing is a non-nullable operand here
        // and the operator applies; the `narrowedNonNull` gate mirrors the
        // #1575 nullable-string guard's static-type approach.
        if (op == "<" || op == ">" || op == "<=" || op == ">=") {
            TypeRef* ltN = be.left  ? ctx_.exprType(*be.left)  : nullptr;
            TypeRef* rtN = be.right ? ctx_.exprType(*be.right) : nullptr;
            // §7.11: "an operand whose static type is a nullable VALUE type
            // is a compile error at every … relational operator". An ENUM is a
            // value type (§10), and §10.3 gives it `<`/`<=`/`>`/`>=` over the
            // underlying integer — so `Code? < Code?` is inside this rule, not
            // beside it. Keying on `Primitive` alone let it through to a
            // codegen that no longer has a lift path, and it died in IR
            // verification (`icmp ult {i8,i8}`). Records and choices reach the
            // user-defined-operator gate above instead, which already rejects
            // a nullable operand with the same §7.11 wording.
            auto isNullableValueOperand = [](const TypeRef* t) {
                if (!t || !t->nullable) return false;
                if (t->kind == TypeRef::Kind::Primitive) return true;
                return t->kind == TypeRef::Kind::Named && t->declaration &&
                       t->declaration->kind == ast::TypeDeclKind::Enum;
            };
            // §7.11 reads the operand's STATIC type at this program point, so a
            // §7.4-narrowed operand is a non-nullable operand here — the ONE
            // narrowing predicate (#3298), not a local copy of it.
            bool lNull = isNullableValueOperand(ltN) &&
                         !isNarrowedNonNullRead(be.left.get());
            bool rNull = isNullableValueOperand(rtN) &&
                         !isNarrowedNonNullRead(be.right.get());
            if (lNull || rNull) {
                ctx_.diagnostics().report(
                    Severity::Error, be.span.begin,
                    std::string("operator `") + std::string(op) +
                    "` is not defined on a nullable value type \xE2\x80\x94 narrow "
                    "first (`x <> null and \xE2\x80\xA6` (\xC2\xA7""7.4), `??` "
                    "(\xC2\xA7""7.7), or `!` (\xC2\xA7""7.8)); the relational "
                    "operators do not lift over `T?` (\xC2\xA7""7.11, v0.267)");
                ctx_.setExprType(&be,
                                 ctx_.primitiveTypeRef(PrimitiveType::Bool));
                return;
            }
        }
        // per §6.2/§5.1 (#1204) and §17.3/§8.4 (fix #4501, v0.306):
        // relational and equality operators require the two operand types to
        // be compatible — one must implicitly convert to the other (§5.1).
        // This arm and the §6.2 arithmetic-promotion arm below are now ONE
        // call rather than two near-identical copies: see
        // `reportNoImplicitConversionBetweenOperands` for the rule, the four
        // measured rows the shared hole admitted (`string = 0`, `bool = 0`,
        // `bool = "x"`, `decimal = "x"`), and why `Object`, nullable operands
        // and the §5.1 constant-narrowing escape (`ulong > 1000`) stay
        // admitted. `and`/`or` are excluded: their operands are checked
        // against `bool` by the §7.11/§6 gates above, not by a conversion.
        if (op != "and" && op != "or") {
            TypeRef* lt = be.left  ? ctx_.exprType(*be.left)  : nullptr;
            TypeRef* rt = be.right ? ctx_.exprType(*be.right) : nullptr;
            if (reportNoImplicitConversionBetweenOperands(
                    be, op, lt, rt, /*allowLiteralAdoption=*/true)) {
                ctx_.setExprType(&be, ctx_.primitiveTypeRef(PrimitiveType::Bool));
                return;
            }
        }
        ctx_.setExprType(&be, ctx_.primitiveTypeRef(PrimitiveType::Bool));
        return;
    }
    // `in` is a membership test → bool.
    if (op == "in") {
        // §17.5 / §16.6 / §7.11 (v0.286, #3298): `x in c` IS the call
        // `c.Contains(x)`, so the left operand is admitted exactly when that
        // call's argument admits it — `in` states no nullability rule of its
        // own (v0.270, #3312.2). For a RANGE right operand the selected member
        // is stdlib §9.8's `Contains(value: T)` over the NON-NULLABLE `T` of
        // §16.3, `T?` does not implicitly convert to `T` (§5.1), and range
        // membership is an ORDERING test which admits no absent participant
        // (§16.3, §7.11) — so a nullable left operand is a compile error,
        // naming §7.11's standing remedies. Without this gate the operand
        // reached codegen as a `{i8, T}` box and `n in 1..5` over an `int?`
        // died in IR verification (`icmp {i8,i32}, i32`).
        //
        // The gate is deliberately keyed on the RANGE receiver and NOT on `in`
        // as such: v0.270 ruled that a `Contains` whose parameter is ITSELF
        // nullable — `Array<int?>.Contains(item: int?)`, the `Span`/
        // `ReadOnlySpan` forms at class-typed `T` — still admits a nullable
        // left operand, because that test is §8.4 equality, which §7.11
        // retains. A blanket rule would make `arr.Contains(n)` compile and
        // `n in arr` not, for the same question.
        //
        // §16.6's parity claim is what this delivers: `n in 1..5` and
        // `case n of 1..5` over an `int?` now give the SAME verdict (the
        // diagnostics differ, as §16.6 says they may; the verdict agrees).
        TypeRef* inLt = be.left ? ctx_.exprType(*be.left) : nullptr;
        TypeRef* inRt = be.right ? ctx_.exprType(*be.right) : nullptr;
        if (inLt && inLt->nullable && inRt &&
            inRt->kind == TypeRef::Kind::Named &&
            inRt->declaration == ctx_.builtinClassSym("Range") &&
            !isNarrowedNonNullRead(be.left.get())) {
            ctx_.diagnostics().report(
                Severity::Error, be.span.begin,
                "the left operand of `in` over a range is nullable \xE2\x80\x94 "
                "a range's `Contains` takes the non-nullable element type "
                "(stdlib \xC2\xA7""9.8) and range membership is an ordering "
                "test, which admits no absent participant (\xC2\xA7""16.3, "
                "\xC2\xA7""16.6, \xC2\xA7""7.11); narrow first "
                "(`x <> null and \xE2\x80\xA6` (\xC2\xA7""7.4), `??` "
                "(\xC2\xA7""7.7), or `!` (\xC2\xA7""7.8))");
        }
        ctx_.setExprType(&be, ctx_.primitiveTypeRef(PrimitiveType::Bool));
        return;
    }
    // Arithmetic / shift / bitwise: result type follows the left
    // operand's type when both sides are present and known.
    if (op == "+" || op == "-" || op == "*" || op == "/" ||
        op == "mod" || op == "&" || op == "|" || op == "^" ||
        op == "<<" || op == ">>") {
        TypeRef* lt = be.left ? ctx_.exprType(*be.left) : nullptr;
        // §4.6 (spec ruling): `s + t` concatenates two STRINGS. The compiler does
        // not implicitly convert other types to `string` — `"x=" + 7` was
        // silently converted, `"v=" + 1.5` IR-failed (`sitofp ptr`), and
        // `42 + " h"` returned garbage. Require both operands to be string; reject
        // a mixed `string + <non-string>` (or reverse) and point at the remedies.
        if (op == "+") {
            TypeRef* rt0 = be.right ? ctx_.exprType(*be.right) : nullptr;
            auto isStr = [](const TypeRef* t) {
                return t && t->kind == TypeRef::Kind::Primitive &&
                       t->primitive == PrimitiveType::String;
            };
            if (isStr(lt) != isStr(rt0)) {  // exactly one operand is string
                ctx_.diagnostics().report(
                    Severity::Error, be.span.begin,
                    "`+` requires both operands to be `string` — the compiler "
                    "does not implicitly convert other types to `string`; use "
                    "interpolation (`$\"...{x}...\"`) or `.ToString()` (§4.6)");
                auto* sTy = ctx_.makeTypeRef();
                sTy->kind = TypeRef::Kind::Primitive;
                sTy->primitive = PrimitiveType::String;
                ctx_.setExprType(&be, sTy);
                return;
            }
        }
        // §2.5 (stdlib L264–265): TimeSpan + TimeSpan → TimeSpan and
        // TimeSpan - TimeSpan → TimeSpan are compiler-intrinsic operators
        // (TimeSpan is a builtin value type backed by a scalar i64 tick count;
        // these operations are plain i64 add/sub). They are declared as
        // `public static method operator +/-(a,b: TimeSpan): TimeSpan` in the
        // stdlib but are NOT user-defined operators in any Bezel source body, so
        // findBinaryOperator cannot find them. Intercept here before the
        // user-Named-type guard below. The operator * (TimeSpan * double) is
        // deliberately excluded — it requires cross-type widening that is
        // deferred per the existing datetime_timespan_test.cpp note.
        if (lt && lt->kind == TypeRef::Kind::Named &&
            lt->declaration && !lt->declaration->decl &&
            lt->declaration->name == "TimeSpan" &&
            (op == "+" || op == "-")) {
            auto* tsTy = ctx_.makeTypeRef();
            tsTy->kind = TypeRef::Kind::Named;
            tsTy->declaration = builtinClassSym("TimeSpan");
            ctx_.setExprType(&be, tsTy);
            return;
        }
        // §25: arithmetic operators (+, -, *, /, mod) are NOT defined on
        // user class/record types unless the type declares a matching
        // `static method operator <op>` in its own body. `findBinaryOperator`
        // already searched the operand types above and found nothing; if we
        // arrive here with a Named left-operand type, there is no applicable
        // overload and the expression is a compile error. (Bitwise/shift ops
        // have their own guard below; string `+` passes through on a Primitive
        // String type which is not Named, so this does not affect it.)
        if (lt && lt->kind == TypeRef::Kind::Named &&
            (op == "+" || op == "-" || op == "*" || op == "/" || op == "mod")) {
            // fix #3930 (§25.2/§11.8, v0.295): `lt->declaration` is null both
            // for a bare, unconstrained generic type parameter AND for a
            // null-declaration builtin (`Array<T>`, `IntPtr`, …) — the code
            // below folded both into the literal placeholder string "?",
            // which for a type parameter is doubly wrong: it hides the real
            // name (`T`, not "?") AND the remedy text ("declare `static
            // method operator +(a: ?, b: ?)` inside `?`") is unactionable —
            // a type parameter declares no members of its own (§11.8), so
            // nothing can be declared "inside" one. `isInScopeTypeParam`
            // (the SAME test the §11.8 constraint-dispatch block above uses
            // via its own `typeParamNameOf` lambda) tells the two null-
            // declaration shapes apart, so this branch fires ONLY for the
            // real type-parameter case and leaves the builtin one on the
            // pre-existing "?" wording below unchanged. Mirrors the ordering-
            // operator diagnostic (`< > <= >=`) a few hundred lines above,
            // same §11.8 authority: a type parameter's operator candidate
            // set is drawn from its static-interface constraints, not the
            // parameter itself.
            if (isInScopeTypeParam(lt->headName())) {
                const std::string tp(lt->headName());
                if (op == "mod") {
                    // Unlike the other four, NEITHER stdlib static-interface
                    // abstraction declares `mod` (`INumber<T>` has `+ - * /`
                    // and `<`; `ISteppable<T>` has `Step` and `<`) — so there
                    // is no single existing bound to name; the real remedy is
                    // a user-declared interface with a matching operator.
                    ctx_.diagnostics().report(
                        Severity::Error, be.span.begin,
                        "operator `mod` is not available on type parameter `" +
                        tp + "` — a type parameter declares no operators of "
                        "its own, and no stdlib static-interface abstraction "
                        "declares `mod` (`INumber<" + tp + ">`/`ISteppable<" +
                        tp + ">` both omit it); declare a `static operator "
                        "mod(a: " + tp + ", b: " + tp + "): " + tp +
                        "` on your own interface and add a matching `where " +
                        tp + ": ...` bound (\xC2\xA7""25.2/\xC2\xA7""11.8)");
                } else {
                    ctx_.diagnostics().report(
                        Severity::Error, be.span.begin,
                        "operator `" + std::string(op) + "` is not available "
                        "on type parameter `" + tp + "` — a type parameter "
                        "declares no operators of its own, and none of its "
                        "constraints declares an arithmetic operator; add a "
                        "`where " + tp + ": INumber<" + tp + ">` bound "
                        "(\xC2\xA7""25.2/\xC2\xA7""11.8)");
                }
                return;
            }
            std::string typeName =
                lt->declaration ? std::string(lt->declaration->name) : "?";
            ctx_.diagnostics().report(
                Severity::Error, be.span.begin,
                std::string("operator `") + std::string(op) +
                "` is not defined for type `" + typeName +
                "` (§25); declare `static method operator " + std::string(op) +
                "(a: " + typeName + ", b: " + typeName + ")` inside `" +
                typeName + "` to overload it");
            return;
        }
        // §6.5 / §6.1 / §17.1: integer-operand operator type checks.
        //   • Bitwise/shift (`& | ^ << >>`) are "not valid on `bool`, `char`, or
        //     floating-point types" (§6.5) — they require integer operands, and
        //     boolean operations use `and`/`or`/`not`. Reject all three operand
        //     kinds (§7.11's lift list applies only where the base operator is
        //     valid for `T`, so it does not extend bitwise to `bool?`/`char?`).
        //   • `mod` is defined on integer types and decimal (§6.1), but NOT on
        //     `float`/`double`, `bool`, or `string` (§3.5: mod is admitted on
        //     the integer types and `decimal`; invalid on `bool`, `char`, and
        //     `string`). `char mod char` is also rejected (narrow #3109), but
        //     `char mod <int>` and `<int> mod char` are admitted via §5.1's
        //     char→int widening — the cast pattern `s[0] mod 256` is canonical.
        // (String/reference bitwise keeps its existing concatenation hint.)
        {
            const bool isBitwise = (op == "&" || op == "|" || op == "^" ||
                                    op == "<<" || op == ">>");
            TypeRef* rt0 = be.right ? ctx_.exprType(*be.right) : nullptr;
            auto primOf = [](TypeRef* t) -> PrimitiveType {
                return (t && t->kind == TypeRef::Kind::Primitive)
                           ? t->primitive : PrimitiveType::None;
            };
            auto isFloatPrim = [](PrimitiveType p) {
                return p == PrimitiveType::Float || p == PrimitiveType::Double;
            };
            // String/reference left operand keeps the concatenation hint.
            if (isBitwise && lt &&
                ((lt->kind == TypeRef::Kind::Primitive &&
                  lt->primitive == PrimitiveType::String) ||
                 lt->kind == TypeRef::Kind::Named)) {
                ctx_.diagnostics().report(Severity::Error, be.span.begin,
                    std::string("operator `") + std::string(op) +
                    "` requires integer operands; use `+` for string concatenation");
                return;
            }
            if (isBitwise) {
                for (TypeRef* o : {lt, rt0}) {
                    PrimitiveType p = primOf(o);
                    // §6.5: not valid on bool, char, or floating-point.
                    const char* badKind =
                        isFloatPrim(p)               ? "floating-point types"
                        : (p == PrimitiveType::Bool) ? "`bool` (boolean operations "
                                                       "use `and`/`or`/`not`)"
                        : (p == PrimitiveType::Char)  ? "`char`"
                                                      : nullptr;
                    if (badKind) {
                        ctx_.diagnostics().report(Severity::Error, be.span.begin,
                            std::string("operator `") + std::string(op) +
                            "` is not valid on " + badKind + " — bitwise and "
                            "shift operators require integer operands (§6.5)");
                        return;
                    }
                }
            }
            if (op == "mod") {
                for (TypeRef* o : {lt, rt0}) {
                    if (isFloatPrim(primOf(o))) {
                        ctx_.diagnostics().report(Severity::Error, be.span.begin,
                            "`mod` is not defined on `float` or `double` — use "
                            "`Math.Fmod` or `Math.IEEERemainder` (§6.1)");
                        return;
                    }
                }
                // #3243 (v0.256 follow-up): the binary `mod` arm previously
                // rejected ONLY `char mod char` (narrow #3109); `bool` and
                // `string` on EITHER side fell through to codegen and aborted
                // with an IR-verify failure. §3.5 / §6.1 admit `mod` only on
                // the integer types and `decimal` — `bool` and `string` have no
                // implicit conversion to an integer type, so widening cannot
                // rescue them. Reject on either side; the wording parallels
                // the compound `mod=` arm (ExprTypecheck.cpp ~11940).
                PrimitiveType lp = primOf(lt);
                PrimitiveType rp = primOf(rt0);
                const char* badKind =
                    (lp == PrimitiveType::Bool || rp == PrimitiveType::Bool)
                        ? "`bool` (boolean operations use `and`/`or`/`not`)"
                    : (lp == PrimitiveType::String || rp == PrimitiveType::String)
                        ? "`string`" : nullptr;
                if (badKind) {
                    ctx_.diagnostics().report(Severity::Error, be.span.begin,
                        std::string("`mod` is not valid on ") + badKind +
                        " — `mod` is defined on the integer types and `decimal` "
                        "(§6.1)");
                    return;
                }
                // NAARROW #3109 (v0.256): reject ONLY `char mod char` (both
                // sides char). The cast pattern `char mod <int>` (e.g. `s[0]
                // mod 256` for exit codes) is admitted via §5.1 widening; the
                // over-broad fix (de3cd71, reverted) broke those tests.
                if (lp == PrimitiveType::Char && rp == PrimitiveType::Char) {
                    ctx_.diagnostics().report(Severity::Error, be.span.begin,
                        "`mod` is not defined on `char` operands (§6.1); a "
                        "`char` remainder has neither a defined result type "
                        "nor use — convert via `int(c) mod int(d)` (§5.2)");
                    return;
                }
            }
            // §6.5 / §17.14 (v0.242, #3045): both operands are now known to be
            // integer-kinded, so the shift count's DOMAIN can be checked. It is
            // `0 .. width−1` of the LEFT operand's own type — §6.5 excludes the
            // count from §6.2's promotion, so `byte << 8` is out of domain even
            // though 8 fits an `int`. A constant out-of-domain count is a
            // compile error here; a runtime one raises `OverflowException`
            // (§31.4), emitted by codegen. Both replace the pre-v0.242 silent
            // masking (`count mod width`).
            if ((op == "<<" || op == ">>") && be.right)
                checkShiftCountDomain(lt, *be.right, op, be.span.begin);
        }
        // §7.11 / proposal 0114 / v0.267 (#3298): arithmetic, bitwise, shift,
        // and relational operators do not lift over a nullable value type.
        // Either operand's static type being a nullable primitive (`int?`,
        // `double?`, `bool?`, …) is a compile error; `=`/`<>` are excluded
        // above (§8.4) and `and`/`or`/`not` above (§17.2). The diagnostic
        // names the three remedies the language uses at every other nullable-
        // meets-non-nullable position: a §7.4 narrowing check, `??` (§7.7),
        // or `!` (§7.8). Because the rule reads the operand's static type at
        // that program point, an operand narrowed non-null by §7.4 flow
        // narrowing is a non-nullable operand here and the ordinary operator
        // applies unchanged — no separate rule is needed for that case.
        if (be.op != "=" && be.op != "<>") {
            TypeRef* lt = be.left ? ctx_.exprType(*be.left) : nullptr;
            TypeRef* rt = be.right ? ctx_.exprType(*be.right) : nullptr;
            auto isNullablePrim = [](const TypeRef* t) {
                return t && t->kind == TypeRef::Kind::Primitive && t->nullable;
            };
            // An operand narrowed non-null by §7.4 is a non-nullable operand
            // at this program point even though exprType keeps the nullable
            // flag; `isNarrowedNonNullRead` is the ONE predicate that decides
            // that (#3298) — mutable fields and properties never narrow (§7.4),
            // and `this`/`base` are not narrowable operands.
            bool lNull = isNullablePrim(lt) &&
                         !isNarrowedNonNullRead(be.left.get());
            bool rNull = isNullablePrim(rt) &&
                         !isNarrowedNonNullRead(be.right.get());
            // Per §3.5 only the §7.11-gated operator families are subject to
            // this rule. Equality (`=`/`<>`) is excluded above. User-declared
            // operators on a nullable user-type operand are rejected at the
            // candidate-set gate further up (operandMatchesParam's nullability
            // gate for primitives, the existing user-operator diagnostic above
            // for Named types). A nullable reference type (`string?`) is
            // already rejected for relational operators above (#1575).
            if ((lNull || rNull) && lt && rt &&
                lt->kind == TypeRef::Kind::Primitive &&
                rt->kind == TypeRef::Kind::Primitive) {
                // Identify the operator family for the diagnostic. Binary
                // shifts (`<<`/`>>`) are family "shift"; relational (`<`
                // `<=` `>` `>=`) are "relational"; everything else on the
                // primitive arithmetic/bitwise set is "arithmetic / bitwise".
                const char* opFamily =
                    (op == "<<" || op == ">>") ? "shift" :
                    (op == "<" || op == "<=" ||
                     op == ">" || op == ">=") ? "relational" :
                    "arithmetic, bitwise, or shift";
                ctx_.diagnostics().report(
                    Severity::Error, be.span.begin,
                    std::string("operator `") + std::string(op) +
                    "` is not defined on a nullable value type — narrow first "
                    "(`x <> null and …` (\xC2\xA7""7.4), `??` (\xC2\xA7""7.7), "
                    "or `!` (\xC2\xA7""7.8)); the " + opFamily +
                    " operators do not lift over `T?` (\xC2\xA7""7.11, v0.267)");
                // Error recovery: type the result as the non-nullable T of the
                // non-nullable operand (or `int` if both are nullable) so
                // downstream uses do not cascade an untyped-expression error.
                TypeRef* recTy = nullptr;
                if (lt && !lt->nullable) recTy = lt;
                else if (rt && !rt->nullable) recTy = rt;
                else recTy = ctx_.primitiveTypeRef(PrimitiveType::Int);
                ctx_.setExprType(&be, recTy);
                return;
            }
        }
        // §7.11 (v0.286, #3298): past the gate above, a surviving nullable
        // primitive operand is one §7.4 has narrowed non-null, and §7.11 reads
        // it as a NON-NULLABLE operand to which "the ordinary operator applies
        // unchanged". Normalize both operand types ONCE here so that §6.2's
        // promotion and the result type below need no narrowing special case:
        // without this, `if n <> null then m := n + 1;` types the sum `int?`
        // and the store into `m: int` is a false reject, and a narrowed
        // `int? + long` never reaches §6.2's widening at all.
        TypeRef* rtNorm = be.right ? ctx_.exprType(*be.right) : nullptr;
        lt      = operandStaticTypeAt(be.left.get(), lt);
        rtNorm  = operandStaticTypeAt(be.right.get(), rtNorm);
        // §6.2 mixed-type arithmetic: "the smaller-range operand is implicitly
        // promoted to the larger type using §5.1. The result has the larger
        // type." For the non-shift operators, when both operands are primitive
        // numerics of different types, the result is the wider operand — the
        // one the other implicitly widens to per §5.1. (Shift ops `<<`/`>>`
        // keep the left operand's type: the right operand is a shift count,
        // not an arithmetic operand, so no promotion applies.)
        // per §5.1 / §6.2: when both operands are primitive numerics of
        // different types, the result is the wider operand. Shift ops `<<`/`>>`
        // keep the left operand's type (right is the shift count, no promotion).
        if (op != "<<" && op != ">>") {
            TypeRef* rt = rtNorm;   // §7.11-normalized (#3298), not the declared type
            if (lt && rt &&
                lt->kind == TypeRef::Kind::Primitive &&
                rt->kind == TypeRef::Kind::Primitive &&
                lt->primitive != rt->primitive &&
                !lt->nullable && !rt->nullable) {
                if (primitiveWidens(lt->primitive, rt->primitive)) {
                    // left widens to right → result is right (the wider).
                    ctx_.setExprType(&be, rt);
                    return;
                }
                if (primitiveWidens(rt->primitive, lt->primitive)) {
                    // right widens to left → result is left (the wider).
                    ctx_.setExprType(&be, lt);
                    return;
                }
                // §6.2: no implicit conversion between the operand types
                // (e.g. `double` and `decimal`, `float` and `decimal`,
                // signed/unsigned same-or-narrower-width pairs) — and, since
                // fix #4501, any other primitive pair §5.1 does not relate
                // (`string * int` died in LLVM's IR verifier before). This is
                // the SAME call the equality/relational arm above makes; the
                // two copies that used to answer it independently shared one
                // hole, which is what made both #4500's and #4501's rows
                // invisible.
                //
                // fix #4512 (§5.1/§6.2, v0.306): `allowLiteralAdoption` is now
                // `true` here too. §5.1's constant-narrowing paragraph states
                // it "applies at every position where this section's implicit
                // conversions apply — there is no position carve-out", and
                // arithmetic is exactly such a position (§6.2 explicitly
                // promotes "using §5.1"); the old asymmetry (relational
                // admitted `ulong > 1000`, arithmetic rejected `ulong + 1000`)
                // had no textual authority — it was `reportNoImplicitConversion
                // BetweenOperands`'s ORIGINAL literal escape's own asymmetry
                // (#1204 was written for the relational/equality arm only,
                // arithmetic was never visited), pinned only by this arm's own
                // test (`t6_arithmetic_arm_has_no_literal_escape_spec_6_2`,
                // now renamed/flipped). Safe to enable now that the escape
                // itself is exact to §5.1's own domain (fix #4512's other
                // half, `isLiteralDerivedExpr`/`isTrueIntegerType` above): it
                // can no longer let a float/decimal constant slip through a
                // pair §5.1 never relates, which is what made a naive flip of
                // this flag alone unsafe before that narrowing.
                if (reportNoImplicitConversionBetweenOperands(
                        be, op, lt, rt, /*allowLiteralAdoption=*/true))
                    return;
            }
        }
        // The result follows the (normalized) left operand: `lt` is already
        // the non-nullable T for a §7.4-narrowed operand, so the both-narrowed
        // `int? + int?` case and the mixed narrowed/non-nullable case take one
        // path instead of two (#3298).
        if (lt) ctx_.setExprType(&be, lt);
        return;
    }
    // `??` null-coalescing (§17.1 L12): the result is the non-nullable
    // form of the LHS — `a ?? b` is `a` when non-null, else `b`, and the
    // spec requires the RHS to be compatible with the non-nullable form
    // of the left (§ null-coalescing). Deriving the result from the LHS
    // (rather than the RHS) keeps the whole `??` typed even when the RHS
    // is a call or postfix-`!` expression whose own type is not recorded.
    if (op == "??") {
        TypeRef* lt = be.left ? ctx_.exprType(*be.left) : nullptr;
        // When the LHS is a null literal (typed as nullable Object) or is
        // untyped, prefer the RHS type — `null ?? x` has x's type.
        bool lhsIsNullLiteral =
            be.left && dynamic_cast<const ast::NullLiteralExpr*>(be.left.get());
        // #1104: the result is nullable iff the RHS is nullable — `a ?? b` is
        // `a`'s value when non-null, else `b`. When `b` is itself nullable
        // (`int? ?? int?`) the result CAN be null (both null), so it stays `T?`;
        // only when the RHS is non-nullable (`int? ?? 5`) does the result narrow
        // to the non-nullable `T`. Typing both-nullable as non-nullable made the
        // codegen value (`{i8,T}`) disagree with the slot type (bare `T`) on a
        // chained `(a ?? b) ?? c` → wrong value / PHI-mismatch crash.
        bool rhsNullable = false;
        if (be.right)
            if (TypeRef* rt = ctx_.exprType(*be.right))
                rhsNullable = rt->nullable;
        if (lt && !lhsIsNullLiteral) {
            if (lt->nullable && !rhsNullable) {
                TypeRef* nn = ctx_.makeTypeRef(*lt);
                nn->nullable = false;
                ctx_.setExprType(&be, nn);
            } else {
                // RHS nullable → keep LHS's nullability; or LHS already
                // non-nullable.
                ctx_.setExprType(&be, lt);
            }
        } else if (be.right) {
            // LHS untyped or null literal — fall back to the RHS type.
            if (TypeRef* rt = ctx_.exprType(*be.right))
                ctx_.setExprType(&be, rt);
        }
        return;
    }
    // Range ops, etc.: leave un-typed (later slices handle).
}

// §17.1 result-type table for unary operators (slice 3c).
// §25: a user-defined unary `operator <op>` (one parameter) declared on
// the operand's type. Overloadable prefix unary operators are `+`/`-`/`~`.
MethodSymbol* ExprTC::findUnaryOperator(const ast::UnaryExpr& ue) {
    // §25.1: the overloadable unary operators are `+`, `-`, `~` (and `++`/`--`,
    // which are handled at their statement/expression sites, not here). Unary
    // `+` was previously omitted, so a record's `operator +(c)` was never
    // resolved and `+c` silently lowered to the primitive identity — the user
    // overload was ignored. The `params.size() == 1` filter below distinguishes
    // the unary overload from the binary `operator +(a, b)`.
    if (ue.op != "+" && ue.op != "-" && ue.op != "~") return nullptr;
    TypeSymbol* ts = ue.operand ? memberTargetType(ue.operand.get())
                                : nullptr;
    if (!ts) return nullptr;
    // §25.2 (v0.181, #1562): "A unary operator consults the operand's type and
    // its base classes, likewise." Walk the inheritance chain so an operator
    // inherited from a base class is found for a derived-typed operand. §12.2's
    // per-token no-hiding rule keeps the chain duplicate-free; take the first
    // (nearest-declared) match.
    for (MethodSymbol* ms : collectInheritedOperators(ts, ue.op, 1))
        return ms;
    return nullptr;
}

// §28.8.1 / §28.11 (issue #423): classify the receiver of a `move coll[i]`.
// `move coll[i]` is admitted only on the enumerated CONCRETE owning receivers
// (§28.11: `OwningArray<T>`, `OwningDictionary<K, V>`, class-typed `OwningBuffer<T>`,
// class-typed `Span<T>`, and the owning inline arrays). An INTERFACE-typed
// receiver is not among them — the interface owns no storage, so there is no
// statically-known slot to leave in moved-from state; codegen's
// `lowerMoveFromCollectionSlot` recognizes only the concrete owning collections
// and would mishandle the moved-out element (a double-free at runtime). Return
// the interface's type name when the receiver is interface-typed, else "".
std::string
ExprTC::moveThroughInterfaceSlotName(const ast::Expr& operand) const {
    const auto* ix = dynamic_cast<const ast::IndexExpr*>(&operand);
    if (!ix || !ix->target) return {};
    const TypeRef* tt = ctx_.exprType(*ix->target);
    if (!tt || tt->kind != TypeRef::Kind::Named) return {};
    // A user-declared `interface` resolves to a Named TypeRef whose declaration
    // is an interface TypeDecl (§11.2). Its name is the diagnostic subject.
    if (tt->declaration) {
        if (tt->declaration->kind == ast::TypeDeclKind::Interface)
            return std::string(tt->declaration->name);
        return {};
    }
    // The built-in collection interfaces resolve to a Named TypeRef with a NULL
    // declaration (TypeResolution_Names) carrying the name on the AST node. The
    // only built-in interfaces that declare an int-indexer — the sole shape a
    // `move coll[i]` can take among built-ins — are the list interfaces
    // `IReadOnlyList<T>`, `IList<T>`, and `IOwningList<T>` (stdlib §4.1–§4.2);
    // every other built-in interface has no indexer, so `coll[i]` would not
    // typecheck on it. (`Array`/`OwningArray`/`OwningDictionary`/… are concrete
    // CLASSES, not interfaces, and are correctly NOT matched here.)
    if (tt->astNode && tt->astNode->name && !tt->astNode->name->parts.empty()) {
        std::string_view cn = tt->headName();
        if (cn == "IReadOnlyList" || cn == "IList" || cn == "IOwningList")
            return std::string(cn);
    }
    return {};
}

// §28.11 / §4.15 (#871): a borrowing-span indexer slot owns no storage, so
// `move span[i]` cannot transfer ownership. The borrowing spans view a
// non-owning source — `ArraySpan<T>` / `ReadOnlyArraySpan<T>` view a borrowing
// `Array<T>`, and the read-only spans (`ReadOnlySpan<T>`, `IReadOnlySpan<T>`,
// `ReadOnlyStringSpan`) view a borrowed string / buffer
// — so their slots hold borrows with nothing to move out. (The OWNING `Span<T>`
// over an `OwningBuffer` is intentionally NOT matched.) Returns the span type
// name when `operand` is `span[i]` over such a span, else the empty string.
std::string
ExprTC::moveThroughBorrowingSpanSlotName(const ast::Expr& operand) const {
    const auto* ix = dynamic_cast<const ast::IndexExpr*>(&operand);
    if (!ix || !ix->target) return {};
    const TypeRef* tt = ctx_.exprType(*ix->target);
    if (!tt || tt->kind != TypeRef::Kind::Named) return {};
    // The span types may resolve either as a built-in (null declaration, name on
    // the AST node) or via a record declaration; read the name from whichever is
    // present so both representations are covered.
    std::string_view name;
    if (tt->declaration)
        name = tt->declaration->name;
    else if (tt->astNode && tt->astNode->name &&
             !tt->astNode->name->parts.empty())
        name = tt->headName();
    if (name == "ArraySpan" || name == "ReadOnlyArraySpan" ||
        name == "ReadOnlySpan" || name == "IReadOnlySpan" ||
        name == "ReadOnlyStringSpan")
        return std::string(name);
    return {};
}

// §19.3 / §15.3 (v0.184, #1665): a `case`/`when` pattern binding over a
// MOVE-ONLY value-type scrutinee (an owning inline array, `[N owned]T` /
// `[N owned]T?`) is a BORROW of the scrutinee's value — "the case construct
// itself never moves from the scrutinee, so the borrow form is the only
// consistent option ... Slot reads through the binding are admitted; `move
// s[i]` through the binding is rejected (the binding does not own the
// array)" (§19.3). Detect `operand` = `s[i]` where `s` resolves to a
// LocalSymbol whose declaration node is a pattern `BindingPattern` (the
// naked `var s` arm form, or a nested `var x` inside a destructuring
// sub-pattern — both are registered with `lsym->decl` pointing at the
// BindingPattern node, which is what distinguishes them from a plain
// `var`-declared local, whose `lsym->decl` is the LocalDecl node instead —
// see the CaseStmt/CaseExpr arm-binding handlers) and whose type is a
// move-only owning inline array. Returns the bound name when this shape is
// matched, else the empty string.
std::string ExprTC::moveThroughBorrowedMoveOnlyArrayBindingName(
        const ast::Expr& operand) const {
    const auto* ix = dynamic_cast<const ast::IndexExpr*>(&operand);
    if (!ix || !ix->target) return {};
    const auto* id = dynamic_cast<const ast::IdentifierExpr*>(ix->target.get());
    if (!id) return {};
    Symbol* sym = ctx_.exprSymbol(*id);
    auto* loc = dynamic_cast<LocalSymbol*>(sym);
    if (!loc || !loc->decl) return {};
    if (!dynamic_cast<const ast::BindingPattern*>(loc->decl)) return {};
    const TypeRef* tt = ctx_.exprType(*ix->target);
    if (!tt || tt->kind != TypeRef::Kind::InlineArray || !tt->arrayOwned)
        return {};
    return std::string(id->name);
}

// §19.3 / §15.3 (v0.196, #1745): the whole-binding sibling of
// moveThroughBorrowedMoveOnlyArrayBindingName. Detects `operand` = a bare
// identifier `s` (not an IndexExpr) that resolves to a LocalSymbol whose
// declaration node is a pattern `BindingPattern` and whose type is a move-only
// owning inline array (kind == InlineArray && arrayOwned). `move s[i]` (a
// single slot) is already rejected by the sibling above; this catches `move s`
// on the WHOLE binding, which the sibling's IndexExpr shape does not match.
// The binding is a borrow of the scrutinee — moving the whole value out would
// launder ownership the original scrutinee still holds, a double-ownership
// hazard identical to the slot case. Returns the bound name when this shape is
// matched, else the empty string.
std::string ExprTC::moveThroughBorrowedMoveOnlyArrayWholeBindingName(
        const ast::Expr& operand) const {
    const auto* id = dynamic_cast<const ast::IdentifierExpr*>(&operand);
    if (!id) return {};
    Symbol* sym = ctx_.exprSymbol(*id);
    auto* loc = dynamic_cast<LocalSymbol*>(sym);
    if (!loc || !loc->decl) return {};
    if (!dynamic_cast<const ast::BindingPattern*>(loc->decl)) return {};
    const TypeRef* tt = ctx_.exprType(*id);
    if (!tt || tt->kind != TypeRef::Kind::InlineArray || !tt->arrayOwned)
        return {};
    return std::string(id->name);
}

// §28.3 / §28.8.1 (issue #4284): a non-`owned` REFERENCE-TYPED FIELD read is a
// borrow (§28.3's "unmarked read never consumes" rule applies uniformly to
// fields), so `move` of such a read is rejected here. The leaf field's `owned`
// modifier is the single authority — the receiver's ownership status is
// irrelevant: a borrow of a non-`owned` field is still a borrow, and the field
// slot owns no storage to transfer. Two read shapes are checked:
//   * explicit MemberAccessExpr `recv.f` — sema attaches the member's
//     FieldSymbol to the MemberAccessExpr (ExprTypecheck).
//   * bare-identifier `_f` resolving to a class field — implicit `this.f` per
//     §28.8.1; sema attaches the field's FieldSymbol to the IdentifierExpr.
// A field with `owned` in its modifiers is NOT matched (the legal
// `move recv.f` / `move this.f` / `move _f` / `move recv.f!` / `move _f!` over an
// `owned` slot — handled by the ownership pass). A PropertySymbol read is
// deliberately NOT matched: a property getter runs user code and the borrow
// classification belongs to the ownership pass (#398); this compile-time
// site-only check keys on the field-shape slot.
//
// Value-typed fields (primitives — `int n`, a `bool`, etc. — value-kind records
// / choices / enums, owning-inline-array fields `[N owned]T` whose borrow kind
// is settled by the move-only-owning-inline-array rule, §15.1.1) are NOT
// matched — a value field has no borrow/own distinction (§3: value types carry
// no storage obligation; reads copy), so the "no slot to take ownership of"
// rationale does not apply. The legal `move box.n` of an `int` field copies
// the value through unchanged (#2002 not-a-regression guard).
//
// Returns the textual form to embed in the diagnostic (the bare identifier for
// the implicit-`this` form; the explicit `recv.f` for the MemberAccessExpr).
// Returns the empty string for any other shape (a borrowed local, an IndexExpr,
// an interface slot, a pattern-binding array — all handled by sibling
// helpers, OR a value-typed field, OR an `owned`-tagged field).
std::string ExprTC::moveOnNonOwnedFieldName(const ast::Expr& operand) const {
    auto isOwnedField = [](sema::Symbol* sym) -> bool {
        auto* fs = dynamic_cast<sema::FieldSymbol*>(sym);
        if (!fs) return false;
        for (std::string_view m : fs->modifiers)
            if (m == "owned") return true;
        return false;
    };
    // A field's RESOLVED type is the level-0 read kind: primitive (no
    // borrow/own distinction), Named class/interface (the reference-shaped
    // field that §28.3's "unmarked read never consumes" targets), or record /
    // choice / enum (value-kind — copies through, no slot to move). Reading
    // the OPERAND's exprType is the simplest path: it's set by the typechecker
    // on the same pass and is the field's resolved type regardless of whether
    // the FieldSymbol is local or imported (the imported path's null `decl`
    // never reaches here, #3149).
    auto isReferenceFieldType = [](const TypeRef* ft) -> bool {
        if (!ft) return false;
        if (ft->kind == TypeRef::Kind::Primitive) return false;
        if (ft->kind == TypeRef::Kind::Named) {
            if (ft->declaration) {
                auto k = ft->declaration->kind;
                return k == ast::TypeDeclKind::Class ||
                       k == ast::TypeDeclKind::Interface;
            }
            // Built-in / imported named type without a declaration — fall
            // back to the head-name heuristic for the well-known reference
            // types. (Conservative: when in doubt, do NOT match — let the
            // ownership pass handle the residual cases it already covers.)
            if (ft->astNode && ft->astNode->name &&
                !ft->astNode->name->parts.empty()) {
                auto hn = ft->headName();
                return hn == "string" || hn == "Shared" || hn == "Weak" ||
                       hn == "Array" || hn == "OwningArray" ||
                       hn == "OwningDictionary" || hn == "Channel";
            }
            return false;
        }
        return false;
    };
    const TypeRef* operandTy = ctx_.exprType(operand);
    // Explicit `recv.f` — sema attaches the member's FieldSymbol to the
    // MemberAccessExpr (ExprTypecheck:3673 et seq.). // per §28.3 / §28.8.1
    if (dynamic_cast<const ast::MemberAccessExpr*>(&operand)) {
        sema::Symbol* sym = ctx_.exprSymbol(operand);
        if (isOwnedField(sym)) return {};
        auto* fs = dynamic_cast<sema::FieldSymbol*>(sym);
        if (!fs) return {};
        if (!isReferenceFieldType(operandTy)) return {};
        if (const auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&operand)) {
            std::string recv;
            if (const auto* rid =
                    dynamic_cast<const ast::IdentifierExpr*>(ma->target.get()))
                recv = std::string(rid->name);
            else
                recv = "<expr>";
            return recv + "." + std::string(ma->member);
        }
        return "<field>";
    }
    // Bare identifier `_f` — implicit `this.f` per §28.8.1; sema attaches the
    // field's FieldSymbol to the IdentifierExpr when the bare name resolves to
    // a class field of the enclosing type. A method-scope local/parameter is
    // not a FieldSymbol, so this matches only a genuine field read.
    if (dynamic_cast<const ast::IdentifierExpr*>(&operand)) {
        sema::Symbol* sym = ctx_.exprSymbol(operand);
        if (isOwnedField(sym)) return {};
        auto* fs = dynamic_cast<sema::FieldSymbol*>(sym);
        if (!fs) return {};
        if (!isReferenceFieldType(operandTy)) return {};
        if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(&operand))
            return std::string(id->name);
        return "<field>";
    }
    return {};
}

void ExprTC::typeUnary(const ast::UnaryExpr& ue) {
    const auto& op = ue.op;
    // §25: a user-defined unary operator overload.
    if (MethodSymbol* opm = findUnaryOperator(ue)) {
        ctx_.setExprSymbol(&ue, opm);
        if (auto* od = dynamic_cast<const ast::OperatorDecl*>(opm->decl)) {
            if (od->returnType)
                if (auto* rt = ctx_.resolvedType(*od->returnType))
                    ctx_.setExprType(&ue, rt);
        } else if (ctx_.isImported(opm)) {
            // §25 / §2.5 (#1485): an imported unary operator overload has no
            // ast::OperatorDecl (decl == nullptr for imported symbols), so the
            // result type must come from the loader's stashed return type —
            // otherwise the UnaryExpr is left untyped and a downstream member
            // access on its value-typed result is silently dropped. Mirrors the
            // binary-operator fallback above and the imported-method fallback in
            // ExprTypecheck_Calls.cpp.
            if (auto* rt = ctx_.importedReturnType(opm))
                ctx_.setExprType(&ue, rt);
        }
        // per §7.4 (v0.182, #1566): a user-defined unary operator is a call that
        // can transfer control to user code — invalidate mutably-captured
        // narrowings (its operand was already visited under the prior narrowing).
        invalidateCapturedNarrowings();
        return;
    }
    if (op == "not") {
        // §7.11: `not bool?` is a compile error.
        if (ue.operand) {
            const TypeRef* ot = ctx_.exprType(*ue.operand);
            if (ot && ot->kind == TypeRef::Kind::Primitive &&
                ot->primitive == PrimitiveType::Bool && ot->nullable) {
                ctx_.diagnostics().report(
                    Severity::Error, ue.span.begin,
                    "`not` requires a non-nullable `bool` operand (§7.11); a"
                    " `bool?` operand is not permitted — there is no"
                    " three-valued logic. Use `??` to coalesce the `bool?` to"
                    " a `bool` first, or narrow with an explicit null check.");
            }
            // §6 (#1040): `not` is boolean-only, so a NON-bool operand
            // (int/uint/char/...) is a compile error. Without this it slipped
            // through (result typed `bool` below) and codegen emitted malformed
            // IR (`not 5` / `not u`). The nullable-`bool` case is handled above;
            // here a KNOWN non-`bool` primitive/type is rejected. Use the
            // bitwise complement `~` (§6.5) for an integer bit-flip.
            else if (ot && !(ot->kind == TypeRef::Kind::Primitive &&
                             ot->primitive == PrimitiveType::Bool)) {
                ctx_.diagnostics().report(
                    Severity::Error, ue.span.begin,
                    "`not` requires a `bool` operand (§6: `and`/`or`/`not` have"
                    " boolean-only semantics). Use the bitwise complement `~`"
                    " for an integer bit-flip.");
            }
        }
        ctx_.setExprType(&ue, ctx_.primitiveTypeRef(PrimitiveType::Bool));
        return;
    }
    // §28.8.1 / §28.11 (issue #423): `move coll[i]` is admitted only on the
    // enumerated CONCRETE owning receivers (§28.11). A `move` through an
    // INTERFACE-typed indexer slot (`IOwningList<T>`, `IList<T>`,
    // `IReadOnlyList<T>`, or a user interface exposing an indexer) is rejected:
    // the interface owns no storage, so there is no slot to leave in moved-from
    // state, and codegen would mishandle the moved-out element (a double-free at
    // runtime). The diagnostic still falls through to set the result type so a
    // downstream consumer of the move result does not cascade unrelated errors.
    if (op == "move" && ue.operand) {
        std::string iface = moveThroughInterfaceSlotName(*ue.operand);
        if (!iface.empty()) {
            ctx_.diagnostics().report(
                Severity::Error, ue.span.begin,
                std::string("`move` is not admitted through an interface-typed "
                            "indexer slot: `") + iface +
                "` is an interface and owns no storage, so there is no slot to "
                "take ownership of (§28.8.1 / §28.11 — `move coll[i]` is "
                "admitted only on concrete owning collections: `OwningArray<T>`, "
                "`OwningDictionary<K, V>`, class-typed `OwningBuffer<T>` / "
                "`Span<T>`, and owning inline arrays)");
        }
        // §28.11 / §4.15 (#871): `move arrSpan[i]` is rejected for a borrowing
        // span (ArraySpan/ReadOnlyArraySpan view a borrowing Array<T>; the
        // read-only spans view a borrowed string/buffer). Their slots hold
        // borrows with no ownership to transfer; accepting the move corrupts
        // memory at runtime (a SIGSEGV / double-free of a borrow-backed slot).
        std::string span = moveThroughBorrowingSpanSlotName(*ue.operand);
        if (!span.empty()) {
            ctx_.diagnostics().report(
                Severity::Error, ue.span.begin,
                std::string("`move` is not admitted through a borrowing span "
                            "slot: `") + span +
                "` views a borrowing source (a non-owning `Array<T>`, `string`, "
                "or read-only buffer), so its slots hold borrows with no "
                "ownership to transfer (§28.11 / §4.15 — `move arrSpan[i]` is "
                "rejected; use the plain indexer-get to read the borrow)");
        }
        // §19.3 / §15.3 (v0.184, #1665): `move s[i]` is rejected when `s` is a
        // `case`/`when` pattern binding over a move-only owning-inline-array
        // scrutinee — the binding is a borrow of the scrutinee's value and
        // does not own the array, so there is no slot to take ownership of.
        std::string patBinding = moveThroughBorrowedMoveOnlyArrayBindingName(*ue.operand);
        if (!patBinding.empty()) {
            ctx_.diagnostics().report(
                Severity::Error, ue.span.begin,
                std::string("`move` is not admitted through a `case`/`when` "
                            "pattern binding of move-only value type: `") +
                patBinding +
                "` is a borrow of the scrutinee — the case construct never "
                "moves from the scrutinee, so the binding does not own the "
                "array and there is no slot to take ownership of (§19.3 / "
                "§15.3 — read the slot with the plain indexer, or move from "
                "the original scrutinee after the match instead)");
        }
        // §19.3 / §15.3 (v0.196, #1745): `move s` on the WHOLE binding is
        // rejected for the same reason as `move s[i]` above — the binding is a
        // borrow of the scrutinee and owns nothing, so moving the whole value
        // out would launder ownership the original scrutinee still holds (a
        // double-ownership hazard). The `patBinding` helper above matches only
        // the `s[i]` slot shape; this matches the bare identifier `s`.
        std::string wholePatBinding =
            moveThroughBorrowedMoveOnlyArrayWholeBindingName(*ue.operand);
        if (!wholePatBinding.empty()) {
            ctx_.diagnostics().report(
                Severity::Error, ue.span.begin,
                std::string("`move` is not admitted through a `case`/`when` "
                            "pattern binding of move-only value type: `") +
                wholePatBinding +
                "` is a borrow of the scrutinee — the case construct never "
                "moves from the scrutinee, so the whole binding does not own "
                "the array and there is nothing to take ownership of (§19.3 / "
                "§15.3 — read slots with the plain indexer, or move from the "
                "original scrutinee after the match instead)");
        }
        // §28.3 / §28.8.1 (issue #4284): `move <field>` on a non-`owned` FIELD
        // is rejected — a class-typed field with no `owned` annotation holds a
        // borrow (§28.3's "unmarked read never consumes" rule covers fields
        // identically), so accepting `move` would classify the read as an owned
        // transfer; codegen's reclaim-eligibility walk then treats the moved-
        // from slot as a fresh owned value, and disposing both the `move` result
        // and the original owner frees the same backing storage twice
        // (measured heap-use-after-free under `--sanitize=address` in the
        // issue's `move h.item` repro). The leaf field's `owned` modifier is
        // the single authority — the receiver's ownership status is irrelevant;
        // only the field-shape "this slot owns storage" qualifies it for a
        // `move` transfer. Legal forms `move obj.f` / `move this.f` / `move _f`
        // (and the `!` null-detach variants) over an `owned`-modified field
        // are NOT matched (the modifier check excludes them).
        std::string nonOwnedField = moveOnNonOwnedFieldName(*ue.operand);
        if (!nonOwnedField.empty()) {
            ctx_.diagnostics().report(
                Severity::Error, ue.span.begin,
                std::string("`move` is not admitted on a non-`owned` field: `") +
                nonOwnedField +
                "` is a borrow (§28.3 — a class-typed field with no `owned` "
                "annotation holds a borrow, not an owning slot), so there is "
                "no storage to transfer. Read the field plainly (the value is "
                "already a borrow for the use site), or move the whole "
                "containing object instead. (§28.3 / §28.8.1 — the `move` "
                "marker transfers an OWNING slot only: `owned` parameter, "
                "`owned` field, `using var`, or `move`-adopted local)");
        }
        // §4.14 (#779): `move sp[i]` on a VALUE-typed owning `Span<T>` is not
        // admitted — value elements are read with the plain indexer and copied
        // at the use site (spec §4.14: "For value T: move span[i] is not
        // admitted"). The class-typed owning `Span<T>` DOES admit `move` (it
        // transfers the owned slot — handled by lowerMoveFromCollectionSlot).
        if (const auto* mix =
                dynamic_cast<const ast::IndexExpr*>(ue.operand.get())) {
            if (mix->target) {
                const TypeRef* tt = ctx_.exprType(*mix->target);
                if (tt && tt->kind == TypeRef::Kind::Named && !tt->declaration &&
                    tt->astNode && tt->astNode->name &&
                    !tt->astNode->name->parts.empty() &&
                    tt->headName() == "Span") {
                    const TypeRef* el =
                        (!tt->genericArgs.empty()) ? tt->genericArgs[0] : nullptr;
                    bool elemIsClass = false;
                    if (el && el->kind == TypeRef::Kind::Named) {
                        if (el->declaration &&
                            (el->declaration->kind == ast::TypeDeclKind::Class ||
                             el->declaration->kind ==
                                 ast::TypeDeclKind::Interface))
                            elemIsClass = true;
                        else if (!el->declaration && el->astNode &&
                                 el->astNode->name &&
                                 !el->astNode->name->parts.empty()) {
                            auto en = el->headName();
                            if (en == "Shared" || en == "Weak")
                                elemIsClass = true;
                        }
                    }
                    if (!elemIsClass)
                        ctx_.diagnostics().report(
                            Severity::Error, ue.span.begin,
                            "`move` is not admitted through a value-typed "
                            "`Span<T>` slot — value elements are read with the "
                            "plain indexer and copied at the use site (§4.14)");
                }
            }
        }
    }
    // `move x` (§28.3) keeps the operand's type — the result is the
    // same value with ownership transferred. Without the type, member
    // access on a moved value (`var s := move r; s.M`) fails to resolve.
    if (op == "+" || op == "-" || op == "~" || op == "move") {
        TypeRef* ot = ue.operand ? ctx_.exprType(*ue.operand) : nullptr;
        // fix #2819 (§3.5, v0.240): the UNARY half of the same rule the binary
        // arm enforces above — unary `+`, `-` and `~` on a unit (`()`) operand.
        // §3.5 groups unary `+`/`-` under "Arithmetic on `byte`, … `decimal`"
        // and `~` under "Bitwise on `byte`, … `ulong`"; `()` is in neither list.
        // The #1574/#1588/#6.5 bad-kind checks below all gate on
        // `Kind::Primitive`, so a Unit operand slipped past every one of them
        // and fell through to the identity pass-through — `-u` compiled and
        // read 0, `~u` compiled and read -1. Included here rather than deferred
        // because it is the same rule, the same §, and the same silent wrong
        // value as the binary form; leaving it would be a known residual.
        // `move` is deliberately excluded: it is not an arithmetic operator
        // (§28.3 ownership transfer) and rejecting it needs its own authority.
        if (op != "move" && ot && ot->kind == TypeRef::Kind::Unit) {
            ctx_.diagnostics().report(Severity::Error, ue.span.begin,
                "unary operator `" + std::string(op) + "` is not defined for a "
                "unit (`()`) operand (\xC2\xA7""3.5) \xE2\x80\x94 the unit type "
                "has exactly one value and no arithmetic or bitwise operations. "
                "A unit-valued expression is a statement, not a value.");
            return;
        }
        // §6.5: unary `~` (bitwise complement) is "not valid on bool, char, or
        // floating-point types" — it requires an integer operand. (`not` is the
        // boolean complement.) Primitive-only; a user type overloading `~` is
        // resolved earlier via findUnaryOperator and never reaches here.
        if (op == "~" && ot && ot->kind == TypeRef::Kind::Primitive) {
            PrimitiveType p = ot->primitive;
            const char* badKind =
                (p == PrimitiveType::Float || p == PrimitiveType::Double)
                    ? "floating-point types"
                : (p == PrimitiveType::Bool)
                    ? "`bool` (use `not` for boolean complement)"
                : (p == PrimitiveType::Char) ? "`char`" : nullptr;
            if (badKind) {
                ctx_.diagnostics().report(Severity::Error, ue.span.begin,
                    std::string("operator `~` is not valid on ") + badKind +
                    " — the bitwise complement requires an integer operand "
                    "(§6.5)");
                return;
            }
        }
        // per §3.5 (v0.182, #1574): unary `+` is the identity, defined only
        // on the numeric primitives (byte…decimal) — "previously grammatical
        // but undefined... invalid on bool, char, and string." Before this
        // check, `+boolExpr`/`+charExpr`/`+stringExpr` fell through to the
        // generic pass-through below (the operand's own type, unchanged),
        // silently admitting an operator the spec defines nowhere for these
        // types. Primitive-only; a user type overloading `+` is resolved
        // earlier via findUnaryOperator and never reaches here.
        if (op == "+" && ot && ot->kind == TypeRef::Kind::Primitive) {
            PrimitiveType p = ot->primitive;
            const char* badKind =
                (p == PrimitiveType::Bool) ? "`bool`"
                : (p == PrimitiveType::Char) ? "`char`"
                : (p == PrimitiveType::String) ? "`string`" : nullptr;
            if (badKind) {
                ctx_.diagnostics().report(Severity::Error, ue.span.begin,
                    std::string("unary operator `+` is not valid on ") +
                    badKind + " — unary `+` is the identity, defined only on "
                    "numeric types (§3.5)");
                return;
            }
        }
        // per §3.5 (#1588): unary `-` (negation), like unary `+`, is grouped
        // under "Arithmetic" and defined only on the numeric primitives
        // (byte…decimal) — "invalid on bool, char, and string." Before this
        // check, sema never inspected `-`'s operand kind at all (only `~` had
        // a bad-kind check), so `-boolExpr`/`-charExpr` silently type-checked
        // as an identity pass-through (the generic fall-through below), and
        // `-stringExpr` reached codegen, which emitted an integer `sub` on a
        // string's ptr representation and crashed on LLVM IR verification
        // instead of raising a clean diagnostic. Primitive-only; a user type
        // overloading `-` is resolved earlier via findUnaryOperator and never
        // reaches here.
        if (op == "-" && ot && ot->kind == TypeRef::Kind::Primitive) {
            PrimitiveType p = ot->primitive;
            const char* badKind =
                (p == PrimitiveType::Bool) ? "`bool`"
                : (p == PrimitiveType::Char) ? "`char`"
                : (p == PrimitiveType::String) ? "`string`" : nullptr;
            if (badKind) {
                ctx_.diagnostics().report(Severity::Error, ue.span.begin,
                    std::string("unary operator `-` is not valid on ") +
                    badKind + " — unary `-` (negation) is defined only on "
                    "numeric types (§3.5)");
                return;
            }
        }
        // §7.11 / proposal 0114 / v0.267 (#3298): unary `+`, `-`, `~` do not
        // lift over a nullable value type — they are defined on the underlying
        // `T` only (`bool`-shaped `not` is §17.2 and excluded above). An operand
        // narrowed non-null by §7.4 (locals/parameters in `nullNarrowed_` / the
        // lookup-time narrowing analysis, #914) is a non-nullable operand here
        // and the ordinary unary operator applies. Mirror the §7.11 binary
        // operator's narrower rejection family so the diagnostic matches.
        if (ot && ot->kind == TypeRef::Kind::Primitive && ot->nullable) {
            if (!isNarrowedNonNullRead(ue.operand.get())) {
                const char* opWord =
                    (op == "~") ? "bitwise complement (`~`)" :
                    (op == "-") ? "negation (unary `-`)" :
                    /* + */       "the identity (unary `+`)";
                ctx_.diagnostics().report(
                    Severity::Error, ue.span.begin,
                    std::string("unary operator `") + std::string(op) +
                    "` is not defined on a nullable value type — narrow first "
                    "(`x <> null and …` (\xC2\xA7""7.4), `??` (\xC2\xA7""7.7), "
                    "or `!` (\xC2\xA7""7.8)); " + opWord +
                    " does not lift over `T?` (\xC2\xA7""7.11, v0.267)");
                ctx_.setExprType(&ue, ctx_.primitiveTypeRef(PrimitiveType::Int));
                return;
            }
            // §7.11 (v0.286, #3298): the surviving operand is §7.4-narrowed, so
            // it is read at the non-nullable T and `-n` / `~n` / `+n` is a `T`,
            // not a `T?` — the same normalization the binary path performs.
            ot = operandStaticTypeAt(ue.operand.get(), ot);
        }
        if (ot) ctx_.setExprType(&ue, ot);
        return;
    }
    // `await` — async-readiness 3b (§30.2 / §30.3 / §30.5).
    if (op == "await") {
        // per §7.4 (v0.182, #1566): across an `await`, another continuation of
        // the same domain may run a closure that nulls a mutably-captured local
        // (§30.13), so the suspension invalidates such a local's narrowing. The
        // operand was already visited (in visitChildren) under the pre-await
        // narrowing, so this drops it for the code that resumes after the await.
        invalidateCapturedNarrowings();
        // §30.2 / Appendix C.6: `await` is valid only inside an `async`
        // method, an `async` lambda, or the synchronous prefix of `await for`
        // (the latter is itself only reachable inside an async context). A
        // static field initializer / static constructor is never async, so an
        // `await` there is caught by this same gate.
        if (!inAsyncContext_) {
            ctx_.diagnostics().report(
                Severity::Error, ue.span.begin,
                "`await` is valid only inside an `async` method or `async` "
                "lambda (§30.2)");
            // Fall through and still produce the result type so downstream
            // typing of the awaited value doesn't cascade unrelated errors.
        } else if (inBindAtom_) {
            // §17.15 / §30.5 (v0.216, #2098): TARGETED REJECTION — `await`
            // inside a binding atom in an async body is not yet supported:
            // the async lowering's frame-local scan is statement-level, so an
            // atom binding would stay alloca-backed across the suspension (a
            // silent miscompile, not a missing diagnostic). Reject cleanly
            // until the state-machine scan learns BindExpr (#2122); the
            // remedy is a statement-level binding (`var t := await …;`)
            // before the atom.
            ctx_.diagnostics().report(
                Severity::Error, ue.span.begin,
                "`await` inside a `(var \xe2\x80\xa6)` binding atom is not "
                "supported in this version \xe2\x80\x94 bind the awaited "
                "value in a statement-level `var` before the atom "
                "(\xc2\xa7""17.15 / \xc2\xa7""30.5, v0.216)");
        }
        // Result type (§30.3 / §30.5):
        //   await Task          -> unit  (no result value)
        //   await Task<T>       -> T     (Pattern A: a borrow the task owns;
        //                                  the borrow/ownership at the await
        //                                  site is the ownership pass's job —
        //                                  3b produces the element type T)
        //   await OwningTask<T>  -> T     (Pattern B: ownership transfers; the
        //                                  result type is the element type T;
        //                                  `owned`-ness is tracked by the
        //                                  ownership pass, not by TypeRef)
        // Awaiting any other task shape (async-iterator MoveNextAsync, etc.)
        // is typed elsewhere — see the deferral note below.
        TypeRef* ot = ue.operand ? ctx_.exprType(*ue.operand) : nullptr;
        // §30.2 / Appendix C.7 (#444 / #509): `await` requires a Task /
        // Task<T> / OwningTask<T> operand. When the operand's type is known and
        // is NOT one of those three forms, reject it with a clean diagnostic
        // rather than silently miscompiling.
        // NOTE: as of #509, the parser correctly handles `Method<T>(…)` as an
        // explicit-type-arg call (§14.1) so `Task.FromResult<int>(42)` now
        // resolves to `Task<int>` and passes through this guard normally.
        // The guard remains for other non-Task operands (plain integer literals,
        // arbitrary bool expressions, etc.).  `await for` desugaring passes
        // Task<bool> through this path, which IS a Named Task type and is NOT
        // flagged; any unresolved / unknown type (ot == nullptr) is left
        // undiagnosed to avoid false positives.
        // fix #2822 (spec §30.2/§30.3/§2.5/§2.8.2, v0.240): recover the awaited
        // type's head NAME without requiring an ast::Type node.
        //
        // Both blocks below used to gate on `ot->astNode && ot->astNode->name`,
        // which is an AST-SHAPE test standing in for the question actually being
        // asked — "is this named type spelled `Task` / `OwningTask`?". An
        // IMPORTED type has NO AST node by construction (ModuleLoader builds its
        // TypeRefs from metadata; there is no ast::Type to point at), so an
        // imported `Task<int>` failed the guard and `await` on a legal
        // cross-module async call was REJECTED:
        //
        //   app.bzl:5:14: `await` requires a `Task`, `Task<T>`, or
        //   `OwningTask<T>` operand (§30.2 / §30.3).
        //
        // The identical program in ONE module compiles — a §2.8.2 clause 2
        // divergence, and one that makes every imported `async` method
        // unusable, since §30 gives no other way to consume a `Task<T>`.
        //
        // `TypeRef::headName()` already falls back to `importedName` for exactly
        // this case (TypeRef.hpp), so the head is recoverable; only the
        // precondition was wrong. This is the #2374 `astNode`-only gate family.
        //
        // The `parts.size() == 1` half is KEPT where an AST node exists — it
        // rejects a QUALIFIED spelling (`Foo.Task`), which must not be taken for
        // the builtin. For an imported ref the metadata's name is already the
        // bare head, so a non-empty `importedName` is the equivalent condition.
        // One definition, used by both blocks, so they cannot drift the way
        // these two mirrors already had.
        auto awaitedTaskHead = [](const TypeRef* t) -> std::string_view {
            if (!t || t->kind != TypeRef::Kind::Named || t->nullable) return {};
            if (t->astNode && t->astNode->name) {
                if (t->astNode->name->parts.size() != 1) return {};
            } else if (t->importedName.empty()) {
                return {};
            }
            return t->headName();
        };
        if (ot) {
            std::string_view head = awaitedTaskHead(ot);
            bool isTaskType = (head == "Task" || head == "OwningTask");
            if (!isTaskType) {
                ctx_.diagnostics().report(
                    Severity::Error, ue.span.begin,
                    "`await` requires a `Task`, `Task<T>`, or `OwningTask<T>` "
                    "operand (§30.2 / §30.3).");
                return;
            }
        }
        // Same predicate as the guard above — see its comment. Sharing it is the
        // point: when these were two copies, lifting only the rejection would
        // have left the await UNTYPED rather than typed, which is a silent wrong
        // value in place of a loud reject.
        if (std::string_view head = awaitedTaskHead(ot); !head.empty()) {
            if (head == "Task") {
                if (ot->genericArgs.empty()) {
                    // `await Task` → unit (no result value).
                    auto* unit = ctx_.makeTypeRef();
                    unit->kind = TypeRef::Kind::Unit;
                    ctx_.setExprType(&ue, unit);
                } else if (ot->genericArgs.front()) {
                    // `await Task<T>` → T (Pattern A; element type).
                    ctx_.setExprType(&ue, ot->genericArgs.front());
                }
            } else if (head == "OwningTask" && !ot->genericArgs.empty() &&
                       ot->genericArgs.front()) {
                // `await OwningTask<T>` → T (Pattern B; element type).
                ctx_.setExprType(&ue, ot->genericArgs.front());
            }
            // NOTE (deferred, §30.7): the result typing specific to async
            // iteration — `await for` desugaring's `MoveNextAsync()` / the
            // owned-yield `GetOwningAsyncEnumerator` path — is deeper than this
            // increment (it depends on the for-loop iteration-variable typing
            // and the IAsyncEnumerator<T> interface members) and is left to
            // the async-iterator typing milestone. 3b scopes the await result
            // rule to the Task / Task<T> / OwningTask<T> forms above.
        }
        return;
    }
}

// §35.5 / §19.3 (#2047 follow-up): see the declaration in ExprTypecheck_impl.hpp.
// A `case this of Ok(…)` inside a choice instance method (the stdlib `Result`'s
// own IsOk, or any user choice method) must resolve `Ok` to the enclosing
// choice's CASE, not to a same-named user class — but sema leaves `this` untyped
// for a named-type method, so the scrutinee-choice-first pattern resolution is
// skipped. Recover the enclosing choice as the scrutinee type here (local to the
// case handler; `this` stays globally untyped, so codegen's own value recovery
// is unaffected).
TypeRef* ExprTC::recoverChoiceThisScrutTy(const ast::Expr* scrutinee,
                                          TypeRef* scrutTy) {
    if (scrutTy || !scrutinee || !currentType_ ||
        currentType_->kind != ast::TypeDeclKind::Choice)
        return scrutTy;
    auto* id = dynamic_cast<const ast::IdentifierExpr*>(scrutinee);
    if (!id || (id->name != "this" && id->name != "base")) return scrutTy;
    auto* t = ctx_.makeTypeRef();
    t->kind = TypeRef::Kind::Named;
    t->declaration = currentType_;
    return t;
}

void ExprTC::visitChildren(const ast::Expr& e) {
    if (auto* be = dynamic_cast<const ast::BinaryExpr*>(&e)) {
        if (be->left) visitExpr(*be->left);
        // §7.4 (v0.226, #2212): in `A and B`, `B` is checked under `A`'s
        // true-facts — "x <> null and x.Length > 0" is well-typed. Narrowing
        // is otherwise established only by collectNarrowings AFTER the
        // condition is fully visited (if/elif/while visitors), never inside
        // visitExpr — so without this, the right operand saw no left-side
        // narrowing and `requires(s <> null and s.Length >= 0)` on a `string?`
        // was wrongly rejected. Per §22.16 ("Flow narrowing works inside
        // clauses unchanged") this applies in contract clauses too. Save/
        // restore mirrors checkOutPostcondAtReturn; collectNarrowings recurses
        // into `and` itself (ExprTypecheck.cpp:6710-6714).
        //
        // fix #3877 (§7.4, v0.294): `or` gets the mirror-image treatment — in
        // `A or B`, `B` is checked under `A`'s FALSE-facts ("x = null or
        // x.Length > 0" is well-typed; `A`'s false-fact is exactly what
        // `collectNarrowings(A, forTrueBranch=false)` computes, since `or`
        // only evaluates `B` when `A` is false — the same short-circuit
        // reasoning the `and` case above already uses for true-facts). Before
        // this fix `or` was left alone entirely (collectNarrowings was
        // and-only), so the stdlib's own documented `String.IsNullOrEmpty`
        // replacement idiom (`s = null or s.IsEmpty`) did not type-check.
        if (be->op == "and" && be->left && be->right) {
            auto savedNull = nullNarrowed_;
            auto savedNarrow = narrowings_;
            auto savedField = fieldNarrowed_;
            collectNarrowings(be->left.get(), /*forTrueBranch=*/true);
            visitExpr(*be->right);
            // fix #2258 / §7.4: un-seed the left operand's true-facts by
            // INTERSECTING with the saved state, never by assigning it. The
            // right operand's visit can legitimately INVALIDATE a fact that
            // held at entry — a `ref`/`out` argument erases it (the #1188
            // argument-invalidation path in the CallExpr handler below), and a
            // call or `await` drops the mutably-captured locals' facts
            // (invalidateCapturedNarrowings) — all per §7.4 "How narrowing is
            // invalidated". Assigning the entry snapshot back RESURRECTED
            // those facts, so `if s = null then return 0; end; var b: bool :=
            // true and C.Clear(ref s);` left `s` narrowed and the following
            // `s.Length` compiled to a null dereference (silent ACCEPT,
            // SIGSEGV). The intersection keeps the drop and still discards the
            // left operand's seeded facts (absent from the snapshot).
            intersectNarrowingSetsWith(savedNull, savedNarrow, savedField);
        } else if (be->op == "or" && be->left && be->right) {
            // fix #3877 (§7.4, v0.294): symmetric to the `and` case above,
            // false-facts instead of true-facts. Same intersect-not-assign
            // reasoning applies: the right operand's visit can invalidate a
            // fact seeded from the left (a `ref`/`out` argument, a call/await
            // dropping mutably-captured locals), and un-seeding must keep
            // that drop rather than resurrecting the entry snapshot.
            auto savedNull = nullNarrowed_;
            auto savedNarrow = narrowings_;
            auto savedField = fieldNarrowed_;
            collectNarrowings(be->left.get(), /*forTrueBranch=*/false);
            visitExpr(*be->right);
            intersectNarrowingSetsWith(savedNull, savedNarrow, savedField);
        } else if (be->right) {
            visitExpr(*be->right);
        }
    } else if (auto* ue = dynamic_cast<const ast::UnaryExpr*>(&e)) {
        if (ue->operand) visitExpr(*ue->operand);
    } else if (auto* ma =
                   dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        // §11.8 (v0.176, #807): mark the target node while visiting it so the
        // IdentifierExpr handler can recognize a type-parameter receiver
        // (`T.Member(...)`) without resolving bare `T` anywhere else.
        const ast::Expr* savedHint = maTargetHint_;
        maTargetHint_ = ma->target.get();
        if (ma->target) visitExpr(*ma->target);
        maTargetHint_ = savedHint;
    } else if (auto* nm =
                   dynamic_cast<const ast::NullCondMemberExpr*>(&e)) {
        if (nm->target) visitExpr(*nm->target);
    } else if (auto* ce = dynamic_cast<const ast::CallExpr*>(&e)) {
        // §27.6 (v0.184): mark the callee node while visiting it so
        // resolveMemberAccess types a primitive-intrinsic static member that is
        // the DIRECT callee (`int.Parse(s)`) as its scalar result, but the same
        // member used as a bare method reference (`.Select(int.Parse)`) as a
        // function value. Saved/restored so a nested call's callee does not leak.
        const ast::Expr* savedCallee = calleeOfCallCtx_;
        calleeOfCallCtx_ = ce->callee.get();
        // fix #2328 (§26.5, v0.236): the call's arity is the gate that
        // arity-aware `lookupExtensionMember` consults while resolving the
        // callee. Stash it on the worker for `resolveMemberAccess` /
        // `resolveNullCondMember` to read; -1 (the default) means "not in
        // a call context" — bare references and the post-arity visitor
        // paths leave it untouched. Nested calls save/restore around their
        // own callee visit, so an inner call's arity does not leak into
        // the outer call's callee-visit lookup.
        std::ptrdiff_t savedArity = currentCallArity_;
        currentCallArity_ =
            static_cast<std::ptrdiff_t>(ce->args.size());
        // fix #3797 (§27.3 / §27.4, v0.286): for an immediately-invoked
        // lambda (`(lambda(params) => body)(args)`), the call's argument
        // types ARE the target-type context §27.4 names ("when the lambda's
        // target type is known from context, parameter type annotations
        // may be omitted"). Peek each argument's type by visiting it BEFORE
        // the callee and seed `pendingLambdaParamTypes_` so the lambda's
        // unannotated params consume the corresponding arg types — the
        // standard thread typed-local init / argument-lambda paths already
        // use (#829 / #3756). Saved/restored so a nested call's
        // pendingLambdaParamTypes_ does not leak into the outer call.
        //
        // Without this, the IIFE compiles clean and runs with the wrong
        // value: codegen's IIFE bypass builds the LLVM function type from
        // the actual argument VALUES (correct for the call signature), but
        // lowerLambda inside the body falls back to i32 for every
        // unannotated param (no resolvedDeclType was set by sema), so the
        // body's references read the wrong bytes — measured:
        // `(lambda(a, b) => b.Length)(3, "hello")` returns 0 instead of
        // 5. The IIFE callee visit below also no longer needs the
        // narrow-#3756 "context exists, but exact types not threaded"
        // carve-out: the per-parameter seeding below carries the real
        // types, so the lambda body's member/operator resolution sees real
        // types.
        std::vector<TypeRef*> savedIifeParams = pendingLambdaParamTypes_;
        if (ce->callee &&
            dynamic_cast<const ast::LambdaExpr*>(ce->callee.get())) {
            std::vector<TypeRef*> iifeParamTypes;
            iifeParamTypes.reserve(ce->args.size());
            for (const auto& a : ce->args) {
                if (a.value) {
                    visitExpr(*a.value);
                    iifeParamTypes.push_back(ctx_.exprType(*a.value));
                } else {
                    iifeParamTypes.push_back(nullptr);
                }
            }
            pendingLambdaParamTypes_ = std::move(iifeParamTypes);
        }
        // narrow #3756 (§27.3 / §27.4, v0.286): an immediately-invoked
        // lambda (`(lambda(x) => ...)(args)`) is the CALLEE of this
        // CallExpr, not one of its arguments — the argument-visiting loop's
        // `pendingLambdaArgContext_ = true` (below, for each `a.value`)
        // never reaches a lambda sitting in callee position, so #3756's new
        // terminal reject fired here even though the call's own arguments
        // are exactly the target-type context §27.4 asks for. Treat a bare-
        // LambdaExpr callee the same way a call-argument lambda is treated:
        // a context exists. Saved/restored so a nested IIFE argument's own
        // callee-visit does not leak here. (fix #3797 above threads the
        // real per-parameter types; this flag is the boolean "context
        // exists" companion that suppresses the §27.4 top-level-only
        // diagnostic for IIFE callees whose args we just visited.)
        bool savedArgCtxIifeCallee = pendingLambdaArgContext_;
        if (dynamic_cast<const ast::LambdaExpr*>(ce->callee.get()))
            pendingLambdaArgContext_ = true;
        if (ce->callee) visitExpr(*ce->callee);
        pendingLambdaArgContext_ = savedArgCtxIifeCallee;
        pendingLambdaParamTypes_ = savedIifeParams;
        currentCallArity_ = savedArity;
        calleeOfCallCtx_ = savedCallee;
        // per §24: an event-handler argument (`<event>.Subscribe(h)` / .Add /
        // .Remove) — push the event payload type so an untyped handler lambda's
        // parameter is typed as `T`, letting its body resolve `e.Field`.
        TypeRef* eventPayload = ce->callee
            ? eventHandlerPayloadType(ce->callee.get()) : nullptr;
        // per §22.12 rule 5 (v0.180, #1542): if callee is `<static event>
        // .Subscribe(...)` / `.Add(...)`, the handler argument (arg 0) must
        // be capture-free — the rule names `+=`/`Add`/`Subscribe`, not
        // `Remove`. Determine this now (the target's EventSymbol is already
        // resolved); the actual capture-freedom check runs in the arg-
        // visiting loop below, once the handler argument has itself been
        // visited (so isCaptureFreeEntry can see a lambda's captures).
        bool subscribeArgNeedsCaptureFree = false;
        if (auto* maCallee = ce->callee
                ? dynamic_cast<const ast::MemberAccessExpr*>(ce->callee.get())
                : nullptr) {
            if (maCallee->target &&
                (maCallee->member == "Subscribe" || maCallee->member == "Add")) {
                if (auto* es = dynamic_cast<EventSymbol*>(
                        ctx_.exprSymbol(*maCallee->target))) {
                    // per §22.12 rule 5 (v0.180, #1542): staticness lives on
                    // ast::EventDecl::isStatic directly — EventSymbol's
                    // inherited `modifiers` field is never populated for
                    // events (SymbolCollection.cpp), unlike Field/Property/
                    // MethodDecl, which carry a generic modifiers list.
                    if (auto* ed = dynamic_cast<const ast::EventDecl*>(es->decl))
                        subscribeArgNeedsCaptureFree = ed->isStatic;
                }
            }
        }
        // stdlib §5 (D7): for a LINQ operator/aggregate over a collection or
        // lazy receiver, the lambda argument is `(T) -> …` over the source
        // element type T. Seed an untyped single-param lambda's parameter with
        // T (same mechanism as the event-handler payload above) so its body
        // resolves member access on the element (e.g. `p.x` over Array<Point>).
        TypeRef* linqElem = ce->callee
            ? linqLambdaElementType(ce->callee.get()) : nullptr;
        // Resolve callee's parameter list to type out-var locals.
        sema::Symbol* calleeSym2 = ce->callee ? ctx_.exprSymbol(*ce->callee) : nullptr;
        const ast::MethodDecl* calleeMd2 = nullptr;
        sema::MethodSymbol* calleeMs2 = nullptr;
        sema::TypeSymbol* calleeParent2 = nullptr;
        if (auto* ms2 = dynamic_cast<sema::MethodSymbol*>(calleeSym2)) {
            calleeMs2 = ms2;
            calleeMd2 = dynamic_cast<const ast::MethodDecl*>(ms2->decl);
            calleeParent2 = ms2->parent;
        }
        // fix #2657 (§22.4 "Inline `out` declarations" / §2.5, v0.236): an
        // IMPORTED method has NO `ast::MethodDecl` — ModuleLoader.cpp:337-346
        // reconstructs a `sema::MethodSymbol` from the artifact's metadata with
        // no `decl`, and the formal parameter types survive only as
        // `sema::ParameterSymbol::resolvedDeclType` (ModuleLoader.cpp:258).
        // The `calleeMd2`-only lookup below therefore left `lsym->declType`
        // NULL for every `out var` recipient of an imported method, which is
        // the root of #2657: with no declType the identifier never gets an
        // exprType (the `if (loc->declType)` guard at ~line 816), so a later
        // `n.Length` fails to lower (the `internal:` #2491/#2434 notes), AND
        // codegen's ensureOutVarSlots (LlvmBackend_Calls2.cpp:1505) falls
        // through its whole cascade to the `i32()` DEFAULT — a 4-byte alloca
        // the imported callee then writes 8 bytes through for a `string`/`long`
        // out parameter. Resolve the formal type through the AST decl when
        // there is one (so local callees are bit-identical) and fall back to
        // the imported symbol's own parameter type otherwise.
        auto formalOutParamType2 =
            [&](std::size_t pos) -> TypeRef* {
            if (calleeMd2) {
                if (pos < calleeMd2->params.size() && calleeMd2->params[pos].type)
                    return ctx_.resolvedType(*calleeMd2->params[pos].type);
                return nullptr;
            }
            if (calleeMs2 && pos < calleeMs2->params.size())
                if (auto* ps2 = dynamic_cast<ParameterSymbol*>(
                        calleeMs2->params[pos]))
                    return ps2->resolvedDeclType;
            return nullptr;
        };
        // fix #4510 (§2.8.2 clause 1 / §27.4 / §27.1, v0.306): the formal a
        // call-argument lambda takes its inferred parameter types from — from
        // BOTH sources, exactly as `formalOutParamType2` above (#2657/#3186)
        // and `formalCtorOutParamType` on the `new` path (#3772) already do.
        //
        // The fix-#829 seeding below used to read `calleeMd2` alone, and an
        // IMPORTED method has no `ast::MethodDecl` at all (ModuleLoader
        // reconstructs a bare `sema::MethodSymbol`; the formal types survive
        // only as `ParameterSymbol::resolvedDeclType`). So across a module
        // boundary an unannotated lambda argument got NO target type —
        // silently, because #3756's "no target type anywhere" diagnostic is
        // suppressed for a call argument (`hadArgContext`), which is exactly
        // the context this thread was supposed to supply. §2.8.2 clause 1
        // makes that a defect: the artifact interface must be sufficient to
        // type-check every well-formed use of the public surface with FULL
        // signatures, and §27.4 admits the omitted annotation whenever the
        // target type is known from context — the boundary does not change
        // whether it is known.
        //
        // MEASURED (real `bezelc`, a `-c` library plus an importing exe, an
        // imported `ApplyHof(f: ((int) -> int) -> int)` called with
        // `lambda(g) => g(9)`): the single-module twin answers 10 and the
        // imported call answered 0. A one-level `(int) -> int` formal masked
        // it — with no type the parameter slot falls through `lowerLambda`'s
        // `llvm::Type* ty = i32()` DEFAULT, which happens to be right for an
        // `int` parameter — while the nested function-typed slot lowered `g`
        // as `i32`, left `g(9)` un-typed, and the body was dropped to
        // `ret i32 0` (read off `BEZEL_DUMP_IR=1`: the twin emits a second
        // lambda `i32 (ptr, ptr)` whose body is an indirect call, the importer
        // emits `i32 (ptr, i32)` whose body is a bare `ret i32 0`).
        //
        // A GENERIC callee is skipped on BOTH sides: there the function-param
        // type may be an unresolved type parameter (`(T) -> int`) inferred
        // from another argument, and the generic-inference path types the
        // lambda parameter correctly (#505).
        auto formalFnParamType2 =
            [&](std::size_t pos) -> TypeRef* {
            if (calleeMd2) {
                if (!calleeMd2->genericParams.empty()) return nullptr;
            } else if (!calleeMs2 || !calleeMs2->genericParams.empty()) {
                return nullptr;
            }
            return formalOutParamType2(pos);
        };
        std::size_t pos2 = 0;
        for (const auto& a : ce->args) {
            // §22.4: `out var x` declares x in the current scope.
            // Register the symbol BEFORE visiting the IdentifierExpr so
            // lookup finds it. Infer type from the callee's parameter.
            if (a.modifier == "out var" && a.value && currentScope_) {
                if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                                   a.value.get())) {
                    auto* lsym = ctx_.makeSymbol<LocalSymbol>();
                    lsym->name = id->name;
                    lsym->decl = id;
                    // Infer type from the callee's formal parameter.
                    // fix #2657: `formalOutParamType2` is the former
                    // `ctx_.resolvedType(*calleeMd2->params[pos2].type)`
                    // lookup plus the imported-callee fallback; for a callee
                    // WITH an ast::MethodDecl it returns exactly what this
                    // condition used to compute.
                    if (TypeRef* formalTy2 = formalOutParamType2(pos2)) {
                        lsym->declType = formalTy2;
                        // §11.8 (v0.176, #807): a STATIC interface member
                        // called through a constrained type parameter
                        // (`T.TryParse(s, out var v)`) declares its out param
                        // in terms of the INTERFACE's recurrence parameter
                        // (`value: T?` on IParsable<T>). Substitute it against
                        // the receiver (whose exprType resolveMemberAccess set
                        // to the constraint instantiation `I<T>`) so `v` is
                        // typed by the CALL SITE's type parameter, not the
                        // interface's unrelated `T` spelling.
                        if (lsym->declType && calleeParent2 &&
                            calleeParent2->kind ==
                                ast::TypeDeclKind::Interface) {
                            if (auto* maCal =
                                    dynamic_cast<const ast::MemberAccessExpr*>(
                                        ce->callee.get())) {
                                if (maCal->target)
                                    lsym->declType = substituteMemberType(
                                        lsym->declType, maCal->target.get(),
                                        calleeParent2);
                            }
                        }
                        // §22.4 / §7.9 (v0.181, #1552): record whether a read
                        // of this inline out var on a path where the call did
                        // not run is a definite-assignment error — true only for
                        // a NON-nullable reference-typed out param (no default).
                        lsym->outVarNeedsDefiniteAssignment =
                            isNonNullableReferenceType(lsym->declType);
                        // §22.4 / §7.9 (#1597): do NOT strip the out-param's
                        // declared nullability. A nullable out param (`T?`)
                        // means the callee may leave the slot null on some
                        // exit path (§22.3's every-exit-path-assigned rule
                        // permits assigning `null` there); the inline
                        // `out var x` recipient must keep that same `T?` type
                        // so a bare `x.Member` after the call is still subject
                        // to §7.4's nullable-member-access rule and so
                        // `when true`/`when false` (§22.4) caller narrowing has
                        // a nullable type to narrow FROM. This block used to
                        // unconditionally clear `nullable` here, reasoning it
                        // was "representationally harmless for a reference
                        // type" — true for CODEGEN's pointer lowering
                        // (LlvmBackend_Calls2.cpp's ensureOutVarSlots resolves
                        // the out-var slot's LLVM type fresh from the callee's
                        // AST parameter, independent of this sema TypeRef) —
                        // but wrong for the type checker, which then treated
                        // `x` as provably non-null forever after, silently
                        // widening past what the callee's signature promised.
                        // A non-nullable out param's declType already has
                        // `nullable == false`, so removing the strip is a
                        // no-op for that case (verified by the control test
                        // below and #1552's DA tests, which stay green).
                    }
                    // v0.171 (#1336): BUILTIN borrowing extraction Trys have no
                    // MethodDecl (calleeMd2 == null), so the block above leaves
                    // declType null. Their out param is `T?`/`V?` — infer the
                    // nullable element/value type from the receiver so the inline
                    // `out var x` recipient is nullable (read with `!` / a null
                    // check on the success path).
                    //   Stack/Queue/OwningStack/OwningQueue
                    //     TryPop/TryDequeue/TryPeek    → T?  (genericArgs[0])
                    //   Dictionary/SortedDictionary/OwningDictionary
                    //     TryGetValue                  → V?  (genericArgs[1])
                    if (!lsym->declType) {
                        if (auto* maCallee =
                                dynamic_cast<const ast::MemberAccessExpr*>(
                                    ce->callee.get())) {
                            if (maCallee->target) {
                                if (const TypeRef* rt =
                                        ctx_.exprType(*maCallee->target)) {
                                    if (rt->kind == TypeRef::Kind::Named &&
                                        !rt->declaration && rt->astNode &&
                                        rt->astNode->name &&
                                        !rt->astNode->name->parts.empty()) {
                                        const std::string_view cn =
                                            rt->headName();
                                        const std::string_view mn =
                                            maCallee->member;
                                        const TypeRef* elemT = nullptr;
                                        bool posElem =
                                            (cn == "Stack" || cn == "Queue" ||
                                             cn == "OwningStack" ||
                                             cn == "OwningQueue") &&
                                            (mn == "TryPop" ||
                                             mn == "TryDequeue" ||
                                             mn == "TryPeek");
                                        bool valElem =
                                            (cn == "Dictionary" ||
                                             cn == "SortedDictionary" ||
                                             cn == "OwningDictionary") &&
                                            mn == "TryGetValue";
                                        if (posElem && !rt->genericArgs.empty())
                                            elemT = rt->genericArgs[0];
                                        else if (valElem &&
                                                 rt->genericArgs.size() >= 2)
                                            elemT = rt->genericArgs[1];
                                        if (elemT) {
                                            TypeRef* nn =
                                                ctx_.makeTypeRef(*elemT);
                                            nn->nullable = true;
                                            lsym->declType = nn;
                                        }
                                    }
                                }
                            }
                        }
                    }
                    currentScope_->names[id->name] = lsym;
                }
            }
            // §22.4 / v0.160 (#1305/#1344): `out owned var x` is the inline
            // declaration form for an `out owned T` parameter.  Register the
            // symbol exactly as for `out var`, so that subsequent uses of `x`
            // in the same scope resolve correctly.  Ownership tracking (marking
            // the local Owned after the call) is handled in
            // markOutOwnedArgs (Ownership_Checks.cpp). per §22.4
            if (a.modifier == "out owned var" && a.value && currentScope_) {
                if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                                   a.value.get())) {
                    auto* lsym = ctx_.makeSymbol<LocalSymbol>();
                    lsym->name = id->name;
                    lsym->decl = id;
                    // Infer type from the callee's `out owned T` parameter.
                    //
                    // fix #3186 (§22.4 / §2.5, v0.254): through
                    // `formalOutParamType2`, the both-sources accessor #2657
                    // already added ~140 lines above for the PLAIN `out var`
                    // twin. This copy was never migrated, so it stayed
                    // `calleeMd2`-only — null for every non-generic imported
                    // method — and an `out owned var` recipient of an imported
                    // callee got NO declType at all. Measured consequences, all
                    // from that one null: `made.Nope` on a member that does not
                    // exist drew no §11.3/§12.1 diagnostic (the single-module
                    // twin says `no member Nope on type ResOo5`); `made.V` read
                    // back 0 where the twin reads 9, behind an `internal:`
                    // #2491/#2434 note; and codegen could lower no disposal of
                    // it, leaking 16 bytes per call under LSan against a clean
                    // twin. It also BLOCKED the `out owned` half of #3176: with
                    // the recipient untyped, arming the §28.3 obligation freed
                    // nothing and added a false reject.
                    if (TypeRef* formalTy2 = formalOutParamType2(pos2)) {
                        lsym->declType = formalTy2;
                        // §22.4 / §7.9 (#1597): keep the `out owned T?`
                        // parameter's declared nullability — mirrors the
                        // plain `out var` fix just above (same reasoning: the
                        // strip was cosmetically harmless for codegen's
                        // pointer lowering but wrong for the type checker).
                        // An `out owned conn: Connection? when true` recipient
                        // must stay `Connection?` so §22.4's postcondition
                        // narrowing and ordinary null checks on it apply; a
                        // non-nullable `out owned T` parameter is unaffected
                        // (declType->nullable is already false).
                        //
                        // §28.1 / §22.4 (v0.196, #1860): the formal type is the
                        // callee's UNSUBSTITUTED parameter. For a GENERIC method
                        // — including a CLASS-generic one
                        // (`Factory<T>.TryMake(out owned result: T)`) — resolve
                        // it through the RECEIVER's concrete generic args so the
                        // recipient carries its SUBSTITUTED type (`T`→`int`), not
                        // the bare `T`. Left as bare-`T`, codegen lowers the
                        // recipient to a pointer and a value read (`n.ToString()`)
                        // dispatches it as an object → SIGSEGV at a value
                        // substitution, and the ownership pass cannot see the
                        // §28.1 marker erasure so it draws a spurious §28.3 auto-
                        // drop warning. Generalizes the #807 interface
                        // substitution the plain `out var` handler does above to
                        // any generic parent; a non-generic / non-member callee
                        // is a no-op (substituteMemberType returns its input).
                        if (lsym->declType && calleeParent2 &&
                            !calleeParent2->genericParams.empty()) {
                            if (auto* maCal =
                                    dynamic_cast<const ast::MemberAccessExpr*>(
                                        ce->callee.get()))
                                if (maCal->target)
                                    lsym->declType = substituteMemberType(
                                        lsym->declType, maCal->target.get(),
                                        calleeParent2);
                        }
                    }
                    // §28.11 / v0.160 (#1305): for BUILTIN owning-collection Try*
                    // methods (TryPop/TryDequeue/TryPopFirst/TryPopLast/TryTake/
                    // TryPopValue/TryReceive) there is no MethodDecl (calleeMd2 is
                    // null), so the callee-param path above leaves declType null.
                    // This means ctx_.exprType on the local returns null later,
                    // and `item?.Dispose()` / `item.Dispose()` in
                    // lowerNullCondCall fail the recvIsClassOrIface check and emit
                    // nothing (the element leaks). Mirror the `out using var`
                    // inference: derive the element type from the receiver's
                    // generic args. per §28.11, §22.4
                    if (!lsym->declType) {
                        if (auto* maCallee =
                                dynamic_cast<const ast::MemberAccessExpr*>(
                                    ce->callee.get())) {
                            if (maCallee->target) {
                                if (const TypeRef* rt =
                                        ctx_.exprType(*maCallee->target)) {
                                    if (rt->kind == TypeRef::Kind::Named &&
                                        !rt->declaration && rt->astNode &&
                                        rt->astNode->name &&
                                        !rt->astNode->name->parts.empty()) {
                                        const std::string_view cn =
                                            rt->headName();
                                        const std::string_view mn =
                                            maCallee->member;
                                        // Owning-collection Try* methods —
                                        // element type from genericArgs[0] (T),
                                        // or genericArgs[1] (V) for
                                        // OwningDictionary.TryPopValue.
                                        const TypeRef* elemT = nullptr;
                                        if ((cn == "OwningStack" &&
                                             mn == "TryPop" && pos2 == 0) ||
                                            (cn == "OwningQueue" &&
                                             mn == "TryDequeue" && pos2 == 0) ||
                                            // §4.5 (#831/#1354): OwningArray
                                            // TryPopFirst/TryPopLast write the
                                            // element type T to the out param.
                                            (cn == "OwningArray" &&
                                             (mn == "TryPopFirst" ||
                                              mn == "TryPopLast") &&
                                             pos2 == 0) ||
                                            (cn == "OwningLinkedList" &&
                                             (mn == "TryPopFirst" ||
                                              mn == "TryPopLast") &&
                                             pos2 == 0) ||
                                            (cn == "OwningSet" &&
                                             mn == "TryTake" && pos2 == 1) ||
                                            (cn == "Channel" &&
                                             mn == "TryReceive" && pos2 == 0)) {
                                            if (!rt->genericArgs.empty() &&
                                                rt->genericArgs[0])
                                                elemT = rt->genericArgs[0];
                                        } else if (cn == "OwningDictionary" &&
                                                   mn == "TryPopValue" &&
                                                   pos2 == 1 &&
                                                   rt->genericArgs.size() >= 2 &&
                                                   rt->genericArgs[1]) {
                                            elemT = rt->genericArgs[1];
                                        }
                                        if (elemT) {
                                            // v0.171 (#1336): every v0.171
                                            // extraction Try's out param is `T?`
                                            // — the inline recipient is nullable
                                            // (its `!`/null-guarded discharge
                                            // narrows on the success path).
                                            // TryTake/TryReceive keep their
                                            // pre-v0.171 non-nullable local (they
                                            // are not part of the v0.171 sweep).
                                            bool v171 =
                                                mn == "TryPop" ||
                                                mn == "TryDequeue" ||
                                                mn == "TryPopFirst" ||
                                                mn == "TryPopLast" ||
                                                mn == "TryPopValue";
                                            TypeRef* nn =
                                                ctx_.makeTypeRef(*elemT);
                                            nn->nullable = v171 ? true : false;
                                            lsym->declType = nn;
                                        }
                                    }
                                }
                            }
                        }
                    }
                    currentScope_->names[id->name] = lsym;
                }
            }
            // §28.13 / §28.11 / stdlib §2.16 (#603 #890): `out using var x`
            // declares a using-var–scoped handle whose type is derived from
            // the receiver's generic element argument.
            //
            // Handled cases (all are builtins with no MethodDecl):
            //   Weak<T>.TryUpgrade                           → Shared<T> (arg 0)
            //   OwningStack<T>.TryPop                         → T         (arg 0)
            //   OwningQueue<T>.TryDequeue                     → T         (arg 0)
            //   OwningArray<T>.TryPopFirst/TryPopLast         → T         (arg 0)
            //   OwningLinkedList<T>.TryPopFirst/TryPopLast    → T         (arg 0)
            //   OwningSet<T>.TryTake                          → T (T?, arg 1)
            //   OwningDictionary<K,V>.TryPopValue             → V         (arg 1)
            //   Channel<T>.TryReceive                        → T (T?, arg 0)
            //
            // For T?-typed out params the spec still delivers the non-nullable
            // T to the `out using var` (parallel to the `out var` strip above).
            if (a.modifier == "out using var" && a.value && currentScope_) {
                if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                                   a.value.get())) {
                    auto* lsym = ctx_.makeSymbol<LocalSymbol>();
                    lsym->name = id->name;
                    lsym->decl = id;
                    // per §22.4 / §28.8 (#1304): infer the type from the
                    // callee's `out owned T` formal parameter for a USER-DEFINED
                    // method (the common case), so a later member access on the
                    // recipient (`res!.v`) resolves. The builtin derivation
                    // below handles the owning-collection / Weak Try* methods,
                    // which have no MethodDecl. Mirrors the `out owned var`
                    // handler just above; without this the recipient's declType
                    // stayed null and every member read on it lowered to a
                    // default value (the #1304 wrong-value bug).
                    //
                    // fix #3188 (§22.4 / §2.5, v0.254): through
                    // `formalOutParamType2`, the both-sources accessor #2657
                    // added for the plain `out var` twin and #3186 wired into
                    // the `out owned var` twin. This was the THIRD copy of the
                    // same question and the last one still `calleeMd2`-only —
                    // null for every non-generic imported method, so an
                    // `out using var` recipient of an imported callee got NO
                    // declType at all. MEASURED on ec6a02f against a
                    // character-identical single-module twin: exit **0 instead
                    // of 211** behind an `internal: return-value expression
                    // failed to lower … #2491` note, plus a **16-byte LSan leak
                    // per call** (`Direct leak of 16 byte(s) in 1 object(s) …
                    // #1 bz$UsLib.Fact$Make`) against a clean twin — the
                    // untyped recipient is a recipient codegen can lower no
                    // disposal for.
                    //
                    // ⚠ THE WIDENING, EXACTLY. `formalOutParamType2` returns
                    // the OLD expression verbatim whenever `calleeMd2` is
                    // non-null (same index, same `ctx_.resolvedType`, same
                    // null-on-missing), so every callee WITH an
                    // `ast::MethodDecl` keeps its answer bit-for-bit. The only
                    // new admission is `calleeMs2 && !calleeMd2` →
                    // `ParameterSymbol::resolvedDeclType`, which is reachable
                    // only for a callee that resolved to a `MethodSymbol`
                    // carrying no `MethodDecl`: the imported non-generic
                    // method. A BUILTIN Try* callee resolves to no
                    // `MethodSymbol` at all, so `calleeMs2` is null, the
                    // accessor still returns null, and the builtin derivation
                    // below runs exactly as before — the row this handler
                    // exists for is untouched.
                    if (TypeRef* formalUsingTy2 = formalOutParamType2(pos2)) {
                        lsym->declType = formalUsingTy2;
                        // §28.1 / §22.4 (v0.196, #1860): resolve a GENERIC
                        // method's formal `out owned T` through the RECEIVER's
                        // concrete generic args (incl. a CLASS-generic
                        // `Factory<T>.TryMake`) so the `out using var` recipient
                        // carries its SUBSTITUTED type (`T`→`int`), not the bare
                        // `T`. See the parallel note in the `out owned var`
                        // handler above: a bare-`T` recipient lowers to a pointer
                        // and a value read dispatches as an object → SIGSEGV at a
                        // value substitution. A non-generic / non-member callee is
                        // a no-op. // per §28.1 v0.196 #1860
                        if (lsym->declType && calleeParent2 &&
                            !calleeParent2->genericParams.empty()) {
                            if (auto* maCal =
                                    dynamic_cast<const ast::MemberAccessExpr*>(
                                        ce->callee.get()))
                                if (maCal->target)
                                    lsym->declType = substituteMemberType(
                                        lsym->declType, maCal->target.get(),
                                        calleeParent2);
                        }
                    }
                    // Derive the type from the builtin receiver.
                    if (!lsym->declType) {
                    if (auto* maCallee =
                            dynamic_cast<const ast::MemberAccessExpr*>(
                                ce->callee.get())) {
                        if (maCallee->target) {
                            if (const TypeRef* rt =
                                    ctx_.exprType(*maCallee->target)) {
                                if (rt->kind == TypeRef::Kind::Named &&
                                    !rt->declaration && rt->astNode &&
                                    rt->astNode->name &&
                                    !rt->astNode->name->parts.empty()) {
                                    const std::string_view cn =
                                        rt->headName();
                                    const std::string_view mn =
                                        maCallee->member;

                                    if (cn == "Weak" &&
                                        mn == "TryUpgrade" &&
                                        pos2 == 0 &&
                                        !rt->genericArgs.empty() &&
                                        rt->genericArgs[0]) {
                                        // Weak<T>.TryUpgrade → Shared<T>
                                        // Build a synthetic Shared<T> astNode
                                        // (same lambda pattern as kWeakNode in
                                        // ExprTypecheck_Calls.cpp ~L1762).
                                        static const ast::Type* kSharedNode =
                                            []() -> ast::Type* {
                                                auto* qn = new ast::QualifiedName;
                                                qn->parts.push_back("Shared");
                                                auto* t  = new ast::Type;
                                                t->kind  = ast::TypeKind::Named;
                                                t->name.reset(qn);
                                                return t;
                                            }();
                                        TypeRef* sharedTy = ctx_.makeTypeRef();
                                        sharedTy->kind = TypeRef::Kind::Named;
                                        sharedTy->declaration = nullptr;
                                        sharedTy->astNode = kSharedNode;
                                        sharedTy->genericArgs.push_back(
                                            const_cast<TypeRef*>(rt->genericArgs[0]));
                                        lsym->declType = sharedTy;

                                    } else if (
                                        // per §28.11, #890: owning-collection
                                        // Try* methods — element type from
                                        // receiver's generic argument.
                                        ((cn == "OwningStack" &&
                                          mn == "TryPop" && pos2 == 0) ||
                                         (cn == "OwningQueue" &&
                                          mn == "TryDequeue" && pos2 == 0) ||
                                         // §4.5 (#831/#1354): OwningArray
                                         // TryPopFirst/TryPopLast (out using var).
                                         (cn == "OwningArray" &&
                                          (mn == "TryPopFirst" ||
                                           mn == "TryPopLast") &&
                                          pos2 == 0) ||
                                         (cn == "OwningLinkedList" &&
                                          (mn == "TryPopFirst" ||
                                           mn == "TryPopLast") &&
                                          pos2 == 0) ||
                                         (cn == "OwningSet" &&
                                          mn == "TryTake" && pos2 == 1) ||
                                         (cn == "Channel" &&
                                          mn == "TryReceive" && pos2 == 0)) &&
                                        !rt->genericArgs.empty() &&
                                        rt->genericArgs[0]) {
                                        // Element type = genericArgs[0] (T).
                                        // v0.171 (#1336): the v0.171 extraction
                                        // Trys (TryPop/TryDequeue/TryPopFirst/
                                        // TryPopLast) deliver `owned T?` — the
                                        // recipient is nullable. TryTake/
                                        // TryReceive are NOT part of the v0.171
                                        // sweep and keep the non-nullable local.
                                        const TypeRef* elemT = rt->genericArgs[0];
                                        bool v171 =
                                            mn == "TryPop" ||
                                            mn == "TryDequeue" ||
                                            mn == "TryPopFirst" ||
                                            mn == "TryPopLast";
                                        TypeRef* nn = ctx_.makeTypeRef(*elemT);
                                        nn->nullable = v171 ? true : false;
                                        lsym->declType = nn;

                                    } else if (
                                        // OwningDictionary<K,V>.TryPopValue
                                        // — value type = genericArgs[1] (V).
                                        cn == "OwningDictionary" &&
                                        mn == "TryPopValue" &&
                                        pos2 == 1 &&
                                        rt->genericArgs.size() >= 2 &&
                                        rt->genericArgs[1]) {
                                        // v0.171 (#1336): `owned V?` recipient.
                                        const TypeRef* valT = rt->genericArgs[1];
                                        TypeRef* nn = ctx_.makeTypeRef(*valT);
                                        nn->nullable = true;
                                        lsym->declType = nn;
                                    }
                                }
                            }
                        }
                    }
                    }  // close `if (!lsym->declType)` (#1304)
                    currentScope_->names[id->name] = lsym;
                }
            }
            if (a.value) {
                // per §24 / stdlib §5 (D7): type the (single) untyped lambda
                // param as the event payload T (handler) or the LINQ source
                // element T, so its body resolves member access on the param.
                TypeRef* savedPending = pendingLambdaParamType_;
                TypeRef* savedLamRet = pendingLambdaReturnType_;
                std::vector<TypeRef*> savedLamParams = pendingLambdaParamTypes_;
                TypeRef* savedLamTargetRet = pendingLambdaTargetReturn_;
                bool savedLamTargetRetOwned = pendingLambdaTargetReturnOwned_;
                bool savedArgCtx = pendingLambdaArgContext_;
                if (auto* lam =
                        dynamic_cast<const ast::LambdaExpr*>(a.value.get())) {
                    // fix #3756 (§27.4): a call-argument lambda always has a
                    // target-type context — the matched formal's declared
                    // slot — whether or not THIS pass can thread its exact
                    // substituted type below.
                    pendingLambdaArgContext_ = true;
                    if (eventPayload)     pendingLambdaParamType_ = eventPayload;
                    else if (linqElem)    pendingLambdaParamType_ = linqElem;
                    // stdlib §5 (#1124): an `Aggregate(seed, (acc, x) => …)` fold
                    // lambda is `(TAccum, T) -> TAccum`. Seed BOTH parameters — acc
                    // from the seed's type, x from the source element — so an
                    // un-annotated `(acc, x)` resolves its body (else the inferred
                    // params stay untyped and codegen emits `ret void` against the
                    // accumulator return type → IR_FAIL).
                    else if (auto aggP = aggregateFoldLambdaParamTypes(*ce);
                             aggP.size() == 2 && lam->params.size() == 2)
                        pendingLambdaParamTypes_ = aggP;
                    // stdlib §5 Combination (#1845): a `Zip(other, (p, q) => …)`
                    // combine lambda is `(T, U) -> R` — seed BOTH params (T from
                    // the receiver element, U from the `other` arg's element) so
                    // an un-annotated `(p, q)` resolves its body and its return
                    // type R. Without this R stays unresolved: the Zip result is
                    // typed IEnumerable<T> (not IEnumerable<R>) and codegen sizes
                    // the yielded element / combine trampoline return by the wrong
                    // type — a heap overflow for an `int` R over a `string`
                    // receiver, or malformed IR for a `string` R.
                    else if (auto zipP = zipCombineLambdaParamTypes(*ce);
                             zipP.size() == 2 && lam->params.size() == 2)
                        pendingLambdaParamTypes_ = zipP;
                    // fix #829: a lambda bound to a function-typed parameter
                    // `(T1, …, Tn) -> R` — thread the positional Ti so an untyped
                    // lambda arg `(a, b) => …` gets its inferred param types (the
                    // body's operator/member resolution then sees real types).
                    // Skip a GENERIC callee: there the function-param type may be
                    // an unresolved type parameter (`(T) -> int`) inferred from
                    // another argument; the existing generic-inference path types
                    // the lambda param correctly, and threading the bare `T` here
                    // would override it with the unsubstituted parameter (#505).
                    // fix #4510: `formalFnParamType2` (both sources; see its
                    // header) in place of the former `calleeMd2`-only lookup.
                    // For a LOCAL callee it resolves through the very same
                    // `ctx_.resolvedType(*calleeMd2->params[pos2].type)` under
                    // the very same guards, so that path is unchanged; for an
                    // IMPORTED one it reads the reconstructed symbol's
                    // `resolvedDeclType`, which is where the formal's type
                    // lives when there is no AST decl (§2.8.2 clause 1).
                    if (calleeMs2) {
                        if (auto* pt = formalFnParamType2(pos2)) {
                            if (pt->kind == TypeRef::Kind::Function &&
                                !pt->funcParams.empty()) {
                                // #1140 / §14: a function-typed param `(T)->R`
                                // on a generic class Box<T> instantiated as
                                // Box<int> must yield `(int)->R` for lambda
                                // param inference, not the raw class type-param
                                // T. Build the class-level substitution map
                                // from the receiver's bound generic args
                                // (mirrors substituteMemberType's direct path).
                                std::unordered_map<std::string_view, TypeRef*>
                                    classSubst;
                                if (auto* ms2 = dynamic_cast<
                                        sema::MethodSymbol*>(calleeSym2)) {
                                    TypeSymbol* parentTs = ms2->parent;
                                    if (parentTs &&
                                        !parentTs->genericParams.empty()) {
                                        const ast::Expr* recv = nullptr;
                                        if (auto* maC = dynamic_cast<
                                                const ast::MemberAccessExpr*>(
                                                    ce->callee.get()))
                                            recv = maC->target.get();
                                        TypeRef* recvTy =
                                            recv ? ctx_.exprType(*recv)
                                                 : nullptr;
                                        // Fallback: local/param declared type.
                                        if (recv && (!recvTy ||
                                            recvTy->declaration != parentTs ||
                                            recvTy->genericArgs.empty())) {
                                            Symbol* sym =
                                                ctx_.exprSymbol(*recv);
                                            if (auto* loc = dynamic_cast<
                                                    LocalSymbol*>(sym)) {
                                                if (loc->declType &&
                                                    loc->declType->declaration
                                                        == parentTs &&
                                                    !loc->declType->genericArgs
                                                        .empty())
                                                    recvTy = loc->declType;
                                            } else if (auto* par =
                                                           dynamic_cast<
                                                               ParameterSymbol*>(
                                                               sym)) {
                                                if (par->declType) {
                                                    if (auto* prt =
                                                            ctx_.resolvedType(
                                                                *par->declType))
                                                        if (prt->declaration
                                                                == parentTs &&
                                                            !prt->genericArgs
                                                                .empty())
                                                            recvTy = prt;
                                                }
                                            }
                                        }
                                        if (recvTy &&
                                            recvTy->declaration == parentTs &&
                                            !recvTy->genericArgs.empty()) {
                                            for (std::size_t gi = 0;
                                                 gi < parentTs->genericParams
                                                              .size() &&
                                                 gi < recvTy->genericArgs
                                                              .size(); ++gi) {
                                                if (parentTs->genericParams[gi]
                                                    && recvTy->genericArgs[gi])
                                                    classSubst[parentTs
                                                        ->genericParams[gi]
                                                        ->name] =
                                                        recvTy->genericArgs[gi];
                                            }
                                        }
                                    }
                                }
                                pendingLambdaParamTypes_.clear();
                                for (const auto& fp : pt->funcParams) {
                                    TypeRef* paramTy = fp.type;
                                    if (!classSubst.empty())
                                        paramTy = substituteReturnType(
                                            paramTy, classSubst);
                                    pendingLambdaParamTypes_.push_back(paramTy);
                                }
                                // §27.3 (v0.226, #2107): thread the target
                                // function type's return slot so the block-body
                                // inference knows a target exists and suppresses
                                // the divergent-return error (divergent returns
                                // are checked against the target under §5.1, not
                                // unified by inference). Apply the same class-
                                // level generic substitution as the params.
                                pendingLambdaTargetReturn_ = pt->funcReturn;
                                // fix #1820 (§28.3 / §28.8): the call-arg's
                                // `funcReturnOwned` marker travels alongside
                                // the inner slot, exactly like the typed-local
                                // and assignment paths. The callee-side rule
                                // reads this to suppress its check on the
                                // §28.8 transferring branch.
                                pendingLambdaTargetReturnOwned_ =
                                    pt->funcReturnOwned;
                                if (!classSubst.empty() &&
                                    pendingLambdaTargetReturn_)
                                    pendingLambdaTargetReturn_ =
                                        substituteReturnType(
                                            pendingLambdaTargetReturn_,
                                            classSubst);
                            }
                        }
                    }
                    // issue #777 / §30.6: when an async lambda is bound to a
                    // function-typed parameter `(…) -> Task<T>` / `OwningTask<T>`,
                    // thread the peeled element T so the lambda visitor rejects a
                    // forgotten `await` in its body (same as the typed-local
                    // path). Positional binding via calleeMd2/pos2 mirrors the
                    // out-var inference above; the common positional call gets
                    // the hint, named/overloaded forms simply do not.
                    // fix #4443 (v0.302, §28.3 Keying rule): a parameter typed
                    // `(…) -> Task?` / `(…) -> Task<T>?` is mint-announcing at
                    // its declared type (nullability stripped per §28.3 Keying);
                    // use the stripped `isTaskFamilyRef` so the lambda body is
                    // checked under the peeled-element context the same way.
                    if (lam->isAsync && calleeMd2 &&
                        pos2 < calleeMd2->params.size() &&
                        calleeMd2->params[pos2].type) {
                        if (auto* pt = ctx_.resolvedType(
                                *calleeMd2->params[pos2].type)) {
                            if (pt->kind == TypeRef::Kind::Function &&
                                pt->funcReturn &&
                                isTaskFamilyRef(pt->funcReturn) &&
                                !pt->funcReturn->genericArgs.empty())
                                pendingLambdaReturnType_ =
                                    pt->funcReturn->genericArgs.front();
                        }
                    }
                    (void)lam;
                }
                // #1051 / §5.1: target-type an unsuffixed integer literal
                // argument to a `uint`/`long`/`ulong` parameter, so a literal
                // larger than `int.MaxValue` widens to the parameter type rather
                // than being rejected — mirroring the variable-initializer hint
                // (#184). Reset to None for any other parameter type so a too-
                // large literal for an `int` parameter still rejects.
                //
                // fix #3220 (§5.1 / §22.5 / §2.5, v0.254): the hint source is
                // the both-sources accessor `formalOutParamType2` (the same one
                // #2657 / #3186 / #3188 use to type `out var` recipients across
                // a module boundary), not the AST `calleeMd2->params[pos].type`
                // directly. Pre-fix the AST arm answered null for every imported
                // callee (ModuleLoader.cpp:337-346), so the hint was never
                // derived for `H.TakeU(3000000000)` against an imported
                // `TakeU(u: uint)` — the literal was rejected ("does not fit in
                // `int`") while the byte-identical local twin widened and ran.
                // Same widening, same answer, both sources — exactly the
                // recipient-typing fix's pattern, just consulted for §5.1 here.
                PrimitiveType savedArgHint = currentLiteralWideningHint_;
                currentLiteralWideningHint_ = PrimitiveType::None;
                if (TypeRef* prt = formalOutParamType2(pos2)) {
                    if (prt->kind == TypeRef::Kind::Primitive &&
                        (prt->primitive == PrimitiveType::Long ||
                         prt->primitive == PrimitiveType::ULong ||
                         prt->primitive == PrimitiveType::UInt))
                        currentLiteralWideningHint_ = prt->primitive;
                }
                // fix #2543 (§23, v0.236): exempt a `ref`/`out` argument
                // operand from the bare-read-of-write-only-indexer gate
                // (IndexExpr visit). The ref/out operand pass
                // `checkRefOutPropertyOperand` (#2530) is the specialized
                // gate and correctly rejects `ref` and admits `out`;
                // without this exemption an `out d[i]` against a
                // write-only indexer would be wrongly rejected here.
                const ast::Expr* savedRefOut = refOutArgTargetNode_;
                refOutArgTargetNode_ =
                    (a.modifier == "ref" || a.modifier == "out" ||
                     a.modifier == "out var" ||
                     a.modifier == "out using var" ||
                     a.modifier == "out owned var")
                        ? a.value.get()
                        : nullptr;
                visitExpr(*a.value);
                refOutArgTargetNode_ = savedRefOut;
                // per §22.12 rule 5 (v0.180, #1542): the handler argument
                // (arg 0) of a `<static event>.Subscribe(...)` / `.Add(...)`
                // call must be capture-free — a non-capturing lambda or a
                // static method reference, the same admission as a
                // Domain.Spawn entry (§30.14, #600). Checked here, after
                // visiting the argument, so isCaptureFreeEntry can see a
                // lambda's captures.
                if (pos2 == 0 && subscribeArgNeedsCaptureFree &&
                    !isCaptureFreeEntry(a.value.get())) {
                    ctx_.diagnostics().report(
                        Severity::Error, a.value->span.begin,
                        "handler subscribed to a `static event` must be "
                        "capture-free: a non-capturing lambda or a static "
                        "method reference — a capturing lambda is rejected "
                        "because the process-global subscriber list "
                        "transports it to whichever domain raises, and its "
                        "captures are borrows of the origin domain "
                        "(\xC2\xA7""22.12 rule 5, #1542)");
                }
                currentLiteralWideningHint_ = savedArgHint;
                pendingLambdaParamType_ = savedPending;
                pendingLambdaReturnType_ = savedLamRet;
                pendingLambdaParamTypes_ = savedLamParams;
                pendingLambdaTargetReturn_ = savedLamTargetRet;
                pendingLambdaTargetReturnOwned_ = savedLamTargetRetOwned;
                pendingLambdaArgContext_ = savedArgCtx;
            }
            // per §7.4 (#1188): a `ref`/`out` argument can be reassigned by the
            // callee — including being set to null — so a prior null-narrowing
            // (`nullNarrowed_`) or §12.7 type-narrowing (`narrowings_`) on the
            // passed-in local no longer holds after the call. Invalidate it,
            // mirroring the AssignmentStmt invalidation, so a later member
            // access re-requires a guard instead of dereferencing a nulled
            // value (ACCEPT_TRAP → SIGSEGV). A by-value argument cannot mutate
            // the caller's local, so its narrowing survives. The `out var` /
            // `out using var` forms declare a fresh local that has no prior
            // narrowing, so erasing is a harmless no-op there.
            if (a.value &&
                (a.modifier == "ref" || a.modifier == "out" ||
                 a.modifier == "out var" || a.modifier == "out using var" ||
                 a.modifier == "out owned var")) {  // per §22.4 / v0.160
                if (auto* aid = dynamic_cast<const ast::IdentifierExpr*>(
                        a.value.get())) {
                    if (aid->name != "this" && aid->name != "base") {
                        if (Symbol* asym = lookupName(aid->name)) {
                            if (dynamic_cast<LocalSymbol*>(asym) ||
                                dynamic_cast<ParameterSymbol*>(asym)) {
                                narrowings_.erase(asym);
                                nullNarrowed_.erase(asym);
                                // §7.4 v0.214 (#2085/#2116): the callee may
                                // rebind the ref/out local, so any `local._f`
                                // facts riding it no longer hold.
                                dropFieldFactsRooted(asym);
                            }
                        }
                    }
                }
            }
            // §22.3 (v0.184, #1682): validate the operand category of a
            // bare `ref`/`out` argument (a no-op for every other modifier).
            // Runs after the argument's own visitExpr above so exprSymbol is
            // populated for it. fix #2320 / §22.3 / §22.4: pass the FORMAL
            // parameter's modifier so the function can emit a clean §22.3
            // rejection for the `out owned` variant when the argument is
            // anything other than an inline `out using var` / `out owned var`
            // declaration (an indexer slot, a property, a bare local, …) —
            // those used to leak through to codegen's #1386 fallback.
            {
                std::string_view formalMod;
                // fix #3220 (§22.3 / §22.4 / §2.5, v0.254): the both-sources
                // version of this read; pre-fix AST-only, the AST arm is null
                // for every imported callee (`ModuleLoader` reconstructs the
                // `ParameterSymbol` list but not an `ast::MethodDecl`), so
                // `formalMod` stayed empty and the `out owned` gate in
                // `checkRefOutOperandCategory` was bypassed — the imported
                // callee silently accepted `Make(out <existing-local>)` (§22.4
                // violation, fixed per §22.4) and leaked through to codegen's
                // #1386 fallback. #3189's `formalParamModifier` (the unified
                // accessor in SymbolCollection.cpp:571-575) consults the AST
                // first then the `ParameterSymbol::modifier` field; for a
                // non-generic imported class member the loader populated
                // `ParameterSymbol::modifier` from the `MetaParam` wire bit
                // (#3176), so the imported arm answers the real modifier.
                //
                // ⚠ THIS FIX STARTS REJECTING PROGRAMS THAT COMPILED TODAY.
                // Per §22.4 an `out owned` formal may only receive an inline
                // `out using var` / `out owned var` declaration; every other
                // shape (a pre-existing local, an indexer slot, a property) is
                // a compile error. The new behaviour matches the local twin,
                // which was already rejecting; the imported callee now agrees.
                // Named-argument lookup mirrors the AST arm: walk the local
                // `ast::Param` list when one exists, otherwise the symbol
                // list, to derive `pidx` (the position the modifier then
                // answers for).
                std::size_t pidx = pos2;
                if (calleeMs2) {
                    if (!a.name.empty()) {
                        if (calleeMd2) {
                            for (std::size_t j = 0;
                                 j < calleeMd2->params.size(); ++j)
                                if (calleeMd2->params[j].name == a.name) {
                                    pidx = j;
                                    break;
                                }
                        } else {
                            for (std::size_t j = 0;
                                 j < calleeMs2->params.size(); ++j)
                                if (calleeMs2->params[j]->name == a.name) {
                                    pidx = j;
                                    break;
                                }
                        }
                    }
                    formalMod =
                        sema::formalParamModifier(*calleeMs2, pidx);
                }
                checkRefOutOperandCategory(a, formalMod);
            }
            ++pos2;
        }
    } else if (auto* nc = dynamic_cast<const ast::NullCondCallExpr*>(&e)) {
        if (nc->callee) visitExpr(*nc->callee);
        for (const auto& a : nc->args) {
            if (a.value) visitExpr(*a.value);
        }
        // §5.3 (#839): `T?(x)` — nullable-target primitive cast. The lexer
        // produces `?(` as a 2-char punctuator, so `long?(x)` is parsed as a
        // NullCondCallExpr whose callee is the bare primitive-type keyword.
        // When the callee is a primitive name with no symbol, set the result
        // type to that primitive nullable (e.g. `long?`) so that inferred
        // locals, null checks, and the `!` unwrap all see the correct type.
        bool typedAsPrimCast = false;
        if (nc->callee) {
            if (auto* id =
                    dynamic_cast<const ast::IdentifierExpr*>(nc->callee.get())) {
                if (!ctx_.exprSymbol(*nc->callee)) {
                    PrimitiveType pt = primitiveFromName(id->name);
                    if (pt != PrimitiveType::None) {
                        ctx_.setExprType(
                            &e, ctx_.primitiveTypeRef(pt, /*nullable=*/true));
                        typedAsPrimCast = true;
                    }
                }
            }
        }
        if (!typedAsPrimCast) {
            // §27.7 (#738): `f?(args)` invokes the nullable function value `f`
            // when non-null, else is a no-op; the result is the function's
            // return type made NULLABLE (null when f is null). Type it so an
            // inferred local / null check / arithmetic on the result resolves
            // (previously untyped → codegen produced a wrong value).
            if (nc->callee) {
                if (TypeRef* ft = ctx_.exprType(*nc->callee))
                    if (ft->kind == TypeRef::Kind::Function && ft->funcReturn) {
                        TypeRef* r = ctx_.makeTypeRef();
                        *r = *ft->funcReturn;
                        r->nullable = true;
                        ctx_.setExprType(&e, r);
                    }
            }
        }
    } else if (auto* ix = dynamic_cast<const ast::IndexExpr*>(&e)) {
        if (ix->target) visitExpr(*ix->target);
        for (const auto& a : ix->indices) {
            if (a.value) visitExpr(*a.value);
        }
        // fix #2758 (§23.5, v0.241): index arguments are positional-only.
        // This arm is the IndexExpr counterpart of the CallExpr arm's
        // per-argument `checkRefOutOperandCategory` above, and — like it —
        // runs once per node inside the single `visitChildren` walk. Placed
        // HERE rather than beside #2619's `checkIndexArgNullability` (which
        // lives under `if (tt)` in the receiver-typing block) because §23.5's
        // rule is a property of the index ARGUMENT alone: it must fire for a
        // receiver whose type never resolves, and it must not depend on which
        // receiver family the indexer belongs to.
        checkIndexArgsPositional(*ix);
    } else if (auto* bg = dynamic_cast<const ast::PostfixBangExpr*>(&e)) {
        if (bg->operand) visitExpr(*bg->operand);
    } else if (auto* ne = dynamic_cast<const ast::NewExpr*>(&e)) {
        // §22.4 (#968): `out var x` at a CONSTRUCTOR call site declares x in the
        // current scope, exactly as for a method call (the CallExpr path above).
        // Without this `new C(out var y)` reported `unresolved identifier y`.
        // Resolve the constructed type's single (or arity-matching) constructor
        // to infer the out-var's type from the matching `out` parameter.
        const ast::ConstructorDecl* ctorDecl = nullptr;
        // partial #2668 (§22.4 "Inline `out` declarations" / §2.5, v0.236): the
        // CONSTRUCTOR twin of #2657. An IMPORTED constructor has no
        // `ast::ConstructorDecl` — ModuleLoader.cpp:455-473 reconstructs a
        // `sema::ConstructorSymbol` whose `ParameterSymbol`s carry
        // `resolvedDeclType` — so the `dynamic_cast` filter below drops every
        // imported constructor and leaves `ctorDecl` null. The `out var`
        // recipient of `new ImportedT(…, out var n)` was therefore never given a
        // type. #2639's codegen half already sizes the caller's SLOT from the
        // imported symbol, so the runtime VALUE is correct; what remained is
        // that sema cannot type-check any read of the recipient, which is a
        // soundness hole, not a cosmetic one — mechanically confirmed on the
        // plain path against an imported `constructor(s: string, out n: string)`:
        //
        //   return n.NoSuchMemberAtAll;  -> ACCEPTED, compiles clean, returns 0
        //   var q: int := n;             -> not rejected; malformed IR in the
        //                                   backend ("Function return type does
        //                                   not match operand type")
        //
        // Track the SYMBOL alongside the decl and fall back to it, using the
        // same "single, else arity match" rule so local behaviour is identical.
        // This mirrors #2657's `formalOutParamType2` exactly, and — unlike the
        // `out owned var` / `out using var` gates below, which #2668 left
        // unfixed — it switches on NO dormant ownership tracking: a plain
        // `out var` recipient carries no disposal obligation (§22.4 rejects
        // plain `out var` at an `out owned` parameter outright).
        //
        // ⚠ UPDATED by #3206: those two gates are no longer unfixed. The
        // ownership tracking #2668 was waiting on landed (#2659, #3176) and was
        // measured live — the imported `out owned var` row already drew the
        // §28.3 auto-drop warning — so both now route through
        // `formalCtorOutParamType` as well. All three `NewExpr` forms consult
        // one accessor, as all three `CallExpr` forms consult
        // `formalOutParamType2` (#2657 / #3186 / #3188).
        const ConstructorSymbol* ctorSym = nullptr;
        if (ne->type) {
            if (auto* rt = ctx_.resolvedType(*ne->type)) {
                if (rt->kind == TypeRef::Kind::Named && rt->declaration) {
                    std::vector<const ast::ConstructorDecl*> cds;
                    std::vector<const ConstructorSymbol*> css;
                    for (Symbol* m : rt->declaration->members)
                        if (auto* cs = dynamic_cast<ConstructorSymbol*>(m))
                            if (!cs->isStatic) {
                                css.push_back(cs);
                                if (auto* cd =
                                        dynamic_cast<const ast::ConstructorDecl*>(
                                            cs->decl))
                                    cds.push_back(cd);
                            }
                    if (cds.size() == 1)
                        ctorDecl = cds[0];
                    else
                        for (auto* cd : cds)
                            if (cd->params.size() == ne->args.size()) {
                                ctorDecl = cd;
                                break;
                            }
                    if (!ctorDecl) {
                        if (css.size() == 1)
                            ctorSym = css[0];
                        else
                            for (auto* cs : css)
                                if (cs->params.size() == ne->args.size()) {
                                    ctorSym = cs;
                                    break;
                                }
                    }
                }
            }
        }
        // partial #2668: the constructor analogue of #2657's
        // `formalOutParamType2`. Prefers the AST declaration (so a LOCAL
        // constructor is bit-identical) and falls back to the imported
        // ConstructorSymbol's own parameter type.
        auto formalCtorOutParamType =
            [&](std::size_t pos) -> TypeRef* {
            if (ctorDecl) {
                if (pos < ctorDecl->params.size() && ctorDecl->params[pos].type)
                    return ctx_.resolvedType(*ctorDecl->params[pos].type);
                return nullptr;
            }
            if (ctorSym && pos < ctorSym->params.size())
                if (auto* ps = dynamic_cast<ParameterSymbol*>(
                        ctorSym->params[pos]))
                    return ps->resolvedDeclType;
            return nullptr;
        };
        std::size_t posn = 0;
        for (const auto& a : ne->args) {
            if (a.modifier == "out var" && a.value && currentScope_) {
                if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                                   a.value.get())) {
                    auto* lsym = ctx_.makeSymbol<LocalSymbol>();
                    lsym->name = id->name;
                    lsym->decl = id;
                    // partial #2668: `formalCtorOutParamType` is the former
                    // `ctx_.resolvedType(*ctorDecl->params[posn].type)` lookup
                    // plus the imported-constructor fallback; for a constructor
                    // WITH an ast::ConstructorDecl it returns exactly what this
                    // condition used to compute. In #2668 only the PLAIN
                    // `out var` gate was changed; fix #3206 moved the
                    // `out owned var` / `out using var` gates below onto the
                    // same accessor once the ownership tracking #2668 was
                    // waiting on had landed (#2659, #3176) and been measured
                    // live. All three forms now read one accessor.
                    if (TypeRef* formalCtorTy = formalCtorOutParamType(posn)) {
                        lsym->declType = formalCtorTy;
                        // §22.4 / §7.9 (#2409, mirroring #1597's CallExpr
                        // fix): record whether a read of this inline
                        // constructor out var on a path where the `new` call
                        // did not run is a definite-assignment error — true
                        // only for a NON-nullable reference-typed out param
                        // (no default). Captured from the DECLARED type
                        // BEFORE any nullable-strip, exactly as the
                        // method-call path (~line 4357) does.
                        lsym->outVarNeedsDefiniteAssignment =
                            isNonNullableReferenceType(lsym->declType);
                        // §22.4 / §7.9 (#2409, mirroring #1597): do NOT strip
                        // the out-param's declared nullability — see the
                        // CallExpr case's #1597 comment (~line 4359) for the
                        // full rationale, which applies identically here: a
                        // nullable ctor `out` param (`T?`) must produce a
                        // `T?` inline recipient so §7.4's nullable-member-
                        // access rule and §22.4's `when true`/`when false`
                        // narrowing have a nullable type to narrow from. This
                        // block used to unconditionally clear `nullable`
                        // here (the pre-#1597 CallExpr behavior, never
                        // updated when constructor out-vars gained this
                        // same inline form, #968/#1305) — harmless for
                        // codegen's pointer lowering but wrong for the type
                        // checker, which then treated the recipient as
                        // provably non-null forever after and silently
                        // widened past the constructor's declared `T?`
                        // signature. A non-nullable out param's declType
                        // already has `nullable == false`, so removing the
                        // strip is a no-op for that case.
                    }
                    currentScope_->names[id->name] = lsym;
                }
            }
            // §22.4 / v0.160 (#1305): `out owned var x` at a constructor call
            // site declares x inline (parallel to `out var` above). per §22.4
            if (a.modifier == "out owned var" && a.value && currentScope_) {
                if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                                   a.value.get())) {
                    auto* lsym = ctx_.makeSymbol<LocalSymbol>();
                    lsym->name = id->name;
                    lsym->decl = id;
                    // fix #3206 (§22.4 / §2.5, v0.254): through
                    // `formalCtorOutParamType`, the both-sources accessor
                    // partial-#2668 added ~140 lines above for the plain
                    // `out var` twin. #2668 left this copy `ctorDecl`-only ON
                    // PURPOSE — "they additionally switch on dormant ownership
                    // tracking that no oracle here can validate" — and that
                    // tracking has since landed (#2659 for constructor formals,
                    // #3176 for the `out owned` recipient pair). MEASURED on
                    // ec6a02f: the imported row ALREADY drew the §28.3
                    // auto-drop warning, so the obligation was armed and only
                    // the TYPE was missing; the recipient therefore read 0
                    // where its single-module twin read 211, behind an
                    // `internal:` #2491 note, and leaked 16 bytes per call
                    // under LSan (`#1 bz$CtLib.FactO$ctor`).
                    //
                    // ⚠ THE WIDENING, EXACTLY: `formalCtorOutParamType` returns
                    // this same expression verbatim whenever `ctorDecl` is
                    // non-null, so a constructor WITH a declaration keeps its
                    // answer bit-for-bit. The one new admission is
                    // `ctorSym && !ctorDecl` — and `ctorSym` is itself only
                    // populated under `if (!ctorDecl)` above — i.e. exactly the
                    // imported non-generic class.
                    if (TypeRef* formalOwnedTy = formalCtorOutParamType(posn)) {
                        lsym->declType = formalOwnedTy;
                        // §22.4 / §7.9 (#2409, mirroring #1597's CallExpr
                        // `out owned var` fix): do NOT strip the declared
                        // nullability — same reasoning as the plain `out var`
                        // case just above. Deliberately NOT setting
                        // `outVarNeedsDefiniteAssignment` here, matching
                        // #1597's own CallExpr `out owned var` block exactly
                        // (~line 4408 above): DefiniteAssignment.cpp's
                        // `visitCallArg` only consults that flag when
                        // `a.modifier == "out var"` literally, never for
                        // `out owned var` — per its own comment, the owning
                        // inline forms carry a per-path disposal obligation
                        // handled by the OWNERSHIP pass instead, and are
                        // deliberately not DA-tracked here.
                    }
                    currentScope_->names[id->name] = lsym;
                }
            }
            // §22.4 (v0.235, #2430): `out using var x` at a CONSTRUCTOR call
            // site declares x inline — the constructor analogue of the
            // `out owned var` block just above (parallel to how that block
            // parallels plain `out var`). §22.4 states the accepted call-site
            // forms for an `out owned` parameter are "`out using var x` (a
            // scope-bound owned local) and `out owned var x`" — both were
            // already handled for a METHOD call (ExprTypecheck_Exprs.cpp
            // ~line 4643), but this NewExpr argument-registration loop had a
            // block for `out var` (#968) and `out owned var` (#1305/#2409)
            // and no block at all for `out using var`, so `n` in
            // `new Widget(s, out using var n)` was never registered as a
            // local: every later reference to it failed the ordinary
            // unresolved-identifier check. Mirrors the `out owned var` block
            // immediately above (declType from the matching `out owned`
            // ctor formal, no generic-substitution / builtin-receiver
            // derivation — a constructor call is never a builtin Try*
            // method, so that machinery in the CallExpr sibling does not
            // apply here, matching this NewExpr path's existing scope for
            // its `out owned var` sibling).
            if (a.modifier == "out using var" && a.value && currentScope_) {
                if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                                   a.value.get())) {
                    auto* lsym = ctx_.makeSymbol<LocalSymbol>();
                    lsym->name = id->name;
                    lsym->decl = id;
                    // fix #3206 (§22.4 / §2.5, v0.254): through
                    // `formalCtorOutParamType`, exactly as its `out owned var`
                    // sibling immediately above now does — see that block for
                    // the measurement and for the widening, which is
                    // character-for-character the same here. With these two the
                    // SIX inline-`out` recipient sites (three at a `CallExpr`,
                    // three at a `NewExpr`) are one view per call form: two
                    // accessors, six callers, no site deciding for itself.
                    if (TypeRef* formalUsingTy = formalCtorOutParamType(posn)) {
                        lsym->declType = formalUsingTy;
                        // per §22.4 / §7.9: do NOT strip the declared
                        // nullability — same reasoning as the `out var` /
                        // `out owned var` blocks above.
                    }
                    currentScope_->names[id->name] = lsym;
                }
            }
            // #1051 / §5.1: target-type an unsuffixed integer literal constructor
            // argument to a `uint`/`long`/`ulong` parameter (an explicit ctor
            // parameter, or a positional-record field), so a literal > int.MaxValue
            // widens instead of being rejected. Reset to None otherwise so a too-
            // large literal for an `int` field still rejects.
            // fix #3220 (§5.1 / §22.5 / §2.5, v0.254): the explicit-ctor-parameter
            // arm reads through `formalCtorOutParamType` (the both-sources accessor
            // #3206 uses for the inline-`out` recipients above), not the AST
            // `ctorDecl->params[posn].type` directly — pre-fix the AST arm answered
            // null for every imported constructor and the widening hint was never
            // derived. The record-positional-field arm below was already symbol-side
            // via #3141.
            PrimitiveType savedNewHint = currentLiteralWideningHint_;
            currentLiteralWideningHint_ = PrimitiveType::None;
            {
                PrimitiveType hintPrim = PrimitiveType::None;
                if (TypeRef* prt = formalCtorOutParamType(posn)) {
                    if (prt->kind == TypeRef::Kind::Primitive)
                        hintPrim = prt->primitive;
                } else if (ne->type) {
                    // fix #3141 (family F3 of umbrella #3152, §9.1 / §5.1 /
                    // §2.8.3 rule 4): the positional field list is read
                    // SYMBOL-side. This block used to reach it through the
                    // record's `ast::TypeDecl`, which an imported record never
                    // has — so no hint was derived across a module boundary and
                    // `new Wide(3000000000, 5)` for an imported
                    // `record Wide(U: uint, L: long)` was REJECTED ("integer
                    // literal does not fit in `int`") while the byte-identical
                    // local record accepted it. `recordPositionalFieldSymbols`
                    // is the indexed view of the same list on both sides
                    // (#2516), and `storageComponentType` resolves a field's
                    // declared type from the positional `Param`, the body decl,
                    // or the importer's side table, in that order.
                    if (auto* rt = ctx_.resolvedType(*ne->type))
                        if (rt->kind == TypeRef::Kind::Named && rt->declaration &&
                            rt->declaration->hasPositionalFields) {
                            const std::vector<const Symbol*> posFields =
                                recordPositionalFieldSymbols(rt->declaration);
                            if (posn < posFields.size()) {
                                const Symbol* pf = posFields[posn];
                                const TypeRef* ftr =
                                    storageComponentType(pf, ctx_);
                                // #1052: a generic positional-record field whose
                                // declared type is a bare type parameter
                                // (`Box<T>(V: T)`) — resolve T to the concrete
                                // generic argument of THIS instantiation
                                // (`Box<uint>` → uint) so the literal widens to
                                // it. The parameter's spelling comes off the
                                // resolved field type's alias-transparent head,
                                // falling back to the local `ast::Param`'s own
                                // name node when the type did not resolve; the
                                // parameter NAMES are on the TypeSymbol on both
                                // sides (convertGenericParams).
                                std::string_view head =
                                    ftr ? ftr->headName() : std::string_view{};
                                if (head.empty())
                                    if (auto* fsy =
                                            dynamic_cast<const FieldSymbol*>(pf))
                                        if (fsy->posParam && fsy->posParam->type &&
                                            fsy->posParam->type->name &&
                                            fsy->posParam->type->name->parts
                                                    .size() == 1)
                                            head = fsy->posParam->type->name
                                                       ->parts[0];
                                bool subst = false;
                                if (!head.empty() &&
                                    !rt->declaration->genericParams.empty()) {
                                    for (std::size_t k = 0;
                                         k < rt->declaration->genericParams.size();
                                         ++k)
                                        if (rt->declaration->genericParams[k]
                                                ->name == head) {
                                            if (k < rt->genericArgs.size() &&
                                                rt->genericArgs[k] &&
                                                rt->genericArgs[k]->kind ==
                                                    TypeRef::Kind::Primitive)
                                                hintPrim =
                                                    rt->genericArgs[k]->primitive;
                                            subst = true;
                                            break;
                                        }
                                }
                                if (!subst && ftr &&
                                    ftr->kind == TypeRef::Kind::Primitive)
                                    hintPrim = ftr->primitive;
                            }
                        }
                }
                if (hintPrim == PrimitiveType::Long ||
                    hintPrim == PrimitiveType::ULong ||
                    hintPrim == PrimitiveType::UInt)
                    currentLiteralWideningHint_ = hintPrim;
            }
            // fix #2543 (§23, v0.236): exempt a `ref`/`out` constructor
            // argument operand from the bare-read-of-write-only-indexer
            // gate (IndexExpr visit). Same exemption as the CallExpr arg
            // visit above — the ref/out operand pass
            // `checkRefOutPropertyOperand` (#2530) is the specialized gate
            // for that position.
            const ast::Expr* savedRefOutNew = refOutArgTargetNode_;
            refOutArgTargetNode_ =
                (a.modifier == "ref" || a.modifier == "out" ||
                 a.modifier == "out var" ||
                 a.modifier == "out using var" ||
                 a.modifier == "out owned var")
                    ? a.value.get()
                    : nullptr;
            // fix #3756 (§27.4): a constructor-argument lambda has a
            // target-type context too — the matched formal's declared
            // slot — even though this NewExpr path has no
            // `pendingLambdaParamTypes_` per-parameter seeding at all
            // (unlike the CallExpr path's fix #829 machinery above).
            bool savedArgCtxNew = pendingLambdaArgContext_;
            // fix #3772 (§27.4 / §14.1, v0.287): when a constructor-argument
            // lambda is bound to a function-typed ctor parameter on a
            // GENERIC class (e.g. `new Box<R>(lambda(x) => x.V())` where
            // `Box<T>::ctor(scoped g: (T) -> int)`), the lambda's
            // unannotated parameter `x` MUST be typed to the concrete
            // generic argument (`R`), not to the bare class type-parameter
            // `T` — otherwise codegen computes the wrong value once the
            // closure escapes the constructor and is invoked from elsewhere
            // (issue #3772: prints `0` instead of `5`). This mirrors the
            // CallExpr path's fix-#829 / fix-#1140 machinery above — build
            // the class-level substitution map from the `new` expression's
            // resolved type's bound generic args and thread it through
            // `pendingLambdaParamTypes_` / `pendingLambdaTargetReturn_`
            // before visiting the lambda literal.
            //
            // Source: `formalCtorOutParamType(posn)` — the same
            // both-sources accessor the inline-`out` recipients (#2668 /
            // #3206 / #3186 / #3188) and the integer-literal widening hint
            // (#3220) above already consult. Returns the formal parameter's
            // resolved TypeRef for BOTH a local ctor (`ctorDecl` non-null)
            // and an imported ctor (`ctorSym` non-null) — the issue's
            // repro sits on the local side, but the imported side is
            // covered by the same accessor without further widening.
            std::vector<TypeRef*> savedLamParamsNew = pendingLambdaParamTypes_;
            TypeRef* savedLamTargetRetNew = pendingLambdaTargetReturn_;
            bool savedLamTargetRetOwnedNew = pendingLambdaTargetReturnOwned_;
            if (a.value &&
                dynamic_cast<const ast::LambdaExpr*>(a.value.get())) {
                pendingLambdaArgContext_ = true;
                if (TypeRef* pt = formalCtorOutParamType(posn)) {
                    if (pt->kind == TypeRef::Kind::Function &&
                        !pt->funcParams.empty()) {
                        // Build the class-level substitution map from the
                        // `new` expression's RESOLVED type's bound generic
                        // args, mapping the class's type-parameter name to
                        // the concrete type argument. The CallExpr path's
                        // `recvTy` analogue here is the resolved NewExpr
                        // target type (`rt = ctx_.resolvedType(*ne->type)`)
                        // — the class itself is `rt->declaration`.
                        std::unordered_map<std::string_view, TypeRef*>
                            classSubst;
                        if (ne->type) {
                            if (auto* rt = ctx_.resolvedType(*ne->type)) {
                                if (rt->declaration &&
                                    !rt->declaration->genericParams.empty() &&
                                    !rt->genericArgs.empty()) {
                                    for (std::size_t gi = 0;
                                         gi < rt->declaration->genericParams
                                                     .size() &&
                                         gi < rt->genericArgs.size(); ++gi) {
                                        if (rt->declaration->genericParams[gi]
                                            && rt->genericArgs[gi])
                                            classSubst[
                                                rt->declaration->genericParams[gi]
                                                    ->name] =
                                                rt->genericArgs[gi];
                                    }
                                }
                            }
                        }
                        pendingLambdaParamTypes_.clear();
                        for (const auto& fp : pt->funcParams) {
                            TypeRef* paramTy = fp.type;
                            if (!classSubst.empty())
                                paramTy = substituteReturnType(
                                    paramTy, classSubst);
                            pendingLambdaParamTypes_.push_back(paramTy);
                        }
                        // §27.3 (v0.226, #2107): thread the target function
                        // type's return slot too, so block-body divergent-
                        // return inference has a target and the inner
                        // §28.3 / §28.8 ownership marker travels. Apply the
                        // same class-level substitution as the params.
                        pendingLambdaTargetReturn_ = pt->funcReturn;
                        pendingLambdaTargetReturnOwned_ = pt->funcReturnOwned;
                        if (!classSubst.empty() &&
                            pendingLambdaTargetReturn_)
                            pendingLambdaTargetReturn_ =
                                substituteReturnType(
                                    pendingLambdaTargetReturn_, classSubst);
                    }
                }
            }
            if (a.value) visitExpr(*a.value);
            pendingLambdaArgContext_ = savedArgCtxNew;
            // fix #3772: restore the lambda-typing threading state so it
            // does not leak into the next constructor argument (parallel
            // to the CallExpr path's savedLamParams / savedLamTargetRet
            // restoration at ~line 7587 above).
            pendingLambdaParamTypes_ = savedLamParamsNew;
            pendingLambdaTargetReturn_ = savedLamTargetRetNew;
            pendingLambdaTargetReturnOwned_ = savedLamTargetRetOwnedNew;
            refOutArgTargetNode_ = savedRefOutNew;
            currentLiteralWideningHint_ = savedNewHint;
            // §22.3 (v0.184, #1682): validate a bare `ref`/`out` constructor
            // argument's operand category (a no-op for every other
            // modifier) — grammar A.4 admits ParamModifiers on a
            // constructor's ParamList same as a method's. fix #2320:
            // thread the constructor's formal param modifier so an
            // `out owned` constructor parameter receives the same
            // §22.3/§22.4 rejection as a method's. The ctor overload
            // resolution that `checkNewExprArgs` already ran BEFORE
            // visitChildren is the authority — the `ctorDecl` chosen
            // here is the same sole-or-first-arity guess the `out var`
            // block above uses, which mis-binds under same-arity
            // overloads (an `out owned Cell` ctor declared before an
            // `out int` ctor would otherwise wrongly tag an
            // `out n` argument as §22.3-out-owned). Re-resolve here by
            // `bindArgsToParams` on every arity-matching ctor, then
            // type-filter (a ref/out argument's type must EXACTLY match
            // the param's referent type per §14.5/§22.3, and a plain
            // argument's type must be §5.1-assignable to the param) to
            // find the candidate overload-resolution would pick. Apply
            // the rejection ONLY when exactly one type-compatible
            // candidate exists. Zero candidates defer; multiple
            // candidates defer (genuinely ambiguous — let overload-
            // resolution emit its own diagnostic). Mirrors
            // checkNewExprArgs's overload handling at ~line 8693.
            {
                std::string_view formalMod;
                if (ctorDecl && ne->type) {
                    if (auto* rt = ctx_.resolvedType(*ne->type)) {
                        if (rt->kind == TypeRef::Kind::Named &&
                            rt->declaration) {
                            const ast::Param* pickedParam = nullptr;
                            int compatibleCount = 0;
                            for (Symbol* m : rt->declaration->members) {
                                if (auto* cs =
                                        dynamic_cast<ConstructorSymbol*>(m)) {
                                    if (cs->isStatic) continue;
                                    auto* cd = dynamic_cast<
                                        const ast::ConstructorDecl*>(
                                        cs->decl);
                                    if (!cd) continue;
                                    if (cd->params.size() != ne->args.size())
                                        continue;
                                    BindResult b = bindArgsToParams(
                                        cd->params, ne->args, ne->span.begin,
                                        /*calleeDesc=*/std::string(),
                                        /*emit=*/false);
                                    if (!b.ok) continue;
                                    // Type filter: each ref/out argument
                                    // must exactly match the param's
                                    // referent type per §14.5/§22.3 (a
                                    // ref/out argument aliases the
                                    // caller's storage, so no implicit
                                    // conversion is admitted). Plain
                                    // positional args without a modifier
                                    // go through the standard overload-
                                    // resolution path (resolveOverload +
                                    // checkArgsAgainstParams in
                                    // checkNewExprArgs) — we do NOT
                                    // duplicate the §5.1 assignability
                                    // probe here; an exact ref/out
                                    // filter is sufficient to pick the
                                    // resolved overload for the §22.3/§22.4
                                    // diagnostic the issue calls out
                                    // (the indexer-slot/property cases
                                    // all carry an explicit `out`/`ref`/
                                    // `out owned` modifier).
                                    bool typeOk = true;
                                    for (std::size_t j = 0;
                                         j < cd->params.size(); ++j) {
                                        int ai = b.paramToArg[j];
                                        if (ai < 0) continue;
                                        const ast::CallArgument& arg =
                                            ne->args[ai];
                                        if (!arg.value) continue;
                                        const std::string_view am =
                                            arg.modifier;
                                        if (am != "ref" && am != "out")
                                            continue;
                                        const ast::Param& rp = cd->params[j];
                                        if (!rp.type) continue;
                                        TypeRef* argTy =
                                            ctx_.exprType(*arg.value);
                                        TypeRef* paramTy =
                                            ctx_.resolvedType(*rp.type);
                                        if (!argTy || !paramTy) continue;
                                        // §14.5 / §22.3: exact type
                                        // match for ref/out.
                                        if (!typeRefEqual(argTy,
                                                          paramTy)) {
                                            typeOk = false;
                                            break;
                                        }
                                    }
                                    if (!typeOk) continue;
                                    ++compatibleCount;
                                    if (compatibleCount == 1) {
                                        std::size_t pidx = posn;
                                        if (!a.name.empty()) {
                                            for (std::size_t j = 0;
                                                 j < cd->params.size(); ++j)
                                                if (cd->params[j].name ==
                                                    a.name) {
                                                    pidx = j;
                                                    break;
                                                }
                                        }
                                        if (pidx < cd->params.size())
                                            pickedParam = &cd->params[pidx];
                                    }
                                }
                            }
                            // Ambiguous or unresolved: defer —
                            // overload-resolution will pick the winner
                            // (and emit its own modifier-match
                            // diagnostic for an unmatched modifier).
                            // Picking here would risk a wrong-formal /
                            // false-reject.
                            if (compatibleCount == 1 && pickedParam)
                                formalMod = pickedParam->modifier;
                        }
                    }
                }
                checkRefOutOperandCategory(a, formalMod);
            }
            ++posn;
        }
    } else if (auto* ce = dynamic_cast<const ast::ConditionalExpr*>(&e)) {
        if (ce->condition) {
            visitExpr(*ce->condition);
            // §20: the `if` expression's condition must be bool.
            checkConditionIsBool(*ce->condition, "if-expression");
        }
        // §7.4 (v0.210, #2105): the §20 conditional atom's condition
        // narrows its own branches exactly as an if-STATEMENT condition
        // narrows its controlled block (per §7.4 v0.210 first bullet,
        // "in an `if`/`elif` condition … of the §20 conditional atom,
        // each condition narrowing its own branches"). Each branch is
        // seeded from the ENTRY state plus its own polarity's facts (the
        // other branch did not execute on this path), mirroring the
        // IfStmt walk in ExprTypecheck.cpp.
        //
        // fix #2258 / §7.4: the after-atom state is `entry ∩ thenExit ∩
        // elseExit`, NOT the entry snapshot. The #2105 landing justified
        // assigning the entry state back with "the post-`if` expression
        // state is irrelevant (a conditional atom yields one of the two
        // branch values, with no following statements on either path)" —
        // which is false: a branch VALUE can be a call, and execution
        // continues after the atom. A branch that invalidated a fact
        // (§7.4 "How narrowing is invalidated": a `ref`/`out` argument,
        // a call/`await` on a mutably-captured local) had that fact
        // RESURRECTED, so `if s = null then return 0; end; var n: int :=
        // (if c then C.ClearI(ref s) else 0); return s.Length;` was a
        // silent ACCEPT that SIGSEGV'd. Exactly one branch runs, so a
        // fact survives the atom only if it survived BOTH — the same
        // all-paths join the IfStmt walk computes over its fall-through
        // exits. Intersecting with `entry` additionally un-seeds what the
        // CONDITION established for a branch (it holds only inside that
        // branch).
        auto entryNarrowings = narrowings_;
        auto entryNullNarrowed = nullNarrowed_;
        auto entryFieldNarrowed = fieldNarrowed_;
        // §28.3 A7 (v0.209, #2080): an if-expression BRANCH is a marked-
        // transfer admission position — announce the exact child node.
        const ast::Expr* savedSlot = markedArmSlot_;
        bool haveThenExit = false;
        std::unordered_set<Symbol*> thenExitNull;
        std::unordered_map<Symbol*, TypeRef*> thenExitNarrowings;
        std::set<std::pair<std::uint32_t, std::uint32_t>> thenExitField;
        if (ce->thenValue) {
            // §12.7 (#2258): reset the TYPE narrowings per branch too — the
            // three sets are one state. Without this the `then` branch's
            // `b is Derived` fact was still live while the `else` VALUE was
            // checked, so `(if b is Derived then 1 else b.M())` resolved `M`
            // against `Derived` and compiled a wrong-typed dispatch on the
            // path where the test FAILED (probe-confirmed silent ACCEPT).
            narrowings_ = entryNarrowings;
            nullNarrowed_ = entryNullNarrowed;
            fieldNarrowed_ = entryFieldNarrowed;
            collectNarrowings(ce->condition.get(), /*forTrueBranch=*/true);
            markedArmSlot_ = ce->thenValue.get();
            visitExpr(*ce->thenValue);
            thenExitNull = nullNarrowed_;
            thenExitNarrowings = narrowings_;
            thenExitField = fieldNarrowed_;
            haveThenExit = true;
        }
        if (ce->elseValue) {
            narrowings_ = entryNarrowings;
            nullNarrowed_ = entryNullNarrowed;
            fieldNarrowed_ = entryFieldNarrowed;
            collectNarrowings(ce->condition.get(), /*forTrueBranch=*/false);
            markedArmSlot_ = ce->elseValue.get();
            visitExpr(*ce->elseValue);
        }
        markedArmSlot_ = savedSlot;
        // The live sets hold the else branch's exit (or, for a branch-less
        // malformed atom, the entry state); fold in the entry snapshot and the
        // then branch's exit. // per §7.4 (#2258)
        intersectNarrowingSetsWith(entryNullNarrowed, entryNarrowings,
                                   entryFieldNarrowed);
        if (haveThenExit)
            intersectNarrowingSetsWith(thenExitNull, thenExitNarrowings,
                                       thenExitField);
    } else if (auto* te = dynamic_cast<const ast::TypeTestExpr*>(&e)) {
        if (te->operand) visitExpr(*te->operand);
    } else if (auto* th = dynamic_cast<const ast::ThrowExpr*>(&e)) {
        if (th->operand) {
            visitExpr(*th->operand);
            checkThrowableType(*th->operand);  // §31.1 (#739)
        } else if (catchDepth_ == 0) {
            // Slice S27: bare `throw` expression re-raise is valid
            // only inside a catch arm body (§31.5 / §17.12).
            ctx_.diagnostics().report(
                Severity::Error, e.span.begin,
                "bare `throw` (re-raise) is only valid inside a "
                "catch arm body (§31.5)");
        }
    } else if (auto* lam = dynamic_cast<const ast::LambdaExpr*>(&e)) {
        // §30.6 (#488): an `async` lambda is the intersection of a closure
        // (§27.5) and an async state machine (§30.1/§30.2) — codegen lowers it
        // via emitAsyncLambda (the closure entry drives the state machine; the
        // captures + params round-trip the async frame). No diagnostic here:
        // the async-readiness gate below (`inAsyncContext_ = lam->isAsync`)
        // makes `await` valid inside the body, and the lambda's function type is
        // inferred normally (the funcReturn is the body's natural result T — the
        // awaited value). The lambda's `(…) -> Task<T>` form is supplied by the
        // call-site annotation (a typed local / function-typed parameter), which
        // codegen reads as the authoritative Task<T> at the closure-call site;
        // function-to-function assignability is not signature-checked (§5.1
        // function-type matching is a future slice), so the inferred `(…) -> T`
        // and the annotated `(…) -> Task<T>` coexist without a spurious error.
        // §A.7 / §21.7: lambdas create their own scope with parameter
        // symbols. Each parameter becomes a ParameterSymbol whose
        // declType is inferred from the explicit type annotation (untyped
        // lambda params remain null — callers must supply context).
        // per §24: an event-handler lambda — give its single untyped param
        // the event payload type T. Consumed once (cleared) so nested lambdas
        // don't inherit it.
        TypeRef* eventParamType = pendingLambdaParamType_;
        pendingLambdaParamType_ = nullptr;
        // fix #829: the inferred positional parameter types from the lambda's
        // expected function type (typed-local init / assignment / call arg).
        // Consumed once and cleared so a nested lambda doesn't inherit them.
        std::vector<TypeRef*> inferredParamTypes = pendingLambdaParamTypes_;
        pendingLambdaParamTypes_.clear();
        // fix #3756 (§27.4): true when this lambda literal is a direct call-
        // or constructor-argument, so a target-type context exists even
        // where neither of the two threads above named its exact type.
        // Consumed once so a nested lambda doesn't inherit it.
        bool hadArgContext = pendingLambdaArgContext_;
        pendingLambdaArgContext_ = false;
        // issue #777 / §30.6: the expected async element type T (peeled from a
        // `(…) -> Task<T>` context), threaded in to detect a forgotten `await`.
        // Consumed once so nested lambdas don't inherit it.
        TypeRef* lamRetHint = pendingLambdaReturnType_;
        pendingLambdaReturnType_ = nullptr;
        // §27.3 (v0.226, #2107): the target function type's return slot,
        // threaded from a typed-local initializer or a function-typed call
        // argument. When present the block body HAS a target return type and
        // the divergent-return inference error is suppressed. Consumed once so
        // a nested lambda doesn't inherit it.
        TypeRef* lamTargetRet = pendingLambdaTargetReturn_;
        pendingLambdaTargetReturn_ = nullptr;
        // fix #1820 (§28.3 / §28.8): the OUTER function type's `owned`
        // marker, threaded alongside the inner slot because the slot itself
        // does not carry the marker. Consumed once so a nested lambda doesn't
        // inherit it.
        bool lamTargetRetOwned = pendingLambdaTargetReturnOwned_;
        pendingLambdaTargetReturnOwned_ = false;
        // §27.3 (v0.217, #2101): the optional return-type ascription. When
        // present it behaves exactly as a method's declared return type —
        // each `return e;` (and an expression body) is checked against it
        // under §5.1 including implicit widening, the v0.159 typed-context
        // literal widening applies (`lambda(): long => 3_000_000_000`), and
        // a declared nullable return admits null-producing arms (§20). A
        // unit ascription (`: ()`) declares a procedure lambda. When absent,
        // all §27.3/§27.4 inference below is UNCHANGED.
        TypeRef* ascribedRet =
            lam->retType ? ctx_.resolvedType(*lam->retType) : nullptr;
        const bool hasAscription = ascribedRet != nullptr;
        const bool ascriptionIsUnit =
            hasAscription && ascribedRet->kind == TypeRef::Kind::Unit;
        // §30.6/§30.1 (v0.217): an `async` lambda's ascription is its
        // declared task-family return type — `Task`, `Task<T>`, or
        // `OwningTask<T>` — exactly as an async method's declared return
        // (§30.1). Anything else (including the unit ascription) is a
        // compile error; the async-iterator interfaces are method-level
        // forms (§30.7, `yield` is rejected in lambda bodies) and do not
        // apply here.
        // fix #4443 (v0.302, §17.13 Keying rule): this site STAYS strict
        // (`isTaskShapedRef`, non-nullable). §30.1's async-readiness mapping
        // is about the DECLARED wrapper — a `Task?` ascription is a different
        // question (the §17.13 Keying-strips-nullability ruling does not
        // extend to the declared-wrapper check).
        if (hasAscription && lam->isAsync && !isTaskShapedRef(ascribedRet)) {
            ctx_.diagnostics().report(
                Severity::Error, lam->span.begin,
                "an `async` lambda's return-type ascription must be `Task`, "
                "`Task<T>`, or `OwningTask<T>` \xe2\x80\x94 the async form's "
                "function type has a task-family return (\xc2\xa7""30.6, "
                "\xc2\xa7""30.1; \xc2\xa7""27.3 v0.217)");
        }
        Scope lamScope;
        lamScope.parent = currentScope_;
        std::vector<sema::Symbol*> lamParamSyms;
        for (size_t pi = 0; pi < lam->params.size(); ++pi) {
            const auto& p = lam->params[pi];
            if (p.destructure) {
                // §27.4 (v0.183, #1408): destructuring lambda parameter.
                // Resolve the head against the target type known from
                // context (if any) and bind each slot into lamScope; the
                // parameter itself keeps a synthetic, unnamed
                // ParameterSymbol of the resolved record type so the
                // funcParams/codegen positional plumbing below sees an
                // ordinary parameter slot (§27.4: "the lambda's function
                // type is unchanged by the spelling").
                TypeRef* target = pi < inferredParamTypes.size()
                    ? inferredParamTypes[pi] : nullptr;
                // §24 / stdlib §5 (D7): the spec's own §27.4 headline example
                // — `entries.Select((Entry(k, v)) => ...)` — is a single-param
                // LINQ/event-handler lambda, whose target type flows through
                // the SINGULAR eventParamType (set from linqLambdaElementType /
                // eventHandlerPayloadType above), not the plural
                // inferredParamTypes vector used by typed-local / multi-param
                // call-argument inference. Fall back to it exactly as the
                // ordinary (non-destructuring) branch below does, so that
                // example type-checks (and a wrong head is still caught).
                if (!target && eventParamType && lam->params.size() == 1)
                    target = eventParamType;
                TypeRef* recTyRef =
                    bindLambdaDestructure(*p.destructure, target, lamScope);
                auto* ps = ctx_.makeSymbol<ParameterSymbol>();
                ps->resolvedDeclType = recTyRef;
                ps->destructure = p.destructure.get();
                lamParamSyms.push_back(ps);
                continue;
            }
            auto* ps = ctx_.makeSymbol<ParameterSymbol>();
            ps->name = p.name;
            ps->modifier = p.modifier;
            // §30.6 / §30.1 (v0.184, #1670): an async lambda's parameter
            // slots are plain, `owned`, or `scoped` only — `ref`/`out` are
            // rejected for the same reason as an async method's (§30.1):
            // the lambda's parameters are captured into a heap-allocated
            // state-machine frame that outlives the synchronous call, so a
            // caller-stack-slot reference would dangle the moment the
            // lambda returns its task at the first suspension point.
            if (lam->isAsync &&
                (p.modifier == "ref" || p.modifier == "out" ||
                 p.modifier == "out owned")) {
                // `ast::Type::span` is never stamped by the parser (see the
                // method-level #1664 precedent), so point at the lambda
                // expression's own (reliably-stamped) span instead.
                ctx_.diagnostics().report(
                    Severity::Error, lam->span.begin,
                    std::string("`") + std::string(p.modifier) +
                    "` parameter `" + std::string(p.name) + "` is not "
                    "permitted on an `async` lambda — its parameters are "
                    "captured into a heap-allocated state-machine frame "
                    "that outlives the synchronous call, so a caller-"
                    "stack-slot reference would dangle the moment the "
                    "lambda returns its task at the first suspension "
                    "point (§30.6, v0.184)");
            }
            if (p.type) ps->declType = p.type.get();
            else if (eventParamType && lam->params.size() == 1)
                ps->resolvedDeclType = eventParamType;  // §24 handler param
            // fix #829: an untyped param takes its inferred type from the
            // lambda's expected function type, positionally.
            else if (pi < inferredParamTypes.size() && inferredParamTypes[pi])
                ps->resolvedDeclType = inferredParamTypes[pi];
            else if (!hadArgContext) {
                // fix #3756 (§27.4 / §27.1, v0.286): NONE of the three
                // context-supplying threads above named a type for this
                // parameter, AND it is not a call/constructor argument
                // either (`hadArgContext`, which covers the cases the
                // threads above cannot — a callee's own generic type
                // parameter, resolved later by monomorphization, and a
                // `new` constructor argument, which has no per-parameter
                // thread at all) — so there is genuinely no target-type
                // context anywhere. §27.4: "In top-level contexts with no
                // target type ... parameter types must be specified."
                // Previously this fell through silently: `ps->declType` /
                // `resolvedDeclType` stayed null, the lambda's own function
                // type recorded a null-typed slot too (the `fp.type` mirror
                // below), and the body's member/operator resolution against
                // the untyped parameter produced a wrong runtime value
                // instead of ANY diagnostic (#3756: `var h := lambda(owned
                // c): int => c.V(); ...; h(move p)` compiled clean and
                // printed `0`, not `5`).
                ctx_.diagnostics().report(
                    Severity::Error, lam->span.begin,
                    "parameter `" + std::string(p.name) + "` needs a type "
                    "annotation: no target type to infer from (§27.4)");
            }
            lamParamSyms.push_back(ps);
            if (!p.name.empty()) lamScope.names[p.name] = ps;
        }
        ctx_.setLambdaParams(lam, std::move(lamParamSyms));
        // §21.7: suppress enclosing callable's return context;
        // §21.5: reset loop/catch depth across the lambda boundary.
        Scope* saveScope = currentScope_;
        ReturnCtx saveRet = returnCtx_;
        TypeRef* saveRetTy = currentReturnType_;
        int saveLoop = loopDepth_;
        // §21.5 (v0.201, #2051): a lambda is its own callable — an enclosing
        // method's loop labels are not in scope inside it (mirrors the loopDepth_
        // reset). Saved/reset/restored across the boundary.
        auto saveLoopLabels = std::move(loopLabelStack_);
        loopLabelStack_.clear();
        // §7.4 (#2152): the break/continue edge-state collectors run parallel to
        // `loopLabelStack_` — save/clear/restore across the lambda boundary so a
        // `break`/`continue` inside the lambda can only snapshot into a
        // lambda-local loop's collector, never an enclosing method's (mirrors
        // the `loopLabelStack_` reset).
        auto saveLoopEdges = std::move(loopEdgeNarrowing_);
        loopEdgeNarrowing_.clear();
        int saveCatch = catchDepth_;
        // per §21.5/§31.2 (v0.182, #1578): a lambda is its own callable, so a
        // `break`/`continue` inside it can only ever target a loop declared
        // inside that same lambda (mirrors the `loopDepth_` reset above) —
        // it is never subject to an enclosing method's `finally` boundary.
        int saveFinallyLoopBoundary = finallyLoopBoundary_;
        // per §21.7 (#1589): a lambda is its own callable — `return` inside
        // it exits the lambda, not the enclosing method — so it is never
        // subject to an enclosing method's `finally` boundary either.
        // `finallyDepth_` must be reset across the lambda boundary (mirrors
        // `finallyLoopBoundary_` above); otherwise a `return` inside a lambda
        // nested in a `finally` is wrongly rejected as if it escaped that
        // `finally`.
        int saveFinallyDepth = finallyDepth_;
        // async-readiness 3b (§30.2 / §30.6): the lambda is its own async
        // boundary — `await` inside it is valid iff the lambda is `async`,
        // regardless of the enclosing method's async-ness.
        bool saveAsync = inAsyncContext_;
        inAsyncContext_ = lam->isAsync;
        // §17.15 (#2098): a lambda body is its own function — an `await` in a
        // nested async lambda suspends the lambda's own state machine, not
        // the frame the enclosing binding atom lives in, so the await-in-atom
        // targeted rejection does not apply across the boundary.
        bool saveInBindAtom = inBindAtom_;
        inBindAtom_ = false;
        // #721: a lambda inside an iterator method is its own callable, not an
        // iterator — `return;` in it is governed by the lambda's own rules.
        bool saveIter = inIteratorMethod_;
        inIteratorMethod_ = false;
        // §29.9 (#747 R42): mark that we are inside a lambda body so a `yield`
        // in it is rejected (yield is method-level only). Saved/restored for
        // nested lambdas.
        bool saveInLambda = inLambda_;
        inLambda_ = true;
        // §22.16 (v0.240, #2942): a lambda body is its own callable, not a
        // clause context. The contextual-keyword diagnostic for `result` /
        // `old` is meaningless inside a lambda (a lambda has no return value
        // to bind to, and is not in any member's clause scope). Saved/restored
        // so a nested lambda inside a clause does not falsely inherit the
        // surrounding clause's tracking.
        const bool saveInContractClause = inContractClause_;
        const ContractClauseKind saveClauseKind = currentClauseKind_;
        inContractClause_ = false;
        currentClauseKind_ = ContractClauseKind::None;
        // §22.4 (v0.183, #1585): a lambda is its own callable — a `return`
        // inside it never discharges the enclosing method's out-nullability
        // postcondition. Clear the set for the lambda body and restore after.
        auto saveOutPostconds = std::move(currentOutPostconds_);
        currentOutPostconds_.clear();
        // §7.4 / §27.5 (v0.236, #2113/#2116/#2259): the lambda body is DEFERRED
        // code — its narrowing fact state is not the enclosing statement's. All
        // three narrowing sets are saved now and restored after the body visit.
        //   IN:  a fact established BEFORE the lambda would ride in (capture),
        //        but §27.5 is explicit that capture is by REFERENCE —
        //        "Modifications to a captured variable inside the lambda are
        //        visible outside, and vice versa" — so the enclosing scope may
        //        rebind a captured local's cell AFTER the closure's creation
        //        but BEFORE its invocation, invalidating a fact the body
        //        type-checked against (#2259: the captured read returned null
        //        and SIGSEGV'd). The field set drops LOCAL-rooted facts here
        //        for this reason (rootId >= 1; #2116); the two local sets are
        //        ENTIRELY local-rooted (LocalSymbol/ParameterSymbol — §7.4
        //        narrows no `this`/static through them; readonly fields go in
        //        fieldNarrowed_), so each captured local's facts must be
        //        dropped here for the identical reason. Clearing the whole set
        //        is observably equivalent to dropping only the captured names'
        //        entries (the body cannot reference an uncaptured local, and
        //        the OUT-side restore below preserves the enclosing facts), so
        //        the simple, obviously-sound rule is to clear both sets — the
        //        direct analog of the field set's "drop all local-rooted". A
        //        `const` local is also cleared here: §7.4 says a const local's
        //        narrowing is "never invalidated" (const cannot be rebound),
        //        so preserving its fact would be sound; the field set already
        //        drops const-local-rooted field facts at this boundary for the
        //        same simplicity, and this fix matches that precedent rather
        //        than split the three sets' boundary policy. Re-narrow inside
        //        the body (`if x <> null then … end`) to recover a fact.
        //   OUT: the enclosing sets are RESTORED — a fact established inside
        //        the lambda body must not leak to the enclosing scope (the body
        //        may run never or later; #2113's hole, now closed for all
        //        three sets).
        auto saveNullNarrowed = nullNarrowed_;
        auto saveNarrowings = narrowings_;
        auto saveFieldNarrowed = fieldNarrowed_;
        // §7.4 (v0.236, fix #2597): drop every local-rooted fact on the way IN
        // — EXCEPT one rooted in a `const` local, which §7.4 exempts by name:
        // "A `const` local's narrowing (§21.1), once established, is never
        // invalidated: no invalidating form can apply to it — it cannot be
        // reassigned, cannot be passed as `ref` or `out`, and cannot be mutably
        // captured (assignment to a `const` is a compile error everywhere,
        // lambda bodies included)."
        //
        // The drop above exists because §27.5's capture is by REFERENCE, so the
        // enclosing scope can rebind a captured `var`'s cell between closure
        // creation and invocation (#2259: the captured read returned null and
        // SIGSEGV'd). A `const` has no such writer: it is the ONE root for which
        // the rebind that motivates the drop cannot happen. Both halves of that
        // premise are enforced by this compiler, not merely asserted by the
        // spec — assigning a const inside a lambda body is rejected ("cannot
        // assign to const local ... (§21.1)") and passing one as a `ref`/`out`
        // argument is rejected ("a `out` argument may not be a const local ...
        // (§22.3 / §21.1)"). So preserving a const-rooted fact admits exactly
        // the code §7.4 says is well-typed and admits nothing #2259 closed.
        //
        // All three sets keep the same boundary policy, as before — the field
        // set's local-rooted entries are filtered by the same predicate on
        // their ROOT (a `local._f` fact rides its root; the field segment is
        // `readonly`, which has no writer outside its construction window, so a
        // const root makes the whole path final). Ambient-rooted (rootId 0)
        // field facts continue to ride in untouched. The OUT-side restore below
        // is unchanged: a fact established INSIDE the body still must not leak.
        auto isConstLocal = [](const Symbol* s) {
            auto* ls = dynamic_cast<const LocalSymbol*>(s);
            return ls != nullptr && ls->isConst;
        };
        for (auto it = fieldNarrowed_.lower_bound({1u, 0u});
             it != fieldNarrowed_.end();) {
            const Symbol* root = it->first < narrowFactIdSyms_.size()
                                     ? narrowFactIdSyms_[it->first]
                                     : nullptr;
            if (isConstLocal(root)) ++it;
            else it = fieldNarrowed_.erase(it);
        }
        for (auto it = nullNarrowed_.begin(); it != nullNarrowed_.end();) {
            if (isConstLocal(*it)) ++it;
            else it = nullNarrowed_.erase(it);
        }
        for (auto it = narrowings_.begin(); it != narrowings_.end();) {
            if (isConstLocal(it->first)) ++it;
            else it = narrowings_.erase(it);
        }
        currentScope_ = &lamScope;
        // §27.3 (v0.217, #2101): with a return-type ascription the body is
        // checked exactly as a method body against a declared return type —
        // returnCtx_/currentReturnType_ drive the existing ReturnStmt
        // machinery (§5.1 assignability, §21.7 return-form rules, the §4.2
        // wider-integer literal hint, the §22.4 postcondition plumbing).
        // Without one the lambda keeps its no-return-context inference
        // posture unchanged.
        // fix #4443 (v0.302): STAYS strict (`isTaskShapedRef`, non-nullable)
        // — peeling the task wrapper (§30.1/§30.6) is keyed on the declared
        // wrapper; the ascription check above already rejected a non-Task-
        // family ascription, and a `Task?` ascription is the same question
        // the negation check decided.
        if (hasAscription && lam->isAsync && isTaskShapedRef(ascribedRet)) {
            // per §30.1/§30.6: peel the task wrapper — `Task` behaves as a
            // void-method context; `Task<T>`/`OwningTask<T>` as a value-
            // method context whose result type is the element T.
            asyncReturnCtxAndType(lam->retType.get(), returnCtx_,
                                  currentReturnType_);
            // issue #777: the ascribed element also serves as the forgotten-
            // `await` hint when no target-threaded hint exists.
            if (!lamRetHint) lamRetHint = currentReturnType_;
        } else if (hasAscription && !lam->isAsync) {
            returnCtx_ = ascriptionIsUnit ? ReturnCtx::VoidMethod
                                          : ReturnCtx::ValueMethod;
            currentReturnType_ = ascriptionIsUnit ? nullptr : ascribedRet;
        } else {
            returnCtx_ = ReturnCtx::None;
            currentReturnType_ = nullptr;
        }
        loopDepth_ = 0;
        catchDepth_ = 0;
        finallyLoopBoundary_ = -1;
        finallyDepth_ = 0;  // per §21.7 (#1589)
        // per §4.2/§5.1 (v0.159, v0.217): a value ascription of a wider
        // integer type is an admitting typed context for the expression
        // body — an unsuffixed literal exceeding `int`'s range widens to it
        // (`lambda(): long => 3_000_000_000`), mirroring the declared-
        // return-type hint at a method's `return` (§4.2, v0.182). Block-body
        // `return` statements get the same hint from the ReturnStmt visitor
        // via returnCtx_/currentReturnType_ set above.
        PrimitiveType saveLamWideningHint = currentLiteralWideningHint_;
        if (hasAscription && returnCtx_ == ReturnCtx::ValueMethod &&
            currentReturnType_ &&
            currentReturnType_->kind == TypeRef::Kind::Primitive) {
            using P = PrimitiveType;
            switch (currentReturnType_->primitive) {
                case P::Long: case P::ULong: case P::UInt:
                    currentLiteralWideningHint_ =
                        currentReturnType_->primitive;
                    break;
                default: break;
            }
        }
        if (lam->exprBody) {
            // per §15.4 (#1825): when a lambda's expression body is a collection
            // literal AND the lambda has a determinable target return type from
            // context (a typed-local / function-typed call argument), the literal
            // IS materialisable against that target — exactly like the call-arg
            // (#1174) and local-decl (#1866) paths. The selector / no-target
            // case still rejects per the L5480 guard below (the target is
            // `IEnumerable<U>`, an interface, never materialisable). Thread the
            // target onto the literal's exprType BEFORE visiting so the
            // CollectionLiteralExpr visitor sees the materialisation hint and
            // leaves the literal typed — codegen (LlvmBackend_Exprs2.cpp
            // lowerLambda) then reads `ctx_.exprType(*lam.exprBody)` and
            // materialises against the target type, mirroring the
            // `return [e1, …]` path in lowerReturnStmt.
            // per §15.4 (#3031): the predicate also consults the literal's
            // FORM. The empty literal `[]` is admitted at every §15.4
            // collection target — the empty carve-out is membership-only.
            const ast::CollectionLiteralExpr* lamBodyAdmittedLiteral = nullptr;
            if (auto* clE = dynamic_cast<const ast::CollectionLiteralExpr*>(
                    lam->exprBody.get())) {
                const bool isEmpty = !clE->isDict && clE->elements.empty();
                const bool admitted = lamTargetRet && (
                    (isEmpty &&
                     isCollectionLiteralEmptyAdmissibleTarget(lamTargetRet)) ||
                    (!isEmpty &&
                     isCollectionLiteralAdmissibleTarget(lamTargetRet,
                                                         clE->isDict)));
                if (admitted) {
                    ctx_.setExprType(lam->exprBody.get(), lamTargetRet);
                    lamBodyAdmittedLiteral = clE;
                }
            }
            // fix #4249 (§17.7, v0.306): the expression-body spelling of the
            // position the ReturnStmt visitor rejects for the block-body
            // spelling (ExprTypecheck.cpp). §17.7 excludes BOTH in one
            // clause — "It is **not** permitted at an expression body or at
            // a lambda's ascribed return" — and member (v)'s third
            // declared-result position is itself "a `return` operand or
            // expression body checked against a lambda's return-type
            // ascription" (§27.3), so rejecting only the statement form
            // would leave the identical silent miscompile one keystroke
            // away. Measured at e672e7e1: `lambda(): W => new()` compiled
            // with NO diagnostic at all (not even #2491's `internal:`
            // Warning the block-body form emits) and SEGV'd on the first
            // member read. Runs BEFORE the body visit, which for a typeless
            // NewExpr would type nothing and report nothing.
            if (auto* shNe =
                    dynamic_cast<const ast::NewExpr*>(lam->exprBody.get())) {
                if (!shNe->type)
                    rejectNewShorthandAtLambdaReturn(
                        *shNe,
                        currentReturnType_ ? currentReturnType_
                                           : lamTargetRet);
            }
            visitExpr(*lam->exprBody);
            // fix #4323 (§5.1/§11.2, v0.298): the SEVENTH site fix #4310
            // missed — an expression-bodied lambda's collection-literal body
            // typed via an outer (INFERRED, not written) function-type
            // target `lamTargetRet`. The admissibility gate above only
            // re-derives "is this target on §15.4's list", exactly like
            // #4310's other six call sites before that fix — it pre-sets the
            // literal's own exprType to the target and never descends into
            // the element expressions, so a class that never lists the
            // target interface in its inheritance clause was silently
            // accepted and interface dispatch on it read a null itable slot
            // (measured SIGSEGV, #4323's own repro: `var f: () -> [1]IHolder
            // := lambda() => [new Widget3(s)];` for a non-conforming
            // `Widget3`). Call the SAME shared helper #4310 factored rather
            // than hand-rolling an eighth element walk (§6/§25) — this is
            // its seventh caller, not a new authority. Runs AFTER
            // `visitExpr` above (unlike the other six sites, which visit
            // their initializer/argument BEFORE this check) because here the
            // target-threading and the literal's own visit happen in the
            // opposite order — the elements must already be typed before
            // `checkCollectionLiteralElementAssignability` reads their
            // `exprType`.
            if (lamBodyAdmittedLiteral)
                checkCollectionLiteralElementAssignability(
                    *lamBodyAdmittedLiteral, lamTargetRet);
        }
        currentLiteralWideningHint_ = saveLamWideningHint;
        // per §15.4 (#1693): a collection literal has no standalone type — it is
        // well-typed only where a *determinable, admissible concrete* target
        // exists (the §15.4 closed list: an inline array, or one of
        // `Array`/`OwningArray`, `Set`, `Stack`, `Queue`, `LinkedList`,
        // `Dictionary` and their owning forms, or a `FromCollectionLiteral`
        // type). A collection INTERFACE — the `IEnumerable<U>` a
        // `SelectMany`/`Select` selector returns — is deliberately NOT on that
        // list, and an expression-bodied lambda supplies no annotation, so a
        // bare collection-literal body has no target: the selector's own
        // element type is itself inferred FROM the body (circular). Without a
        // target the literal stayed untyped, `ctx_.exprType` was null, and
        // codegen emitted a null/empty sequence — a SILENT miscompile
        // (`xs.SelectMany((x) => [x, x + 10])` yielded an empty flattened
        // sequence). This is the same §15.4 defect #1174 fixed for a literal
        // passed DIRECTLY to an `IEnumerable<T>` parameter (rejected there);
        // reject it here too rather than miscompile. The idiom is a block body
        // binding an explicitly-typed local:
        // `(x) => do var inner: Array<U> := [...]; return inner; end`.
        // per §15.4 (#3031): the form-vs-target split applies here too — a
        // dict literal at a sequence-shaped lambda target (or vice versa) is
        // a form mismatch, not a missing-target case. Distinguish the two
        // with the dedicated form-specific diagnostic so the user is told
        // to change the literal's form, not bind a local.
        if (lam->exprBody &&
            dynamic_cast<const ast::CollectionLiteralExpr*>(lam->exprBody.get()) &&
            !ctx_.exprType(*lam->exprBody)) {
            const auto* cl =
                static_cast<const ast::CollectionLiteralExpr*>(
                    lam->exprBody.get());
            if (lamTargetRet &&
                !isCollectionLiteralInterfaceTarget(lamTargetRet) &&
                isCollectionLiteralAdmissibleTarget(lamTargetRet, /*isDict=*/false)) {
                if (cl->isDict)
                    reportCollectionLiteralDictFormBadTarget(
                        ctx_.diagnostics(), lam->exprBody->span.begin,
                        lamTargetRet);
                else
                    reportCollectionLiteralBadTarget(
                        ctx_.diagnostics(), lam->exprBody->span.begin,
                        lamTargetRet);
            } else {
                ctx_.diagnostics().report(
                    Severity::Error, lam->exprBody->span.begin,
                    "collection literal has no determinable target type as a "
                    "lambda's expression body — a collection literal is well-typed "
                    "only against a concrete collection target (an inline array, or "
                    "`Array<T>`/`OwningArray<T>`, `Set`, `Stack`, `Queue`, "
                    "`LinkedList`, `Dictionary` and their owning forms), never a "
                    "collection interface such as the `IEnumerable<U>` a "
                    "`SelectMany`/`Select` selector returns; use a block body with "
                    "an explicitly-typed local instead "
                    "(`(x) => do var inner: Array<U> := [...]; return inner; end`) "
                    "(§15.4)");
            }
        }
        for (const auto& s : lam->stmts) if (s) visitStmt(*s);
        // fix #1820 (§28.3, v0.223 ruling; carried in v0.282): Concrete-slot
        // function bodies — the row's callee-side half, mandatory per the
        // ruling (delta (4) was originally drafted as optional; the post-
        // critique ruling made it normative). When this lambda is checked
        // against a target whose return slot is a concrete class/interface
        // (per isConcreteSlotReturnType), every body return path must not
        // produce a fresh owned value (a `new T()`, a `move`, a Task-family
        // call result, or any other `owned`-returning call). The §28.3
        // producer row classifies every function-value invocation as a
        // borrow, so no caller would ever dispose a minted value — a
        // silent leak (cx3, cx4) or live two-owners pairing (p1820e family).
        // Remedy in the diagnostic: spell the slot's return `owned` to
        // transfer (the stdlib §4.14 convention `fn: (int) -> owned T`).
        //
        // The check runs only when lamTargetRet (the captured target
        // return slot, threaded from pendingLambdaTargetReturn_) is a
        // concrete class/interface slot, so an untyped-context lambda
        // (`var f := lambda() => new Res();` with no annotation) is
        // unaffected — the no-target inference still picks `Res` and the
        // §21.9 / §28.6 paths handle the rest. The check also does NOT
        // fire on an ASCRIBED lambda: an ascription is the programmer-
        // declared return slot, and its `return` statements go through
        // the ordinary `checkAssignable(currentReturnType_, …)` path
        // (line 8090+) which already constrains the body's types.
        // The two paths are distinct (typed-context vs. ascription) per
        // #2107's threading distinction.
        if (lamTargetRet && !hasAscription) {
            checkConcreteSlotLambdaBody(*lam, lamTargetRet,
                                        lamTargetRetOwned,
                                        /*savedCurrentReturnType=*/nullptr);
        }
        // fix #4340 (§5.1/§11.2, v0.298): the EIGHTH site fix #4310 missed —
        // a BLOCK-bodied lambda's `return` statement whose value is a
        // collection literal, typed via an outer (INFERRED, not written)
        // function-type target `lamTargetRet`. #4323 fixed the
        // EXPRESSION-bodied mirror (`lam->exprBody` is a collection literal);
        // this is its block-bodied twin — `do return [...]; end`. Both have
        // the identical §5.1/§11.2 element-conformance question, and both
        // miss the existing `ReturnStmt` site's `returnCtx_ == ValueMethod &&
        // currentReturnType_` gate: this branch leaves `returnCtx_ = None`
        // and `currentReturnType_ = nullptr` when `!hasAscription`, so the
        // site never re-reads the per-element types left behind by
        // `visitStmt`, the literal stays untyped (`ctx_.exprType(*literal)
        // == nullptr`), codegen's #2491 backstop silently degrades the
        // return value to a null/zero slot, and the typed-local caller then
        // SIGSEGVs on the first interface dispatch (#4323's signature).
        //
        // The fix mirrors #4323's pattern exactly — `setExprType` so the
        // literal is materialisable against `lamTargetRet` at codegen, then
        // the SAME shared `checkCollectionLiteralElementAssignability`
        // helper #4310 factored (§6/§25: "is there a fact the compiler
        // ALREADY COMPUTES that decides this?"; this is its EIGHTH caller,
        // not a new hand-rolled element walk). Walks every value-carrying
        // `return e;` in the block body (descending through do/end, if/elif/
        // else, while, for, try/catch/finally, statement-position case
        // block arms) via the same `forEachLambdaReturnValue` helper
        // `checkConcreteSlotLambdaBody` (#1820) uses, so the rejection sees
        // the same return-path set the inference sees.
        //
        // Scoped to the unascribed-but-target-typed form (the exact twin of
        // #4323's mirror arm): `lam->isBlockBody` (not the expression-body
        // case `#4323` already patched), `!hasAscription` (the ascription
        // branch above already sets `returnCtx_ = ValueMethod`/
        // `currentReturnType_ = ascribedRet` and reaches the same
        // per-element check through #4310's own first-return twin at
        // `ExprTypecheck.cpp:14885-14918`), and `lamTargetRet` is on
        // §15.4's closed collection-target list (the empty carve-out
        // included — `[]` is admissible at every §15.4 collection target;
        // the form-vs-target question is carried through unchanged). A
        // non-collection `lamTargetRet` (the `var f: () -> int := …` body
        // where every `return e;` happens to be a literal-free expression)
        // is unaffected, so the broader "every target return checks
        // returnable values" question (a real but separate defect class)
        // stays out of scope — its shape differs enough that bundling would
        // dilute this regression test.
        if (!hasAscription && !lam->isAsync && lam->isBlockBody &&
            lamTargetRet && !lamTargetRetOwned &&
            (isCollectionLiteralAdmissibleTarget(lamTargetRet,
                                                 /*isDict=*/false) ||
             isCollectionLiteralEmptyAdmissibleTarget(lamTargetRet))) {
            forEachLambdaReturnValue(*lam, [&](const ast::Expr* retVal) {
                if (!retVal) return;
                auto* clE =
                    dynamic_cast<const ast::CollectionLiteralExpr*>(retVal);
                if (!clE) return;
                // The expression-body arm `lam->exprBody` is non-null
                // ONLY for #4323's case (an expression-bodied lambda); we
                // require `isBlockBody` above, so `lam->exprBody` is null
                // and `forEachLambdaReturnValue` will not visit it here.
                // The dynamic_cast inside `forEachLambdaReturnValue`'s
                // expr-body slot would anyway only fire for an expression-
                // bodied lambda — its visited body is the literal, and the
                // #4323 fix already threads it; double-checking would
                // duplicate the same diagnostic if it ever leaked through.
                // Skip the form-vs-target mismatch (`[k: v]` at a
                // sequence-shaped target — distinct from the
                // bad/missing-target question, which `reportCollection-
                // LiteralBadTarget` would handle at L14885+ when that gate
                // is reached via the ascribed path; here we just refuse to
                // materialise the mismatched form against the target).
                const bool isEmpty =
                    !clE->isDict && clE->elements.empty();
                const bool retValAdmitted = isEmpty
                    ? isCollectionLiteralEmptyAdmissibleTarget(lamTargetRet)
                    : isCollectionLiteralAdmissibleTarget(lamTargetRet,
                                                          clE->isDict);
                if (!retValAdmitted) return;
                ctx_.setExprType(clE, lamTargetRet);
                checkCollectionLiteralElementAssignability(
                    *clE, lamTargetRet);
            });
        }
        // issue #777 / §30.6: forgotten `await` in an expression-bodied async
        // lambda. The body must produce the element type T threaded in as
        // lamRetHint, not a `Task` value. A Task-shaped body where T is not
        // itself Task-shaped is a forgotten await — codegen would store the
        // task pointer into the element-sized async result slot (corruption /
        // SIGSEGV). The legitimate `(…) -> Task<Task<U>>` case is exempt because
        // lamRetHint (= Task<U>) is itself Task-shaped.
        // fix #4443 (v0.302): STAYS strict (`isTaskShapedRef`, non-nullable).
        // lamRetHint is the peeled element type threaded in from the declared
        // ascription (non-nullable per §30.1), and a body's static type at
        // this position is the body's declared wrapper check — a `Task?` body
        // is a different question the dedicated diagnostics handle.
        if (lam->isAsync && lam->exprBody && lamRetHint &&
            !isTaskShapedRef(lamRetHint) &&
            isTaskShapedRef(ctx_.exprType(*lam->exprBody))) {
            ctx_.diagnostics().report(
                Severity::Error, lam->exprBody->span.begin,
                "an expression-bodied `async` lambda's body must produce the "
                "element type of its `Task<T>`/`OwningTask<T>` return, not a "
                "`Task` value; add `await` (e.g. `async () => await Producer()`)"
                " (§30.6)");
        }
        // §27.3 (v0.217, #2101): an ascribed EXPRESSION body must satisfy the
        // ascription under §5.1, exactly as a method's expression body
        // satisfies its declared return type (for an async lambda the peeled
        // element type is the check target, §30.1/§30.6). A unit ascription's
        // expression body is §27.3's discarded-result procedure form — no
        // check.
        if (hasAscription && !ascriptionIsUnit && lam->exprBody &&
            returnCtx_ == ReturnCtx::ValueMethod && currentReturnType_) {
            checkAssignable(ctx_.exprType(*lam->exprBody), currentReturnType_,
                            lam->exprBody->span.begin, "returned value",
                            lam->exprBody.get());
        }
        // §27.3/§21.7 (v0.217): a value-ascribed BLOCK body must return a
        // value on every path — the same totality rule as a method with a
        // declared return type (and as §27.3's own "return is required on
        // every exit path").
        if (hasAscription && lam->isBlockBody &&
            returnCtx_ == ReturnCtx::ValueMethod &&
            !bodyAlwaysExits(lam->stmts)) {
            ctx_.diagnostics().report(
                Severity::Error, lam->span.begin,
                "lambda has a return-type ascription but does not return a "
                "value on every path (\xc2\xa7""27.3, \xc2\xa7""21.7)");
        }
        currentScope_ = saveScope;
        returnCtx_ = saveRet;
        currentReturnType_ = saveRetTy;
        // §7.4 (v0.226, #2113/#2116): restore the enclosing fact sets — facts
        // established inside the (deferred) lambda body never leak out.
        nullNarrowed_ = std::move(saveNullNarrowed);
        narrowings_ = std::move(saveNarrowings);
        fieldNarrowed_ = std::move(saveFieldNarrowed);
        currentOutPostconds_ = std::move(saveOutPostconds);  // §22.4 (#1585)
        inAsyncContext_ = saveAsync;
        inBindAtom_ = saveInBindAtom;  // §17.15 (#2098)
        inIteratorMethod_ = saveIter;
        inLambda_ = saveInLambda;
        inContractClause_ = saveInContractClause;
        currentClauseKind_ = saveClauseKind;
        loopDepth_ = saveLoop;
        loopLabelStack_ = std::move(saveLoopLabels);  // §21.5 (v0.201)
        loopEdgeNarrowing_ = std::move(saveLoopEdges);  // §7.4 (#2152)
        catchDepth_ = saveCatch;
        finallyLoopBoundary_ = saveFinallyLoopBoundary;
        finallyDepth_ = saveFinallyDepth;  // per §21.7 (#1589)
        // §A.7: set the lambda's exprType to a Function TypeRef so
        // that `var f := lam; f(...)` calls type-check correctly and
        // lowerCall can detect it as a function-typed local.
        {
            auto* lamTy = ctx_.makeTypeRef();
            lamTy->kind = TypeRef::Kind::Function;
            const auto* lamParamSymsOut = ctx_.lambdaParams(lam);
            for (size_t pi = 0; pi < lam->params.size(); ++pi) {
                const auto& p = lam->params[pi];
                TypeRef::FuncParam fp;
                if (p.destructure) {
                    // §27.4: "the lambda's function type is unchanged by the
                    // spelling (`(Entry) -> R`)" — the positional slot's type
                    // is the exact record type resolved in the param-binding
                    // loop above (no name: destructuring has no single
                    // parameter identifier).
                    if (lamParamSymsOut && pi < lamParamSymsOut->size()) {
                        if (auto* ps = dynamic_cast<ParameterSymbol*>(
                                (*lamParamSymsOut)[pi]))
                            fp.type = ps->resolvedDeclType;
                    }
                    lamTy->funcParams.push_back(std::move(fp));
                    continue;
                }
                fp.modifier = p.modifier;
                fp.name = p.name;
                if (p.type) fp.type = ctx_.resolvedType(*p.type);
                // fix #829: reflect an inferred param type in the lambda's own
                // function type so a later `f(...)` call type-checks against it.
                else if (pi < inferredParamTypes.size() && inferredParamTypes[pi])
                    fp.type = inferredParamTypes[pi];
                lamTy->funcParams.push_back(std::move(fp));
            }
            // Infer the return type: from exprBody for expression lambdas,
            // or by scanning block-body stmts for the first `return expr`.
            // §27.3 (v0.217, #2101): a return-type ASCRIPTION overrides both —
            // it is the declared return type, carried in the function type as
            // written (the task-family type itself for an async lambda,
            // §30.6; the Unit type for a `: ()` procedure ascription).
            TypeRef* inferredReturn = nullptr;
            if (hasAscription) {
                inferredReturn = ascribedRet;
            } else if (lam->exprBody) {
                inferredReturn = ctx_.exprType(*lam->exprBody);
            } else {
                // §27.3 (v0.226, #2107): recursively collect EVERY value-
                // carrying `return e;` type in the block body (descending
                // through statement wrappers — do/end, if/elif/else, while,
                // for, try/catch/finally, statement-position case block arms
                // — but NOT into nested lambdas, which are their own
                // callables). The first collected type is the inferred return,
                // replacing the old flat first-return scan that missed returns
                // nested below one wrapper level and silently fell back to
                // `int`. And under §27.3, a no-target block body with
                // divergent value-return types is a compile error — the
                // compiler does not unify them to a common supertype.
                std::vector<TypeRef*> returnTypes;
                collectLambdaReturnTypes(lam->stmts, returnTypes);
                if (!returnTypes.empty())
                    inferredReturn = returnTypes.front();
                // Fallback: use int (i32) for lambdas with no visible value
                // return (a procedure lambda / all-bare-returns body).
                if (!inferredReturn)
                    inferredReturn = ctx_.primitiveTypeRef(PrimitiveType::Int);
                // §27.3 (v0.226, #2107): the divergent-return error fires only
                // when the lambda has NEITHER a return-type ascription NOR a
                // target return type (lamTargetRet), AND the body has ≥2 value
                // returns whose types are not all the SAME type. The programmer
                // states the intended type instead — a target type, a return-
                // type ascription, or an ascribed variable type.
                if (!hasAscription && lamTargetRet == nullptr &&
                    returnTypes.size() >= 2) {
                    TypeRef* first = returnTypes.front();
                    bool divergent = false;
                    for (std::size_t ri = 1; ri < returnTypes.size(); ++ri) {
                        if (!functionSlotTypeRefEqual(first, returnTypes[ri])) {
                            divergent = true;
                            break;
                        }
                    }
                    if (divergent) {
                        ctx_.diagnostics().report(
                            Severity::Error, lam->span.begin,
                            "a block-bodied lambda with no target type has "
                            "divergent `return` types \xe2\x80\x94 its return "
                            "type cannot be inferred; give it a target type, a "
                            "return-type ascription "
                            "(`lambda(\xe2\x80\xa6): T => \xe2\x80\xa6`), or an "
                            "ascribed variable type "
                            "(`var f: (\xe2\x80\xa6) -> T := \xe2\x80\xa6`) "
                            "(\xc2\xa7""27.3)");
                    }
                }
            }
            // §30.6 (#800, partial): the function type of an ASYNC lambda has a
            // `Task<T>` (or `OwningTask<T>` for a class-typed, ownership-
            // transferring result) return — the body's natural result T is
            // wrapped, exactly as for an async method (§30.1). Without this the
            // INFERRED type of `var f := async () => do return 1; end` was
            // `() -> T`, so `await f()` was rejected ("await requires a Task
            // operand"); an explicit `f: () -> Task<T>` annotation supplied the
            // wrapping but inference did not.
            //
            // Applies to BOTH block- and expression-bodied async lambdas (#800
            // Layer-2). The codegen reads this wrapped funcReturn (Task<T>) for an
            // expression-bodied async lambda too (LlvmBackend_Exprs.cpp lowerLambda
            // derives the closure's async result type from the lambda's exprType
            // funcReturn when it is async), so the body's value is wrapped into the
            // outer Task instead of returned raw.
            // §27.3 (v0.217): an ascribed async lambda already states its
            // task-family type — no wrap (hasAscription excluded below).
            if (!hasAscription && lam->isAsync && inferredReturn) {
                bool ownedResult =
                    inferredReturn->kind == TypeRef::Kind::Named &&
                    inferredReturn->declaration &&
                    inferredReturn->declaration->kind == ast::TypeDeclKind::Class;
                auto* taskTy = ctx_.makeTypeRef();
                taskTy->kind = TypeRef::Kind::Named;
                taskTy->nullable = false;
                auto* ann = new ast::Type();
                ann->kind = ast::TypeKind::Named;
                auto* nm = new ast::QualifiedName();
                nm->parts.push_back(ownedResult ? "OwningTask" : "Task");
                ann->name.reset(nm);
                taskTy->astNode = ann;
                taskTy->genericArgs.push_back(inferredReturn);
                inferredReturn = taskTy;
            }
            lamTy->funcReturn = inferredReturn;
            // fix #4027 (§28.3 *Factory methods and sourceless returns*,
            // v0.295): an UNTYPED lambda literal — neither a return-type
            // ascription (`hasAscription` true) NOR a target function type
            // whose return slot is marked `owned` (`lamTargetRetOwned` true)
            // NOR any target return at all (`lamTargetRet` null) — must
            // still set `funcReturnOwned = true` on its inferred function
            // type when its body UNCONDITIONALLY returns a fresh owned
            // class production. Without this the producerState classifier
            // at Ownership.cpp's CallExpr arm (~line 4597,
            // `calleeTy->kind == Function && calleeTy->funcReturnOwned`)
            // classifies the call result BORROWED for the untyped lambda's
            // own `() -> T` callee TypeRef — `lamTy->funcReturnOwned` is
            // never set here even when the body's inferred return IS a
            // fresh owned production — so the freshly-allocated object is
            // never tracked for disposal and LEAKS (the issue's measured
            // 16 B leak on `var f := lambda() => new Res(9); var c := f();`).
            // Mirrors `staticFactoryReturnsFreshOwned` (OwnershipFacts.cpp)
            // for the named-method / static-factory case: that helper is
            // already consulted by producerState for every non-`owned`
            // static return whose body is unambiguously fresh — the
            // lambda form was simply never wired into the same signal.
            // Gated on `!hasAscription && !lamTargetRetOwned && !lamTargetRet`:
            //   - `!hasAscription` — the user explicitly typed the return,
            //     do not override the declaration they wrote;
            //   - `!lamTargetRetOwned` — the existing concrete-slot and
            //     owned-target paths (#1820, #4005) already cover the case
            //     where a target is present (target-side is read by
            //     checkOneLambdaOwnedReturnDirectBorrow / -TaskFamilyBorrow
            //     Return via `expectedFnType` OR'd into `effOwned` at
            //     Ownership.cpp:9628, distinct from the lambda literal's
            //     OWN `lamTy->funcReturnOwned` this fix populates);
            //   - `!lamTargetRet` — when a target function type IS present
            //     but its return slot is plain `T` (not `owned T`), the
            //     concrete-slot callee-side admission (§28.3 *Concrete-slot
            //     function bodies*) rejects the fresh-production return
            //     path; marking the lambda literal's own `funcReturnOwned`
            //     here would BOTH bypass that rejection (the OR'd
            //     `expectedFnType` would be the dominant signal but the
            //     lambda-literal type itself would still read as owned) and
            //     flip the classifier when called against an unannotated
            //     local — keep the two regimes separate.
            // Gated on a class result type: a record/struct/enum/value-
            // builtin return carries no heap obligation; the body's
            // classification is correct but the call classifier never
            // promotes such a return to Owned regardless of `owned`. // per
            // §28.3, #4027
            if (!hasAscription && !lamTargetRetOwned && !lamTargetRet &&
                inferredReturn &&
                inferredReturn->kind == sema::TypeRef::Kind::Named &&
                inferredReturn->declaration &&
                inferredReturn->declaration->kind ==
                    ast::TypeDeclKind::Class &&
                !inferredReturn->declaration->isValueBuiltin) {
                auto identityResolver =
                    [](const sema::TypeRef* t) { return t; };
                bool bodyIsFreshOwned = false;
                if (lam->exprBody) {
                    bodyIsFreshOwned =
                        common::isFreshOwnedClassProducer(
                            lam->exprBody.get(), ctx_, identityResolver);
                } else if (!lam->stmts.empty()) {
                    bool sawReturn = false;
                    bool allFresh = true;
                    for (const auto& s : lam->stmts)
                        common::scanReturnsAllFreshOwned(
                            s.get(), ctx_, identityResolver,
                            sawReturn, allFresh);
                    bodyIsFreshOwned = sawReturn && allFresh;
                }
                if (bodyIsFreshOwned) lamTy->funcReturnOwned = true;
            }
            ctx_.setExprType(lam, lamTy);
        }
    } else if (auto* ce = dynamic_cast<const ast::CaseExpr*>(&e)) {
        if (ce->scrutinee) {
            // §35.5 (v0.209/#2080 F4): a `case owned x of` scrutinee
            // marker parses as a scrutinee-rooted MarkedTransferExpr —
            // announce the node so the marker arm routes it to the
            // §35.5 validation instead of the A7 positional reject.
            const ast::Expr* savedScrutSlot = caseScrutineeSlot_;
            caseScrutineeSlot_ = ce->scrutinee.get();
            visitExpr(*ce->scrutinee);
            caseScrutineeSlot_ = savedScrutSlot;
        }
        validateCaseArms(ce->arms);  // §16.4 / §19.6 (#738)
        TypeRef* scrutTy =
            ce->scrutinee ? ctx_.exprType(*ce->scrutinee) : nullptr;
        scrutTy = recoverChoiceThisScrutTy(ce->scrutinee.get(), scrutTy);
        // §19.3 / §16.6 / §7.11 (v0.286, #3298): a relational or range
        // pattern requires a NON-NULLABLE scrutinee — value or reference, and
        // keyed on the pattern's KIND rather than on which bounds are written
        // (§19.3, v0.286). One shared checker with the CaseStmt site below.
        checkOrderingPatternsOverNullableScrutinee(scrutTy, ce->arms);
        // §10.6 (#772): "Inside a `case` whose scrutinee is an enum, the enum
        // prefix may be omitted." This holds throughout the construct, INCLUDING
        // arm bodies (`Red => Green`), not only patterns. Inject the scrutinee
        // enum's constants into a scope that parents the arm bodies so a bare
        // `Green` resolves; the per-arm binding/destructuring scopes set
        // `parent = currentScope_`, so they inherit it too. Restored after the
        // loop. (The qualified `Color.Green` form already worked.) // per §10.6
        Scope armEnumScope;
        Scope* preArmEnumScope = currentScope_;
        bool savedEnumArms = inEnumScrutineeCaseArms_;
        bool scrutIsEnum = scrutTy && scrutTy->kind == TypeRef::Kind::Named &&
            scrutTy->declaration &&
            scrutTy->declaration->kind == ast::TypeDeclKind::Enum;
        // §35.5 (v0.200, #2045): the scrutinee-type prefix-omission rule
        // extends to a choice scrutinee. Inject the choice's PAYLOAD-FREE
        // case names so a bare `None` resolves in patterns and arm bodies.
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
        // §19/§5.1 (#1202): a relational (`> EXPR`) or range (`lo..hi`) pattern
        // operand is compared against the scrutinee by codegen with a single
        // ICmp/FCmp keyed on the scrutinee's representation (litAtWidth/reconcile,
        // LlvmBackend_Exprs.cpp). When the operand's primitive is in a DIFFERENT
        // comparison class than the scrutinee — a string pointer vs a scalar, a
        // bool i1 vs a wider int, a float vs an integer, a decimal vs either —
        // those reach CreateICmp with mismatched operand types and abort the
        // backend. Mirror the #964 ConstantPattern guard: reject such a pattern
        // here with the same §19 diagnostic. A genuine cross-numeric
        // accept/reconcile is a separate §19.3 ruling and is out of scope. A
        // `decimal` SCRUTINEE is excluded — its codegen path (decOperand/decCmp,
        // #1152) already bridges any numeric bound, so a decimal scrutinee with
        // an integer/float bound compiles and must keep doing so.
        auto cmpClassOf = [](PrimitiveType p) -> int {
            switch (p) {
                case PrimitiveType::String: return 1;
                case PrimitiveType::Bool:   return 2;
                case PrimitiveType::Char:
                case PrimitiveType::Byte:  case PrimitiveType::SByte:
                case PrimitiveType::Short: case PrimitiveType::UShort:
                case PrimitiveType::Int:   case PrimitiveType::UInt:
                case PrimitiveType::Long:  case PrimitiveType::ULong:
                    return 3;  // integer family — codegen sext/zext-bridges widths
                case PrimitiveType::Float:
                case PrimitiveType::Double:
                    return 4;  // floating family
                case PrimitiveType::Decimal: return 5;
                default: return 0;  // None/Object — not a relational/range operand
            }
        };
        auto checkPatternOperand = [&](const ast::Expr* operand) {
            if (!operand) return;
            if (!(ce->scrutinee && scrutTy &&
                  scrutTy->kind == TypeRef::Kind::Primitive))
                return;
            const TypeRef* ot = ctx_.exprType(*operand);
            if (!ot || ot->kind != TypeRef::Kind::Primitive) return;
            int sc = cmpClassOf(scrutTy->primitive);
            int oc = cmpClassOf(ot->primitive);
            // Fire only when the scrutinee is one of the directly-compared
            // classes (string/bool/integer/float) and the operand is a primitive
            // in a different comparison class. A decimal scrutinee (sc == 5) is
            // excluded — see the note above.
            if ((sc >= 1 && sc <= 4) && oc != 0 && sc != oc)
                ctx_.diagnostics().report(
                    Severity::Error, operand->span.begin,
                    "case pattern operand type is not compatible with the "
                    "scrutinee type (\xC2\xA7""19) — a `case` pattern operand "
                    "must be comparable to the scrutinee");
        };
        // §7.4 (v0.214, #2116): case arms are ALTERNATIVES — the same
        // running-intersection per-arm base + after-case join as the CaseStmt
        // handler (see the note there for why it is drop-respecting). An
        // expression-case arm's `do … end` body can contain early-exit `if`
        // statements that establish facts.
        auto caseRunningField = fieldNarrowed_;
        // §7.4 (#2258): fold THIS arm's exit into the running intersection.
        // Named (not inlined at the loop tail) because one arm path — the
        // choice-destructure arm below — `continue`s past the tail, and
        // skipping the fold there loses that arm's DROPS.
        auto foldArmExitIntoRunning = [&] {
            for (auto it = caseRunningField.begin();
                 it != caseRunningField.end();) {
                if (fieldNarrowed_.count(*it)) ++it;
                else it = caseRunningField.erase(it);
            }
        };
        for (const auto& arm : ce->arms) {
            fieldNarrowed_ = caseRunningField;  // §7.4 #2116: per-arm base
            // §19 / §A (#727): in a SCRUTINEE-LESS `case`, an arm's pattern is a
            // boolean CONDITION expression (a ConstantPattern) — typecheck its
            // constants so identifiers resolve and codegen can lower them.
            // §19 / §A.8 (#957): a scrutinee-BEARING arm whose pattern is a
            // ConstantPattern referencing a named const (`C.K`, a module-level
            // const) must also be typechecked, so the const acquires an
            // exprType/symbol and codegen can lower it. Enum-constant patterns
            // over an enum scrutinee are excluded — those are bare/qualified
            // member names resolved by name against the scrutinee enum in
            // codegen (enumConstantOfPatternExpr); visiting them here would
            // either fail to resolve (plain scope) or divert that path.
            if (!ce->scrutinee || !scrutIsEnum) {
                if (auto* cp = dynamic_cast<const ast::ConstantPattern*>(
                        arm.pattern.get()))
                    for (const auto& c : cp->constants) {
                        if (!c) continue;
                        visitExpr(*c);
                        // §19 (#964/#1213): a scrutinee-bearing arm whose
                        // constant has a comparison class incompatible with the
                        // scrutinee crashes codegen (ICmp with mismatched LLVM
                        // operand types — SIGABRT in ICmpInst::AssertOK). The
                        // #964 guard was string/bool-only; #1213 extends it to
                        // cover float/decimal constants over an integer scrutinee
                        // (and the symmetric directions) by reusing the same
                        // cmpClassOf-based checkPatternOperand that #1202 added
                        // for RelOpPattern and RangePattern. per §19/§5.1.
                        checkPatternOperand(c.get());  // per §19/§5.1 (#1213)
                    }
            }
            // §19.3 (#726): a relational pattern (`> limit`) carries an operand
            // expression whose identifiers must be typed so codegen can lower a
            // VARIABLE operand (a literal already worked). Visit it.
            if (auto* rop =
                    dynamic_cast<const ast::RelOpPattern*>(arm.pattern.get())) {
                if (rop->expr) {
                    visitExpr(*rop->expr);
                    // per §19/§5.1 (#1202)
                    checkPatternOperand(rop->expr.get());
                }
            }
            // §19.3 / §A.8 (#1202): a range pattern's bounds are constant
            // operands; type them (so the compatibility check and codegen see
            // their types) and reject a bound whose comparison class is
            // incompatible with the scrutinee instead of aborting the backend.
            if (auto* rangep =
                    dynamic_cast<const ast::RangePattern*>(arm.pattern.get())) {
                if (auto* rng = dynamic_cast<const ast::RangeExpr*>(
                        rangep->rangeExpr.get())) {
                    if (rng->lo) {
                        visitExpr(*rng->lo);
                        checkPatternOperand(rng->lo.get());  // per §19/§5.1 (#1202)
                    }
                    if (rng->hi) {
                        visitExpr(*rng->hi);
                        checkPatternOperand(rng->hi.get());  // per §19/§5.1 (#1202)
                    }
                }
            }
            // §19.3 (v0.183, #1584): a heterogeneous alternative list — type and
            // range-check each member's operand exprs exactly as a top-level
            // member (constant, relational, range), so codegen can lower a
            // variable operand and an incompatible comparison class is rejected
            // rather than aborting the backend. A ConstantPattern member over an
            // enum scrutinee is excluded for the same reason as above.
            if (auto* ap =
                    dynamic_cast<const ast::AltPattern*>(arm.pattern.get())) {
                for (const auto& m : ap->members) {
                    if (auto* mcp =
                            dynamic_cast<const ast::ConstantPattern*>(m.get())) {
                        if (ce->scrutinee && scrutIsEnum) continue;
                        for (const auto& c : mcp->constants) {
                            if (!c) continue;
                            visitExpr(*c);
                            checkPatternOperand(c.get());
                        }
                    } else if (auto* mrop =
                                   dynamic_cast<const ast::RelOpPattern*>(
                                       m.get())) {
                        if (mrop->expr) {
                            visitExpr(*mrop->expr);
                            checkPatternOperand(mrop->expr.get());
                        }
                    } else if (auto* mrng =
                                   dynamic_cast<const ast::RangePattern*>(
                                       m.get())) {
                        if (auto* rng = dynamic_cast<const ast::RangeExpr*>(
                                mrng->rangeExpr.get())) {
                            if (rng->lo) {
                                visitExpr(*rng->lo);
                                checkPatternOperand(rng->lo.get());
                            }
                            if (rng->hi) {
                                visitExpr(*rng->hi);
                                checkPatternOperand(rng->hi.get());
                            }
                        }
                    }
                }
            }
            // §19.3 (#1847): a top-level bare-identifier pattern must be a type
            // test, a named constant, or an enum constant; reject one that
            // resolves to a runtime value or an unresolved name — otherwise
            // codegen leaves `case.end` unterminated (malformed IR / ICE).
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
                        // §12.7/§2.8.3 rule 4/§17.4 (v0.197, #2018): the VALUE/expression
                        // `case` form's copy of #2012's PLAIN-scrutinee erasure
                        // guard — the sibling of ExprTypecheck.cpp's CaseStmt
                        // handler, and the case-binding analog of the #2006
                        // `is`/`as` guard that already lives above in THIS file
                        // (both reuse the same file-local
                        // typeTestTargetMentionsTypeVariable mirror and the shared
                        // typeArgRuntimeReified oracle). typeTestInterfaceBindAmbiguous
                        // only reasons about a generic-INTERFACE scrutinee (its
                        // `None` there means "soundly admitted" — the unique
                        // conformer — so respect it and skip that domain below).
                        // For a PLAIN scrutinee (Object, a class, a NON-generic
                        // interface) a `var x: T<erased-args>` reaches the
                        // COLLAPSED per-monomorphization descriptor with no guard:
                        // appendDescKey does not encode a function type's identity
                        // (Kind::Function → "F" at any depth), so
                        // `var n := case o of var x: Wrap<(string)->string> => …`
                        // false-positive-MATCHES a `Wrap<(int)->int>` value and
                        // binds it at a wrong static type — the same memory-unsafe
                        // type confusion #2001/#2006/#2012 fixed. Bring the value
                        // form to parity: reject a CONSTRUCTED generic target whose
                        // type argument is not runtime-reified (typeArgRuntimeReified
                        // is the oracle — a folded inline array stays admitted
                        // post-#2001) and that carries no type variable (a bare
                        // parameter resolves to a concrete arg at monomorphization,
                        // #1843 — admit it; the design-gated #2012 residual). Hold
                        // OFF a generic-INTERFACE scrutinee — that is
                        // typeTestInterfaceBindAmbiguous's domain (its sound
                        // unique-conformer admission must not be overridden, and any
                        // erased case there is already rejected by it or upstream by
                        // #1913/#1920), so re-deciding here would double-report.
                        else if (lsym->declType->kind ==
                                     TypeRef::Kind::Named &&
                                 !lsym->declType->genericArgs.empty() &&
                                 !typeTestTargetMentionsTypeVariable(
                                     lsym->declType) &&
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
                                "(§19.3/§12.7/§2.8.3 rule 4/§17.4)");
                    }
                }
                Scope* saveScope = currentScope_;
                currentScope_ = &armScope;
                if (arm.guard) visitExpr(*arm.guard);
                if (arm.exprBody) {
                    // §28.3 A7 (v0.209, #2080): an expression-`case`/
                    // `when` ARM RESULT is a marked-transfer admission
                    // position — announce the exact child node.
                    const ast::Expr* savedMtSlot = markedArmSlot_;
                    markedArmSlot_ = arm.exprBody.get();
                    visitExpr(*arm.exprBody);
                    markedArmSlot_ = savedMtSlot;
                }
                for (const auto& s : arm.stmts) if (s) visitStmt(*s);
                currentScope_ = saveScope;
            } else if (dp && currentScope_ && dp->typeName) {
                // §19: DestructuringPattern — same logic as in CaseStmt.
                TypeSymbol* recTy = nullptr;
                std::string_view lastName = dp->typeName->parts.empty()
                    ? std::string_view{}
                    : dp->typeName->parts.back();
                // §35.5 (v0.200, #2045): CHOICE case destructuring — mirrors
                // the CaseStmt handler. When the scrutinee is a choice and
                // the pattern's head names one of its cases, bind the case's
                // payload fields as locals. Payload bindings are copies
                // (value-like) or borrows (borrow-bearing/owning) per the
                // §35.5 borrowing-match default; the owning-match transfer
                // machinery lands in a follow-up slice.
                if (scrutIsChoice) {
                    auto* cc = const_cast<ChoiceCaseSymbol*>(
                        findChoiceCaseOnScrutinee(scrutTy, lastName));
                    if (cc && cc->caseDecl) {
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
                        // fix #3938 (§19.3 / §14.1, v0.294): build the
                        // nested-pattern worklist here so each pending
                        // pattern carries its own ALREADY-SUBSTITUTED
                        // scrutinee TypeRef (this slot's substituted
                        // `fieldTy`, computed via #2219's
                        // `substituteGenericFieldType(fieldTy, scrutTy)`)
                        // — the nested walk needs the parent slot's
                        // substituted type to thread generic substitution
                        // through `Outer(Box(1..5, 1), 9)` over
                        // `Outer<int?>` (the bare `T` is no longer enough
                        // to decide the nested pattern's own slot-level
                        // ordering-pattern gate).
                        std::vector<PendingNestedDestructuring> nestedWork;
                        for (std::size_t i = 0; i < dp->subPatterns.size();
                             ++i) {
                            const auto& sp = dp->subPatterns[i];
                            TypeRef* fieldTy = nullptr;
                            if (i < cc->caseDecl->payload.size() &&
                                cc->caseDecl->payload[i].type) {
                                fieldTy = ctx_.resolvedType(
                                    *cc->caseDecl->payload[i].type);
                                // #2219: a generic choice at the scrutinee
                                // substitutes its bare-type-param payload types
                                // with the actual instantiation args so the
                                // destructured slot is bound at the right type
                                // (and both arms' common-type computation sees
                                // both at `int`, not at unrelated `T`/`E`).
                                // See CaseStmt analogue for the rationale.
                                if (fieldTy) {
                                    fieldTy = substituteGenericFieldType(
                                        fieldTy, scrutTy);
                                }
                            }
                            // §19.3 (v0.294, #3692): the slot's own declared
                            // type, not the arm scrutinee's, governs whether
                            // an ordering sub-pattern is admitted here.
                            checkOrderingPatternOverNullableType(
                                fieldTy, sp.pattern.get());
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
                            } else if (auto* deeper =
                                           dynamic_cast<const ast::DestructuringPattern*>(
                                               sp.pattern.get())) {
                                // fix #3938: hand the nested pattern this
                                // slot's already-substituted `fieldTy` as
                                // its scrutinee type so the nested walk
                                // can re-substitute THIS slot's bare
                                // generic-parameter fields correctly.
                                nestedWork.push_back({deeper, fieldTy});
                            }
                        }
                        // §35.5 / fix #3909 / fix #3938: bind names inside a
                        // NESTED destructuring sub-pattern one level below a
                        // choice case's payload slots (`Wrap(Inner(v)) => v`,
                        // `Inner` a record) — the loop above only registers a
                        // LocalSymbol for a TOP-LEVEL slot's own naked/`var`
                        // binding, so `v` was left unresolved even though the
                        // identical shape over a record scrutinee
                        // (`Outer(Inner(v)) => v`) already worked. Call the
                        // shared walk (see its declaration comment in
                        // ExprTypecheck_impl.hpp) — also used by the record-
                        // scrutinee handler below and by the CaseStmt twin —
                        // rather than adding a third/fourth hand-copy of it.
                        // fix #3938: pass the worklist pre-populated with
                        // each nested pattern's own substituted scrutTy
                        // rather than the outer subPatterns — the walk now
                        // needs the parent slot's substituted type to
                        // thread generic substitution through.
                        bindNestedDestructuringNames(armScope, std::move(nestedWork));
                        Scope* saveScope = currentScope_;
                        currentScope_ = &armScope;
                        if (arm.guard) visitExpr(*arm.guard);
                        if (arm.exprBody) {
                            // §28.3 A7 (v0.209, #2080): arm-result
                            // marked-transfer admission position.
                            const ast::Expr* savedMtSlot = markedArmSlot_;
                            markedArmSlot_ = arm.exprBody.get();
                            visitExpr(*arm.exprBody);
                            markedArmSlot_ = savedMtSlot;
                        }
                        for (const auto& bs : arm.stmts)
                            if (bs) visitStmt(*bs);
                        currentScope_ = saveScope;
                        // §7.4 (#2258): this path leaves the arm loop early,
                        // so fold the arm's exit HERE — the loop-tail fold
                        // never runs for it, and a fact this arm DROPPED
                        // would be resurrected in the after-case state.
                        foldArmExitIntoRunning();
                        continue;  // skip the record-destructure path
                    }
                }
                // §19.3: "Destructuring applies to records only — classes do
                // not have a positional-destructuring surface … To match a
                // class hierarchy, use a type-test pattern (`Dog`, `var d:
                // Dog`) and access fields through the bound name."
                // Detect whether the named type is a class and, if so, emit
                // a compile error. We check BEFORE the record search so that
                // a class whose name happens to shadow a record is caught.
                // fix #3957: fall back to `ctx_.importedSymbols()` after the
                // local pass misses — an imported CLASS used as a
                // destructuring head is the same local-only-lookup defect
                // this issue names for records (below); without the
                // fallback this diagnostic never fires for an imported
                // class and the program falls through to codegen, which
                // emits malformed IR ("case.end has no terminator") instead
                // of this clean §19.3 rejection (measured).
                {
                    // fix #4050 (§19.3/§27.8, v0.294): a `type` alias of a
                    // class used as a destructuring head (`AliasOfDog(_)`)
                    // resolves to a `TypeAliasSymbol`, which none of these
                    // `dynamic_cast<TypeSymbol*>` name-match loops recognize
                    // — the identical missing-arm shape #4038/#4048 closed
                    // in their own call chains. Resolve it through the
                    // shared `resolveAliasToTypeSymbol` helper
                    // (ExprTypecheck.cpp) so an alias of a class gets the
                    // SAME "not valid on a class type" §19.3 rejection a
                    // direct class name already gets.
                    bool isClassHead = false;
                    for (const auto& sym : ctx_.symbols()) {
                        if (auto* ts = dynamic_cast<TypeSymbol*>(sym.get())) {
                            if (ts->name == lastName &&
                                ts->kind == ast::TypeDeclKind::Class) {
                                isClassHead = true; break;
                            }
                        } else if (auto* aliasSym =
                                       dynamic_cast<TypeAliasSymbol*>(sym.get())) {
                            if (aliasSym->name == lastName) {
                                TypeSymbol* aliasedTs =
                                    resolveAliasToTypeSymbol(aliasSym);
                                if (aliasedTs &&
                                    aliasedTs->kind == ast::TypeDeclKind::Class) {
                                    isClassHead = true; break;
                                }
                            }
                        }
                    }
                    if (!isClassHead) {
                        for (Symbol* sym : ctx_.importedSymbols()) {
                            if (auto* ts = dynamic_cast<TypeSymbol*>(sym)) {
                                if (ts->name == lastName &&
                                    ts->kind == ast::TypeDeclKind::Class) {
                                    isClassHead = true; break;
                                }
                            }
                        }
                    }
                    if (isClassHead) {
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
                    }
                }
                // fix #4381 (§19.3, v0.298): after the class-head check, detect
                // the dual: a pattern head that names a choice CASE (e.g.
                // `Fetched(_)`) but a scrutinee that is NOT a choice. Sema
                // must reject this rather than letting it through to codegen
                // which would emit malformed IR (case.end has no terminator).
                if (ctx_.module()) {
                    bool isChoiceCaseHead = false;
                    for (Symbol* child : ctx_.module()->children) {
                        if (auto* ts = dynamic_cast<TypeSymbol*>(child)) {
                            if (ts->kind == ast::TypeDeclKind::Choice) {
                                for (Symbol* m : ts->members) {
                                    if (auto* cc =
                                            dynamic_cast<ChoiceCaseSymbol*>(m)) {
                                        if (cc->name == lastName) {
                                            isChoiceCaseHead = true;
                                            break;
                                        }
                                    }
                                }
                                if (isChoiceCaseHead) break;
                            }
                        }
                    }
                    if (!isChoiceCaseHead) {
                        for (Symbol* sym : ctx_.importedSymbols()) {
                            if (auto* ts = dynamic_cast<TypeSymbol*>(sym)) {
                                if (ts->kind == ast::TypeDeclKind::Choice) {
                                    for (Symbol* m : ts->members) {
                                        if (auto* cc =
                                                dynamic_cast<ChoiceCaseSymbol*>(m)) {
                                            if (cc->name == lastName) {
                                                isChoiceCaseHead = true;
                                                break;
                                            }
                                        }
                                    }
                                    if (isChoiceCaseHead) break;
                                }
                            }
                        }
                    }
                    if (isChoiceCaseHead) {
                        ctx_.diagnostics().report(
                            Severity::Error, dp->span.begin,
                            "case pattern head `" + std::string(lastName) +
                                "(...)` names a choice case, but the scrutinee "
                                "type is not a choice — a choice-case pattern is "
                                "only valid when the scrutinee is that specific "
                                "choice type (§19.3)");
                    }
                }
                // Search ctx_.symbols() for the record TypeSymbol by name.
                // This is more robust than searching only module children
                // since symbols may be nested or added during synthesis.
                // fix #3957 (§19.3/§2.8.3 rule 4, v0.294): fall back to
                // `ctx_.importedSymbols()` — mirroring codegen's own
                // import-aware `patternMatches` lookup (LlvmBackend_Exprs2.cpp,
                // fix #2844) and `bindLambdaDestructure` above in this same
                // file. Without this fallback an IMPORTED record scrutinee
                // never resolves `recTy`, so `fields` stays empty and every
                // downstream §19.3 check below (arity, #2178 generic
                // erasure, and — once the field-type read above also
                // consults `storageComponentType` — the nullable-ordering
                // gate) is silently skipped for a cross-module program that
                // an identical single-module program correctly rejects.
                //
                // fix #4050 (§19.3/§27.8, v0.294): same alias arm as
                // `isClassHead` above — a `type` alias of a record used as a
                // destructuring head (`AliasOfPoint(x, y)`) must resolve to
                // the aliased record's TypeSymbol here too.
                for (const auto& sym : ctx_.symbols()) {
                    if (auto* ts = dynamic_cast<TypeSymbol*>(sym.get())) {
                        if (ts->name == lastName &&
                            ts->kind == ast::TypeDeclKind::Record) {
                            recTy = ts; break;
                        }
                    } else if (auto* aliasSym =
                                   dynamic_cast<TypeAliasSymbol*>(sym.get())) {
                        if (aliasSym->name == lastName) {
                            TypeSymbol* aliasedTs =
                                resolveAliasToTypeSymbol(aliasSym);
                            if (aliasedTs &&
                                aliasedTs->kind == ast::TypeDeclKind::Record) {
                                recTy = aliasedTs; break;
                            }
                        }
                    }
                }
                if (!recTy) {
                    for (Symbol* sym : ctx_.importedSymbols()) {
                        if (auto* ts = dynamic_cast<TypeSymbol*>(sym)) {
                            if (ts->name == lastName &&
                                ts->kind == ast::TypeDeclKind::Record) {
                                recTy = ts; break;
                            }
                        }
                    }
                }
                std::vector<FieldSymbol*> fields;
                if (recTy) {
                    for (Symbol* m : recTy->members) {
                        if (auto* fs = dynamic_cast<FieldSymbol*>(m)) {
                            bool isStatic = false;
                            for (auto& mod : fs->modifiers)
                                if (mod == "static") { isStatic = true; break; }
                            if (!isStatic) fields.push_back(fs);
                        }
                    }
                }
                // §19.3 (v0.226, #2178): a generic-record DESTRUCTURING pattern
                // (the form `Pair(a, b)` against a record `record Pair<A,B>(…)`)
                // names no type arguments, so the record's instantiation is
                // undeterminable from the pattern. Today the implementation
                // resolves the destructured record's SHARED per-declaration
                // descriptor and binds the fields at TEMPLATE offsets — a box
                // of ANY instantiation is accepted and the arm's bindings
                // land at the WRONG static type, the same memory-unsafe class
                // the #2001/#2006/#2012/#2018 erasure guards close. A
                // destructuring pattern with no type-args spelling is the
                // natural site for a static rejection; the typed binding
                // `var p: Pair<int, string>` remains the precise spelling.
                // The destructured record's `genericParams` non-empty is the
                // trigger; the scrutinee's static type must NOT erase the
                // instantiation — the strict reading of the issue's
                // ERASED+RECORD = REJECT criterion. `Object` is the canonical
                // erased scrutinee; a non-generic interface and a class
                // scrutinee are likewise erased (a class has no relationship
                // to the destructured record, and a non-generic interface
                // cannot carry the record's instantiation in its args). A
                // scrutinee of the destructured record itself (the
                // `p: Pair<int, string>` case) is the admit path; this fix
                // does not change that arm's already-typed binding treatment.
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
                // §19.3 / Appendix C: "a record-destructuring pattern's
                // positional-binding count matches the record's positional-
                // field count." Without this check an over- or under-bound
                // pattern (`Pt(x, y, z)` on a 2-field record) was accepted, then
                // codegen's patternMatches could not destructure it and bailed,
                // leaving malformed IR ("case.end has no terminator").
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
                // fix #3938 (§19.3 / §14.1, v0.294): build the
                // nested-pattern worklist here so each pending pattern
                // carries its own ALREADY-SUBSTITUTED scrutinee TypeRef
                // (this slot's substituted `fieldTy`, computed via
                // #3898's `substituteGenericFieldType(fieldTy, scrutTy)`)
                // — the nested walk needs the parent slot's substituted
                // type to thread generic substitution through
                // `Outer(Box(1..5, 1), 9)` over `Outer<int?>` (the bare
                // `T` is no longer enough to decide the nested pattern's
                // own slot-level ordering-pattern gate).
                std::vector<PendingNestedDestructuring> nestedWork;
                for (std::size_t i = 0; i < dp->subPatterns.size(); ++i) {
                    const auto& sp = dp->subPatterns[i];
                    TypeRef* fieldTy = nullptr;
                    if (i < fields.size()) {
                        // fix #3957 (§19.3/§9.1/§9.2, v0.294): read the
                        // field's declared type through `storageComponentType`
                        // (sema/Symbol.hpp) rather than the bare
                        // `posParam->type` — the ONE accessor that also
                        // falls back to `ctx_.importedMemberType` for an
                        // IMPORTED record field, whose synthetic `posParam`
                        // deliberately carries a null `type` (#2516). The
                        // bare read this replaces returned null for every
                        // imported record's positional field, so the
                        // ordering-pattern gate below never saw a type to
                        // reject on (part of the #3957 "imported record
                        // scrutinee bypasses every §19.3 check" defect).
                        fieldTy = const_cast<TypeRef*>(
                            storageComponentType(fields[i], ctx_));
                        // fix #3898 (§14.1 / §19.3, v0.294): a GENERIC
                        // record's positional field may be declared at a bare
                        // type parameter (`record Box<T>(X: T, Y: int)`); the
                        // scrutinee's instantiation (`Box<int?>`) must be
                        // substituted through here exactly as the choice-case
                        // payload path above already does, or the
                        // ordering-pattern gate just below sees the bare,
                        // non-nullable `T` instead of the substituted `int?`
                        // and wrongly admits `Box(1..5, 1)`.
                        if (fieldTy) {
                            fieldTy = substituteGenericFieldType(fieldTy,
                                                                 scrutTy);
                        }
                    }
                    // §19.3 (v0.294, #3692): slot-declared-type gate — see
                    // the choice-case call site above for the rationale.
                    checkOrderingPatternOverNullableType(fieldTy,
                                                         sp.pattern.get());
                    if (!sp.nakedBinding.empty()) {
                        // Bare `Pt(x, y)` — nakedBinding carries the name.
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
                    } else if (auto* deeper =
                                   dynamic_cast<const ast::DestructuringPattern*>(
                                       sp.pattern.get())) {
                        // fix #3938: hand the nested pattern this slot's
                        // already-substituted `fieldTy` as its scrutinee
                        // type so the nested walk can re-substitute THIS
                        // slot's bare generic-parameter fields correctly.
                        nestedWork.push_back({deeper, fieldTy});
                    }
                }
                // #726 / §19.3 / #3909 / #3938: also bind the names inside
                // NESTED destructuring sub-patterns (`Outer(Inner(v), b)`)
                // — the ONE shared walk (see bindNestedDestructuringNames's
                // declaration comment in ExprTypecheck_impl.hpp), also
                // called from the CaseStmt twin and from both the
                // CHOICE-case-payload handlers. fix #3938: pass the worklist
                // pre-populated with each nested pattern's own substituted
                // scrutTy rather than the outer subPatterns — the walk now
                // needs the parent slot's substituted type to thread
                // generic substitution through.
                bindNestedDestructuringNames(armScope, std::move(nestedWork));
                Scope* saveScope = currentScope_;
                currentScope_ = &armScope;
                if (arm.guard) visitExpr(*arm.guard);
                if (arm.exprBody) {
                    // §28.3 A7 (v0.209, #2080): an expression-`case`/
                    // `when` ARM RESULT is a marked-transfer admission
                    // position — announce the exact child node.
                    const ast::Expr* savedMtSlot = markedArmSlot_;
                    markedArmSlot_ = arm.exprBody.get();
                    visitExpr(*arm.exprBody);
                    markedArmSlot_ = savedMtSlot;
                }
                for (const auto& s : arm.stmts) if (s) visitStmt(*s);
                currentScope_ = saveScope;
            } else {
                // §7.4 (v0.210, #2105): a scrutinee-less `when` arm's
                // own boolean CONDITION narrows that arm's own body —
                // per §7.4 v0.210 first bullet: "a `when` guard … or a
                // `(when …)` arm condition (§19.1, narrowing that arm's
                // body)". For the expression-position form, the
                // condition is the ConstantPattern's first constant
                // (the parser wraps a single boolean expression in a
                // ConstantPattern; multiple constants are alternatives
                // for an enum scrutinee pattern, irrelevant here).
                // Save the running narrowings, push true-branch facts
                // for this arm's condition, restore on exit. Mirrors
                // the IfStmt save/restore in ExprTypecheck.cpp and the
                // §20 conditional atom save/restore above (#2105).
                //
                // fix #2258 / §7.4: the un-seeding below is an
                // INTERSECTION, never an assignment — restoring the
                // arm's ENTRY snapshot resurrected every fact the arm
                // legitimately invalidated (a `ref`/`out` argument in
                // the arm's result expression, a call on a mutably-
                // captured local; §7.4 "How narrowing is invalidated"),
                // and since it ran BEFORE the #2116 fold the fold saw
                // the resurrected state instead of the arm's real exit.
                // The intersection keeps the DROPS and still discards
                // what the arm CONDITION seeded. Same reasoning and
                // same wording as the CaseStmt counterpart.
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
                if (arm.exprBody) {
                    // §28.3 A7 (v0.209, #2080): an expression-`case`/
                    // `when` ARM RESULT is a marked-transfer admission
                    // position — announce the exact child node.
                    const ast::Expr* savedMtSlot = markedArmSlot_;
                    markedArmSlot_ = arm.exprBody.get();
                    visitExpr(*arm.exprBody);
                    markedArmSlot_ = savedMtSlot;
                }
                for (const auto& s : arm.stmts) if (s) visitStmt(*s);
                intersectNarrowingSetsWith(armEntryNull, armEntryNarrow,
                                           armEntryField);  // §7.4 #2258
            }
            // §7.4 #2116/#2258: fold this arm's exit into the running
            // intersection (see the lambda).
            foldArmExitIntoRunning();
        }
        fieldNarrowed_ = std::move(caseRunningField);  // §7.4 #2116
        currentScope_ = preArmEnumScope;  // §10.6 (#772): pop the enum scope
        inEnumScrutineeCaseArms_ = savedEnumArms;
        // §19.6: an expression-position `case` whose value cannot be proven
        // to be covered by some arm raises MatchFailureException at runtime;
        // the compiler emits a non-exhaustiveness warning here. Statement-
        // position `case` (CaseStmt) is exempt — an unmatched value simply
        // falls through with no action (§19.6).
        checkCaseExhaustiveness(*ce, scrutTy);
    } else if (auto* we = dynamic_cast<const ast::WithExpr*>(&e)) {
        // narrow #4059 (§9.5, v0.295): the `with(...)` clause IS the
        // construction form for a body-only record source (§9.5: "EXPR with
        // ( InitAssign ("," InitAssign)* ","?)" applies to any record-typed
        // source, and the source itself need not be default-constructible
        // when the with-clause supplies every field). Teach
        // `checkNewExprArgs`'s §9.1/§9.2 body-only-record rejection to step
        // aside for `new T() with(F := v, ...)` — the with-clause assigns
        // the fields directly, no implicit default ctor is needed. Mirrors
        // the `inBindAtom_` save/restore pattern at the BindExpr arm.
        bool savedInWithExprSource = inWithExprSource_;
        inWithExprSource_ = true;
        if (we->source) visitExpr(*we->source);
        inWithExprSource_ = savedInWithExprSource;
        // §9.5 (v0.188, #1735, proposal 0037 graduation): the source
        // expression of a `with` MUST be record-typed; a class- (or
        // interface-)typed source is a compile error naming this rule,
        // regardless of whether the source is a freshly-`new`-constructed
        // instance or an existing one — the discriminator is the source's
        // TYPE KIND, never the presence of `new` (§9.5: "new Point(1, 2)
        // with Y := 9 end has a record-typed source ... the same spelling
        // on a class-typed new expression is rejected"). This retires the
        // former §22.11 class-source "object-initializer block" reading
        // (§22.11 is now a tombstone); the diagnostic names both remedies
        // in the spec's order: named/optional constructor arguments
        // (§22.5), which run the constructor and respect its invariants, or
        // binding the instance to a named local and assigning its members
        // as statements. Records are a copy and accept any record-typed
        // source, so this gate is class/interface/enum-only.
        //
        // §9.5/§10.5 (#1738): an ENUM source is likewise rejected — §9.5's
        // "must be record-typed" is a blanket rule with no enum carve-out,
        // and per §10.5 an enum may not declare instance fields or
        // backing-storage properties (its runtime representation is the
        // underlying integer), so an enum source has no possible valid
        // `with` target. The class-source diagnostic's two remedies (named
        // constructor arguments, bind-and-assign) presuppose fields to set
        // and don't fit a fieldless enum value, so enum gets its own
        // message naming §10.5 instead of reusing the class wording.
        if (we->source) {
            if (auto* st = ctx_.exprType(*we->source)) {
                if (st->kind == TypeRef::Kind::Named && st->declaration &&
                    st->declaration->kind == ast::TypeDeclKind::Enum) {
                    ctx_.diagnostics().report(
                        Severity::Error, we->source->span.begin,
                        "`with` requires a record-typed source; this source "
                        "is enum-typed (§9.5) — an enum's runtime "
                        "representation is its underlying integer and it "
                        "may not declare instance fields or backing-storage "
                        "properties (§10.5), so it has no possible `with` "
                        "target; construct the desired enum constant "
                        "directly instead");
                } else if (st->kind == TypeRef::Kind::Named && st->declaration &&
                           st->declaration->kind != ast::TypeDeclKind::Record) {
                    ctx_.diagnostics().report(
                        Severity::Error, we->source->span.begin,
                        "`with` requires a record-typed source; this source "
                        "is class-typed (§9.5, v0.188) — the object-"
                        "initializer reading of `with` on a class type is "
                        "retired (formerly §22.11, now a tombstone). Use "
                        "named/optional constructor arguments instead "
                        "(§22.5), which run the constructor and respect its "
                        "invariants, or bind the instance to a named local "
                        "and assign its members as statements");
                }
            }
        }
        // §9.5, #875-878: validate each `with` assignment target.
        // Resolve the source's TypeSymbol so we can look up named fields.
        TypeSymbol* srcTs = nullptr;
        if (we->source) {
            if (auto* st = ctx_.exprType(*we->source)) {
                if (st->kind == TypeRef::Kind::Named && st->declaration &&
                    st->declaration->kind == ast::TypeDeclKind::Record)
                    srcTs = st->declaration;
            }
        }
        // §9.5 (#935): a field name may appear at most once in a `with`
        // initializer list. The names are static identifiers, so a duplicate is
        // always statically detectable; the first assignment would be dead code.
        // This mirrors the collection-literal rule (§15.4): a duplicate
        // Dictionary key is a compile error when all duplicates are compile-time
        // constants. Runs independently of the source-type validation above —
        // a rejected class-typed source still gets this diagnostic too, so a
        // program with both defects sees both.
        std::unordered_set<std::string_view> seenWithNames;
        for (const auto& ia : we->assigns) {
            if (ia.value) visitExpr(*ia.value);
            if (!seenWithNames.insert(ia.name).second) {
                ctx_.diagnostics().report(
                    Severity::Error,
                    ia.value ? ia.value->span.begin : we->span.begin,
                    "field `" + std::string(ia.name) + "` is assigned more "
                    "than once in this `with` expression (§9.5)");
                continue;
            }
            // §9.5: validate only when we have a known record source type.
            if (!srcTs) continue;
            SourceLocation valLoc =
                ia.value ? ia.value->span.begin : we->span.begin;
            // fix #4330 (§9.5/§5.1/§11.2/§15.4, v0.298): a `with`-clause
            // replacement value is NEVER target-typed at all — unlike every
            // other #4310-family position (LocalDecl/FieldDecl/PropertyDecl/
            // ReturnStmt/call-arg/lambda-body), this handler visits
            // `ia.value` (`visitExpr` a few lines above) BEFORE it even
            // looks up the target field's type, so there is no
            // `ctx_.setExprType`, no §15.4 admissibility gate, and
            // (consequently) #4310's shared per-element walk is never
            // reached either — measured SIGSEGV: `b1 with(Items :=
            // [new Widget3Bad(s)])` for a non-conforming `Widget3Bad`
            // dispatches `.GetV()` through a null itable slot, #4302's
            // signature.
            //
            // Runs the SAME admissibility-gate-then-per-element-walk pattern
            // #4310's other positions use, called once here rather than
            // duplicated by hand at the PropertySymbol and FieldSymbol arms
            // below (LANE_RULES §6/§25 — reuse the existing authorities, not
            // a ninth hand-rolled element walk). Returns true when it
            // reported a diagnostic; the caller then `continue`s, mirroring
            // the other positions' early `return` on a rejected literal.
            auto gateWithTargetCollectionLiteral =
                [&](const TypeRef* fieldTyConst) -> bool {
                if (!ia.value) return false;
                auto* cl = dynamic_cast<const ast::CollectionLiteralExpr*>(
                    ia.value.get());
                if (!cl) return false;
                TypeRef* fieldTy = const_cast<TypeRef*>(fieldTyConst);
                const bool isEmpty = !cl->isDict && cl->elements.empty();
                const bool admitted =
                    isEmpty
                        ? isCollectionLiteralEmptyAdmissibleTarget(fieldTy)
                        : isCollectionLiteralAdmissibleTarget(fieldTy,
                                                              cl->isDict);
                if (fieldTy && fieldTy->kind != TypeRef::Kind::Error &&
                    !admitted) {
                    if (isCollectionLiteralInterfaceTarget(fieldTy))
                        reportCollectionLiteralInterfaceTarget(
                            ctx_.diagnostics(), valLoc,
                            exprsNamedTypeSimpleName(fieldTy));
                    else if (cl->isDict &&
                             isCollectionLiteralAdmissibleTarget(
                                 fieldTy, /*isDict=*/false))
                        reportCollectionLiteralDictFormBadTarget(
                            ctx_.diagnostics(), valLoc, fieldTy);
                    else
                        reportCollectionLiteralBadTarget(
                            ctx_.diagnostics(), valLoc, fieldTy);
                    return true;
                }
                // per §5.1/§11.2 — #4310's shared per-element interface-
                // conformance walk, reused rather than a ninth hand-rolled
                // copy. Closes #4302's exact crash signature at this
                // position (filed as #4330).
                checkCollectionLiteralElementAssignability(*cl, fieldTy);
                return false;
            };
            Symbol* mem = lookupMember(srcTs, ia.name);
            if (!mem) {
                // #876: nonexistent field name.
                ctx_.diagnostics().report(
                    Severity::Error, valLoc,
                    "field `" + std::string(ia.name) + "` does not exist "
                    "in record `" + std::string(srcTs->name) +
                    "` — `with` targets must be positional-field properties, "
                    "auto-properties, or public fields (§9.5, #876)");
                continue;
            }
            // §13.1 (#934): enforce member visibility on `with` targets.
            // A private property/field is inaccessible from outside the
            // declaring type — the same rule that governs ordinary member
            // access (checkMemberVisibility). Without this check a `with`
            // expression silently bypassed visibility for private backing
            // storage (BAD_ACCEPT).
            if (auto* msym = dynamic_cast<MemberSymbol*>(mem))
                checkMemberVisibility(msym, ia.name, valLoc);
            if (dynamic_cast<MethodSymbol*>(mem) ||
                dynamic_cast<ConstructorSymbol*>(mem) ||
                dynamic_cast<DestructorSymbol*>(mem)) {
                // #877: method is not a valid with target.
                ctx_.diagnostics().report(
                    Severity::Error, valLoc,
                    "`" + std::string(ia.name) + "` is a method, not a "
                    "valid `with` target — `with` targets must be "
                    "positional-field properties, auto-properties, or "
                    "public fields (§9.5, #877)");
                continue;
            }
            if (auto* ps = dynamic_cast<PropertySymbol*>(mem)) {
                // fix #3152 (§9.5/§9.2/§2.8.2 clause 1): the STORAGE predicate.
                // §9.5 makes "has backing storage" the `with`-target rule, and
                // the AST-only predicate answered `false` for every IMPORTED
                // auto-property, so `r with(A := 40)` on an imported record was
                // rejected as "a computed property" while its character-
                // identical single-module twin returned 42. MEASURED.
                if (!isAutoPropertyStorageSymbol(ps, ctx_)) {
                    // #878: computed (expression-bodied) property has no
                    // backing storage and is not a valid with target.
                    ctx_.diagnostics().report(
                        Severity::Error, valLoc,
                        "`" + std::string(ia.name) + "` is a computed "
                        "property and cannot be a `with` target — `with` "
                        "requires backing storage (auto-property) (§9.5, #878)");
                    continue;
                }
                // Auto-property: check replacement value type (§5.1, #875).
                //
                // fix #3177 (§9.5 / §5.1 / §2.5, v0.254, family M4): the target
                // type through the ONE both-sources accessor. Reading it off
                // `ast::PropertyDecl` alone left `fieldTy` null for every
                // IMPORTED auto-property, so `checkAssignable` was SKIPPED
                // ENTIRELY and §5.1 went unenforced across the boundary. This
                // became live when #3152 fixed the storage gate 30 lines above
                // (`isAutoPropertyStorageSymbol`) — until then the imported
                // auto-property was rejected as "computed" before reaching
                // here, which is exactly the LATENT-to-LIVE transition the
                // audit's Appendix A row for this site predicted.
                //
                // MEASURED on ec6a02f (positional twin, same defect):
                // `r with(A := "oops")` on an imported record compiled clean
                // and printed `A=4438432|B=0` — a `string` pointer read back as
                // the `int` component — while the single-module twin was
                // rejected with the §5.1 diagnostic.
                if (ia.value) {
                    TypeRef* valTy = ctx_.exprType(*ia.value);
                    if (const TypeRef* fieldTy = storageComponentType(ps, ctx_)) {
                        // fix #4330: §15.4 admissibility gate + #4310's
                        // per-element interface-conformance walk — see
                        // `gateWithTargetCollectionLiteral`'s header comment
                        // above. A rejected literal reports its own
                        // diagnostic and skips the whole-value
                        // `checkAssignable` below (its `fromTy` is null for
                        // a collection literal at every #4310-family
                        // position, so calling it too would be a harmless
                        // no-op, not a double diagnostic — skipped anyway
                        // for symmetry with the other positions' early
                        // `return`).
                        if (gateWithTargetCollectionLiteral(fieldTy))
                            continue;
                        checkAssignable(
                            valTy, const_cast<TypeRef*>(fieldTy), valLoc,
                            "replacement value for `" +
                            std::string(ia.name) + "`",
                            ia.value.get());
                    }
                }
                continue;
            }
            if (auto* fs = dynamic_cast<FieldSymbol*>(mem)) {
                // Valid field target: check replacement value type (§5.1, #875).
                //
                // fix #3177 (§9.5 / §5.1 / §9.1 / §2.5, v0.254, family M4): the
                // §9.1 positional twin of the auto-property arm above, and it
                // failed for the SAME reason plus one more — an imported
                // positional field's `posParam` is non-null with a NULL `type`
                // (#2516), so neither source answered and `checkAssignable` was
                // skipped. MEASURED: `new RposP9(1, 2) with(A := "oops")` on an
                // imported record compiled clean, exit 0, and printed
                // `A=4438432|B=0`.
                if (ia.value) {
                    TypeRef* valTy = ctx_.exprType(*ia.value);
                    if (const TypeRef* fieldTy = storageComponentType(fs, ctx_)) {
                        // fix #4330: the positional-field twin of the
                        // auto-property arm's gate above.
                        if (gateWithTargetCollectionLiteral(fieldTy))
                            continue;
                        checkAssignable(
                            valTy, const_cast<TypeRef*>(fieldTy), valLoc,
                            "replacement value for `" +
                            std::string(ia.name) + "`",
                            ia.value.get());
                    }
                }
                continue;
            }
            // Any other member kind (EnumConstant, NestedType, etc.) is invalid.
            ctx_.diagnostics().report(
                Severity::Error, valLoc,
                "`" + std::string(ia.name) + "` is not a valid `with` "
                "target — `with` requires a positional-field property, "
                "auto-property, or public field (§9.5)");
        }
        // §A.7: a `source with …` expression has the same type as its
        // source (an object initializer / record copy preserves type).
        if (we->source) {
            if (auto* st = ctx_.exprType(*we->source))
                ctx_.setExprType(&e, st);
        }
    } else if (auto* cl =
                   dynamic_cast<const ast::CollectionLiteralExpr*>(&e)) {
        for (const auto& k : cl->keys) if (k) visitExpr(*k);
        for (const auto& v : cl->elements) if (v) visitExpr(*v);
        // fix #2678 / §28.11 / §15.4 (v0.282): a NESTED `CollectionLiteralExpr`
        // in a dictionary-literal KEY position is degenerate at run time — the
        // literal mints a FRESH mutable collection (§15.4's literal-tree rule),
        // the dictionary would then key by reference identity of that fresh
        // node (§4.8's `=`/`<>GetHashCode` contract expects content equality),
        // and a fresh mutable aggregate has no content-equality semantics that
        // survive mutation. Codegen ICEs when `materializeHeapDictLiteral`
        // falls through `lowerExpr(*cl->keys[i])` for the nested literal (#1386
        // call). Reject at the literal site so the user gets a clear message
        // naming the §28.11 / §4.8 / §15.4 authority instead of an internal
        // diagnostic — every nested CollectionLiteralExpr-in-dict-key program
        // produces the same shape, and the same code path underneath.
        //
        // LITERAL-ONLY: mirrors §15.4's resolution 5 ("`Add` after construction
        // is not a literal position"). The literal here lives at the literal
        // site, mints a fresh node, and would become a stable dictionary
        // entry's hash source — which is exactly the well-definedness that
        // §4.8 cannot supply for a freshly-minted mutable aggregate. A call-
        // argument `[k: v]` passed to `Dictionary<…>.Add(...)` is a transient
        // node too, but the call is *not* a literal position (§15.4 resolution
        // 5's recorded asymmetry) and stays admissible here.
        if (!cl->keys.empty()) {
            for (const auto& k : cl->keys) {
                if (!k) continue;
                if (dynamic_cast<const ast::CollectionLiteralExpr*>(k.get())) {
                    ctx_.diagnostics().report(
                        Severity::Error, k->span.begin,
                        "a nested collection literal is not admissible as a "
                        "dictionary KEY: the literal mints a fresh mutable "
                        "collection whose hashing contract is undefined "
                        "(\xC2\xA7""28.11); use a value-typed key instead");
                    break;
                }
            }
        }
        // §15.4 (#1297): a Dictionary<K,V> / OwningDictionary<K,V> literal (the
        // dict form has a non-empty `keys` vector) with DUPLICATE keys is a
        // compile error when all the duplicates are compile-time constants;
        // only when duplicates are not statically detectable does the last
        // value win at runtime. Detect duplicates among the literal-constant
        // keys (int/string/char/bool) — the common case — and report the second
        // occurrence. Non-constant / not-yet-handled keys are skipped (they fall
        // back to the runtime last-wins path, never a false positive).
        if (!cl->keys.empty()) {
            auto constKeyCanon =
                [](const ast::Expr* k) -> std::optional<std::string> {
                if (auto* il = dynamic_cast<const ast::IntLiteralExpr*>(k)) {
                    std::string s;
                    for (char c : il->text) if (c != '_') s += c;
                    if (s.empty()) return std::nullopt;
                    bool neg = false;
                    if (s[0] == '+' || s[0] == '-') { neg = (s[0]=='-'); s = s.substr(1); }
                    int base = 10;
                    if (s.size() > 2 && s[0] == '0' &&
                        (s[1] == 'x' || s[1] == 'X')) { base = 16; s = s.substr(2); }
                    else if (s.size() > 2 && s[0] == '0' &&
                        (s[1] == 'b' || s[1] == 'B')) { base = 2; s = s.substr(2); }
                    else if (s.size() > 2 && s[0] == '0' &&
                        (s[1] == 'o' || s[1] == 'O')) { base = 8; s = s.substr(2); }
                    try {
                        long long v = std::stoll(s, nullptr, base);
                        return std::string("i:") + std::to_string(neg ? -v : v);
                    } catch (...) { return std::nullopt; }
                }
                if (auto* sl = dynamic_cast<const ast::StringLiteralExpr*>(k))
                    return std::string("s:") + std::string(sl->text);
                if (auto* cl2 = dynamic_cast<const ast::CharLiteralExpr*>(k))
                    return std::string("c:") + std::string(cl2->text);
                if (auto* bl = dynamic_cast<const ast::BoolLiteralExpr*>(k))
                    return std::string("b:") + (bl->value ? "1" : "0");
                return std::nullopt;
            };
            std::unordered_set<std::string> seenKeys;
            for (const auto& k : cl->keys) {
                if (!k) continue;
                auto canon = constKeyCanon(k.get());
                if (!canon) continue;
                if (!seenKeys.insert(*canon).second) {
                    ctx_.diagnostics().report(
                        Severity::Error, k->span.begin,
                        "duplicate key in this dictionary literal \xE2\x80\x94 "
                        "the same compile-time-constant key appears more than "
                        "once, which is a compile error (\xC2\xA7""15.4); remove "
                        "the redundant entry (the earlier value would be dead)");
                }
            }
        }
    } else if (auto* re = dynamic_cast<const ast::RangeExpr*>(&e)) {
        // §16: visit lo, hi, and step so that identifiers inside range
        // bounds (e.g. `1..n`) get their symbols resolved.
        if (re->lo) visitExpr(*re->lo);
        if (re->hi) visitExpr(*re->hi);
        if (re->step) visitExpr(*re->step);
        // §16.4 (v0.242, #3052 — was #1628), verbatim: "A step of zero is
        // rejected **at construction**: a literal (or constant-folded) step of
        // zero is a compile error at the range expression, and evaluating a
        // range expression whose computed step is zero raises
        // `OverflowException` at the expression."
        //
        // The literal/constant-folded boundary is the same compile-time
        // constant fold already used a few lines up this section for the
        // direction/sign-mismatch warning ("both endpoints and step are
        // constants or constant after folding") — foldIntConstExprTC resolves
        // an integer literal, unary +/-/~, const-arithmetic, and a `const`
        // identifier reference, which is exactly the set of forms LLVM also
        // collapses to a ConstantInt during lowering (the `cs->isZero()`
        // check in Emitter::lowerRangeFor, LlvmBackend_Stmts.cpp).
        //
        // A non-constant (runtime-computed) step folds to nullopt and is left
        // alone here — that case is the construction-site emitGuardThrow in
        // codegen. So does a step expression carrying its OWN constant fault:
        // §17.14 evaluation faults make foldIntConst DECLINE (a constant
        // out-of-domain shift count, §6.5/#3045; a constant `x / 0`), so
        // `step (1 << 64)` is reported as the shift fault it is and never
        // misread here as a zero step.
        if (re->step) {
            if (auto stepV = foldIntConstExprTC(*re->step); stepV && *stepV == 0)
                ctx_.diagnostics().report(
                    Severity::Error, re->step->span.begin,
                    "a range step of zero is a compile error (§16.4) when "
                    "the step is a compile-time constant, including after "
                    "constant folding — a step whose value is only known at "
                    "runtime is still permitted, and raises "
                    "`OverflowException` at the range expression if it "
                    "evaluates to zero");
        }
        // fix #3247 / §16.3 (v0.277): `Range<T>`'s T is restricted by the
        // compiler to the integer types (byte, sbyte, short, ushort, int, uint,
        // long, ulong) and char — "Other substitutions are rejected. This
        // restriction is enforced as a compiler-intrinsic rule." Before this
        // fix the rejection was *implicit* — the codegen `sext <ty> to i64`
        // for a float/double/decimal/string element typed an `i64 sext` over
        // a non-integer operand and the LLVM verifier rejected it ("SExt only
        // operates on integer"); the user saw a malformed-IR / ICE-class
        // diagnostic, not a clean §16.3 rejection. Reject at sema with the
        // §16.3 message so the compiler's promise is loud instead of silent.
        // The check fires AFTER lo/hi are visited (so we have their types) and
        // BEFORE the RangeExpr's type is set, so the diagnostic points at the
        // range expression and the TypeRef constructed below carries no
        // out-of-policy element type.
        {
            TypeRef* elemTy = nullptr;
            if (re->lo)  elemTy = ctx_.exprType(*re->lo);
            if (!elemTy && re->hi) elemTy = ctx_.exprType(*re->hi);
            if (elemTy && !isValidRangeElementType(elemTy)) {
                ctx_.diagnostics().report(
                    Severity::Error, e.span.begin,
                    std::string("a `Range<T>` value requires T to be one of "
                                "the integer types or `char` \xe2\x80\x94 "
                                "got `") + rangeElementTypeName(elemTy) +
                        "` (\xc2\xa7""16.3)");
            }
            // fix #499 / §9.8: give the RangeExpr itself a Range-named type
            // so that `var r := a..b` infers lsym->declType = Range (not null).
            // The element type is inferred from lo (or hi if lo is absent);
            // it is recorded as the first generic arg so codegen can read it
            // back when choosing the integer width for the Range struct's
            // lo/hi fields. A Named TypeRef with declaration =
            // builtinClassSym("Range") and declaration->decl = nullptr is
            // the same shape used for DateTime / TimeSpan (scalar/struct
            // value builtins), and lowerTypeRef will recognise it by
            // declaration->name == "Range".
            auto* rt = ctx_.makeTypeRef();
            rt->kind = TypeRef::Kind::Named;
            rt->declaration = builtinClassSym("Range");
            if (elemTy) rt->genericArgs.push_back(elemTy);
            ctx_.setExprType(&e, rt);
        }
    }
    // Identifier / This / Base / TypeOf / NameOf / Default —
    // no expression children to recurse into.
}

// fix #3247 / §16.3 (v0.277): `Range<T>`'s T is restricted by the compiler
// to the integer types (byte, sbyte, short, ushort, int, uint, long, ulong)
// and char. `isValidRangeElementType` reads a resolved TypeRef and returns
// true iff its kind is Primitive and its primitive value is one of those
// eight integer primitives or `char`. A non-Primitive (class, interface,
// string) is rejected at the same site — the intrinsic rule admits no
// non-integer element type, so a class-typed T is also out of policy.
//
// The pre-empt carve-out: a Kind::Error element (a forward reference that
// failed to resolve, or a type that the importer could not place) returns
// **true** so the §16.3 gate does NOT fire on it — the resolution failure
// is reported elsewhere as a different error class, and double-reporting
// `Range<UndefinedType>` as both "unresolved type" and "`Range<T>` requires
// T to be one of the integer types" would be a noisy false-positive. The
// gate fires on the *resolution's* answer, not on the *resolution itself*.
// Lives next to `visitExpr` because the two sites that read the answer
// (the RangeExpr case in `visitExpr` and the `Range<T>` annotation in
// `TypeResolution_Names.cpp`) both reach for the same predicate.
bool isValidRangeElementType(const sema::TypeRef* ty) {
    if (!ty) return true;  // pre-empt: no resolution yet — let the resolver fire
    if (ty->kind == sema::TypeRef::Kind::Error) return true;  // pre-empt: don't double-report
    // per #3242 (narrow-fix-forward on #3247 / v0.277): an unresolved
    // generic type-parameter T — a `Range<T>` reference inside a generic
    // body, before the §29.3 substitution lands at the instantiation site
    // — is ADMITTED. The §16.3 rule applies to the SUBSTITUTED T (T=int,
    // T=char, T=float, ...); rejecting the bare T at the source site would
    // bar every `public method Wrap<T>(r: Range<T>)` whose T will later be
    // bound to char / int (the supported substitutions), even though the
    // #3242 codegen tests for exactly that generic-substitution shape.
    // Detected via the TypeRef's declaration being a GenericParamSymbol —
    // the same shape `resolveTypeParamRef` and the §14.2 generic-param
    // machinery produce for an unresolved `T`. Carve-out, not bypass: the
    // downstream instantiation site re-runs this predicate on the
    // substituted T (e.g. `Range<char>``) and rejects there if the bound
    // violates §16.3.
    if (ty->kind == sema::TypeRef::Kind::Named) {
        // per #3242 (narrow-fix-forward on #3247 / v0.277): an unresolved
        // generic type-parameter T — a `Range<T>` reference inside a generic
        // body, before the §29.3 substitution lands at the instantiation site
        // — is ADMITTED. The §16.3 rule applies to the SUBSTITUTED T (T=int,
        // T=char, T=float, ...); rejecting the bare T at the source site would
        // bar every `public method Wrap<T>(r: Range<T>)` whose T will later be
        // bound to char / int (the supported substitutions), even though the
        // #3242 codegen tests for exactly that generic-substitution shape.
        //
        // resolveNamed (TypeResolution_Names.cpp:1548-1554) records an
        // unresolved generic param as a Named TypeRef with **declaration =
        // nullptr** — it cannot pre-resolve T to a GenericParamSymbol because
        // type-param substitution is the SEMANTIC pass's job, not the
        // resolver's. The detection here is therefore: kind == Named AND
        // declaration == nullptr (and the AST is a single-part bare
        // identifier, which is the only shape resolveNamed produces with no
        // declaration). Carvement, not bypass: the downstream instantiation
        // site re-runs this predicate on the SUBSTITUTED T (e.g. `Range<char>``)
        // and rejects there if the bound violates §16.3.
        if (!ty->declaration) return true;
    }
    if (ty->nullable) return false;  // §16.3: T is non-nullable
    if (ty->kind != sema::TypeRef::Kind::Primitive) return false;
    switch (ty->primitive) {
        case sema::PrimitiveType::Byte:
        case sema::PrimitiveType::SByte:
        case sema::PrimitiveType::Short:
        case sema::PrimitiveType::UShort:
        case sema::PrimitiveType::Int:
        case sema::PrimitiveType::UInt:
        case sema::PrimitiveType::Long:
        case sema::PrimitiveType::ULong:
        case sema::PrimitiveType::Char:
            return true;
        default:
            return false;
    }
}

// fix #3247 / §16.3: a one-line rendering of an element-type TypeRef for
// the diagnostic above. The function name (e.g. `float`, `string`,
// `MyClass`) is what the user sees in the message; the predicate's
// admit/reject answer and the diagnostic text must agree, so the
// predicate and the name helper live in the same translation unit.
//
// `rangeElementTypeName_inner` is the implementation; it lives at the same
// namespace scope (not in an anonymous namespace) because the outer
// `rangeElementTypeName` calls it from this same translation unit and
// requires the unqualified name to resolve. Putting it in an anonymous
// namespace would hide the symbol from the call site, the same bug the
// earlier draft hit.
std::string rangeElementTypeName_inner(const sema::TypeRef* ty);

std::string rangeElementTypeName(const sema::TypeRef* ty) {
    if (!ty) return "<unresolved>";
    if (ty->nullable) {
        // Range<T?> is rejected by the predicate's nullable guard; name the
        // nullability so the message is not a silent `Range<T?>` omission.
        return rangeElementTypeName_inner(ty) + "?";
    }
    return rangeElementTypeName_inner(ty);
}

std::string rangeElementTypeName_inner(const sema::TypeRef* ty) {
    if (!ty) return "<unresolved>";
    if (ty->kind == sema::TypeRef::Kind::Primitive) {
        switch (ty->primitive) {
            case sema::PrimitiveType::None:    return "<unresolved>";
            case sema::PrimitiveType::Bool:    return "bool";
            case sema::PrimitiveType::Char:    return "char";
            case sema::PrimitiveType::String:  return "string";
            case sema::PrimitiveType::Byte:    return "byte";
            case sema::PrimitiveType::SByte:   return "sbyte";
            case sema::PrimitiveType::Short:   return "short";
            case sema::PrimitiveType::UShort:  return "ushort";
            case sema::PrimitiveType::Int:     return "int";
            case sema::PrimitiveType::UInt:    return "uint";
            case sema::PrimitiveType::Long:    return "long";
            case sema::PrimitiveType::ULong:   return "ulong";
            case sema::PrimitiveType::Float:   return "float";
            case sema::PrimitiveType::Double:  return "double";
            case sema::PrimitiveType::Decimal: return "decimal";
            case sema::PrimitiveType::Object:  return "Object";
        }
    }
    if (ty->kind == sema::TypeRef::Kind::Named) {
        if (ty->declaration) return std::string(ty->declaration->name);
        if (ty->astNode && ty->astNode->name && !ty->astNode->name->parts.empty())
            return std::string(ty->astNode->name->parts.back());
        if (!ty->importedName.empty())
            return std::string(ty->importedName);
    }
    if (ty->kind == sema::TypeRef::Kind::InlineArray) {
        return "[N]" + rangeElementTypeName(ty->genericArgs.empty()
                                                ? nullptr
                                                : ty->genericArgs[0]);
    }
    if (ty->kind == sema::TypeRef::Kind::Error) return "<unresolved>";
    return "<unknown>";
}

} // namespace bezel::sema
