// fix #3643 (§11.8 / §14.2 / §26.4 / §28.2, v0.286): a class-1 drain of the
// ExprTypecheck family (issue #3617's own coordinator brief, R10) turned up
// four MORE consumers of "is `name` an in-scope generic type parameter" that
// share #3083's blind spot — #3083 fixed `isEnclosingTypeParamName` and
// `typeParamHasExceptionBound` (the §31.1/§31.2.1 throw/catch-arm predicates)
// so an extension's own §26.4 block-level type parameter is recognized via
// `isExtensionTargetTypeParam`'s walk of the extension's `targetType`
// argument positions, and explicitly flagged: "audit every other consumer of
// isEnclosingTypeParamName for the same blind spot ... any rule keyed on 'is
// this name an enclosing type parameter' is equally blind inside an
// extension." Nobody had, until this fix.
//
// THE SHAPE. Inside an extension body `currentType_` is set to the EXTENDED
// type (`ExprTC::visitTopDecl`'s `ExtensionDecl` arm), so `this`/bare-member
// lookups resolve against it. Each of the four consumers below checked
// `currentMethodDecl_`'s own generic params, then — WRONGLY — `currentType_`'s
// own declared params (`dynamic_cast<const ast::TypeDecl*>(currentType_->decl)
// ->genericParams`), with no `currentExtension_` arm at all. `extension
// Box<T>` binds a FRESH `T` belonging to the EXTENSION (§26.4: "each bare
// single-segment non-nullable identifier introduces a fresh block-level type
// parameter — always"), not to `Box`'s own declaration — `ast::ExtensionDecl`
// carries no `genericParams` field of its own (#3083). So the `currentType_`
// arm answers correctly only by NAME-COINCIDENCE (an author reusing the same
// letter for both, e.g. `extension Box<T>` over `class Box<T>`), and always
// answers false when the extended type is IMPORTED: `TypeSymbol::decl` is
// unconditionally null for an imported type (`ModuleLoader.cpp`'s Pass-3
// generic-body reparse attaches a decl to individual MEMBER symbols only,
// never to the TYPE's own `decl`).
//
// T1 — `ExprTC::isInScopeTypeParam` (§11.8, gates `T.Member(...)` receiver
// recognition) + `ExprTC::forEachConstraintBound` (§11.8, walks constraint
// bounds for a static-member-via-constraint lookup), BOTH in
// ExprTypecheck.cpp. MEASURED against a character-identical single-module
// twin, `interface IZero<T> { static method Zero(): owned T; }` +
// `class Box<T> end Box;`, extended by
// `extension Box<T> where T: IZero<T> { method Reset(): owned T
//   return T.Zero(); end; }`:
//   local    -> `T` recognized (via the `currentType_` coincidence — Box's
//               OWN declared `T` happens to match), but `forEachConstraintBound`
//               then fails to find `IZero<T>` (declared on the EXTENSION,
//               which it never scanned): "type parameter `T` has no static
//               member `Zero`".
//   imported -> `T` is NOT recognized AT ALL: "unresolved identifier `T`" —
//               a categorically worse diagnostic for a well-formed generic
//               extension, purely because the coincidence the local case
//               relied on cannot fire across a module boundary.
// With BOTH functions given the missing `currentExtension_` arm (reusing
// `isExtensionTargetTypeParam`, promoted from an ExprTypecheck.cpp
// file-static to an ExprTC member for this cross-file reuse), NEITHER
// diagnostic fires, on either side.
//
// T2 — the two identically-shaped `isDeclaredParam`/`scanRefConstraint`
// copies in `ExprTC::typeCall`'s §28.2 `T.Dispose()` gate
// (ExprTypecheck_Calls.cpp), fixed with a third `currentExtension_` arm on
// both `scanRefConstraint`'s scan list and `isDeclaredParam`. MEASURED, two
// DIFFERENT shapes because the bug's local and imported halves manifest
// differently here (unlike T1, where both halves are diagnostic-observable
// the same way):
//   local, `class Holder<T> end Holder;` extended by `extension Holder<T>
//   where T: class { method Clear(owned x: T) x.Dispose(); end; }` — an
//   OVER-REJECTION: `isDeclaredParam` was already true pre-fix (via the
//   `currentType_`/`Holder` name-coincidence `isInScopeTypeParam`'s comment
//   describes), so `scanRefConstraint` missing the extension's OWN
//   `where T: class` produced a wrong, loud reject: "no member `Dispose` on
//   unconstrained type parameter `T`".
//   imported, same `Holder`/extension shape — pre-fix `isDeclaredParam` was
//   ALSO false here (the `currentType_` coincidence cannot fire across a
//   module boundary, same root cause as T1), so the diagnostic branch
//   (which requires `isDeclaredParam`) never ran at all: a SILENT DEFERRAL,
//   not a correct accept — confirmed zero diagnostics with the `where`
//   clause EITHER present or absent, since presence/absence only feeds
//   `scanRefConstraint`, downstream of the `isDeclaredParam` gate that was
//   already false. Fixed, this becomes reachable and CORRECT either way:
//   ref-constrained now silently admits (matching T2a's fixed local
//   behavior); UNCONSTRAINED now loudly and correctly rejects — the
//   diagnostically-observable half of the imported repro this file pins.
//
// WHY THE SEMA-ONLY HARNESS. Both repros are extension MEMBER BODIES, which
// `ExprTC::visitTopDecl`'s `ExtensionDecl` arm typechecks unconditionally via
// `visitMember` for every declared member — no external CALL to `Reset`/
// `Clear` is needed to reach the buggy code, and this file deliberately never
// makes one: `new Box<Counter>().Reset()`/`new Holder<Res>().Clear(...)`-style
// external extension-method calls hit an unrelated, pre-existing arity bug
// ("no extension method `M` on type `X` takes N argument(s) (§26.5)") for a
// generic-parameter-typed argument/return, measured independently of this fix
// and out of scope here (not filed as its own issue by this commit — flagged
// in the PR-adjacent #3643 issue thread instead of a second ticket, since the
// call-arity resolver lives outside this fix's four functions and reproducing
// it standalone is future work, not blocking this drain).
//
// FALSE-REJECT COST: the two rows per repro are diagnostic PRESENCE/ABSENCE
// checks, not accept/reject counts over a corpus; see the commit body for the
// lit + full-suite filter results this fix was verified against.

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
#include "sema/TypeResolution.hpp"
#include "support/Diagnostic.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

