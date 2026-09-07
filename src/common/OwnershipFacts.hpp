#pragma once

// #1414 Stage 4 Phase A — the shared ownership-FACTS core (behaviour-preserving).
//
// The compiler decides "does this expression PRODUCE a fresh owned value?" and
// "does this call TRANSFER an owned element to the caller?" in several places.
// Historically each place carried its OWN copy of that expression-shape
// classification, and the copies drifted silently past a green suite — the
// #1388 drift class (and #1415, found during the review of this plan).  This
// module is the ONE home for the expression-SHAPE half of that decision, so a
// single fact lives in a single place.
//
// Two consumers query this core (see the delegating wrappers, each commented as
// "the {sema,codegen}-side consumer of the shared ownership-facts core"):
//   * SEMA — the ownership pass (src/ownership/Ownership.cpp `producerState`),
//     which runs BEFORE monomorphization and supplies an identity resolver.
//   * CODEGEN — `Emitter::isFreshOwnedClassProducer` /
//     `Emitter::isBuiltinOwnedTransferCall` (src/codegen/LlvmBackend_Setup.cpp),
//     which run inside monomorphized bodies and inject `resolveTypeParamRef`
//     (backed by `genericSubstSemaTy_`) as the resolver hook.
//
// Scope boundaries (per the plan, §"Stage 4", amendments 1 & 3):
//   * This core owns EXPRESSION SHAPE only.  MECHANISM selection — disposerFns_,
//     runtime-vs-vtable routing, unwind-ineligibility, the §28 discharge
//     machinery — stays in codegen and is NOT moved here.
//   * All NAME-KEYED tables live in the Stage-3 BuiltinRegistry; this core
//     CONSULTS the registry rather than re-listing builtin names.  (The
//     Pop/Dequeue-on-`OwningStack`/`OwningQueue` and TakeCurrent-on-owning-
//     enumerator guards below are per-method operand pairings, NOT a reusable
//     name table, so they stay inline — see OwnershipFacts.cpp.)
//   * fix #3634: a predicate is SHAPE, not MECHANISM, when it is a pure read
//     of a Symbol/AST node's OWN fields (no Emitter state, no LLVM type, no
//     disposerFns_/vtable-slot/unwind lookup) and answers a yes/no
//     CLASSIFICATION question about that node — "is this a captured `owned`
//     primary-ctor param slot?", not "which function/slot implements it?".
//     That a shape fact is later CONSUMED by a mechanism decision (which
//     capture slot to allocate, which vtable route to install) does not
//     retroactively make the fact itself mechanism — by that reading almost
//     every predicate in this file would qualify, since codegen consumes all
//     of them.  `isCapturedOwnedPrimaryParam` (below, added by #3634) is the
//     worked example: it takes only a `FieldSymbol*` and touches none of the
//     mechanism state named above.
//
// Phase A is a BEHAVIOUR-PRESERVING extraction: every classification result is
// byte-identical to the pre-refactor wrappers.  The drift fixes (#1388, #1415)
// are Phase B, deliberately NOT done here.

#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bezel::ast {
struct Expr;
struct Stmt;
struct CallExpr;
struct IndexExpr;
struct MethodDecl;
struct BindExpr;
struct LocalDecl;
struct Param;
struct TypeDecl;
}  // namespace bezel::ast

namespace bezel::sema {
class SemaContext;
struct TypeRef;
struct Symbol;
struct TypeSymbol;
struct FieldSymbol;
}  // namespace bezel::sema

