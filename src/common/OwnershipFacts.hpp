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

// Shared shape-recognition: if the callee of `call` resolves to a user
// `ast::MethodDecl` (a MethodSymbol whose `decl` is a MethodDecl), return it;
// null otherwise (unresolved overload, a builtin with no MethodSymbol, or a
// call through a value/function).  Both consumers reach the `returnOwned`
// authority through this one navigation.
const ast::MethodDecl* calleeMethodDecl(const ast::CallExpr& call,
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

// §28.3 arm-meet (v0.184, #1661): true when `e` is an if-expression
// (`v1 if c else v2`) or an expression-position `case`/`when` whose ownership
// meets to OWNED — i.e. EVERY value-producing arm is itself a fresh owned
// producer (isFreshOwnedClassProducer, or a nested owned-meet if/case) and at
// least one such arm exists. A `throw` arm (bottom-typed) and a `null`-literal
// arm are ABSORBING (they impose no ownership and are skipped); a borrow /
// named-owned-binding / non-fresh arm DISQUALIFIES the whole expression (the
// meet is then a borrow — `false` here). A `case`/`when` with any BLOCK-body
// arm (`do … end`) is conservatively NOT owned-meet (its value expression is
// not structurally inspectable here) — leak-but-sound, never a wrong dispose.
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
const sema::TypeRef* substitutedChoicePayloadType(
    const ast::Param& pf, const ast::TypeDecl* decl, const sema::TypeRef* rt,
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
