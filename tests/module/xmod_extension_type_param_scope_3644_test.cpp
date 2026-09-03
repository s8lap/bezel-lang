// fix #3644 (§11.8 / §14.1 / §14.5 / §17.7 / §17.11 / §26.4, v0.286): follow-up
// to #3643. #3643 gave four `ExprTypecheck.cpp`/`ExprTypecheck_Calls.cpp`
// consumers of "is `name` an in-scope generic type parameter" the missing
// `currentExtension_` arm #3083 had already given `isEnclosingTypeParamName`/
// `typeParamHasExceptionBound`. `ExprTypecheck_Exprs.cpp` was not audited when
// #3643 landed and has FIVE more sites of the identical shape (checked
// `currentMethodDecl_`'s own generic params, then — wrongly, by
// NAME-COINCIDENCE only — `currentType_`'s, with no `currentExtension_` arm):
//
//   S1 `ExprTC::isEnclosingGenericTypeParamName` (§14.1/§17.7 shape guard for
//      `checkBuiltinClosedConstructorSurface`) — a direct twin of #3643's
//      `isInScopeTypeParam`.
//   S2 the `tryIndexerConstraint` lambda inline in `visitExpr` (§14.2/§23.4:
//      `x[i]` on a bare type-parameter-typed `x`, resolved through a
//      `where T: <indexable interface>` bound).
//   S3 the `checkGPs` lambda inline in `visitExpr` (§14.1/§14.5: `new T()`
//      where T is a generic type parameter requires `where T: new()`).
//   S4 the `checkConstraintsFor` lambda inline in `visitExpr` (§17.11:
//      `default(T)` is a compile error when T is constrained non-null).
//   S5 `paramHasValueConstraint`/`recordDefaultTypeParamUse` (§17.11/#3044:
//      the deferred `where T: value` / `default(T)` instantiation-site
//      obligation) — see the S5 NOTE below.
//
// SHAPE, verbatim from #3643 (unchanged here): inside an extension body
// `currentType_` names the EXTENDED type (`ExprTC::visitTopDecl`'s
// `ExtensionDecl` arm), so a `currentType_`-only read answers "is `name` the
// extension's own §26.4 block-level type parameter?" only by name-coincidence
// (an author reusing the same letter for the class and the extension), and
// never at all when the extended type is IMPORTED (`TypeSymbol::decl` is
// unconditionally null for an import). Every row below therefore uses a
// class-level type-parameter name (`K`) DIFFERENT from the extension's own
// (`T`/`Guid`) — deliberately NOT a name-coincidence repro — both because a
// non-coincidental name is a strictly stronger defect demonstration (broken
// even for a purely LOCAL program, not merely "categorically worse when
// imported") and because it doubles as the brief's requested "must not
// alias" control: with `K` and `T` un-aliased, `currentType_`'s own
// `genericParams`/`genericConstraints` can never accidentally satisfy a
// question that is really about the extension's own `T`.
//
// S5 NOTE — implemented for symmetry, NOT independently testable today.
// `recordDefaultTypeParamUse`'s deferred join (`checkDeferredDefaultInstantiations`,
// ExprTypecheck.cpp) matches a recorded `default(T)` use against
// `genericInstantiationSites_` by `owner` pointer identity. The ONLY site
// that ever pushes into `genericInstantiationSites_` is
// `ExprTC::typeCall`'s `checkMethodGenericConstraints` lambda
// (ExprTypecheck_Calls.cpp:2824), which unconditionally records
// `owner = md` — the CALLED METHOD's own `ast::MethodDecl*`. A `default(T)`
// used inside an extension method body records
// `owner = currentExtension_->decl` (an `ast::ExtensionDecl*`, this fix) or,
// pre-existing, `owner = currentType_->decl` (an `ast::TypeDecl*`, the
// CLASS-level arm a few lines up) — NEITHER of which a `MethodDecl*` can ever
// equal. So the class-level arm has been dead code (recorded, never joined)
// since #3044 introduced it, and the extension-level arm this fix adds
// mirrors it exactly — equally correct, equally inert, for the identical
// structural reason. This is NOT a defect #3644 introduces (the class arm's
// unreachability predates this fix and is unrelated to `currentExtension_`);
// it is a broader gap in the deferred-obligation join that #3644's own scope
// (five `currentExtension_`-shaped predicates) does not cover. Filed
// separately as issue 3719 rather than silently left unfixed OR silently
// claimed to work. No diagnostic-observable row is possible for S5 without
// first fixing that join, so S5 gets a single "does not crash / does not
// mis-typecheck" sanity row instead of a FAIL-WITHOUT/PASS-WITH pair.