namespace bezel::common {

// A hook that resolves a bare type-parameter TypeRef (`T` / `K`, a single-part
// Named ref with no `declaration`) to its CONCRETE substituted sema type in the
// caller's current context.  Codegen supplies `resolveTypeParamRef` (backed by
// the active `genericSubstSemaTy_` monomorphization binding); sema, which runs
// before monomorphization, supplies the identity resolver.  Any non-parameter
// ref must be returned unchanged. // per §14.1 / fix #567
using TypeParamResolver =
    std::function<const sema::TypeRef*(const sema::TypeRef*)>;

// §28.9 "Trivial-dispose classes" (v0.237) / §22.5 (#1684, #2654): does
// `decl`'s class chain make it NON-trivial-dispose — a destructor, or a
// non-static `owned` field (declared, or a captured `owned` primary-
// constructor parameter, §22.10), anywhere from `decl` up through its base
// classes?  The inverse of the spec's trivial-dispose classification.
//
// ⚠ THE QUOTE THAT USED TO STAND HERE IS FROM A PRE-v0.286 REVISION and is
// left un-rewritten on purpose (issue 3632).  It read "…it has no destructor,
// no `owned` fields, … and its base class (if any, other than `Object`) is
// itself trivial-dispose".  §28.9 at v0.286 reads "no `owned` fields OF
// NON-TRIVIAL-DISPOSE TYPE", and spells the consequence out: "an `owned` field
// whose declared type is itself trivial-dispose does not disqualify the
// holder".  This function does NOT implement that recursive clause — the
// `isOwned && !isStatic` arm below never reads the field's type — so updating
// the quotation to v0.286's words would make this comment describe a rule the
// code does not follow, which is the defect class this file's own history is
// about.  Measured over-rejection, safe direction, filed with the widening's
// hazards (the coinductive walk, §3.6 rule 2's per-instantiation reading, and
// the codegen cascade that must be confirmed BEFORE sema is widened) as
// issue 3632.
//
// Relocated here VERBATIM from `ExprTC::isNonTrivialDisposeClass` (#2654) so
// the sema consumers (§22.5 default-value check, §21.9 bare-discard) and the
// ownership pass's §28.3 auto-drop warning gate share ONE copy rather than
// drifting.  `ExprTC::isNonTrivialDisposeClass` is now a delegating wrapper.
// The body is a pure function of its argument — it reads no ExprTC state —
// which is what makes the relocation behaviour-preserving.
//
// fix #2666: the owning-inline-array arm of the spec's third disqualifier
// (`[N owned]T?` / `[N owned]T`, §15.1.1) is tested here directly — read
// from `fs->posParam->type` for primary-ctor parameters and from the body
// `FieldDecl`'s type otherwise, via `sema::fieldDeclTypeOf`
// (src/sema/FieldSymbolAccessors.hpp, fix #3650) so this file holds no AST
// cast of its own.  The IMPORTED-class short-circuit
// (`importedNonTrivialDispose`) above carries the exporter's verdict on the
// wire; the exporter runs this same predicate.  Callers that previously kept
// their own inline-array walk (`MethodOwnership::classChainHasOwningInlineArrayField`
// at the §28.3 gate in Ownership.cpp, #2654) consult this predicate only and
// delete the local companion.
//
// fix #3588 (§28.9 / §22.15 / §21.9, v0.286): THIS IS NOW THE ONLY BODY OF
// THAT CLASSIFICATION IN THE TREE.  Two hand-maintained copies were collapsed
// onto it — `PureBodyWalker::isTrivialDisposeClass` (§22.15's throw carve-out,
// which is this predicate INVERTED) and the inline walk in
// `ExprTypecheck.cpp`'s §21.9 ExpressionStmt arm.  Measured at 11920b8 by a
// normalizing diff (comments and whitespace stripped, namespace qualifiers
// folded): the §22.15 copy differed from this one ONLY in spelling
// (per-level `return false` where this accumulates into `reject`), and the
// §21.9 copy ONLY in gating its carried-verdict arm — which is now the
// `ImportedDisposeVerdict` parameter below.  Not one classification arm
// disagreed.
//
// The paragraph that used to stand here said the copies were "updated in
// lockstep ... so the four walks cannot drift"; `PureChecker.cpp`'s said they
// were "kept in sync with the predicate's shape by construction".  Neither was
// true as written: they were kept in sync BY HAND, three times over, which is
// why #2666 had to edit all three and #3191 had to edit two of them, in
// separate commits.  It is true by construction now.
//
// `MethodOwnership::rhsIsDisposableClassType` (Ownership.cpp) is deliberately
// NOT one of these and was NOT collapsed: it answers a coarser, differently
// scoped question for the §28.10 vacuous-pin lint — it matches INTERFACES, it
// does not walk base chains, it has no inline-array arm, no primary-ctor-
// parameter arm and no `static owned` exclusion, and it pairs with a builtin
// head-name table this predicate does not consult.  It disagrees with this one
// in BOTH directions on real inputs; its own comment is the record of why.
// See #3588 for the full per-copy exclusion diff.
//
// fix #3634 (§22.10 / §28.9, v0.286): the captured `owned` primary-ctor
// parameter arm below used to be a SECOND spelling of codegen's
// `Emitter::isCapturedOwnedPrimaryParam` (LlvmBackend_impl.hpp) — character-
// identical, per #3205's own comment there, because a captured slot is
// non-trivial-dispose EXACTLY when its generated Dispose has a slot to walk:
// the trivial-dispose verdict and the capture decision are the same fact
// asked twice.  #3588 declined to collapse the pair in passing, because doing
// so silently would cross this file's own MECHANISM/EXPRESSION-SHAPE boundary
// (lines 22-25 as they stood then) without arguing it — filed as issue 3634.
//
// THE ARGUMENT (settling 3634): `isCapturedOwnedPrimaryParam` below is FACT,
// not MECHANISM.  It reads exactly `fs->posParam`, `fs->parent->kind`,
// `fs->posParam->modifier` and `fs->posParamInitRef` — four AST/Symbol-level
// fields on the argument itself.  It touches no Emitter state, no LLVM type,
// no disposerFns_, no vtable/itable slot, no unwind bookkeeping — none of the
// MECHANISM vocabulary lines 22-25 name.  What codegen does with the answer
// (allocate a capture field, walk it in the generated Dispose, choose a
// runtime-vs-vtable Dispose route) IS mechanism and stays entirely in
// codegen; only the classification test — "is this FieldSymbol a captured
// `owned` primary-ctor param?" — moves here, exactly the shape this core
// already holds for every other classification in this file (including its
// OWN prior copy of this same arm, two paragraphs below, which was already
// living in `common` unnamed before this fix gave it a name).  So naming and
// sharing it does not newly cross the boundary: the boundary comment above
// (see the #3634 bullet) is amended to say so explicitly, so a future
// predicate can be checked against the same worked example rather than
// re-litigating the question.
//
// `capturedOwnedPrimaryParamType` (codegen, LlvmBackend_impl.hpp) is
// DELIBERATELY NOT moved alongside it: it calls `ctx_.resolvedType(...)`, an
// Emitter/SemaContext operation with no argument-only equivalent here, so it
// stays exactly where the #3205 comment already puts it.
//
// Cycle-guarded (mirrors the #1624 precedent) so a malformed base-type graph
// that survives error recovery cannot hang the compiler.
//
// fix #3191: the parameter is `const` because the exporter
// (`ModuleMetadataBuilder::convertTypeDecl`) must compute this verdict from a
// `const sema::TypeSymbol&` to put it on the wire. The body was already a pure
// read of its argument — the paragraph above says so — so this only makes the
// existing contract expressible in the type. Every pre-existing caller passes a
// non-const `TypeSymbol*`, which converts implicitly.
//
// fix #3588 (§28.9 / §21.9 / §3.6, v0.286): the carried-verdict knob, and the
// ONE substantive difference among the copies this predicate absorbed.
//
//   Consult (the default; what every caller but one wants) — an IMPORTED
//     class's carried §3.6 verdict (`TypeSymbol::importedNonTrivialDispose`,
//     put on the wire by #3191) disqualifies, checked per level.  This is the
//     answer §2.8.2 requires: the property is part of the public contract, and
//     "an interface that omitted them would force the consumer to either
//     reject imported types at those rules or guess."
//
//   Ignore — the carried verdict is NOT read; only the structural member scan
//     decides.  Exactly one caller asks for this: §21.9's bare-discard check,
//     on its CALL arm only.  `Cell.MakeIt(i);` over an imported non-trivial-
//     dispose class is reclaimed through `disposeOwnedValue` per #3189's
//     design intent, so firing §21.9's hard error there aborts a build that
//     must run — a regression measurable on
//     `xmod_owned_return_channel_3189.t2_imported_bare_discard_reclaimed_21_9`
//     (imported count reads 7, expected 0).  Its NEW arm passes `Consult` and
//     keeps #3191's rejection.  See fix #3191's follow-up (0bb85f1) for the
//     full argument; the call site carries it too.
//
// `Consult` is the DEFAULT deliberately: it is the MORE-REJECTING answer, and
// the two wrong answers are not symmetric.  A wrong "non-trivial" runs a
// `Dispose()` that is provably a no-op; a wrong "trivial" leaks (or, at the
// §21.9 site, silently drops a resource the single-module twin rejects).  A
// caller that does not think about this knob therefore gets the safe
// direction.
enum class ImportedDisposeVerdict { Consult, Ignore };

bool isNonTrivialDisposeClass(
    const sema::TypeSymbol* decl,
    ImportedDisposeVerdict importedVerdict = ImportedDisposeVerdict::Consult);

// fix #3632 (§28.9 v0.286): the §28.9 "no `owned` fields OF NON-TRIVIAL-DISPOSE
// TYPE" recursive clause, which the predicate above cannot implement because
// reading a field's RESOLVED TypeRef (its post-substitution genericArgs
// included, §3.6 rule 2) requires a `SemaContext`. This overload takes the
// resolver hook the existing API cannot, walks each non-static `owned` field
// of the holder, resolves the field's TypeRef through `ctx`, and recurses
// into the field type — so an `owned inner: Inner?` field whose `Inner`
// itself classifies trivial-dispose (no destructor, no non-trivial-dispose
// `owned` field, no owning-inline-array, trivial-dispose base) does NOT
// disqualify the holder, exactly as §28.9's first ceremony-exemption bullet
// requires.
//
// WHY THIS IS AN OVERLOAD, NOT A REPLACEMENT. Five call sites use the
// `TypeSymbol`-taking form above (§21.9's `ExpressionStmt` arm, §15.4's
// borrowing-family literal gate, §28.10's vacuous-pin lint, the §28.3
// auto-drop warning gate, and the §22.15 throw carve-out — the exporter
// `ModuleMetadataBuilder::convertTypeDecl` is a sixth). Three of those
// live in files this lane does not own (`ExprTypecheck*.cpp` is L1,
// `Ownership*.cpp` is W2-L2, `ModuleMetadataBuilder.cpp` is shared);
// threading a `SemaContext*` through every call site is a coordinated
// second step that this PR deliberately does NOT make. The widening's
// SAFE direction keeps the overload's pre-fix semantics — any non-static
// `owned` field disqualifies when the resolver hook is missing — because
// the rejected direction is a deterministic "free a value the spec says
// is null-obligation-bearing". Once a call site opts in by passing
// `ctx`, the new arm checks the field's TYPE.
//
// PER-INSTANTIATION (§3.6 rule 2). `OwningBox<T>` qualifies per the
// §28.9 example as trivial-dispose only when the substituted T is
// trivial-dispose; the uninstantiated `T` (a bare type-parameter Named
// TypeRef with no `declaration`, not a user record's auto-field) is read
// post-substitution through `rt->genericArgs`, matching the §9.7 / #2145
// `RecordSubstEnv` discipline and #3145's both-sources rule. A bare
// parameter that does not resolve through `genericArgs` keeps the
// conservative reading — uninstantiated-still-generic stays rejecting,
// because per-instantiation reclassification at a use site can only
// RELAX it.
//
// COINDUCTION. The walk carries an in-progress path set of `TypeSymbol*`
// through both the BASE chain and the FIELD-type recursion; re-entry of
// a TypeSymbol already on the path answers "trivially trivial-dispose"
// (return false) — which is the coinductive fixpoint. The pre-#3632 cycle
// guard on the BASE chain alone (line 50) happens to coincide because
// `break` with `reject == false` and a return-false are the same
// outcome; the §28.9 spec text says explicitly "the property is
// evaluated coinductively", so the new entry point reads as written.
//
// WIDENING SCOPE. Only `isOwned && !isStatic` is widened to
// `isNonTrivialDisposeClass(fieldType)`. The destructor arm and the
// inline-array arm are spec-stated disqualifications and stay as-is; an
// `owned` field whose declared type is itself an owning-inline-array
// (`[N owned]T`) still disqualifies, because the field storage has its
// own disposal work (the `OwningBox<[3 owned]Cell>` shape).
bool isNonTrivialDisposeClass(
    const sema::TypeRef* rt,
    const sema::SemaContext& ctx,
    ImportedDisposeVerdict importedVerdict = ImportedDisposeVerdict::Consult);

// fix #3634 (§22.10 v0.180 / #3205, v0.286): is `fs` a captured `owned`
// CLASS primary-constructor parameter — one whose value is NOT consumed by
// an owned-field initializer or the inheritance clause's base call
// (`!posParamInitRef`), and therefore flows into a compiler-generated
// `owned` capture field that the generated `Dispose()` (§28.4) must walk?
// See the isNonTrivialDisposeClass comment above ("fix #3634") for the
// fact/mechanism argument that settles why this lives here rather than
// staying codegen-only, and `Emitter::isCapturedOwnedPrimaryParam`
// (LlvmBackend_impl.hpp) — now a one-line delegating wrapper, the #2654
// pattern — for the codegen consumer and #3205's leak history.
bool isCapturedOwnedPrimaryParam(const sema::FieldSymbol* fs);

// Shared shape-recognition: if the callee of `call` resolves to a user
// `ast::MethodDecl` (a MethodSymbol whose `decl` is a MethodDecl), return it;
// null otherwise (unresolved overload, a builtin with no MethodSymbol, or a
// call through a value/function).
//
// ⚠ THIS PREDICATE'S SUBJECT IS DECL PRESENCE, NOT THE `owned` RETURN.
// Its null is load-bearing in two CONTRADICTORY directions, which is why
// #3189 did not "fix" it: at `Ownership_Checks.cpp:9665`, `:10162`, `:10203`
// and `:10357` a null means "there is no user method here, so run the BUILTIN
// intrinsic rule" (`Select` / `Min` / `Max` / the stdlib string-mint set).
//
// ⚠ THE DIRECTION HERE WAS STATED BACKWARDS IN THE FIRST DRAFT, and the error
// matters because this header is what the next lane reads when deciding whether
// to fix #3192. It said "widening it would silently reclassify a user method as
// an intrinsic". Measured, the opposite is true — the CURRENT narrow behaviour
// is the reclassifier:
//
//     imported user  Q.Select(lambda(x: int) => new Res())
//       -> REJECTED: "lazy `Select` selector ... launders"
//     character-identical single-module twin        -> BUILDS
//
// An imported user method named `Select` is ALREADY being treated as the stdlib
// intrinsic; that is #3192. So the decision not to unify this predicate stands
// on its OTHER leg — the four gates read decl PRESENCE, and routing the
// `owned`-return marker through a presence test is a category error — not on a
// reclassification risk that runs the other way. As written it argued against
// #3192's fix on a false premise. Sites
// that want the `owned`-return FACT must call `calleeReturnsOwned` below,
// which consults both sources; sites that genuinely need the AST node (a body
// scan, a generic-parameter list, a modifier list) keep calling this one.
const ast::MethodDecl* calleeMethodDecl(const ast::CallExpr& call,
                                        const sema::SemaContext& ctx);

// §28.1 / §28.3 "Producers" / §2.5 (v0.254, #3189 — family M2 of umbrella
// #3152): does the callee of `call` declare an `owned` return? THE shared
// navigation to the `returnOwned` authority, replacing the
// `calleeMethodDecl(...)->returnOwned` idiom at every site whose subject is
// the marker rather than the AST node. Both sources are consulted through
// `sema::methodReturnsOwned` (see sema/Symbol.hpp), so an imported callee —
// whose `MemberSymbol::decl` is null for every non-generic imported type —
// answers with the exporter's declared marker instead of a silent `false`.
//
// False for a null callee, a callee with no `MethodSymbol` (a builtin, or a
// call through a value / function), and any method whose return is unmarked.
bool calleeReturnsOwned(const ast::CallExpr& call,
                        const sema::SemaContext& ctx);

// EXPRESSION-SHAPE producer classification (#1039 / #1053 — the weakest of the
// four historic copies): a literal `new T(...)` or a call to a method declared
// to return `owned T`, whose STATIC result type is a heap CLASS.  A variable /
// field / `this` / property receiver is neither, so it is never a fresh
// producer; a null / value-type / value-builtin result is excluded.
//
// `resolveParam` is the documented shared resolver hook; the weakest-copy shape
// intentionally does NOT resolve the receiver type-parameter (a `new T()` in a
// monomorphized body is conservatively NOT a fresh producer here — leak-but-
// sound), so behaviour matches the pre-refactor codegen predicate exactly.
bool isFreshOwnedClassProducer(const ast::Expr* e,
                               const sema::SemaContext& ctx,
                               const TypeParamResolver& resolveParam);

// §28.3 K3.4 (v0.209, #2080; extended #4006 for the `move` spelling; #4016):
// true when `e` is a BARE `move <identifier>` — the spec's CURRENT canonical
// K3.4 marked-arm-transfer spelling (§28.3 consume table row K3.4; §7.7's own
// coalescing example `var r := (if a <> null then move a else move b);`).
// Matches ONLY the arm's own, unwrapped value: a `move` nested inside a
// larger arm expression (`new Holder(move a)`) or a non-identifier operand
// (`move a.f`, `move a!`) does not qualify — mirrors `MarkedTransferExpr`'s
// own bare-name-only grammar (`"owned" Identifier`, §A.7).
//
// The SHAPE half of the K3.4 marked-transfer recognition shared by the
// ownership pass (`MethodOwnership::isBareMoveArmValue`, which delegates
// here) and `conditionalCaseArmMeetIsOwned` below. #4016: before this, the
// ownership pass's own arm-transfer machinery (`processMarkedArmConstruct`)
// recognized a bare `move x` arm, but this file's independent arm-meet
// classification did not — so `(if useFresh then new Res(1) else move
// cached)` (a `move`-marked arm MIXED with a fresh-producer sibling) was
// classified a BORROW meet here while the ownership pass correctly treated
// `cached` as transferred, and codegen's `unwindOwnedLocalClass` /
// `usingVarOwnsClassObjectDynamically` (both funnel through
// `conditionalCaseArmMeetIsOwned`) then found no disposal obligation for the
// binding — an explicit `x.Dispose()` fell to the dispatch-only fallback and
// its block leaked. A SYMMETRIC `move`+`move` construct never exercised this
// path in the same way: `dischargedByArmTransfer`'s own per-path scope-end
// dispose covers the losing side there, masking the gap this file has.
bool isBareMoveArmValue(const ast::Expr& e);

// §28.3 arm-meet (v0.184, #1661): true when `e` is an if-expression
// (`v1 if c else v2`) or an expression-position `case`/`when` whose ownership
// meets to OWNED — i.e. EVERY value-producing arm is itself a fresh owned
// producer (isFreshOwnedClassProducer, a MARKED transfer — `owned x`
// (MarkedTransferExpr) or the bare `move x` spelling (isBareMoveArmValue,
// #4016) — or a nested owned-meet if/case) and at least one such arm exists.
// A `throw` arm (bottom-typed) and a `null`-literal arm are ABSORBING (they
// impose no ownership and are skipped); a borrow / non-fresh arm
// DISQUALIFIES the whole expression (the meet is then a borrow — `false`
// here). A `case`/`when` with any BLOCK-body arm (`do … end`) is
// conservatively NOT owned-meet (its value expression is not structurally
// inspectable here) — leak-but-sound, never a wrong dispose.
// The single home both the ownership pass (producerState) and codegen
// (usingVarOwnsClassObjectDynamically / unwindOwnedLocalClass) consult, so the
// two never diverge on whether such an initializer carries a disposal
// obligation. Exactly one arm runs at runtime and yields a +1 the caller owns,
// so disposing the bound value once is leak-free and double-free-free.
bool conditionalCaseArmMeetIsOwned(const ast::Expr& e,
                                   const sema::SemaContext& ctx,
                                   const TypeParamResolver& resolveParam);

// EXPRESSION-SHAPE transfer classification (#1349 / #642 / §4.5–§4.11 / §2.14 /
// §29.1): builtin methods that TRANSFER an owned class element to the caller.
// These have no MethodSymbol, so recognition keys on the callee name plus — for
// the names with a borrowing counterpart — an OWNING receiver type.
bool isBuiltinOwnedTransferCall(const ast::CallExpr& call,
                                const sema::SemaContext& ctx);

// §28.10 / §28.11 / stdlib §4.5 (v0.195) (#2146): a builtin owning-collection
// `recv.Drain()` call whose receiver is a STABLE NAMED source. Stdlib §4.5:
// "`Drain()` on `OwningArray<T>` returns the array itself at that static type
// — no separate enumerable object" — the drain view ALIASES the receiver, so
// no fresh frame exists for the §28.3 iterator-factory convention to hand
// over; the receiver's owner keeps the ONE container obligation (§28.11's
// "ownership out" row transfers ELEMENTS, never the container). Binding the
// view therefore takes NO disposal obligation (§28.10's borrow-call row); a
// registered second disposal double-frees the collection (the #2146 SEGV).
//
// Positive recognition only: the callee must be the BUILTIN Drain (no
// resolved MethodSymbol — a user method named `Drain` is not this shape), the
// receiver's alias-resolved type head an owning-collection, and the receiver
// a stable named place (an identifier bound to a local/parameter/field, a
// field member-access, or an index slot). A DIRECT temporary receiver
// (`MakeArr().Drain()`) is NOT recognized — the temporary's own obligation
// story is separate (#2169) — and keeps its current classification. The
// stability gate is on the receiver FORM only, not its chain root: a
// CHAIN-ROOTED temporary (`Make().Items.Drain()`, `Make()[0].Drain()`) IS
// recognized — the alias fact holds regardless (the view still aliases the
// slot/field, and pinning registers no second disposal; pre-#2146 those
// shapes SEGV'd on the iterator-format dispose), while the root temporary's
// undischarged obligation remains #2169's (today it leaks, LSan-visible).
// Consumed by BOTH
// the ownership pass (producerState / the §28.10 using-var pin) and codegen
// (the iterable-source disposal registrations), so the two can never diverge
// on the alias fact.
bool isOwningCollectionDrainAliasCall(const ast::CallExpr& call,
                                      const sema::SemaContext& ctx);

// §21.1 (v0.201) / stdlib §5, §4.4, §9.6 (fix #2709): a BUILTIN COLLECTION
// MATERIALIZER call — `recv.ToArray()`, `.ToSet()`, `.ToDictionary(…)`,
// `.Slice(r)`, `Encoding.GetBytes(s)`,
// `.Sorted(…)`, `.Reversed()`, `s.Split(…)`. Every one is declared
// `owned`-returning by the stdlib ("`ToArray()` returns a fresh `Array<T>` …
// The return is `owned` — the caller is responsible for disposing it", stdlib
// §5; likewise §4.4 for `Sorted`/`Reversed` and §9.6 for `Split`), so §21.1's
// const rejection names them: "Rejected […]: a fresh `new`, an
// `owned`-returning call […]".
//
// WHY THIS IS NOT `producerState`. These are compiler-known members with no
// user `MethodDecl`, so `producerState`'s `returnOwned` authority never sees
// them and they fall through to `Borrowed`. That verdict is deliberate and
// LOAD-BEARING — `producerState` is the free-deciding input at
// `handleLocalDecl` (Ownership.cpp), and codegen already reclaims these values
// on its own recognition for both bindable spellings (a plain `var` via
// `isFreshBorrowingArrayMaterializerLocal`, a `using var` via
// `registerUsingDisposal`). Reclassifying them `Owned` would register a SECOND
// free on top of codegen's — the double-free direction. This predicate is
// therefore a pure DIAGNOSTIC-side second opinion, consulted only where §21.1
// asks "is this initializer an owned production?"; it changes no disposal.
//
// The gap it closes: a `const` local is neither a `LocalDecl` (so codegen's
// plain-`var` materializer reclaim never sees it) nor a `using var`, so NOTHING
// frees it — measured at 100-of-100 leaked objects per materializer under LSan.
// §21.1 requires the rejection; the rejection removes the leak by removing the
// program.
//
// BOTH INSTANCE-CALL SPELLINGS (fix #2749 item 3). The callee may be a
// `MemberAccessExpr` (`recv.M(..)`) or a `NullCondMemberExpr` (`recv?.M(..)`) —
// sibling `Expr` kinds with identical `target`/`member` fields, so the original
// `dynamic_cast<MemberAccessExpr*>`-only gate missed every `?.` spelling and
// `const b := s?.Split(",");` was accepted. This is sema's own #2581 precedent
// (`ExprTC::builtinInstanceCallee`), applied here. `?.` does not change the
// classification: §21.1 rejects "an `owned`-returning call", `Split` is declared
// `owned`-returning, and the null path merely adds §28.3's `null` row, which
// "has no value to own, so it is compatible with either an owned target or a
// borrowed target" — it cannot demote the non-null path's owned production.
// The `?(` invocation form (`ast::NullCondCallExpr`) is deliberately NOT
// covered: it invokes a FUNCTION VALUE, which §28.3 classifies as Borrowed and
// §21.1 admits.
//
// ⚠⚠ THIS WIDENING IS INERT TODAY, AND THAT IS THE POINT — do not delete it as
// dead code. MEASURED on 6cd9b70 with temporary instrumentation in the body
// below: with the widening in place the predicate IS now reached for
// `a?.Sorted()`, `a?.Reversed()`, `a?.Slice(0..1)` and `t?.Split(",")`, with no
// symbol on the callee (so the narrowness gate passes them) — but every one of
// them arrives with `ctx.exprType(call) == nullptr`, because SEMA NEVER TYPES A
// BUILTIN INSTANCE CALL REACHED THROUGH `?.` at all. That is the same defect
// behind their #2434 codegen degradation, and it is filed separately as #2756.
// The result-head gate below then fails OPEN, so no rejection is emitted and no
// program's diagnostics change.
//
// It is shipped anyway, deliberately, for a soundness reason rather than a
// stylistic one: today `const b := s?.Split(",");` is not a leak (the value
// never lowers — the local degrades to zero-initialized), but the moment #2756
// lands the lowering, it becomes a REAL accept-and-leak of exactly #2709's
// shape. A lane whose scope is "make `?.` builtin calls lower" would have no
// reason to also add the §21.1 rejection. Pre-placing the guard means that fix
// arrives already covered instead of re-opening the defect. The tripwire test
// `const_nullcond_materializer_2749.residual_*_pending_2756` pins the current
// state and is MEANT to go red when #2756 lands; the correct response is to flip
// it into a rejection assertion.
//
// NARROWNESS (a false reject here breaks working code). Recognized only when
// sema attached NO SYMBOL AT ALL to the callee — `ctx.exprSymbol(callee) ==
// nullptr`. The result type must additionally be a declaration-less builtin
// `Array` / `Set` / `Dictionary` head. A USER method named `ToArray` keeps the
// ordinary user-call rules; a borrow-returning one stays admissible as a
// `const` initializer, which §21.1 explicitly permits.
//
// ⚠ That narrowness had to be RE-ESTABLISHED for `?.`, not assumed, because the
// null-conditional form is typed by a separate function with its own resolution
// mirror. `ExprTC::resolveNullCondMember` calls `setExprSymbol(&nm, member)`
// unconditionally once `lookupMember`/`lookupExtensionMember` resolved one, and
// returns early attaching NOTHING when neither did — the builtin case. So a user
// method reached as `obj?.Sorted()` attaches a MethodSymbol and a function-typed
// field reached as `cfg?.Split(line)` attaches a FieldSymbol, and the single
// `exprSymbol == nullptr` line excludes both, exactly as it does for their `.`
// spellings.
//
// ⚠ DO NOT "restore consistency" with `isOwningCollectionDrainAliasCall`'s
// `dynamic_cast<sema::MethodSymbol*>` gate. That is what this predicate shipped
// first, and it FALSELY REJECTED a legal program: a call through a
// FUNCTION-TYPED FIELD named one of these (e.g. `public Split: (string) ->
// Array<string>;`) attaches a FieldSymbol, not a MethodSymbol, so the
// MethodSymbol-only cast let it through — and §28.3 classifies a function-value
// invocation with a non-`owned` concrete return as BORROWED, which §21.1
// admits. `isOwningCollectionDrainAliasCall` has the same hole latently; it is
// unfixed there only because `Drain` is a less plausible field name.
//
// The `exprSymbol == nullptr` form is sound because sema's builtin member-call
// typing path lives entirely inside `if (!calleeSym)`
// (sema/ExprTypecheck_Calls.cpp:2873, block extent 2873-8103), and every site
// that attaches a symbol to a MemberAccessExpr requires a RESOLVED declaration.
// ⚠ That is also what keeps the CROSS-MODULE case safe by one line:
// ModuleLoader reconstructs a real MethodSymbol into the imported type's
// members, so an imported `Sorted(): Array<int>` — whose importer-side TypeRef
// is byte-identical to the builtin shape this predicate accepts — is excluded
// by the symbol check alone. There is no test pinning that; add one if you
// touch this.
//
// Deliberately NOT unified with codegen's same-named recognizers
// (`isFreshBorrowingArrayMaterializerLocal` / `isFreshOwnerlessDictMaterializer
// Expr`, LlvmBackend_Setup.cpp): those answer "must codegen free this
// temporary?" and exclude `ToSet` on purpose (its temporary is reclaimed on
// another path — a second free would double-free). This answers the different
// question "does the stdlib declare this return `owned`?", for which `ToSet`
// belongs. Sharing one list would fuse two questions with different answers.
bool isBuiltinOwnedMaterializerCall(const ast::CallExpr& call,
                                    const sema::SemaContext& ctx);

// §28.3 producers table, "Range slice" row (v0.240) / §15.6 (fix #2749 item 2):
// the RANGE-SLICE INDEXER form `arr[r]` on a builtin `Array<T>`. The spec row is
// verbatim:
//
//   | Range slice `arr[r]` on `Array<T>` (§15.6) | Owned — a fresh `Array<T>`,
//   | the compiler-recognized `Slice(r: Range<int>): owned Array<T>` form
//   | (stdlib §4.4); **not** the indexer row above. |
//
// So the spec treats `arr[0..2]` and `arr.Slice(0..2)` as ONE operation, and
// §21.1's "Rejected […]: a fresh `new`, an `owned`-returning call" therefore
// covers both spellings identically.
//
// WHY IT NEEDS ITS OWN PREDICATE. #2709's `isBuiltinOwnedMaterializerCall` is
// keyed on an `ast::CallExpr`; `arr[0..2]` is an `ast::IndexExpr` and cannot
// reach it, and `producerState` has no `IndexExpr` arm either. The consequence
// was a LIVE REMEDY LOOP, measured on 0b9693e: `using var s := arr[0..2];` drew
// the #2073 vacuous lint ("use `const x := …;`") and `const s := arr[0..2];` was
// then ACCEPTED — #2709's exact defect, reachable through the more idiomatic
// spelling, and an inconsistency #2709 itself introduced by fixing only
// `.Slice(r)`.
//
// WHAT IT RECOGNIZES, and why that cannot over-fire. It mirrors the ONE sema arm
// that produces this classification (`ExprTypecheck_Exprs.cpp`, the
// `if (isRangeIdx && cn == "Array") ctx_.setExprType(&e, tt);` line) on all
// three of its conditions, AND cross-checks sema's own verdict by pointer:
//   (a) the RECEIVER types as a declaration-less builtin `Array` head with a
//       non-empty generic-argument list — which excludes `OwningArray<T>` (where
//       §15.6 slicing is rejected outright), the span families (`Span`,
//       `ReadOnlySpan`, `ArraySpan`, `ReadOnlyArraySpan` — sema types a range
//       index on those as the ELEMENT, and their stdlib `Slice` overloads return
//       span heads, not owned arrays), `string`, inline arrays `[N]T`, the
//       dictionaries, and every USER type (whose TypeRef carries a
//       `declaration`, including a user class that shadows the name `Array`);
//   (b) the first index is a range — a syntactic `RangeExpr` (`arr[0..2]`) or a
//       `Range`-typed value (`var r := 0..2; arr[r]`, #1079), the same two forms
//       sema accepts — which leaves the single-element read `arr[i]` alone; it
//       is §28.3's *indexer* row, a Borrow, which §21.1 admits verbatim;
//   (c) sema in fact typed the whole index expression as the receiver's OWN
//       TypeRef object. ⚠ NARROWLY: within the receiver family (a) selects —
//       Named, declaration-less, head `Array` — that arm is the only one storing
//       `tt` itself; every other arm there stores `tt->genericArgs[0]` or `[1]`,
//       which cannot alias `tt`. It is NOT unique across ALL IndexExpr arms: an
//       earlier draft claimed that, and it is FALSE, because
//       `SemaContext::primitiveTypeRef` is an INTERN TABLE (SemaContext.hpp
//       :655-666, primitiveCache_ at :569) — so the STRING range-slice arm
//       (ExprTypecheck_Exprs.cpp:1491-1494) also stores a pointer frequently
//       identical to its receiver's exprType. That collision is harmless only
//       because (a) rejects a `string` receiver as Kind::Primitive before this
//       check runs, and it is DATA-DEPENDENT (an explicitly annotated
//       `var s: string` mints a fresh TypeRef and does NOT collide). Anyone
//       relaxing (a) — to cover `string` slices, or to widen the head test —
//       must not inherit the false invariant.
//
// All three are AND-ed and every one of them FAILS OPEN: a shape this cannot
// type, or one sema types differently than the mirror expects, is ACCEPTED, not
// rejected. That is the deliberate polarity #2709 chose for its own null-
// `exprType` gate — a missing rejection is a leak already filed, while a false
// reject breaks a legal program.
//
// DIAGNOSTIC-ONLY, exactly like `isBuiltinOwnedMaterializerCall`: it is reached
// only through `MethodOwnership::constInitializerRejected`, changes no disposal,
// retain, or dataflow state, and the code generated for every program that still
// compiles is what it was.
//
// ⚠ THE `arr[r]` SLICE ALSO LEAKS UNDER THE REMEDIES, and that is a SEPARATE,
// larger defect this predicate does not touch — filed as #2753. Measured on
// 0b9693e (64-iteration amplified probe, LSan, values pinned through exit
// codes): `const`, `using var` AND plain `var` bindings of `arr[0..2]` each
// leaked 64 of 64 `bz_array_slice` allocations, while the equivalent
// `arr.Slice(0..2)` under `using var` ran clean. So codegen reclaims the CALL
// spelling and reclaims nothing for the INDEX spelling. This predicate is the
// §21.1 half only: it removes the `const` binding (which has no free path at
// all, by construction) and stops the lint from advertising it. It does NOT make
// `using var arr[r]` reclaim — do not "finish the job" here by reclassifying,
// which is the double-free direction #2709 declined for enumerated reasons.
bool isBuiltinArrayRangeSliceIndex(const ast::IndexExpr& ix,
                                   const sema::SemaContext& ctx);

// §21.1 (v0.286) / stdlib §2.11 / §6.5 (fix #3694): a `.Subscribe(handler)`
// member call on an `IEvent<T>` / `IAsyncEvent<T>` / `ISharedEvent<T>` face, on
// the producer-side `EventSource<T>` / `AsyncEventSource<T>` /
// `SharedEventSource<T>` sealed classes, or on a direct event member access
// (carrying an `EventSymbol`, §24.1). Every one of those returns
// `owned Subscription` (stdlib §2.11), so by §21.1's text it MUST be rejected
// as a `const` initializer — but the call is to a COMPILER-KNOWN MEMBER with
// no user `MethodDecl`, so `producerState`'s `md->returnOwned` arm (the only
// call-side Owned authority) cannot see it. Without this disjunct the §21.1
// rejection falls through (this is the exact #2709 shape recurring for the
// event-family, documented in #3694) and a `const sub := face.Subscribe(...)`
// compiles and LEAKS — measured 16000 B / 500 iterations, one `Subscription`
// handle each.
//
// WHAT IT RECOGNIZES, and why it cannot over-fire. Mirrors codegen's
// `Emitter::isSubscribeCall` structural test (LlvmBackend_Calls2.cpp:3847) on
// all three of its clauses, AND widens it with the producer-side sealed
// classes (codegen's predicate is exact-only — codegen's `isIEventTypeName`
// gate is the one copy of the face check; the producer-side sealed class is
// also a `Subscription` producer, but codegen reaches it through sema's
// primary `isSubscriptionType` typing of `EventSource.Subscribe(...)` rather
// than through this structural fallback). The four clauses AND'd:
//   (a) the callee is `recv.M(...)` or `recv?.M(...)` (the same
//       `builtinInstanceCallee` helper the two preceding predicates use, so the
//       three cannot drift on the `.` / `?.` widening #2581 / #2749 item 3
//       closed);
//   (b) the member name is `Subscribe` (one arg only — `Subscribe(handler)`
//       per stdlib §6.5; an `AsyncSubscribe` does not exist);
//   (c) the receiver is one of the six builtin event-family types listed
//       above (Named / declaration-less TypeRef whose head name matches) OR a
//       direct event member access whose target exprSymbol is an `EventSymbol`.
//       The declaration-less gate is what excludes a user class whose
//       `Subscribe` member happens to share the name: a user class carries a
//       declaration and `eventSymbolOf` is the only path that lets one
//       through (an event member on `Bell` accesses the symbol, a `Subscribe`
//       call on a user-typed receiver does not).
//
// A USER method named `Subscribe` that returns `owned Subscription` (per its
// own `MethodDecl::returnOwned`) is ALREADY handled by the existing first
// disjunct of `constInitializerRejected` (producerState → Owned via
// `md->returnOwned`), and this predicate stays out of its way: the user
// `MethodDecl` carries a symbol that reaches the user-method path, not the
// builtin path. The codegen-side structural test has the same shape and the
// same narrowness.
//
// DIAGNOSTIC-ONLY, exactly like `isBuiltinOwnedMaterializerCall` and
// `isBuiltinArrayRangeSliceIndex`: it is reached only through
// `MethodOwnership::constInitializerRejected`, changes no disposal, retain, or
// dataflow state, and the code generated for every program that still
// compiles is what it was. // per §21.1 v0.286, stdlib §2.11 / §6.5, #3694
bool isEventSubscribeCall(const ast::CallExpr& call,
                          const sema::SemaContext& ctx);

// §28.3 "Factory methods and sourceless returns" / Appendix C.3 (fix #1499): a
// STATIC method returning a plain class/interface type with NO `owned`
// annotation has no STRUCTURAL borrow source — a static call has no receiver —
// so the CALLER must discharge its result (bind with `using var`, store in an
// `owned` field, pass to an `owned` parameter, or dispose) exactly as for
// `new`. This predicate is true ONLY when the callee genuinely PRODUCES a fresh
// owned value: it fires when `md` is `static`, its resolved result `resultTy`
// is a plain class/interface (NOT a value-builtin, NOT an owning-collection/
// -wrapper head — those carry the more specific §28.6 documented-source rule),
// AND EVERY return path of `md`'s body yields a fresh owned producer (`new
// T(...)` or an `owned`-returning call — via isFreshOwnedClassProducer).
//
// A static method that returns a BORROW on ANY path is DELIBERATELY excluded:
// a static/instance FIELD read (`_shared` — process-lived source, §28.3
// producers "Static property access ... Borrowed"), a parameter, `this`, or a
// plain local alias. Classifying such a borrow-return as owned would make
// `using var r := Get()` DISPOSE a value the callee does not own — a
// double-free of a shared/borrowed object (§28.10) — the exact soundness
// regression this predicate must avoid (the caller-side owned-assumption is
// sound only opposite the callee-side borrow-return prohibition, §28.3
// "Factory methods" non-normative note / borrow-lifetime rules; #1499).
//
// Callers must have already established `md.returnOwned == false` (an explicit
// `owned` return is owned directly, without this rule). `resolveParam` is the
// shared type-parameter resolver hook, threaded into isFreshOwnedClassProducer.
bool staticFactoryReturnsFreshOwned(const ast::MethodDecl& md,
                                    const sema::TypeRef* resultTy,
                                    const sema::SemaContext& ctx,
                                    const TypeParamResolver& resolveParam);

// §30.1 / §9.7 (v0.179, #1438; re-applied post-substitution at a generic
// method's INSTANTIATION site, #1595): true when `rt` is a borrow-bearing
// record — a user record (§9.7) carrying at least one borrow-carrying field
// (a class/interface reference field, an inline array of reference
// elements, or, transitively, another borrow-bearing record) — OR a builtin
// span VIEW over an `Array`/`OwningBuffer` (`Span`/`ReadOnlySpan`, stdlib
// §4.14; `ArraySpan`/`ReadOnlyArraySpan`, stdlib §4.15), which carries a
// borrow of its backing reference-typed store but has no user Record
// TypeDecl. `CancellationToken` (§30.8, the single stdlib-certified
// exception) and the string-backed spans (`ReadOnlyStringSpan` /
// `ReadOnlyStringSpan`, over the runtime-managed `string`) is correctly
// excluded.
//
// Two consumers need the IDENTICAL predicate: the async-signature
// DECLARATION-site check (`MethodOwnership::checkAsyncSignatureBorrowBearing`,
// ownership pass) sees only the unsubstituted method type parameter `T` and
// cannot reject an instantiation that later binds `T` to a borrow-bearing
// type; the INSTANTIATION-site re-check §30.1 requires ("for a generic
// async method the check is re-applied after substitution, so an
// instantiation whose type argument makes a parameter borrow-bearing is
// rejected at the instantiation site") runs at each call site
// (`ExprTypecheck_Calls.cpp`, #1595) against the SUBSTITUTED parameter/
// result type. Living here (rather than duplicated in each pass) keeps the
// two checks from drifting apart.
bool isBorrowBearingRecordOrSpan(const sema::TypeRef* rt,
                                 const sema::SemaContext& ctx);

// §9.7 / §30.1 (v0.220, #2145): the INSTANTIATION-SITE substitution
// environment for the borrow-bearing-record field walk. A generic record's
// field type resolves to the UNINSTANTIATED bare type parameter (`V: T` in
// `record Wrap<T>(V: T)` resolves to a declaration-less Named `T`), so the
// field walk alone cannot see that `Wrap<ArraySpan<int>>` / `Wrap<Res>`
// substitutes a borrow-carrying type there. Each record frame of the walk
// builds one environment from the record declaration's generic params and the
// visited TypeRef's `genericArgs`, mapping each param NAME to the substituted
// TypeRef *and* the environment in which THAT TypeRef's own bare-parameter
// mentions resolve (so `Outer<Res>` → `Inner<T>` → `V: U := T := Res`
// composes through nesting). An empty/absent environment reproduces the
// declaration-site behaviour exactly: a bare parameter classifies
// not-borrow-carrying, deferring to the §30.1 instantiation-site re-check.
// Both copies of the walk (common::isBorrowBearingRecordCommon and
// MethodOwnership::isBorrowBearingRecordImpl, #1595 lockstep) share this type.
struct RecordSubstEnv {
    struct Entry {
        const sema::TypeRef* ty = nullptr;   // the substituted type argument
        const RecordSubstEnv* env = nullptr; // env for ty's own bare params
    };
    // Lookup-only (never iterated), so the unordered map cannot introduce
    // #2116-class ordering nondeterminism.
    std::unordered_map<std::string_view, Entry> map;
};

// Seed `out` for one record frame of the field walk: each of `decl`'s generic
// params maps to the corresponding `rt->genericArgs` entry (paired with
// `incoming` as that argument's own resolution environment); an argument that
// is ITSELF a bare parameter of the enclosing frame is resolved through
// `incoming` immediately (one composition hop), so chains of generic records
// (`Outer<T>` wrapping `Inner<T>`) resolve to the outermost concrete argument.
// Leaves `out` empty for a non-generic record or an uninstantiated use (no
// genericArgs) — the declaration-site conservative behaviour. // per §9.7 #2145
// fix #3145 (§2.8.3 rule 4, v0.252): the symbol-side overload. Prefer it — it is
// correct for an imported type, where the AST overload has no decl to be given.
void seedRecordSubstEnv(const sema::TypeSymbol& ts, const sema::TypeRef& rt,
                        const RecordSubstEnv* incoming, RecordSubstEnv& out);
void seedRecordSubstEnv(const ast::TypeDecl& decl, const sema::TypeRef& rt,
                        const RecordSubstEnv* incoming, RecordSubstEnv& out);

// Resolve `ft` through `env` when it is a BARE type-parameter reference (a
// declaration-less, argument-less Named TypeRef whose head names an entry).
// Returns the substituted entry, or {ft, env} unchanged when no substitution
// applies (concrete type, no env, or an unknown head — a null-declaration
// builtin like `Array` is never a key, so it passes through). // per §9.7 #2145
RecordSubstEnv::Entry resolveRecordFieldSubst(const sema::TypeRef* ft,
                                              const RecordSubstEnv* env);

// §35.4 (v0.200, #2045): the three-way **choice classification** — a derived
// type property computed over ALL of the choice's cases' payload fields. The
// decision proceeds in spec order (owning > borrow-bearing > value-like):
//   1. **Owning** — any case has a payload field marked `owned` (admissible
//      per §28.1 on class/interface/bare-type-parameter), or a payload field
//      of a move-only value type per §15.3 (an owning inline array, or
//      transitively another owning choice).
//   2. **BorrowBearing** — any case has a plain (unmarked) borrow-carrying
//      payload field under §9.7's definition: a class/interface reference
//      (nullable or not), an inline array of reference-typed elements, a
//      borrow-bearing record, or transitively a borrow-bearing choice.
//   3. **ValueLike** — otherwise: every payload is a value, `string`, a
//      runtime-managed type, or (transitively) a value-like record or choice.
//      A value-like choice is an ordinary copyable value type.
//
// Returns `std::nullopt` when `rt` is not a user choice type (so a caller
// can gate on the kind in one test). Generic choices classify per-
// instantiation at the USE SITE (§35.11): the CONCRETE type arguments in
// `rt->genericArgs` are threaded into the borrow leg (a bare `T` payload is
// reclassified as its substituted type — `Opt<int>` is value-like,
// `Opt<ArraySpan<int>>` / `Opt<Res>` borrow-bearing), mirroring the §9.7
// record instantiation-site substitution (#2145). For an UNINSTANTIATED or
// still-generic choice (a payload whose substituted type is itself a bare
// parameter — no concrete argument available) this function takes the
// conservative most-restrictive reading of a bare type-parameter payload (a
// marked `owned T` payload counts as Owning; an unmarked `T` payload counts as
// BorrowBearing) — sound because per-instantiation reclassification at a use
// site can only RELAX it.
//
// `env` (#2183) is an OPTIONAL incoming substitution environment for `rt`'s own
// bare-parameter arguments, used when this choice is a field/component of an
// enclosing generic type whose parameters supply those arguments (a
// `record Holder<T>(K: Opt<T>)` classified at `Holder<int>` resolves `Opt<T>`'s
// `T` through the record's env). Null for a top-level use — the frame env is
// then seeded from `rt->genericArgs` alone.
//
// Recurses through value-typed components (record fields, inline-array
// elements, other choices' payloads) per §35.4. A cycle guard bounds the
// recursion through self- or mutually-referential payload types, including the
// record↔choice mutual recursion the §9.7 transitivity opens (#2159): an
// instance-field value cycle is already rejected by §35.1's no-self-
// containment rule; the guard mirrors isBorrowBearingRecordOrSpan's #1611
// discipline.
enum class ChoiceClassification {
    ValueLike,      // §35.4.3
    BorrowBearing,  // §35.4.2
    Owning,         // §35.4.1
};

std::optional<ChoiceClassification> classifyChoiceType(
    const sema::TypeRef* rt, const sema::SemaContext& ctx,
    const RecordSubstEnv* env = nullptr);

// per §15.3 (#3775, promoted from a TU-local predicate in ExprTypecheck.cpp
// that carried the same #2912 citation): is `t` a MOVE-ONLY value type? §15.3
// names exactly two kinds — the owning inline array (`[N owned]T`/`[N owned]T?`,
// §15.1.1) and the owning-classified choice (§35.4) — and "Move-only
// membership follows from *structure*, never from a declaration form", so this
// reads the same two structural facts `classifyChoiceType` and `arrayOwned`
// already compute rather than re-deciding either. `kindOut`, if non-null,
// receives the spec's own name for whichever kind matched ("an owning inline
// array" / "an owning-classified choice"), for use in a diagnostic. THE single
// authority for this decision: the §8.6 boxing rejection (ExprTypecheck.cpp)
// and the §28.11 collection-element-category instantiation check
// (TypeResolution_Names.cpp) both consult it, so they cannot give different
// answers for the same type.
bool isMoveOnlyValueType(const sema::TypeRef* t, const sema::SemaContext& ctx,
                         const char*& kindOut);

// §28.1 (v0.196) per-instantiation erasure of the `owned` payload marker, as
// §35.4 requires for a choice's classification: given a payload field's type
// resolved AT the choice's instantiation, is the `owned` marker MEANINGFUL —
// an owning heap type that participates in disposal — or ERASED (a value or
// runtime-managed type)? Owning (true): a class or interface reference, an
// owning inline array, a transitively-owning choice, or a STILL-generic bare
// type parameter (conservative — the uninstantiated template classifies
// owning). Erased (false): a primitive (incl. `string`/`Object`, runtime-
// managed), a record, an enum, a value-type builtin, and a value-like or
// borrow-bearing choice. A null type is conservative-owning (true). This is
// the single authority both the choice classifier and the owning-choice
// disposal codegen consult so `Result<int, string>` (both `owned` markers
// erased → value-like) and `Result<Connection, string>` (only the class
// payload disposes) agree.
bool ownedMarkerActiveOnInstantiatedType(const sema::TypeRef* ft,
                                         const sema::SemaContext& ctx);

// §28.1 / §35.4: resolve a choice case payload field's DECLARED type through a
// concrete instantiation `rt` (whose `genericArgs` line up with the choice's
// generic params). A bare type-parameter field type (`owned value: T`) maps to
// the matching `rt->genericArgs[i]`; a concrete or nested-generic field type is
// returned as resolved. Returns null when `pf` has no type. When `rt` carries no
// generic args (the uninstantiated template), the declared type is returned
// unchanged, so a template classifies conservatively.
//
// `decl` is the choice's `sema::TypeSymbol` (fix #3144, family F9 of #3152). It
// was the `ast::TypeDecl`, which every caller obtained by
// `dynamic_cast<const ast::TypeDecl*>(choiceTy->decl)` — null for every
// IMPORTED choice — so the substitution silently did not happen across a module
// boundary. The parameter NAMES this reads live on the TypeSymbol on both sides.
const sema::TypeRef* substitutedChoicePayloadType(
    const ast::Param& pf, const sema::TypeSymbol* decl, const sema::TypeRef* rt,
    const sema::SemaContext& ctx);

// §28.3 *Bare-type-parameter returns* (v0.194, #1801). True when `md`'s
// DECLARED return type — read *before* generic substitution, nullability
// stripped (`T?` qualifies) — is a **bare type parameter** of `md` itself: a
// non-`owned` Named return that resolves to a null-`declaration` TypeRef (the
// generic-param shape TypeResolution stamps, TypeResolution_Names.cpp) whose
// head name is one of `md`'s method-level `genericParams`. `owned T` returns
// are excluded (they are the ordinary ownership-transferring return, §28.8).
//
// This is the *method-level* half of the recognizer. A return that is a bare
// parameter of `md`'s ENCLOSING TYPE (`class Box<T> ... method Get(): T`)
// resolves to the same shape but its head is a type-level param, matched at the
// call site through the callee symbol's parent (the caller passes it) — the
// name-set membership test that distinguishes a bare type parameter from a
// 0-arity builtin, which shares the null-declaration Named shape.
bool declaredReturnIsBareTypeParam(const ast::MethodDecl& md,
                                   const sema::SemaContext& ctx);

// §28.3 (v0.194, #1801). The call's **potential source set** for a
// bare-type-parameter return: the receiver root (for an instance or extension
// callee — a static call has no value receiver), together with, read
// **positionally**, the identifier root of every argument bound to a **plain**
// (non-`owned`) parameter whose declared type **mentions** the returned type
// parameter — keeping **only** arguments whose resolved type is borrowable (a
// class/interface reference); value and runtime-managed arguments contribute no
// constraint and are dropped (so `Pick(1, 2)` yields the empty set). The
// returned roots are the source bindings whose consumption the union borrow of
// the result freezes while the result is live. Pre-substitution keyed.
std::vector<sema::Symbol*> bareTypeParamCallSourceRoots(
    const ast::CallExpr& ce, const ast::MethodDecl& md,
    const sema::SemaContext& ctx);

// §17.15 (v0.216, #2098): peel a binding atom `(var x := E; body)` to the
// expression that supplies its value, type, and ownership classification —
// its body, transitively through nested atoms (§28.3 producers row: "The
// body's classification, transparently — the atom is not a join"). Every
// initializer-form classification site (the ownership pass's producerState,
// codegen's unwindOwnedLocalClass / usingVarOwnsClassObjectDynamically
// family) peels through this one helper so the two passes cannot diverge on
// what a BindExpr-initialized binding's obligation is — the #1661 pattern.
// Null-safe; a non-atom expression is returned unchanged.
const ast::Expr* peelBindAtomToBody(const ast::Expr* e);

// §17.15 / §28.3 / K8.5 (v0.216, #2098): is `clause`'s initializer an OWNED
// production, making the clause binding a named unbound owned temporary
// bounded by the atom? The decidable shape set of this first landing:
//   * the marked binding transfer `var y := owned x;` (K3.4 — the binding
//     adopts the obligation);
//   * a `new T(...)` of a non-value kind (user class/interface and the
//     builtin heap types registerUsingDisposal knows how to dispose; value
//     records/enums/choices and the value builtins are excluded, mirroring
//     producerState's NewExpr arm);
//   * an `owned`-returning user call with a class result
//     (isFreshOwnedClassProducer) or a builtin owned-transfer call
//     (isBuiltinOwnedTransferCall);
//   * an owned-meet if/case expression (conditionalCaseArmMeetIsOwned);
//   * a nested binding atom, classified by its peeled body.
// Owned producer forms OUTSIDE this set (a Task-family mint, a `move`, a
// `??`-owned) are NOT claimed here — the ownership pass rejects such clause
// initializers with a targeted diagnostic instead of silently leaking
// (decline-with-reasons, #2098). Both the ownership pass (finishBindAtom)
// and codegen (lowerBindExpr) consult THIS predicate for the disposal
// decision, so an expression-bodied member — whose body the ownership
// dataflow does not run on — gets the identical atom-boundary disposal.
bool bindClauseIsOwnedProduction(const ast::LocalDecl& clause,
                                 const sema::SemaContext& ctx,
                                 const TypeParamResolver& resolveParam);

// §17.15 / K3 (v0.216, #2098): is clause `clauseIdx`'s binding CONSUMED by a
// K3 position spelled inside the atom (a later clause's initializer or the
// body)? The closed structural shape set — each a genuine K3 consuming
// position for a named owned binding, so a hit is a real discharge in every
// program the ownership pass accepts:
//   * a call/ctor argument `M(owned x)` / `new T(owned x)` naming it;
//   * an explicit `x.Dispose()` (incl. `x?.Dispose()` / `x!.Dispose()`);
//   * a `throw x` (expression position);
//   * a later clause's marked transfer `var y := owned x;`;
//   * `move x` (an owning-collection whole transfer).
// A consume spelled inside a nested LAMBDA body is deliberately NOT counted
// — the ownership pass rejects a closure consuming a captured owned binding
// (§28.3 v0.184, #1958; wired for atom clauses in processBindAtom), so it
// cannot occur in an accepted program. Shadowing is honored: a nested atom
// clause rebinding the same name ends the outer binding's visibility for
// the remainder of that nested atom, and a lambda parameter of the name
// shadows its body. The scan finds a consume ANYWHERE in the later
// clauses/body, including conditionally-evaluated arms — the ownership pass
// separately rejects a CONDITIONAL consume of an atom binding (its
// dataflow-vs-scan cross-check in finishBindAtom), so where this predicate
// governs an accepted program the consume is unconditional.
bool bindClauseConsumedInAtom(const ast::BindExpr& bx, std::size_t clauseIdx);

// §17.15 / K7.4 (#2080 F2): the STRAIGHT-LINE variant of the scan — true
// only when a K3 consume of clause `clauseIdx` is spelled on the atom's
// UNCONDITIONAL evaluation (if/case ARM positions are excluded; conditions,
// scrutinees, and guards evaluate unconditionally and are included).
// `bindClauseConsumedInAtom && !bindClauseConsumedUnconditionally` is the
// structural signature of a CONDITIONAL consume — the shape the first
// landing rejects — decidable without the dataflow, so the expression-body
// path (`=> E`, where the ownership dataflow never runs) enforces the same
// bound the block-body finishBindAtom cross-check does.
bool bindClauseConsumedUnconditionally(const ast::BindExpr& bx,
                                       std::size_t clauseIdx);

// §17.15 / K8.5 (v0.216, #2098): the one shared atom-boundary disposal
// decision — clause `clauseIdx` needs the disposal at the atom's completion
// iff its initializer is an owned production (bindClauseIsOwnedProduction)
// AND no K3 position inside the atom discharges it
// (bindClauseConsumedInAtom). Codegen's lowerBindExpr registers exactly the
// clauses this returns true for; the ownership pass's finishBindAtom
// cross-checks its dataflow against the same predicate and rejects on
// disagreement, so the two can never split into a leak or a double-free.
bool bindClauseNeedsAtomDisposal(const ast::BindExpr& bx,
                                 std::size_t clauseIdx,
                                 const sema::SemaContext& ctx,
                                 const TypeParamResolver& resolveParam);

// §29.8 / §30.1 (#973a): the ONE shared gate for the CONDITIONALLY-discharged
// `owned` parameter of a state-machine method (iterator, async iterator, plain
// `async`).
//
// The frame disposes an `owned` parameter "that has not been moved out of by
// the body" (§29.8) — a RUNTIME property when the move-out is conditional. The
// implementation encodes it as a runtime moved-from flag: the frame's capture
// slot is NULLED at the move-out site, and the frame's terminal-state disposal
// is null-guarded, so the discharged path skips and the fall-through path
// disposes exactly once. That is sound only if EVERY move-out of the parameter
// reaches a codegen site that nulls the FRAME slot — a missed site leaves the
// slot non-null after a real move and the frame double-frees (the #841 class).
//
// This predicate is that guarantee, stated syntactically and CONSERVATIVELY:
// true iff every occurrence of `param` in `body` is one of
//   (a) the bare-identifier initializer of a `LocalDecl` carrying the `owned`
//       binding-position transfer marker — `var y := owned p;` /
//       `using var y := owned p;` (§28.3 v0.180, #1962) — the ONE discharge
//       form whose lowering nulls the frame capture slot
//       (Emitter::nullStateMachineOwnedParamCapture, called beside the
//       existing #1978 `nullTransferredOwnedLocalSlot`); or
//   (b) the TARGET of a member access / member call (`p.F`, `p.M(a)`) whose
//       member is not `Dispose` — a borrow read that discharges nothing.
// EVERY other occurrence, and every AST node the scan does not recognise,
// makes this false: the parameter then keeps today's §28.3 drop-reject
// (conservative-safe — a valid program is rejected, never miscompiled). The
// other discharge forms (`p.Dispose()`, `_ := p`, `M(owned p)`,
// `y := owned p`, `yield return p`, an owning-collection transfer) each null
// only the caller's ALLOCA today, not the frame slot; admitting them needs the
// same frame-null at each of their lowering sites and is tracked separately.
//
// The ownership pass is the single caller: a `true` here suppresses the reject
// and sets `ParameterSymbol::iteratorFrameConditionalAutoDispose`, which is what
// codegen's three frame-disposal loops read. Codegen cannot drift ACCEPT-ward
// past it, because codegen's frame-slot nulling is emitted UNCONDITIONALLY at
// form (a) — every marked adoption of a frame-captured `owned` parameter nulls
// the slot, whether or not this predicate admitted the parameter. So the set of
// nulled sites is always a superset of the set of admitted parameters' discharge
// sites: sema can only ever be more conservative than the runtime flag, never
// less (the #1661 no-drift direction that matters — an accept can never outrun
// the disposal that has to make it exactly-once).
bool stateMachineOwnedParamIsFrameNullable(
    const sema::SemaContext& ctx, const sema::Symbol* param,
    const std::vector<std::unique_ptr<ast::Stmt>>& body);

}  // namespace bezel::common