ModuleMetadata buildMeta3643(std::string_view source) {
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

struct Run3643 {
    DiagnosticEngine diags;
    std::unique_ptr<ast::SourceFile> sf;
    std::unique_ptr<sema::SemaContext> ctx;
    bool parsed = false;
};

std::unique_ptr<Run3643> run3643(const ModuleMetadata* dep,
                                 std::string_view src) {
    auto r = std::make_unique<Run3643>();
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

bool sawDiag3643(const Run3643& r, std::string_view needle) {
    for (const auto& d : r.diags.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

// Verbatim fragments of the three diagnostics this fix eliminates from the
// two repro shapes below.
constexpr const char* kUnresolvedT3643 = "unresolved identifier `T`";
constexpr const char* kNoStaticZero3643 =
    "type parameter `T` has no static member `Zero`";
constexpr const char* kNoDispose3643 =
    "no member `Dispose` on unconstrained type parameter `T`";

// --- T1: isInScopeTypeParam + forEachConstraintBound ------------------------

const char* kLibT1_3643 =
    "module Lib; "
    "public interface IZero<T> "
    "  static method Zero(): owned T; "
    "end IZero; "
    "public class Box<T> "
    "end Box; ";

// The extension's own §26.4 head parameter is spelled `T`, matching `Box`'s
// (and, for the local twin, also `IZero`'s) own declared parameter letter —
// the exact naming convention that makes the `currentType_` coincidence
// mask the bug for a LOCAL extended type.
const char* kExtT1Body_3643 =
    "extension Box<T> where T: IZero<T> "
    "  public method Reset(): owned T "
    "    return T.Zero(); "
    "  end Reset; "
    "end; ";

std::string appT1_3643() {
    return std::string("module App; import Lib; ") + kExtT1Body_3643;
}

std::string localT1_3643() {
    return std::string(
               "module App; "
               "interface IZero<T> "
               "  static method Zero(): owned T; "
               "end IZero; "
               "class Box<T> "
               "end Box; ") +
           kExtT1Body_3643;
}

// #3643 T1, THE DEFECT (imported half). Pre-fix: "unresolved identifier `T`".
BEZEL_TEST(xmod_extension_type_param_scope_3643,
           imported_extension_type_param_resolves_static_member_spec_11_8) {
    ModuleMetadata dep = buildMeta3643(kLibT1_3643);
    auto r = run3643(&dep, appT1_3643());
    BEZEL_EXPECT_TRUE(r->parsed);
    BEZEL_EXPECT_TRUE(!sawDiag3643(*r, kUnresolvedT3643));
    BEZEL_EXPECT_TRUE(!sawDiag3643(*r, kNoStaticZero3643));
}

// #3643 T1, the single-module control. Pre-fix: "has no static member
// `Zero`" (a DIFFERENT, less-broken diagnostic than the imported side —
// `isInScopeTypeParam` succeeds here via the `currentType_` coincidence, but
// `forEachConstraintBound` still cannot see the extension's own `where`
// clause). Post-fix: clean on both sides identically.
BEZEL_TEST(xmod_extension_type_param_scope_3643,
           local_extension_type_param_resolves_static_member_spec_11_8) {
    auto r = run3643(nullptr, localT1_3643());
    BEZEL_EXPECT_TRUE(r->parsed);
    BEZEL_EXPECT_TRUE(!sawDiag3643(*r, kUnresolvedT3643));
    BEZEL_EXPECT_TRUE(!sawDiag3643(*r, kNoStaticZero3643));
}

// --- T2: the ExprTypecheck_Calls.cpp T.Dispose() gate -----------------------

const char* kLibT2_3643 =
    "module Lib; "
    "public class Holder<T> "
    "end Holder; ";

// T2a's body: the extension's `where T: class` bound (declared on the
// EXTENSION, not on `Holder`) SHOULD entitle `T.Dispose()` (§28.2: "every
// class implicitly implements IDisposable ... a class, interface, or
// class-type constraint makes the call legal").
const char* kExtT2ConstrainedBody_3643 =
    "extension Holder<T> where T: class "
    "  public method Clear(owned x: T): int "
    "    x.Dispose(); "
    "    return 7; "
    "  end Clear; "
    "end; ";

// T2b's body: NO `where` clause at all — `T` is a genuine, declared extension
// type parameter, but carries no reference-type bound, so §28.2/§14.2
// correctly reject `T.Dispose()` ("add a `where T: <constraint>` that
// provides this member").
const char* kExtT2UnconstrainedBody_3643 =
    "extension Holder<T> "
    "  public method Clear(owned x: T): int "
    "    x.Dispose(); "
    "    return 7; "
    "  end Clear; "
    "end; ";

std::string localT2Constrained_3643() {
    return std::string("module App; class Holder<T> end Holder; ") +
           kExtT2ConstrainedBody_3643;
}

std::string appT2Unconstrained_3643() {
    return std::string("module App; import Lib; ") +
           kExtT2UnconstrainedBody_3643;
}

// #3643 T2a, THE OVER-REJECTION DEFECT (local half — the fix's effect IS
// diagnostically observable here). Pre-fix: `scanRefConstraint` never scanned
// the EXTENSION's own `where T: class` (only `currentMethodDecl_`'s and
// `currentType_`'s, i.e. `Holder`'s own declared constraints, which are
// empty), so a legally ref-constrained `T` was wrongly rejected: "no member
// `Dispose` on unconstrained type parameter `T`". `isDeclaredParam` itself
// was ALREADY true pre-fix here (via the `currentType_`-`Holder` coincidence
// `isInScopeTypeParam`'s comment describes — `Holder`'s own declared `T`
// happens to match), which is exactly why this diagnostic fired at all
// instead of the code silently falling through, and why this row can pin the
// defect via diagnostic ABSENCE while T2b (below) needs diagnostic PRESENCE.
BEZEL_TEST(xmod_extension_type_param_scope_3643,
           local_extension_ref_constrained_param_admits_dispose_spec_28_2) {
    auto r = run3643(nullptr, localT2Constrained_3643());
    BEZEL_EXPECT_TRUE(r->parsed);
    BEZEL_EXPECT_TRUE(!sawDiag3643(*r, kNoDispose3643));
}

// #3643 T2b, THE SILENT-DEFERRAL DEFECT (imported half). Unlike T2a, an
// IMPORTED `Holder`'s `currentType_->decl` is unconditionally null, so
// pre-fix `isDeclaredParam` was ALSO false here regardless of the extension's
// `where` clause — the diagnostic-reporting branch requires `isDeclaredParam`
// to be true, so pre-fix this shape produced ZERO diagnostics at ANY
// severity (verified directly against this exact input, both with and
// without the `where T: class` clause): not a correct accept, but every
// check in `ExprTC::typeCall`'s Dispose gate silently deferring past this
// site entirely.
//
// That means T2a's OWN imported/constrained twin (`extension Holder<T>
// where T: class`, same as T2a but over an imported `Holder`) is NOT
// diagnostically distinguishable pre- vs. post-fix — both produce zero
// diagnostics here, since post-fix it is admitted silently (correctly,
// confirmed by direct inspection of `refConstrained`/`isObjectMember` during
// development) and pre-fix it was silently deferred (unverified). So this
// row drops the `where` clause instead: `T` is then a genuine but
// UNCONSTRAINED extension parameter, and post-fix the new
// `currentExtension_` arm correctly recognizes it as declared and PRODUCES
// the loud, correct §14.2 reject — exactly the diagnostic T2a's local twin
// pins for the ref-constrained case, now reachable on the imported side too.
BEZEL_TEST(xmod_extension_type_param_scope_3643,
           imported_extension_unconstrained_param_rejects_dispose_spec_14_2) {
    ModuleMetadata dep = buildMeta3643(kLibT2_3643);
    auto r = run3643(&dep, appT2Unconstrained_3643());
    BEZEL_EXPECT_TRUE(r->parsed);
    BEZEL_EXPECT_TRUE(sawDiag3643(*r, kNoDispose3643));
}

}  // namespace
}  // namespace bezel::module