#include "module/ModuleLoader.hpp"

#include <memory>
#include <string>
#include <string_view>

#include "ast/Node.hpp"
#include "module/ModuleMetadata.hpp"
#include "module/ModuleMetadataBuilder.hpp"
#include "lexer/Lexer.hpp"
#include "parser/Parser.hpp"
#include "sema/DefiniteAssignment.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeRef.hpp"
#include "sema/TypeResolution.hpp"
#include "support/Diagnostic.hpp"
#include "test_harness.hpp"

using bezel::sema::PrimitiveType;
using bezel::sema::TypeRef;

namespace bezel::module {
namespace {

ModuleMetadata buildMeta3644(std::string_view source) {
    DiagnosticEngine diags;
    Lexer lexer(source, diags);
    Parser parser(lexer, diags);
    auto sf = parser.parseCompilationUnit();
    if (!sf) return {};
    sema::SemaContext ctx(diags);
    sema::collectSymbols(*sf, ctx);
    sema::resolveTypes(*sf, ctx);
    sema::typecheckExpressions(*sf, ctx);
    return buildModuleMetadata(ctx, source);
}

struct Run3644 {
    DiagnosticEngine diags;
    std::unique_ptr<ast::SourceFile> sf;
    std::unique_ptr<sema::SemaContext> ctx;
    bool parsed = false;
};

std::unique_ptr<Run3644> run3644(const ModuleMetadata* dep,
                                 std::string_view src) {
    auto r = std::make_unique<Run3644>();
    Lexer lexer(src, r->diags);
    Parser parser(lexer, r->diags);
    r->sf = parser.parseCompilationUnit();
    if (!r->sf) return r;
    r->parsed = true;
    r->ctx = std::make_unique<sema::SemaContext>(r->diags);
    sema::collectSymbols(*r->sf, *r->ctx);
    if (dep) loadMetadata(*dep, *r->ctx);
    sema::resolveTypes(*r->sf, *r->ctx);
    sema::typecheckExpressions(*r->sf, *r->ctx);
    sema::checkDefiniteAssignment(*r->sf, *r->ctx);
    return r;
}

bool sawDiag3644(const Run3644& r, std::string_view needle) {
    for (const auto& d : r.diags.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

// Count of diagnostics whose message contains `needle` — the S4
// double-report REVIEW FINDING regression (below) needs an occurrence
// COUNT, not mere presence/absence: the pre-fix defect is that the correct
// diagnostic fires TWICE (identical text, identical span) rather than a
// wrong diagnostic firing instead of the right one, so `sawDiag3644` alone
// (true both before and after) cannot observe it.
int countDiag3644(const Run3644& r, std::string_view needle) {
    int count = 0;
    for (const auto& d : r.diags.diagnostics())
        if (d.message.find(needle) != std::string::npos) ++count;
    return count;
}

// Find the first `ast::IndexExpr` reachable as a `DiscardStmt`'s value inside
// any `ExtensionDecl` member method's block body — the shape every S2 program
// below uses (`_ := t[i];`). Returns null if not found (a defensively narrow
// walk, not a general AST visitor — every program below has exactly one).
const ast::IndexExpr* findExtDiscardIndexExpr3644(const ast::SourceFile& sf) {
    for (const auto& td : sf.topDecls) {
        auto* ed = dynamic_cast<const ast::ExtensionDecl*>(td.get());
        if (!ed) continue;
        for (const auto& m : ed->members) {
            auto* md = dynamic_cast<const ast::MethodDecl*>(m.get());
            if (!md) continue;
            for (const auto& s : md->stmts) {
                auto* ds = dynamic_cast<const ast::DiscardStmt*>(s.get());
                if (ds && ds->value) {
                    if (auto* ix =
                            dynamic_cast<const ast::IndexExpr*>(ds->value.get()))
                        return ix;
                }
            }
        }
    }
    return nullptr;
}

// --- S1: isEnclosingGenericTypeParamName -----------------------------------

// `Box`'s own class-level parameter is `K` (never `Guid`) — no coincidence is
// possible; the extension's own `Guid` (§26.4) must be recognized purely
// through `isExtensionTargetTypeParam`. `where Guid: new()` is present so
// S3's OWN (separately fixed) `where T: new()` obligation does not also fire
// here and confound the S1-only diagnostic this row targets.
const char* kLibS1_3644 = "public class Box<K> end Box; ";

const char* kExtS1Body_3644 =
    "extension Box<Guid> where Guid: new() "
    "  public method Make(): int "
    "    var t := new Guid(); "
    "    return 5; "
    "  end Make; "
    "end; ";

std::string localS1_3644() {
    return std::string("module App; ") + kLibS1_3644 + kExtS1Body_3644;
}

std::string appS1_3644() {
    return std::string("module App; import Lib; ") + kExtS1Body_3644;
}

// The §17.7 diagnostic this fix eliminates when `Guid` names a genuine
// extension type parameter instead of the real builtin.
constexpr const char* kNoCtorGuid3644 =
    "`Guid` is a standard-library builtin and declares no constructor";

// S1, THE DEFECT (local half — no import needed, since `K` <> `Guid`).
// Pre-fix: `isEnclosingGenericTypeParamName("Guid")` finds nothing in either
// `currentMethodDecl_` (Make is non-generic) or `currentType_`'s own params
// (`K`), so `new Guid()` is treated as the REAL builtin — rejected. Post-fix:
// the extension arm recognizes `Guid` as the extension's own §26.4 parameter,
// and the parallel S3 fix confirms `where Guid: new()` — construction admitted.
BEZEL_TEST(xmod_extension_type_param_scope_3644,
           local_extension_type_param_named_guid_admits_construction_spec_14_1_17_7) {
    auto r = run3644(nullptr, localS1_3644());
    BEZEL_EXPECT_TRUE(r->parsed);
    BEZEL_EXPECT_TRUE(!sawDiag3644(*r, kNoCtorGuid3644));
    BEZEL_EXPECT_TRUE(!r->diags.hasErrors());
}

// S1, the IMPORTED twin — confirms the fix holds across the module boundary
// too (`currentType_->decl` null for imported `Box`, same as the local `K`
// case above: neither arm can answer via `currentType_` either way).
BEZEL_TEST(xmod_extension_type_param_scope_3644,
           imported_extension_type_param_named_guid_admits_construction_spec_14_1_17_7) {
    ModuleMetadata dep = buildMeta3644(std::string("module Lib; ") + kLibS1_3644);
    auto r = run3644(&dep, appS1_3644());
    BEZEL_EXPECT_TRUE(r->parsed);
    BEZEL_EXPECT_TRUE(!sawDiag3644(*r, kNoCtorGuid3644));
    BEZEL_EXPECT_TRUE(!r->diags.hasErrors());
}

// --- S2: tryIndexerConstraint -----------------------------------------------

const char* kLibS2_3644 =
    "public interface IBox "
    "  readonly property [i: int]: int; "
    "end IBox; "
    "public class Box<K> end Box; ";

const char* kExtS2Body_3644 =
    "extension Box<T> where T: IBox "
    "  public method Get(t: T): int "
    "    _ := t[0]; "
    "    return 5; "
    "  end Get; "
    "end; ";

std::string localS2_3644() {
    return std::string("module App; ") + kLibS2_3644 + kExtS2Body_3644;
}

std::string appS2_3644() {
    return std::string("module App; import Lib; ") + kExtS2Body_3644;
}

// S2, THE DEFECT (local half). Pre-fix: `t[0]`'s IndexExpr is left UNTYPED —
// neither `currentMethodDecl_` (Get has no generic params of its own) nor
// `currentType_` (`Box`'s own `K` <> `T`) resolves `T`'s constraint, and no
// other branch of the big index-typing if/else-chain matches a bare
// type-parameter receiver (the `else if (tt->declaration)` user-defined-
// indexer arm a few lines down requires a non-null `declaration`, which a
// bare generic parameter never has) — so `ctx_.exprType(*ix)` stays null,
// silently (matches #794's own original description of this failure mode).
// Post-fix: the extension arm resolves `T`'s `where T: IBox` bound and types
// the IndexExpr as `IBox`'s declared indexer return type, `int`.
BEZEL_TEST(xmod_extension_type_param_scope_3644,
           local_extension_constraint_indexer_types_via_extension_bound_spec_14_2_23_4) {
    auto r = run3644(nullptr, localS2_3644());
    BEZEL_EXPECT_TRUE(r->parsed);
    BEZEL_EXPECT_TRUE(r->ctx != nullptr);
    if (!r->ctx) return;
    const ast::IndexExpr* ix = findExtDiscardIndexExpr3644(*r->sf);
    BEZEL_EXPECT_TRUE(ix != nullptr);
    if (!ix) return;
    const TypeRef* ty = r->ctx->exprType(*ix);
    BEZEL_EXPECT_TRUE(ty != nullptr);
    if (!ty) return;
    BEZEL_EXPECT_TRUE(ty->kind == TypeRef::Kind::Primitive);
    BEZEL_EXPECT_TRUE(ty->primitive == PrimitiveType::Int);
}

// S2, the IMPORTED twin — `currentType_->decl` is unconditionally null for
// the imported `Box`, so pre-fix this side is broken for the identical
// reason as the local side above (there is no name-coincidence to even
// partially mask it, unlike #3643's own T1 row).
BEZEL_TEST(xmod_extension_type_param_scope_3644,
           imported_extension_constraint_indexer_types_via_extension_bound_spec_14_2_23_4) {
    ModuleMetadata dep = buildMeta3644(std::string("module Lib; ") + kLibS2_3644);
    auto r = run3644(&dep, appS2_3644());
    BEZEL_EXPECT_TRUE(r->parsed);
    BEZEL_EXPECT_TRUE(r->ctx != nullptr);
    if (!r->ctx) return;
    const ast::IndexExpr* ix = findExtDiscardIndexExpr3644(*r->sf);
    BEZEL_EXPECT_TRUE(ix != nullptr);
    if (!ix) return;
    const TypeRef* ty = r->ctx->exprType(*ix);
    BEZEL_EXPECT_TRUE(ty != nullptr);
    if (!ty) return;
    BEZEL_EXPECT_TRUE(ty->kind == TypeRef::Kind::Primitive);
    BEZEL_EXPECT_TRUE(ty->primitive == PrimitiveType::Int);
}

// --- S3: checkGPs (`new T()` / `where T: new()`) ----------------------------

const char* kLibS3_3644 = "public class Box<K> end Box; ";

// No `where T: new()` on the extension — §14.1 requires the reject.
const char* kExtS3UnconstrainedBody_3644 =
    "extension Box<T> "
    "  public method Make(): int "
    "    var t := new T(); "
    "    return 5; "
    "  end Make; "
    "end; ";

// The accept-control counterpart: `where T: new()` IS present.
const char* kExtS3ConstrainedBody_3644 =
    "extension Box<T> where T: new() "
    "  public method Make(): int "
    "    var t := new T(); "
    "    return 5; "
    "  end Make; "
    "end; ";

constexpr const char* kNeedsNewCtor3644 =
    "requires the type parameter `T` to be constrained `where T: new()`";

// S3, THE DEFECT (local half). Pre-fix: `checkGPs`'s `isTypeParam` never
// becomes true for the extension's own `T` (no `currentExtension_` arm), so
// `if (isTypeParam && !hasNewConstraint)` never even evaluates — `new T()`
// on a genuinely UNCONSTRAINED extension type parameter silently compiles,
// a real §14.1 soundness hole (the body type-checks under the weakest
// assumption its constraints establish, i.e. NONE, yet construction is
// assumed). Post-fix: `T` is recognized, `hasNewConstraint` is false, and the
// loud, correct reject fires.
BEZEL_TEST(xmod_extension_type_param_scope_3644,
           local_extension_new_expr_without_new_constraint_rejected_spec_14_1) {
    auto r = run3644(nullptr, std::string("module App; ") + kLibS3_3644 +
                                   kExtS3UnconstrainedBody_3644);
    BEZEL_EXPECT_TRUE(r->parsed);
    BEZEL_EXPECT_TRUE(sawDiag3644(*r, kNeedsNewCtor3644));
}

// S3, the IMPORTED twin.
BEZEL_TEST(xmod_extension_type_param_scope_3644,
           imported_extension_new_expr_without_new_constraint_rejected_spec_14_1) {
    ModuleMetadata dep = buildMeta3644(std::string("module Lib; ") + kLibS3_3644);
    auto r = run3644(&dep, std::string("module App; import Lib; ") +
                               kExtS3UnconstrainedBody_3644);
    BEZEL_EXPECT_TRUE(r->parsed);
    BEZEL_EXPECT_TRUE(sawDiag3644(*r, kNeedsNewCtor3644));
}

// S3, ACCEPT CONTROL — `where T: new()` genuinely present on the extension.
// Both pre- and post-fix are silent here (pre-fix because `isTypeParam` never
// becomes true at all, so the whole check is skipped; post-fix because the
// obligation is correctly discharged) — not a FAIL-WITHOUT/PASS-WITH pair,
// but a non-widening guard that a properly constrained extension type
// parameter still compiles after the fix.
BEZEL_TEST(xmod_extension_type_param_scope_3644,
           local_extension_new_expr_with_new_constraint_accepted_spec_14_1) {
    auto r = run3644(nullptr, std::string("module App; ") + kLibS3_3644 +
                                   kExtS3ConstrainedBody_3644);
    BEZEL_EXPECT_TRUE(r->parsed);
    BEZEL_EXPECT_TRUE(!sawDiag3644(*r, kNeedsNewCtor3644));
    BEZEL_EXPECT_TRUE(!r->diags.hasErrors());
}

// --- S4: checkConstraintsFor (`default(T)` under a non-null constraint) ----

const char* kLibS4_3644 = "public class Box<K> end Box; ";

// `where T: class` on the EXTENSION — §17.11 requires `default(T)` to reject.
const char* kExtS4ClassBody_3644 =
    "extension Box<T> where T: class "
    "  public method Make(): int "
    "    _ := default(T); "
    "    return 5; "
    "  end Make; "
    "end; ";

// The accept-control counterpart: NO constraint at all — §17.11's own text,
// "unconstrained T ... permitted but its result is treated as possibly
// null" — must stay accepted.
const char* kExtS4UnconstrainedBody_3644 =
    "extension Box<T> "
    "  public method Make(): int "
    "    _ := default(T); "
    "    return 5; "
    "  end Make; "
    "end; ";

constexpr const char* kDefaultNonNull3644 =
    "is a compile error when T is constrained non-null";

// S4, THE DEFECT (local half). Pre-fix: `checkConstraintsFor` never scans
// the EXTENSION's own `where T: class` (only `currentMethodDecl_`'s and
// `currentType_`'s — `Box`'s own, which declares no constraint at all), so
// `default(T)` silently produces null even though the extension's own `T`
// contract is non-null — a real soundness hole, not merely a missed
// diagnostic (the produced value violates T's contract). Post-fix: the
// extension's own `where T: class` is seen and the loud, correct reject
// fires.
BEZEL_TEST(xmod_extension_type_param_scope_3644,
           local_extension_default_of_class_constrained_param_rejected_spec_17_11) {
    auto r = run3644(nullptr, std::string("module App; ") + kLibS4_3644 +
                                   kExtS4ClassBody_3644);
    BEZEL_EXPECT_TRUE(r->parsed);
    BEZEL_EXPECT_TRUE(sawDiag3644(*r, kDefaultNonNull3644));
}

// S4, the IMPORTED twin.
BEZEL_TEST(xmod_extension_type_param_scope_3644,
           imported_extension_default_of_class_constrained_param_rejected_spec_17_11) {
    ModuleMetadata dep = buildMeta3644(std::string("module Lib; ") + kLibS4_3644);
    auto r = run3644(&dep, std::string("module App; import Lib; ") +
                               kExtS4ClassBody_3644);
    BEZEL_EXPECT_TRUE(r->parsed);
    BEZEL_EXPECT_TRUE(sawDiag3644(*r, kDefaultNonNull3644));
}

// S4, ACCEPT CONTROL — an UNCONSTRAINED extension type parameter's
// `default(T)` must stay legal both before and after the fix (§17.11: "under
// [it], default(T) is permitted"). Guards against an over-broad fix that
// would reject every `default(T)` inside an extension unconditionally.
BEZEL_TEST(xmod_extension_type_param_scope_3644,
           local_extension_default_of_unconstrained_param_accepted_spec_17_11) {
    auto r = run3644(nullptr, std::string("module App; ") + kLibS4_3644 +
                                   kExtS4UnconstrainedBody_3644);
    BEZEL_EXPECT_TRUE(r->parsed);
    BEZEL_EXPECT_TRUE(!sawDiag3644(*r, kDefaultNonNull3644));
    BEZEL_EXPECT_TRUE(!r->diags.hasErrors());
}

// S4, REVIEW FINDING regression — a NAME-COINCIDENT class-level and
// extension-level constraint (`class Box<T> where T: class`, extended by
// `extension Box<T> where T: class`, `T` spelled the same on both) must
// still produce the §17.11 `default(T)` reject EXACTLY ONCE. The fix's
// first cut gated `checkConstraintsFor`'s extension arm only on
// `!isMethodParam` (unlike its sibling `checkGPs`, which correctly gated on
// `!isTypeParam`), so BOTH the type arm (Box's own `where T: class`) and
// the extension arm (never gated on the type arm's own success) reported
// the IDENTICAL diagnostic at the IDENTICAL span — a double-fire, not a
// wrong-vs-right diagnostic divergence, so `sawDiag3644` alone cannot pin
// it (true both before and after). Pre-fix (measured on 66be06c): count 2.
// Post-fix: count 1 — this is the discriminating oracle.
BEZEL_TEST(xmod_extension_type_param_scope_3644,
           local_class_and_extension_coincident_constraint_reports_default_reject_once_spec_17_11) {
    auto r = run3644(nullptr,
                      std::string("module App; "
                                   "class Box<T> where T: class end Box; ") +
                          kExtS4ClassBody_3644);
    BEZEL_EXPECT_TRUE(r->parsed);
    BEZEL_EXPECT_EQ(1, countDiag3644(*r, kDefaultNonNull3644));
}

// --- S5: paramHasValueConstraint / recordDefaultTypeParamUse ---------------

// See the S5 NOTE at the top of this file: the deferred join this arm feeds
// is unreachable for ANY class- or extension-level owner today (a
// pre-existing, #3644-unrelated limitation the class-level arm shares), so
// this is a sanity row — the extension's own `where T: value` obligation is
// recognized and recorded without crashing or mis-typechecking the rest of
// the body — not a FAIL-WITHOUT/PASS-WITH diagnostic pin.
const char* kExtS5Body_3644 =
    "extension Box<T> where T: value "
    "  public method Make(): int "
    "    _ := default(T); "
    "    return 5; "
    "  end Make; "
    "end; ";

BEZEL_TEST(xmod_extension_type_param_scope_3644,
           local_extension_default_of_value_constrained_param_compiles_clean_spec_17_11) {
    auto r = run3644(nullptr, std::string("module App; ") + kLibS4_3644 +
                                   kExtS5Body_3644);
    BEZEL_EXPECT_TRUE(r->parsed);
    BEZEL_EXPECT_TRUE(!r->diags.hasErrors());
}

}  // namespace
}  // namespace bezel::module
