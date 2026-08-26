#pragma once

// Private implementation header for the expression-typecheck worker.
//
// Declares the `ExprTC` worker class (sema slice 3 — see ExprTypecheck.hpp for
// the milestone narrative) whose methods are defined across several translation
// units (ExprTypecheck.cpp and ExprTypecheck_*.cpp). This mirrors the codegen
// refactor (cf. LlvmBackend_impl.hpp): the public surface stays in
// ExprTypecheck.hpp (the `typecheckExpressions` free function), while the worker
// class — previously placed entirely inline in an anonymous namespace — is split
// so its definitions can live in separately-compiled units.
//
// `Scope` is declared as a PRIVATE nested type of `ExprTC`. A namespace-scope
// `bezel::sema::Scope` also exists in TypeResolution.cpp; nesting it here avoids
// a program-wide ODR violation. Member bodies refer to it unqualified, which
// resolves to the nested type, so the bodies need no change.

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <utility>
#include <vector>

#include "ast/Node.hpp"
#include "sema/SemaContext.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeRef.hpp"

namespace bezel::sema {

// Declared in TypeResolution.hpp. Forward-declared rather than included to keep
// this header's include graph unchanged. #2787: the §14.2 rule-3 self-recurrence
// test now lives there as the single shared definition.
bool typeRefEqual(const TypeRef* a, const TypeRef* b);
bool selfComparableIface(const TypeRef* t, const TypeSymbol* icomparable);

// per §15.4 (#1174 / #1866): a collection literal's admissible TARGET types are
// a CLOSED list of CONCRETE collections (inline array, `Array`/`OwningArray`,
// `Set`, `Stack`, `Queue`, `LinkedList`, `Dictionary`, and their owning forms,
// or a `FromCollectionLiteral` user type). A collection INTERFACE
// (`IEnumerable<T>`/`IList<T>`/`ICollection<T>`/`IReadOnly*`/…) is NOT on that
// list — it has no concrete backing the literal could be materialized into.
// `isCollectionLiteralInterfaceTarget` recognises a non-admissible interface
// target; `reportCollectionLiteralInterfaceTarget` emits the §15.4 rejection.
// Shared across the call-argument (#1174), string-method (#1174), and
// local-declaration (#1866) positions. `isCollectionLiteralAdmissibleTarget`
// is the positive predicate (concrete target → OK to materialise), used by
// the lambda-body materialisation path (#1825): when a lambda's expression
// body is a collection literal AND its target return type (a typed local /
// call-arg function-typed slot) is a §15.4 admissible concrete target, the
// literal is materialised against the target exactly like the call-arg and
// local-decl paths already do.
// per §15.4 (#2914): `isCollectionLiteralAdmissibleTarget` is ALSO the gate at
// the local-declaration position, which until #2914 asked only the NEGATIVE
// interface question — so every OTHER non-admissible target (`Object`,
// `Object?`, `int`, `string`, a class with no `FromCollectionLiteral`, a bare
// type parameter) walked past it into codegen. `reportCollectionLiteralBadTarget`
// emits the general §15.4 rejection for those; the interface report above stays
// the more specific message for the shape it names.
// per §15.4 (#3031): the predicate ALSO consults the literal's FORM. The
// dictionary-literal form `[k: v, ...]` is admitted only at the dictionary
// family (`Dictionary<K,V>`/`OwningDictionary<K,V>`/`SortedDictionary<K,V>`,
// §15.4 last paragraph); the sequence-literal form `[e1, e2, ...]` is admitted
// at every sequence-shaped target but NOT at the dictionary family. Pre-fix the
// predicate took the target alone, so a dict literal at any sequence-shaped
// target (and vice versa) reached codegen, neither materializer claimed it,
// and the slot was degraded to a zero-initialized value (the #2434/#2491
// `internal:` notes — a silent wrong value, the worse failure mode). The `isDict`
// argument is the literal's `cl->isDict`. The empty-literal carve-out
// (`[]` admitted at every §15.4 collection target, including dictionary —
// the spec lets the target type choose between empty seq and empty dict)
// is handled at the call sites because they already have the literal
// available; the predicate's form-only answer is the right one for sites
// that have ALREADY addressed the empty case independently (the inline-array
// N-element check at ExprTypecheck.cpp:9717 is the existing instance).
bool isCollectionLiteralInterfaceTarget(const TypeRef* t);
bool isCollectionLiteralAdmissibleTarget(const TypeRef* t, bool isDict);
// §15.4 (v0.240): the empty literal carve-out — is `t` on the §15.4 closed
// collection-target list? The empty literal `[]` is admitted at every
// listed target (the target type decides between empty seq and empty dict);
// at every other target (Object, int, a class with no
// `FromCollectionLiteral`, a bare type parameter) the empty literal is
// still rejected with the generic "cannot target" diagnostic (the §15.4
// carve-out sentence: "An empty literal in a non-collection or
// undeterminable context is the usual 'cannot infer target type' compile
// error"). The inline-array case is separate: inline arrays require `≥ 1`
// elements regardless of form/empty status, so this predicate returns
// false for inline arrays and the call sites' emptiness check is
// independent.
bool isCollectionLiteralEmptyAdmissibleTarget(const TypeRef* t);
void reportCollectionLiteralInterfaceTarget(DiagnosticEngine& diags,
                                            SourceLocation loc,
                                            std::string_view ifaceName);
void reportCollectionLiteralBadTarget(DiagnosticEngine& diags,
                                      SourceLocation loc, const TypeRef* target);
void reportCollectionLiteralDictFormBadTarget(DiagnosticEngine& diags,
                                              SourceLocation loc,
                                              const TypeRef* target);

// §27.1.1 (v0.180): exact structural equality of two TypeRefs — the INVARIANT
// fallback for a function-value slot that is not a bare, non-generic
// class/interface reference. Defined in ExprTypecheck.cpp (mirrors
// Resolver::typeRefEqual); shared with ExprTypecheck_Exprs.cpp for the
// §27.3 divergent-return check (#2107).
bool functionSlotTypeRefEqual(const TypeRef* a, const TypeRef* b);

// §14.1 (#1756's lesson, reused by #2926): does `t` mention any of `tpNames`
// ANYWHERE — as itself, a generic argument, an inline-array element, or a
// function parameter/return? Defined in ExprTypecheck_Calls.cpp for the §22.3
// ref/out deferral gate; declared here so the field-initializer check can use
// the SAME superset walk. Answering true only DEFERS a check, so a superset is
// the safe direction — a MISSED mention is what makes a check run against an
// unsubstituted type parameter and false-reject.
bool refOutParamTypeMentionsTypeParam(
    const TypeRef* t, const std::unordered_set<std::string_view>& tpNames);

// fix #3247 / §16.3 (v0.277): `Range<T>`'s T is restricted by the compiler
// to the integer types (byte, sbyte, short, ushort, int, uint, long, ulong)
// and char. The predicate is read by `ExprTC::visitExpr` (the RangeExpr arm
// at ExprTypecheck_Exprs.cpp) AND by `Resolver::resolveType` (the `Range<T>`
// annotation at TypeResolution_Names.cpp) so the admit/reject answer stays
// in lockstep. Lives at namespace scope so the type-resolution pass can
// call it without going through ExprTC.
bool isValidRangeElementType(const TypeRef* ty);
std::string rangeElementTypeName(const TypeRef* ty);

class ExprTC {
public:
    ExprTC(SemaContext& ctx) : ctx_(ctx) {}

    void run(const ast::SourceFile& src);

private:
    // Per-walker scope for identifier lookups inside method bodies (slice 3b).
    struct Scope {
        std::unordered_map<std::string_view, Symbol*> names;
        Scope* parent = nullptr;
    };

    SemaContext& ctx_;
    std::unique_ptr<Scope> moduleScope_;
    Scope* currentScope_ = nullptr;
    TypeSymbol* currentType_ = nullptr;
    // §26.1: the ExtensionSymbol whose members are being type-checked, or null
    // outside an extension block. An extension method's params live on the
    // ExtensionSymbol (not on currentType_, which points at the extended type),
    // so paramSymbolsOf consults this when set.
    ExtensionSymbol* currentExtension_ = nullptr;

    // §2.2 v0.164 / #1365: import aliases for the current source file.
    // Mirrors TypeResolution's importAliases_; used in resolveMemberAccess to
    // support `Alias.TypeName.Member` in expression position (§2.7).
    std::unordered_map<std::string, std::string> importAliases_;
    // §2.2 v0.164 / #1365: explicitly-imported module names (dotted) for the
    // current source file. Used for module-prefix resolution (§2.7 rule 2).
    std::unordered_set<std::string> sourceImports_;
    // §2.2 v0.164 / #1365: bare names ambiguous due to import collision.
    std::unordered_set<std::string> ambiguousImportNames_;
    // §26.5 step 2 / §2.5 (#735): the extension blocks reconstructed from the
    // modules this source file `import`s — §26.5 requires the search to cover
    // "the current module AND all imported modules". Populated in run() from
    // SemaContext::importedExtensionsOf for each explicit import (so an
    // artifact loaded but not imported stays invisible, matching the
    // imported-TYPE gating), and searched by lookupExtensionMember alongside
    // the current module's own ExtensionSymbols.
    std::vector<ExtensionSymbol*> importedExtensions_;
    // §26.5 rule 4 (#1307): the defining module (dotted name) of each entry in
    // `importedExtensions_`, captured at the same population site — an
    // ExtensionSymbol carries no module-name field of its own (unlike a
    // TypeSymbol registered under ctx_.importedTypesOf), so the ambiguity
    // diagnostic's "defining module" fact for an imported candidate has
    // nowhere else to come from. A local (current-module) candidate's module
    // is ctx_.module()->name; this map covers only the imported ones.
    std::unordered_map<ExtensionSymbol*, std::string> importedExtensionModule_;
    // §2.7 v0.164 / #1365: maps a MemberAccessExpr* → dotted module name when
    // that expression resolves to a partial module-name prefix in expression
    // position (e.g. `BankingApp.Transactions` in `BankingApp.Transactions.Calc`).
    // Lets the next MemberAccess level detect the complete module name.
    std::unordered_map<const ast::MemberAccessExpr*, std::string> partialModulePaths_;

    // §12.7 flow narrowing. When `if x is T` (or the else branch of
    // `if x is not T`) narrows a simple variable `x` to `T`, an entry maps the
    // variable's resolved symbol (a LocalSymbol or ParameterSymbol) to the
    // narrowed TypeRef for the duration of the controlled block. An
    // IdentifierExpr that resolves to a narrowed symbol takes the narrowed type
    // in place of its declared one, so member access on it resolves against `T`
    // (`resolveMemberAccess` reads the target's exprType first). The map is
    // saved/restored around each controlled block so narrowing never leaks past
    // it; a reassignment of `x` inside the block drops its entry from that point
    // (re-widening to the declared type).
    std::unordered_map<Symbol*, TypeRef*> narrowings_;

    // §7.4 / §8.4 nullability flow-narrowing (issue #418). The set of simple
    // variables (LocalSymbol / ParameterSymbol) statically known to be non-null
    // at the current program point. A symbol is added when a null test proves
    // it non-null (`if x <> null` then-block, `if x = null` else-block, the
    // matching `is not null` / `is null` forms, an early-return `= null` guard),
    // when a non-null value is assigned to it, and on the all-paths join of an
    // if-statement; it is removed when a (possibly-)null value is assigned. The
    // §7.4 member-access gate consults this set: a bare member access on a
    // nullable simple variable NOT in this set is rejected. Unlike `narrowings_`
    // this does NOT rewrite the variable's exprType — the declared (nullable)
    // type is preserved so codegen's nullable-value layout logic (the tagged
    // {hasValue; payload} representation of `record?`/`int?`) stays correct.
    // Block-scoped the same way `narrowings_` is (saved/restored per block).
    std::unordered_set<Symbol*> nullNarrowed_;

    // §7.4 (v0.214, #2085 redone per #2116): flow-narrowing facts for nullable
    // `readonly` fields. Spec §7.4 (v0.214): "A nullable `readonly` field —
    // instance or `static` — participates in flow narrowing … The tracked shape
    // is exactly one field off an admitted root — `this._f`, `TypeName._s`, or
    // `local._f` where `local` is itself narrowing-tracked; a fact rides its
    // root (reassigning `local` drops it), and any non-final field segment makes
    // a path non-trackable."
    //
    // DETERMINISM-BY-CONSTRUCTION (#2116 redo requirement). The #2085 landing
    // keyed facts on raw (Symbol*, FieldSymbol*) pointers in a pointer-ORDERED
    // std::set; it was reverted for concurrency-nondeterministic wrong-result
    // flakes under the 8-way sharded ASan gate. This redo stores NO pointers in
    // the fact set at all:
    //   * A fact is a pair of DENSE INTEGER ids `(rootId, fieldId)`. rootId 0
    //     is the ambient root (`this` / bare instance field / `base` / a
    //     static field through its type name or bare); a local/parameter root
    //     gets a per-run dense id from `narrowFactIds_` below. fieldId is the
    //     FieldSymbol's dense id.
    //   * Ids are handed out lazily, in FIRST-TRACKING-USE order — a pure
    //     function of the single-threaded, source-ordered AST walk. Two runs of
    //     the same input assign identical ids regardless of address layout.
    //   * The container is a std::set over integer pairs, so even its ITERATION
    //     order is address-independent; and every consumer decision (gate
    //     membership, root-drop, mutable-capture drop, branch/loop/arm
    //     intersection joins) is additionally order-independent set algebra.
    //   * `narrowFactIds_` is pointer-KEYED but access is find/emplace only —
    //     it is NEVER iterated (auditable by inspection of narrowFactIdOf /
    //     narrowFactIdIfTracked, its only accessors).
    //
    // Fact lifetime (§7.4 v0.214): "outside its construction window (§22.8's
    // single-assignment rule) a `readonly` field has no writer on any path,
    // through any alias, so no call, `await`, or closure can invalidate a fact
    // about its value" — hence ambient-rooted (rootId 0) facts are never
    // invalidated. A `local._f` fact rides its ROOT: reassignment (`:=`/`??=`),
    // ref/out passing, and the §7.4 mutable-capture regime drop it, mirroring
    // `nullNarrowed_`. "The rule does not apply inside the declaring type's
    // constructors, field initializers, static constructor, or static field
    // initializers (the assignment window)" — narrowFromFieldNullTest applies
    // the carve-out at establishment. Saved/restored/joined like
    // `nullNarrowed_` through the if-statement walk, plus (unlike the legacy
    // local set — see the pre-existing holes filed from this lane) reset at
    // elif-condition visits, per-case-arm, and across lambda-body boundaries
    // (#2113 closed for THIS set).
    std::set<std::pair<std::uint32_t, std::uint32_t>> fieldNarrowed_;

    // §7.4 / #2116: identity → dense-id table for the fact keys above, plus the
    // reverse (dense id → symbol) vector used when a local-rooted fact must be
    // matched against the mutable-capture name set. Insert-only for the run;
    // never iterated; never cleared (ids stay valid across method bodies —
    // facts themselves are method-local via runMethodBody's clear).
    std::unordered_map<const Symbol*, std::uint32_t> narrowFactIds_;
    std::vector<const Symbol*> narrowFactIdSyms_;  // [id] → symbol; [0] = null

    // §7.4 / #2116 (pre-gate review): monotone counter bumped whenever a
    // runtime DROP EDGE removes a readonly-field fact (dropFieldFactsRooted,
    // invalidateCapturedNarrowings). The try-statement walk compares it across
    // the body/arm visits: a try body can be interrupted at ANY point, and an
    // exit-state intersection is blind to an interior drop that a later
    // re-establishment masks (drop `a`, throw window, re-guard `a._f` — the
    // exit state carries the fact the interrupted path lost). If no drop edge
    // fired, entry facts are valid at every interrupt point; if one fired,
    // only ambient facts (never droppable) are. A pure function of the
    // single-threaded walk — no determinism impact.
    std::uint64_t fieldFactDropEpoch_ = 0;

    // §7.4 (v0.182, #1566): the set of local/parameter NAMES that are "mutably
    // captured" — assigned, or passed `ref`/`out`, anywhere inside ANY lambda of
    // the enclosing method body. Computed once per body (computeMutablyCaptured)
    // at the start of `runMethodBody`; the spec's rule is deliberately syntactic
    // and conservative (any lambda in the body counts, whether or not it escapes,
    // so no escape analysis is needed). While such a local is narrowed non-null,
    // any operation inside the controlled block that can transfer control to user
    // code — a call, a function-value invocation, a constructor, or an `await` —
    // may run the capturing closure and null the local, so it drops the narrowing
    // (invalidateCapturedNarrowings). Keyed by name (string_view into the stable
    // source text); matched against a narrowed symbol's `name`. Empty in the
    // common case, which makes the per-op invalidation a cheap no-op.
    std::unordered_set<std::string_view> mutablyCapturedNames_;

    // §22.5 (issue #1160): while type-checking parameter i's default-value
    // expression, that parameter and every parameter declared AFTER it are out
    // of scope — a default may read only EARLIER parameters. This set holds the
    // forbidden (self-and-later) parameter symbols for the default currently
    // being checked; it is empty outside that window. `lookupName` consults it
    // and records the offending name in `forwardParamRef_` so `runMethodBody`
    // can report the §22.5 error at the default expression.
    std::unordered_set<Symbol*> forbiddenForwardParams_;
    std::string forwardParamRef_;

    // issue #484 / §14.2: the MethodDecl currently being type-checked, or null
    // when outside a method body. Used by resolveMemberAccess to walk the
    // method's generic constraints when the receiver is a type parameter —
    // `x: T` where T has `where T: ISome` requires dispatching through the
    // constraint's member set, not the (null) declaration of T's TypeRef.
    const ast::MethodDecl* currentMethodDecl_ = nullptr;

    // §11.8 (v0.176, #807): while visiting a MemberAccessExpr's TARGET child,
    // this points at that target node. It lets the IdentifierExpr handler
    // recognize `T` in `T.Member(...)` — a type-parameter receiver — and mark
    // it with a GenericParamSymbol instead of reporting "unresolved
    // identifier". A bare `T` in any other expression position keeps today's
    // error. Saved/restored around each target visit.
    const ast::Expr* maTargetHint_ = nullptr;
    // §27.6 (v0.184): while visiting a CallExpr's *callee* child, this points at
    // that callee node. resolveMemberAccess consults it to tell a primitive-
    // intrinsic static member that is the DIRECT callee of a call (`int.Parse(s)`
    // — typed as its scalar result, dispatched by member/receiver in typeCall)
    // from one used as a BARE method reference (`strings.Select(int.Parse)` —
    // typed as the function value `(string) -> int` per §27.6). Identity-based
    // (the specific callee node), so a stale value never yields a false positive.
    // Saved/restored around each callee visit.
    const ast::Expr* calleeOfCallCtx_ = nullptr;
    // fix #2328 (§26.5, v0.236): the call-site arity the currently-being-
    // visited MemberAccessExpr belongs to (its enclosing CallExpr's argument
    // count, when there is one). -1 means "no call context" — a bare-
    // reference access (a method reference, a property read, etc.), which
    // leaves `lookupExtensionMember`'s arity filter un-applied. `+N` means
    // "this access is the callee of a `M(...)` call with N args" — the
    // lookup filters out any same-named extension member whose parameter
    // list has a different arity, BEFORE the §26.5.1 specificity ranking,
    // so the per-call arity check that `checkCallArity` performs for
    // ordinary methods happens in the same place for extensions and the
    // user never sees a bogus §26.5 rule-4 ambiguity on a wrong-arity
    // call. Saved/restored around each callee visit (a nested call sets
    // the OUTER arity while the inner call's own arity is in flight).
    std::ptrdiff_t currentCallArity_ = -1;
    // §11.8: one synthesized GenericParamSymbol marker per type-parameter
    // name, attached as the exprSymbol of a `T` member-access receiver so
    // resolveMemberAccess (and codegen) can identify the receiver kind.
    std::unordered_map<std::string_view, GenericParamSymbol*> typeParamMarkers_;
    // §11.8: is `name` a declared generic type parameter of the enclosing
    // method or type?
    bool isInScopeTypeParam(std::string_view name) const;
    // §11.8: the cached marker symbol for an in-scope type-parameter name.
    GenericParamSymbol* typeParamMarker(std::string_view name);
    // §11.8: search the enclosing method's constraints (then the enclosing
    // type's) for a `where <paramName>: I<...>` bound whose interface declares
    // a static OPERATOR member matching `op` with two parameters of the
    // interface's own recurrence parameter. Returns the interface's operator
    // MethodSymbol and stores the bound's instantiation TypeRef in *outBound.
    MethodSymbol* findConstraintStaticOperator(std::string_view paramName,
                                               std::string_view op,
                                               TypeRef** outBound);
    // §11.8: enumerate the `type`-kind constraint bounds declared for
    // `paramName` (method constraints first, then the enclosing type's),
    // invoking `fn` with each resolved bound TypeRef until it returns true.
    bool forEachConstraintBound(std::string_view paramName,
                                const std::function<bool(TypeRef*)>& fn);

    // Slice S6: in-progress method static-ness, so visitExpr's
    // IdentifierExpr handling can diagnose `this`/`base` in static
    // member bodies.
    bool inStaticMember_ = false;
    // §5 / issue #184: when a LocalDecl has an explicit wider integer type
    // (long, ulong, uint) and the initializer is an unsuffixed int literal
    // that overflows int, widen it to the declared type instead of erroring.
    PrimitiveType currentLiteralWideningHint_ = PrimitiveType::None;
    // §4.2 / issue #1155: true while typing an integer literal that is the
    // direct operand of a unary minus. A signed type's negative boundary
    // magnitude (|long.MinValue| = 2^63) is one greater than its positive
    // maximum, so `-9223372036854775808L` is long.MinValue and must be
    // admitted even though the bare literal `9223372036854775808L` overflows
    // `long`. Set by visitExpr around the operand of a unary `-`.
    bool literalUnderUnaryMinus_ = false;
    // Slice S7: tracks whether we're inside a constructor (or static
    // constructor) body — the only places where `this.<readonly>` may
    // be assigned. Field initializers are handled separately (slice
    // S7 doesn't flag those — they're part of the initialization flow).
    bool inConstructor_ = false;
    // Slice S21: the return-form context of the callable currently
    // being walked, for the §21.7 form rules.
    enum class ReturnCtx { None, ValueMethod, VoidMethod, CtorDtor };
    ReturnCtx returnCtx_ = ReturnCtx::None;
    // Slice T3: the resolved return type of the value-returning
    // callable currently being walked (null otherwise), for primitive
    // return-value assignability (§5.1).
    TypeRef* currentReturnType_ = nullptr;
    // Slice S26: enclosing-loop nesting depth, for the §21.5 rule that
    // `break`/`continue` are legal only inside a loop.
    int loopDepth_ = 0;
    // §21.5 (v0.201, #2051): the loop label of each enclosing loop, outermost
    // first — one entry per open loop (empty for an unlabeled loop). Its size is
    // kept equal to `loopDepth_`. Used to reject duplicate labels ("labels do
    // not shadow"), to resolve a labeled `break`/`continue` target (and compute
    // its nesting offset for codegen), and to apply the `finally`-boundary rule
    // to the resolved target. Reset across the callable (lambda) boundary
    // alongside `loopDepth_`, so a label is visible only within its own callable.
    std::vector<std::string_view> loopLabelStack_;
    // §7.4 (#2152): per-loop collector of the flow-narrowing state snapshotted
    // at each `break`/`continue` edge. Both edge kinds can reach the after-loop
    // program point — `break` exits the loop directly; `continue` jumps to the
    // condition re-check, which on cond-false also exits — so each edge's state
    // is a possible after-loop state and must be folded into the join. #2118's
    // entry ∩ fall-through-exit model missed these edges, so a local an edge
    // left possibly-null was wrongly narrowed non-null after the loop
    // (over-narrowing → a bare member access accepted that §7.4 rejects). One
    // collector per active loop, parallel to `loopLabelStack_` (back =
    // innermost); a labeled `break L`/`continue L` routes its snapshot to the
    // TARGET loop's collector via `targetLoopOffset`. Saved/cleared/restored
    // across the lambda boundary alongside `loopLabelStack_` (a lambda is its
    // own callable — a `break` inside it cannot target an enclosing loop).
    struct LoopEdgeNarrowing {
        std::vector<std::unordered_set<Symbol*>> nullEdges;
        std::vector<std::unordered_map<Symbol*, TypeRef*>> typeEdges;
        std::vector<std::set<std::pair<std::uint32_t, std::uint32_t>>>
            fieldEdges;
    };
    std::vector<LoopEdgeNarrowing> loopEdgeNarrowing_;
    // Slice S27: enclosing catch-arm-body depth, for the §31.5 rule
    // that a bare `throw;` / `throw` re-raise is valid only in a catch
    // arm body.
    int catchDepth_ = 0;
    // per §31.2: `return` inside a `finally` block is a compile error.
    // Appendix A grammar: "`return` does not appear in `finally` blocks."
    // Tracks finally-block nesting so visitStmt can reject ReturnStmt there.
    // per §21.7 (#1589): also reset (to 0) across a lambda boundary — a
    // lambda is its own callable, so a `return` inside it exits the lambda,
    // not the enclosing method, and is unaffected by that method's `finally`
    // (mirrors the `loopDepth_`/`catchDepth_`/`finallyLoopBoundary_` reset).
    int finallyDepth_ = 0;
    // per §21.5/§31.2 (v0.182, #1578): `loopDepth_` measured at the point
    // the nearest enclosing `finally` body was entered. A `break`/`continue`
    // whose target loop (the loop active at `loopDepth_` when the jump is
    // visited) was already open at that boundary was opened *outside* the
    // `finally` and therefore encloses it — such a jump would silently
    // discard an in-flight exception when the `finally` runs during
    // exception propagation. A target loop opened strictly after the
    // boundary is wholly inside the `finally` and is unaffected. -1 means
    // "not currently inside a finally"; also reset across a lambda boundary
    // (a lambda is its own callable, unaffected by an enclosing method's
    // `finally` — mirrors the `loopDepth_`/`catchDepth_` reset there).
    int finallyLoopBoundary_ = -1;
    // Slice S29: true while walking a field/auto-property initializer,
    // for the §22.8 rule that initializers may not observe instance
    // state (`this`, `base`, instance members).
    bool inFieldInitializer_ = false;
    // fix #3227 (§22.10 / §22.5 / §22.8 step 0, v0.254): true while walking a
    // primary-ctor parameter's `:= Expr` default. The default may read earlier
    // primary-ctor parameters (§22.5 "Permitted default-value expressions"),
    // and those references resolve against the SAME object instance — the
    // captured parameter is consumed BY the default's evaluation, not consumed
    // and routed onward as an init field's value (the §22.8 case `inFieldInitializer_`
    // covers). The §22.10 capture-iff-used rule below routes the reference
    // to `posParamBodyRef` (capture the parameter as a field) rather than
    // `posParamInitRef` (consume into an init field), so an earlier parameter
    // referenced ONLY inside a later default still becomes a real captured
    // field slot. Distinct from `inFieldInitializer_` so the §22.8 instance-
    // member gate (this/base/instance field) still fires inside defaults.
    bool inPrimaryCtorDefault_ = false;
    // §10.6 (#772): true while typechecking the arm guards/bodies of a `case`
    // whose scrutinee is an enum (the scope into which that enum's constants are
    // injected). Gates the bare-enum-constant exprType attachment so it applies
    // only there — never in an enum-constant initializer's sibling arithmetic.
    bool inEnumScrutineeCaseArms_ = false;
    // async-readiness 3b (§30.2 / Appendix C.6): true while walking the body
    // of an `async` method or `async` lambda — the contexts where `await` is
    // valid. Reset to false across every lambda boundary (a non-async lambda
    // nested in an async method is itself a synchronous context) and set from
    // the lambda's `isAsync` flag. Static field initializers and static
    // constructors are never async, so an `await` there is rejected by this
    // gate without special-casing (Appendix C.6).
    bool inAsyncContext_ = false;
    // §17.15 / §30.5 (v0.216, #2098): true while visiting the clauses/body of
    // a binding atom `(var x := E; body)`. Consulted by the `await` typing arm
    // to reject `await` INSIDE a binding atom in an async body — the async
    // lowering's hand-rolled state machine performs a statement-level
    // frame-local scan, so an atom binding would stay alloca-backed across
    // the suspension (a silent miscompile); until the scan learns BindExpr
    // (#2122) the conservative targeted rejection stands. Reset to false
    // across a lambda boundary (a nested lambda's body is its own function —
    // an await there suspends the lambda's state machine, not the atom's
    // enclosing frame).
    bool inBindAtom_ = false;
    // §28.3 A7 (v0.209, #2080): the ONE expression node currently admitted to
    // be a MarkedTransferExpr — set (saved/restored) around the visit of each
    // admitted child: an if-expression branch (ConditionalExpr then/else) or
    // an expression-`case`/`when` arm result (CaseArm exprBody). The
    // MarkedTransferExpr arm checks `&e == markedArmSlot_`, so a marker
    // NESTED inside an admitted child (`(if c then f(owned x) else y)`) is
    // still rejected — admission is the exact arm position, not the subtree.
    const ast::Expr* markedArmSlot_ = nullptr;
    // §35.5 (v0.209/#2080 F4): the case-SCRUTINEE node currently being
    // visited (CaseExpr/CaseStmt). A MarkedTransferExpr here is the separate
    // owning-match scrutinee production — routed to the §35.5 targeted
    // rejection (#2124), not the A7 positional reject.
    const ast::Expr* caseScrutineeSlot_ = nullptr;
    // #721/§29.5: true while visiting an iterator method body (sync or async,
    // i.e. it contains `yield`). A plain `return;` in an iterator is forbidden
    // (use `yield break`); for an ASYNC iterator §21.7 does not catch it (it is
    // a VoidMethod ctx), so checkReturnForm consults this flag.
    bool inIteratorMethod_ = false;
    // §29.9: true while visiting a lambda body. `yield` is a method-level
    // construct only, so a `yield`/`yield break` inside a lambda is rejected
    // (otherwise codegen builds a malformed iterator state machine for the
    // lambda and the program SIGSEGVs at runtime — #747 R42). Saved/restored
    // (not just set) so nested lambdas behave correctly.
    bool inLambda_ = false;

    // §22.16 (v0.240, #2942): true while visiting a contract clause's
    // expression (`requires` / `ensures`). Used by the IdentifierExpr
    // resolution path to give a context-aware diagnostic when the contextual
    // keywords `result` / `old` appear inside a clause (or outside one):
    // inside `ensures`, they are rejected as not-yet-implemented per #2942;
    // outside any clause (or inside a `requires`/`invariant` clause, where
    // §22.16 also forbids them), they are reported as contextual keywords
    // reserved for `ensures` rather than the generic "unresolved identifier"
    // — the loud-reject path the issue asks for. Reset to false across every
    // lambda boundary (a nested lambda body is its own callable, not a
    // clause context) and set from `runMethodBody`'s clause loop.
    bool inContractClause_ = false;
    // §22.16 (v0.240, #2942): the kind of the clause currently being visited,
    // or `None` when not inside a clause. Read by the IdentifierExpr handler
    // to choose between the inside-`ensures` (not-yet-implemented) and
    // outside-`ensures` (contextual-keyword) diagnostics. Mirror of the
    // AST's `ast::ContractClause::Kind`, kept as a plain enum here so the
    // sema layer does not pull in the AST header for a 3-valued flag.
    enum class ContractClauseKind { None, Requires, Ensures };
    ContractClauseKind currentClauseKind_ = ContractClauseKind::None;

    // per §24.3: when about to visit an event-handler lambda argument
    // (`<event>.Subscribe(h)` / `.Add` / `.Remove`, or `event += h`), this
    // holds the event's payload type so the lambda's untyped parameter is
    // given the handler's parameter type `T` — letting the body resolve
    // `e.Field` etc. Null when not in an event-handler context. Consumed (and
    // cleared) by visitExpr(LambdaExpr).
    TypeRef* pendingLambdaParamType_ = nullptr;

    // §23 (#1192): the member-access node that is the DIRECT target of a plain
    // `:=` assignment (e.g. `c.X` in `c.X := 7`). Set around the assignment
    // target's visitExpr in the AssignmentStmt handler; consumed by
    // resolveMemberAccess to EXEMPT exactly that node from the write-only
    // (set-only) property read diagnostic — a plain `:=` to a set-only property
    // is a write (handled by the setter), but any other access position (a
    // read, a compound-assignment target, an intermediate `.x` in a chain, an
    // RHS) is a read and must be rejected.
    const ast::Expr* plainSetAssignTargetNode_ = nullptr;

    // fix #2543 (§23, v0.236): mirror of `plainSetAssignTargetNode_` for
    // `ref`/`out` call/constructor argument operands. The bare-read-of-
    // write-only-indexer gate (ExprTypecheck_Exprs.cpp user-defined-indexer
    // visit) must NOT fire when the IndexExpr is the operand of a `ref`/`out`
    // argument: that operand position has its OWN specialized check in
    // `checkRefOutPropertyOperand` (#2530), which correctly rejects `ref`
    // and admits `out`. Without this exemption, an `out d[i]` against a
    // write-only indexer would be incorrectly rejected here BEFORE the
    // (correct) ref/out operand pass even runs. Saved/restored around each
    // CallArgument/NewExpr-arg visit. A `nullptr` (default) means the
    // current IndexExpr is NOT a ref/out operand and the gate fires
    // normally for write-only indexers.
    const ast::Expr* refOutArgTargetNode_ = nullptr;

    // fix #829 / §A.7: when a lambda's expected type is a known function type
    // `(T1, …, Tn) -> R` (a typed-local initializer, an assignment RHS, or a
    // call argument bound to a function-typed parameter), this holds those
    // positional parameter types so an UNTYPED lambda parameter (`(a, b) => …`)
    // is given its inferred type. Without it the params stay untyped and the
    // body's operator/member resolution falls back to primitives — e.g. a
    // user-defined `operator +` on a record is missed and codegen emits a raw
    // `add` on the record struct (malformed IR). Consumed (and cleared) by
    // visitExpr(LambdaExpr); positionally matched to the lambda's params.
    std::vector<TypeRef*> pendingLambdaParamTypes_;

    // issue #777 / §30.6: when about to visit an `async` lambda whose expected
    // function type is `(…) -> Task<T>` / `(…) -> OwningTask<T>`, this holds the
    // peeled element type T so the lambda visitor can reject a forgotten `await`
    // (an expression body that produces a `Task` value where T is expected).
    // Null otherwise. Consumed (and cleared) by visitExpr(LambdaExpr).
    TypeRef* pendingLambdaReturnType_ = nullptr;

    // §27.3 (v0.226, #2107): when about to visit a lambda whose expected
    // function type is known from context (a typed local/field initializer
    // `var f: (…) -> R := …`, or a call argument to a `(…) -> R` parameter),
    // this holds the target's RETURN slot R so the block-body inference knows a
    // target exists and suppresses the divergent-return error — divergent
    // returns are checked against the target under §5.1 elsewhere, not unified
    // by inference. Distinct from pendingLambdaReturnType_ (which carries only
    // the peeled Task element T for the async forgotten-`await` check). Null
    // when no target return type is available. Consumed (and cleared) by
    // visitExpr(LambdaExpr) so a nested lambda never inherits it.
    TypeRef* pendingLambdaTargetReturn_ = nullptr;

    // fix #1820 (§28.3 / §28.8): the `funcReturnOwned` flag of the OUTER
    // function type (`(…) -> owned T`) whose return slot is in
    // pendingLambdaTargetReturn_. The flag does not live on the inner slot
    // itself, so the callee-side rule needs a separate thread-in. When true
    // the §28.8 transferring rule governs the slot and the §28.3 borrow
    // rule's check is suppressed. Cleared alongside pendingLambdaTargetReturn_.
    bool pendingLambdaTargetReturnOwned_ = false;

    // issue #777 / §30.6: true when `t` is a Named `Task<…>` / `OwningTask<…>`
    // (the async task wrapper). Used to detect a forgotten `await`.
    bool isTaskShapedRef(const TypeRef* t) const;

    // fix #1820 (§28.3, v0.223 ruling; carried in v0.282): is `t` a concrete
    // class-or-interface slot that triggers the Concrete-slot function bodies
    // callee-side rule? Returns false for: null/Error type, an `owned`-marked
    // slot (transferred by §28.8), a `Task`-family slot (claimed by v0.190),
    // one of the eight iterator/enumerator interface heads (#1787),
    // a bare type parameter (claimed by v0.194), an inline-array/unit/primitive
    // slot, or a record/enum/choice/builtin value type.
    bool isConcreteSlotReturnType(const TypeRef* t) const;

    // fix #1820 (§28.3, v0.223 ruling): is `e` a fresh owned-producing
    // expression — `new T()` over a class/interface, or a call whose callee
    // returns `owned` (per `common::calleeReturnsOwned`, which covers
    // methods, function values, extern calls, and operators)? Used by the
    // Concrete-slot function bodies callee-side rule to reject fresh mints
    // at concrete class/interface return slots.
    bool isFreshOwnedProducerExpr(const ast::Expr& e) const;

    // fix #1820 (§28.3): walk a lambda body and call `fn` for every value-
    // carrying return statement and the expression body (if present). The
    // walker descends through do/end, if/elif/else, while, for,
    // try/catch/finally, statement-position case block arms — matching
    // collectLambdaReturnTypes exactly so the rejection sees the same set
    // of return paths the inference sees.
    template <typename F>
    void forEachLambdaReturnValue(
        const ast::LambdaExpr& lam, F&& fn) const;

    // fix #1820 (§28.3): Concrete-slot function bodies callee-side check.
    // When a lambda is being type-checked at a function-type target whose
    // return slot is a concrete class/interface (per isConcreteSlotReturnType
    // above), reject every fresh-owned body return path. The caller is the
    // only parameter-thread-in point (`pendingLambdaTargetReturn_`).
    // `targetOwnedMarker` is the OUTER function type's `funcReturnOwned` flag
    // (the `(…) -> owned T` keyword) — when true, the §28.8 transferring
    // rule governs the slot and the §28.3 producer-row borrow rule does NOT
    // apply, so the callee-side check is suppressed entirely.
    void checkConcreteSlotLambdaBody(
        const ast::LambdaExpr& lam, const TypeRef* targetSlot,
        bool targetOwnedMarker,
        TypeRef* savedCurrentReturnType);

    // §27.3 (v0.226, #2107): recursively collect the resolved type of every
    // value-carrying `return e;` in a block-bodied lambda body, descending
    // through statement wrappers (do/end, if/elif/else, while, for,
    // try/catch/finally, statement-position case block arms) but NOT into
    // nested lambdas (a nested lambda is an expression value and its own
    // callable — its returns are its own). Used by the no-target block-body
    // return-type inference to (a) pick the first return's type, replacing the
    // old flat first-return scan that missed returns nested below one wrapper
    // level and silently fell back to `int`, and (b) detect §27.3 divergent
    // returns (≥2 value returns whose types are not all the same).
    void collectLambdaReturnTypes(
        const std::vector<std::unique_ptr<ast::Stmt>>& stmts,
        std::vector<TypeRef*>& out);
    void collectLambdaReturnTypeFromStmt(const ast::Stmt* s,
                                         std::vector<TypeRef*>& out);

    // per §24.3: the payload type of an event-handler call's callee, or null
    // when `callee` is not `<event>.Subscribe/.Add/.Remove`. Used to set
    // pendingLambdaParamType_ before visiting the handler argument.
    TypeRef* eventHandlerPayloadType(const ast::Expr* callee);

    // §21.9 / §17.13 / §2.11 (#949): true when `e` is an `<event>.Subscribe(...)`
    // call. Subscribe returns an `owned Subscription` (a non-trivial-dispose
    // class, stdlib §2.11) but sema does not (yet) type the call result, so the
    // type-based drop/discard checks cannot see it. Recognized structurally so a
    // dropped or discarded Subscription is the hard error §21.9 / §17.13 require.
    bool isEventSubscribeDrop(const ast::Expr* e);

    // §28.9 / §22.5 (issue #1684): does `decl`'s class chain make it
    // non-trivial-dispose — a destructor, or a non-static `owned` field
    // (declared, or a captured `owned` primary-constructor parameter,
    // §22.10), anywhere from `decl` up through its base classes? Mirrors the
    // walk visitStmt's ExpressionStmt arm performs inline for the §21.9
    // bare-discard check (this file); duplicated rather than shared because
    // that walk is not exposed as a callable predicate, but both apply the
    // same §28.9 test.
    bool isNonTrivialDisposeClass(TypeSymbol* decl) const;

    // §22.5 v0.184 (issue #1684): is `e` a fresh owned-producing expression —
    // `new T()` or a call to an `owned`-returning method — whose produced
    // class type is non-trivial-dispose (isNonTrivialDisposeClass above)?
    // Scoped to the shapes §22.5's "Permitted default-value expressions"
    // actually admits in a default position (no `this`, no locals reach
    // here, so only `new` and a static-factory call are possible producers).
    // Used to reject such a default on a plain (non-`owned`) parameter — the
    // produced instance would have no owner to discharge it.
    bool exprIsOwnedProducerOfNonTrivialDisposeClass(const ast::Expr& e) const;

    // §15.4 (v0.268, fix #2673): for a BORROWING-FAMILY target (`Array<T>`,
    // `Set<T>`, `LinkedList<T>`, `Stack<T>`, `Queue<T>`, and both the key and
    // value positions of `Dictionary<K, V>`), reject every element expression
    // that is a fresh producer of non-trivial-dispose class type — the
    // temporary is disposed at statement end and the collection holds a
    // dangling borrow from birth. No-op for owning-family targets, for
    // trivial-dispose elements, and for borrows. Delegates the
    // fresh-producer × non-trivial-dispose question to
    // exprIsOwnedProducerOfNonTrivialDisposeClass above rather than
    // re-deriving it. See the .cpp definition for the declined third
    // fresh-producer form (nested literal at a non-trivial-dispose target).
    void checkBorrowingFamilyLiteralElements(
        const ast::CollectionLiteralExpr& cl, const TypeRef* target);

    // stdlib §5 (D7): the source element type T of a LINQ operator/aggregate
    // call whose lambda argument is `(T) -> …` (Where/Select/Count/Any/All/
    // First/OrderBy/…), or null otherwise. Used to set pendingLambdaParamType_
    // before visiting the lambda argument so an untyped LINQ lambda parameter
    // is typed as the element type (its body can then resolve `p.x`).
    TypeRef* linqLambdaElementType(const ast::Expr* callee);

    // stdlib §5 / §9.6: the element type T of a LINQ *source* TypeRef (a
    // collection, lazy IEnumerable sentinel, Range<T>, or string). Factored out
    // of linqLambdaElementType so both a LINQ operator's receiver element and a
    // Zip combiner's `other` element (U) can be derived uniformly. Null when the
    // TypeRef is not a LINQ source.
    TypeRef* linqSourceElementType(TypeRef* recvTy);

    // stdlib §5 Combination (#1845): for a `Zip(other, (p, q) => …)` call, return
    // the combine lambda's two parameter types {T, U} — T = the receiver's source
    // element, U = the `other` argument's source element — so an untyped `(p, q)`
    // combiner gets its parameters typed (its body then resolves and its return
    // type R is known). Empty when not such a call.
    std::vector<TypeRef*> zipCombineLambdaParamTypes(const ast::CallExpr& ce);

    // stdlib §5 (#1124): for an `Aggregate(seed, (acc, x) => …)` call, return the
    // fold lambda's two parameter types {TAccum, T} — TAccum = the seed argument's
    // type, T = the source element type — so an untyped `(acc, x)` fold lambda gets
    // its parameters typed (its body then resolves; otherwise codegen emits a
    // `ret void` against the i32 accumulator return type). Empty when not such a call.
    std::vector<TypeRef*> aggregateFoldLambdaParamTypes(const ast::CallExpr& ce);

    // §2.3: cache of synthetic TypeSymbol objects for always-available
    // built-in classes (Console, Math, Convert, Environment).
    std::unordered_map<std::string, TypeSymbol*> builtinClassCache_;

    // Return (or lazily create) a synthetic TypeSymbol for a built-in class.
    TypeSymbol* builtinClassSym(std::string_view name);

    // §32.7 / stdlib §10: build a Named TypeRef for a value-builtin resolved
    // by NAME (declaration=null, astNode carries the single-part name). This is
    // the shape codegen's layout keys off to lower IntPtr/UIntPtr to a
    // pointer-sized integer (i64 on host) — a builtinClassSym (Class-kind)
    // declaration would instead lower as a reference pointer. Mirrors the
    // `rb.Pointer` → IntPtr synthesis (fix #985).
    TypeRef* namedValueBuiltinRef(std::string_view name);

    // Slice S29: report when a bare identifier inside a field
    // initializer resolves to a non-static member of the current type
    // (an implicit-`this` reference, forbidden by §22.8). Static
    // members, class constants, enum constants, locals, parameters,
    // types, and free names are all permitted.
    void checkNotInstanceMember(const ast::IdentifierExpr& id, Symbol* sym);

    // §27.4 (v0.183, #1408) / A.7 LambdaDestructure: resolve a destructuring
    // lambda parameter's head to its exact positional-record TypeSymbol
    // (checked against `targetType` when the lambda's expected function type
    // supplies one — §27.4 "no subtype test"; when `targetType` is null the
    // head itself supplies the parameter's type), then bind every (possibly
    // nested) slot as a LocalSymbol into `scope`, typed as the corresponding
    // positional field, mirroring the §19.3 case-pattern destructuring desugar
    // (ExprTypecheck.cpp's CaseStmt/CaseExpr DestructuringPattern handling).
    // Returns a Named TypeRef for the resolved record type, or null when a
    // diagnostic was already reported (unresolved head / class head / target
    // mismatch / arity mismatch).
    TypeRef* bindLambdaDestructure(const ast::DestructuringPattern& dp,
                                   TypeRef* targetType, Scope& scope);

    // §17.11 (#951; enum arm added #3044/v0.242): does `default(T)` have NO
    // well-defined zero value? True for a non-nullable reference type
    // (string/class/interface/function), an enum with no declared constant of
    // underlying value 0, and, recursively, a record any of whose positional
    // fields — or an inline array whose element — is such a type. Nullable
    // types, value primitives, value-builtins, and generic-parameter/unresolved
    // refs return false.
    bool defaultIllFormed(const TypeRef* t);

    // fix #3044 (§10.4 / stdlib §9.9, v0.242): the ENUM TypeSymbol an
    // expression NAMES as a type (an `E` identifier, or a nested
    // `Outer.Inner`), or null when the expression is not an enum type name.
    // Used to recognise the compiler-provided `E.FromUnderlyingOrNull(value)`
    // static member — a static call, so a receiver that is an enum *value*
    // deliberately does not match.
    const TypeSymbol* enumTypeNamedBy(const ast::Expr& e);

    // fix #3044 (§17.11 / §3.6 rule 2, v0.242): `default(T)` over a bare TYPE
    // PARAMETER cannot be judged in the generic body — "a generic body
    // type-checks under the weakest assumption its constraints establish" — so
    // §17.11's closing rule defers it: "A substitution whose `default` is not
    // well-defined — an enum with no zero-valued constant included — is
    // rejected at the instantiation site (the §3.6 rule 2 pattern)."
    //
    // Deciding that needs two facts that are discovered at different points in
    // one pass, in either order: WHICH parameters a generic body takes
    // `default(T)` of, and WHAT each instantiation substitutes for them. Both
    // are recorded as they are seen and joined once at the end of the pass
    // (`checkDeferredDefaultInstantiations`), so a call that precedes its
    // callee's declaration in source is checked exactly like one that follows
    // it. (Scanning the callee's body AST at the call site would also be
    // order-independent, but the AST has no uniform child enumeration, so a
    // hand-written walk would silently miss whichever node kinds it forgot.)
    struct DefaultTypeParamUse {
        const void* owner;        // the ast::MethodDecl / ast::TypeDecl
        std::string param;        // the type-parameter name
        SourceLocation loc;       // the `default(T)` site (for the note)
    };
    struct GenericInstantiationSite {
        const void* owner;        // the ast::MethodDecl being instantiated
        std::string param;
        TypeRef* arg = nullptr;   // the substituted type argument
        std::string argName;      // its spelling, for the diagnostic
        SourceLocation loc;       // the instantiation site
    };
    std::vector<DefaultTypeParamUse> defaultTypeParamUses_;
    std::vector<GenericInstantiationSite> genericInstantiationSites_;

    // Record a `default(T)` over the bare type parameter `param`, owned by
    // `owner` (the method when `param` is method-level, else the enclosing
    // type).
    void recordDefaultTypeParamUse(const void* owner, std::string_view param,
                                   SourceLocation loc);
    // Record one type-argument binding of a generic METHOD instantiation.
    void recordGenericInstantiationSite(const void* owner,
                                        std::string_view param, TypeRef* arg,
                                        std::string argName,
                                        SourceLocation loc);
    // §3.6 rule 2 join, run once at the end of the pass.
    void checkDeferredDefaultInstantiations();

    // fix #2911 + fix #2962 (§17.4 / §3.6 rule 2, v0.241): the generic reach of
    // §17.4's "Degenerate value-type targets" paragraph for a METHOD-level type
    // parameter. "When the target is a generic type parameter, the check applies
    // per instantiation (§3.6 rule 2): `x as T` at an inline-array or unit
    // substitution is rejected at the instantiation site."
    //
    // Same two-facts-discovered-in-either-order shape as the #3044 `default(T)`
    // join above, and it REUSES that join's `genericInstantiationSites_`
    // recording rather than adding a second one — the call-site hook already
    // records every binding, "constrained or not: the obligation comes from the
    // BODY, not from the constraint list". Only the body-side obligation is new.
    //
    // Not gated on any constraint: unlike `default(T)`, whose guarantee exists
    // only under `where T: value`, §17.4's rule is unconditional — an
    // inline-array or unit substitution has no admissible `as` result whatever
    // the parameter's bounds say (unit does satisfy `where T: value`).
    struct AsTargetTypeParamUse {
        const void* owner;   // the ast::MethodDecl whose body holds the `as`
        std::string param;   // the type parameter used as the `as` target
        SourceLocation loc;  // the `x as T` site (named in the diagnostic)
    };
    std::vector<AsTargetTypeParamUse> asTargetTypeParamUses_;
    // Record that `owner`'s body uses `param` as an `as` target.
    void recordAsTargetTypeParamUse(const void* owner, std::string_view param,
                                    SourceLocation loc);
    // §3.6 rule 2 join, run once at the end of the pass beside the #3044 one.
    void checkDeferredAsTargetInstantiations();

    // Slice DR1 (§21.7): does control definitely leave this statement
    // list abruptly (return / throw / non-terminating loop) before
    // reaching its end? Conservatively returns true only when certain,
    // so a value-method check that flags `!bodyAlwaysExits` never
    // false-positives on a body that does always exit (modulo the
    // noreturn-call case, which mainstream compilers also flag).
    bool bodyAlwaysExits(
        const std::vector<std::unique_ptr<ast::Stmt>>& body);

    // §29.3: check if a statement list contains any yield return/break
    // (making it an iterator method body that is exempt from DR1).
    static bool stmtsContainYield(
        const std::vector<std::unique_ptr<ast::Stmt>>& body);

    bool stmtAlwaysExits(const ast::Stmt& s);

    bool armBodyAlwaysExits(const ast::CaseArm& arm);

    // §19.6: exhaustiveness analysis for an EXPRESSION-position `case`.
    // In expression position an unmatched value raises MatchFailureException
    // at runtime; the spec says the compiler WARNS (not errors) when it
    // cannot prove exhaustiveness statically. Statement-position `case`
    // falls through with no action and is exempt (handled in CaseStmt). The
    // analysis proves exhaustiveness for the four spec-listed scrutinee
    // shapes (enum / bool / `closed`-rooted class hierarchy / nullable),
    // and treats an unguarded `_` or unconstrained `var x` arm as a
    // catch-all that is always exhaustive.
    void checkCaseExhaustiveness(const ast::CaseExpr& ce, TypeRef* scrutTy);

    // §19.6 / §35.6 (#2129): the shared exhaustiveness VERDICT. Returns true iff
    // `arms` provably cover every value of the scrutinee. Drives BOTH the
    // expression-position warning (checkCaseExhaustiveness, quiet=false) and the
    // statement-position §21.7 totality analysis (stmtAlwaysExits, quiet=true — a
    // case STATEMENT whose arms are exhaustive AND all diverge is total, since an
    // unmatched value in statement position falls through, §19.6). In quiet mode
    // no diagnostic is emitted and the §5.1 pattern-range side-check is skipped.
    bool caseArmsExhaustive(const std::vector<ast::CaseArm>& arms,
                            const ast::Expr* scrutinee, TypeRef* scrutTy,
                            const SourceLocation& warnLoc, bool quiet);

    // §19.6 (v0.196, #1816): compose a leaf class's ROOT INSTANTIATION — the
    // instantiation of the root declaration `root` that `leaf` derives from,
    // obtained by composing the base-clause substitutions along `leaf`'s
    // single-inheritance derivation chain (unique per §19.6). The result is a
    // Named TypeRef of `root` whose generic args are expressed in LEAF's own
    // type parameters: for `IntBox: Box<int>` it is `Box<int>`; for
    // `L<Y>: M<Y>` with `M<Z>: D<Array<Z>>` it is `D<Array<Y>>`. Returns null
    // when the chain cannot be composed (a missing/unresolved base ref, a
    // generic base link with mismatched arity, a cycle, or `root` not on the
    // class chain) — the caller then falls back to today's name-match. Reads
    // baseTypeRefs (parallel to baseTypes, carrying generic args) and reuses
    // substituteReturnType as the per-level substitution engine.
    TypeRef* leafRootInstantiation(TypeSymbol* leaf, TypeSymbol* root);

    // §19.3/§5.1/§12.9 (v0.196, #1913): project the Named type `sub` — whose
    // declaration is `ancDecl` or a CLASS-chain subtype of it — up to its
    // instantiation of `ancDecl`, expressed in `sub`'s own concrete generic
    // arguments (`Ok<string>` up to `Result` is `Result<string>`). Composes
    // leafRootInstantiation (which walks the class chain) and substitutes
    // `sub`'s actual args. Returns null when the chain cannot be composed or
    // `ancDecl` is not reached as a class ancestor (an interface ancestor, an
    // arity mismatch) — the caller then conservatively admits the pattern.
    TypeRef* projectTypeToAncestorDecl(const TypeRef* sub, TypeSymbol* ancDecl);

    // §19.6 v0.196 (#1874/#1912): does an unguarded typed binding `var x: bt`
    // cover EVERY non-null value of a scrutinee whose instantiation, projected
    // to `bt`'s declaration, is `proj`? (In the same-declaration case `proj` is
    // the scrutinee itself; in the cross-declaration/ancestor case it is the
    // scrutinee projected up the class chain, cf. projectTypeToAncestorDecl.)
    // Arg-SENSITIVE per §12.9: variance-compatible AND — because the "covers all
    // values" proof must hold at EVERY instantiation of the enclosing body
    // (§31.2.1) — a RIGID scrutinee argument is covered only by that SAME rigid
    // parameter, never a ground or other arg. Shared by the same-declaration and
    // cross-declaration disjuncts of checkCaseExhaustiveness's #784 block.
    bool typedBindingCoversAllValues(const TypeRef* proj, const TypeRef* bt);

    // §19.3/§19.5/§5.1/§12.9 (v0.196, #1913): is the typed binding pattern
    // `var x: patTy` a type-test that NO runtime value of scrutinee type
    // `scrutTy` could ever satisfy — because its generic INSTANTIATION is
    // incompatible with the scrutinee? A typed binding binds the matched value
    // at the STATIC type `patTy`, but the runtime type-test erases generic
    // arguments (checks only the class tag), so a wrong instantiation
    // (`var x: Ok<string>` at scrutinee `Result<int>`) would match at runtime and
    // bind `Ok<int>` as `Ok<string>` — a type-safety hole (§5.1: `Result<int>`
    // is not assignable to `Result<string>`; class hierarchies are invariant,
    // §12.9). This is the ADMISSION counterpart of the arg-SENSITIVE §19.6
    // coverage the #1874 exhaustiveness fix installed. Returns true (=> the
    // caller REJECTS) only when incompatibility is PROVABLE on ground structure:
    // `scrutTy` and `patTy` project (through the class chain) to a common CLASS
    // declaration whose invariant arguments are disjoint and neither
    // reference-converts to the other under §12.9. A rigid/free type parameter,
    // an uncomposable chain, an interface common declaration, an unrelated
    // declaration, or any non-class/interface shape yields a CONSERVATIVE accept
    // (false) — never an over-rejection.
    bool typeTestGenericInstantiationImpossible(const TypeRef* scrutTy,
                                                const TypeRef* patTy);

    // §19.3/§5.1/§12.9 (v0.197, #1920): collect every instantiation of the
    // interface declaration `ifaceDecl` that the Named type `classTy` (a class
    // or interface carrying concrete generic args) implements or extends, each
    // re-expressed in `classTy`'s OWN concrete arguments (`Box<string>` over
    // `Box<T>: IBox<T>` yields `IBox<string>`). BFS over the ancestor graph
    // (single class base + interface bases), substituting the carried concrete
    // args at each level; a declaration reached at TWO distinct instantiations
    // (`C<T>: I<T>, I<int>` — the implemented-twice shape) contributes BOTH
    // entries, deduplicated only by structural identity so diamonds are bounded
    // without merging distinct instantiations. Appends to `out`.
    void collectAncestorInstantiationsOf(const TypeRef* classTy,
                                         TypeSymbol* ifaceDecl,
                                         std::vector<TypeRef*>& out);

    // §19.3/§19.5/§5.1/§12.9 (v0.197, #1920): the INTERFACE-scrutinee
    // counterpart of typeTestGenericInstantiationImpossible. Is the typed
    // binding `var x: patTy` (a class pattern) a type-test that NO runtime value
    // of the generic-interface scrutinee `scrutTy` (`IBox<int>`) could satisfy —
    // because none of the pattern class's implemented instantiations of that
    // interface is compatible with the scrutinee (`Box<string>` implements only
    // `IBox<string>`, disjoint from `IBox<int>` under §12.9)? The erased runtime
    // type-test checks only the class tag, so such a binding would match and
    // bind at a wrong static type. Returns true (=> the caller REJECTS) only for
    // a FINAL pattern class (plain `sealed`, non-abstract — no subclass can add
    // a compatible implementation, §12.4) whose FULL implemented-interface set
    // is disjoint from the scrutinee. A non-final class, an interface pattern, a
    // non-generic or non-interface scrutinee, a class that does not implement
    // the interface at all, or any compatible (incl. rigid/free-parameter)
    // instantiation yields a CONSERVATIVE accept (false) — never an
    // over-rejection.
    bool typeTestInterfaceInstantiationImpossible(const TypeRef* scrutTy,
                                                  const TypeRef* patTy);

    // §19.3/§19.5/§5.1/§12.9 (v0.197, #1927; narrowed/refined #1946): admission
    // of a typed binding `var x: C<A>` (a class pattern) against a generic-
    // interface scrutinee `I<B>`. Returns which rejection (if any) applies:
    enum class IfaceBindReject {
        None,          // admit — the arm is live and soundly matched
        Unreachable,   // C<A> provably cannot conform to I<B> — a DEAD arm (#1920's
                       // non-final counterpart)
        ErasedAmbiguous  // C<A> conforms, but its differing type argument is a
                         // still-ERASED kind (#1939: a function type, inline array,
                         // …) so the runtime `is C<A>` degrades to the class-tag-only
                         // test, AND another instantiation of C also conforms — the
                         // erased test would bind a `C<Q≠A>` value at C<A>.
    };
    // Post-#1843 the runtime `is C<A>` test is PRECISE when every type argument of
    // A carries a runtime identity token (descArgsConcrete / typeRefIdentityToken:
    // a concrete class/record/enum/primitive, recursively) — it then matches only
    // genuine C<A> values, so a live conforming arm is admitted even if C
    // implements I twice (the #1927 over-rejection #1946 removed). It is NOT
    // precise for a still-erased #1939 arg kind (function type, inline array, …):
    // the descriptor collapses siblings, so an implemented-twice / free-parameter
    // sibling would mis-bind — those keep the old uniqueness rejection. An
    // INVARIANT scrutinee interface only (a variant one admits a subtype range — a
    // residual); a rigid/free scrutinee or pattern argument, an imported interface
    // of unknown variance, a non-generic pattern class, or any undecidable match
    // yields a CONSERVATIVE admit (None) — never an over-rejection.
    IfaceBindReject typeTestInterfaceBindAmbiguous(const TypeRef* scrutTy,
                                                   const TypeRef* patTy);
    // Mirror of codegen typeRefIdentityToken/descArgsConcrete: does `t` carry a
    // runtime identity token all the way down (concrete Named/primitive only), so
    // the per-monomorphization descriptor (#1843) distinguishes it from a sibling?
    static bool typeArgRuntimeReified(const TypeRef* t);

    // §19.6 (v0.196, #1874): does an ANCESTOR arm of type `armTy` (an
    // instantiation of `matchTy`, a strict ancestor of the DETERMINED generic
    // leaf `leaf`) cover `leaf`'s sole inhabiting instantiation `leaf<σ>`? The
    // subtype walk (§19.3) lets a pattern naming an ancestor cover a leaf
    // descendant, but for a determined generic leaf that cover is arg-SENSITIVE:
    // the leaf is covered only "by an unguarded type pattern it is assignable to"
    // (§19.6), so the leaf's determined instantiation projected up to `matchTy`
    // (leafRootInstantiation(leaf, matchTy) substituted by σ = `solution`) must
    // be IDENTICAL to `armTy` component-wise. A wrong-instantiation ancestor arm
    // (`Result<string>` at scrutinee `Result<int>`) does NOT cover the determined
    // `Ok<int>`/`Err<int>`, closing the residual the direct-path #1862 fix left
    // in the ancestor branch; a bare NamedPattern ancestor (armTy == nullptr)
    // carries no instantiation and never discharges a determined generic leaf.
    bool ancestorArmCoversDeterminedLeaf(
        const TypeRef* armTy, TypeSymbol* leaf, TypeSymbol* matchTy,
        const std::unordered_map<std::string_view, TypeRef*>& solution);

    // §19.6 (v0.196, #1816 s2): the determination outcome of solving a closed-
    // hierarchy leaf's type parameters against a GROUND scrutinee `S` — the
    // multiplicity of the leaf's inhabiting instantiations at `S`. `NoSolution`
    // (empty set) ⇒ the leaf is uninhabited at `S` and EXEMPT from coverage;
    // `Determined` (a singleton) ⇒ the leaf's obligation is the single admitted
    // instantiation carried in `solution`; `Undetermined` (>1, or undecidable at
    // this decidability level) ⇒ leaf coverage is not pinned here (later lanes).
    struct LeafInstSolutions {
        enum class Outcome { NoSolution, Determined, Undetermined };
        Outcome outcome = Outcome::Undetermined;
        // The sole inhabiting substitution (leaf-param name -> ground TypeRef),
        // populated only when `outcome == Determined`.
        std::unordered_map<std::string_view, TypeRef*> solution;
        // §19.6 (v0.196, #1816 s3): the leaf parameters left FREE after a clean
        // solve — those with NO occurrence in the root instantiation. Populated
        // (in declaration order) only in the free-parameter `Undetermined` sub-
        // case; empty for the undecidable (InvMatch::Unknown) `Undetermined`
        // sub-case. A non-empty list marks the leaf's coverage statically
        // UNATTAINABLE (every substitution inhabits — the opposite of exempt):
        // no type-test arm covers it, only an irrefutable arm does.
        std::vector<std::string_view> freeParams;
        // §19.6 (v0.196, #1816 s4): set by the RIGID-scrutinee solver when the
        // leaf's coverage is statically UNATTAINABLE — either an undecidable
        // rigid-identity component (a ground/composite/different-rigid component
        // of the root instantiation equated against a rigid enclosing parameter,
        // e.g. `IntError: Result<int>` at `Result<T>`) or a free leaf parameter.
        // The leaf is NEVER exempt under a rigid `S`; when this is set the caller
        // records it in the unattainable set (naming `freeParams`, possibly
        // empty). Defaults false — the ground solver never sets it, so s1/s2/s3
        // are undisturbed.
        bool unattainable = false;
    };

    // §19.6 (v0.196, #1816 s2): solve `leaf`'s type parameters against a GROUND
    // scrutinee `scrutTy` (the caller passes the NON-NULLABLE form of `S`).
    // Composes `leaf`'s root instantiation (leafRootInstantiation) and unifies it
    // against `scrutTy` COMPONENT-WISE, INVARIANT, EXACT — no §5.1 conversion and
    // no §12.9 variance step, the syntactic/decidable image of the runtime test.
    // A structural mismatch (distinct ground constructors or ground arguments) or
    // a repeated-parameter inconsistency (`L<Y>: D<Y,Y>` against `D<int,string>`)
    // yields NoSolution ⇒ exempt; a clean solve binding every leaf parameter
    // yields Determined; a leaf parameter left unbound (no occurrence in the root
    // instantiation) or any undecidable component yields Undetermined. The caller
    // MUST gate this on `isFullyGroundTypeRef(scrutTy)` — a rigid `S` is a later
    // lane's domain and this solver does not engage on it.
    LeafInstSolutions solveLeafInstantiations(TypeSymbol* leaf, TypeSymbol* root,
                                              const TypeRef* scrutTy);

    // §19.6 (v0.196, #1816 s4): the NON-GROUND (rigid) counterpart of
    // solveLeafInstantiations — engaged when the scrutinee `S` contains a rigid
    // enclosing-body type parameter (`isFullyGroundTypeRef(scrutTy)` is false),
    // the case the ground solver defers. Matches the leaf's composed root
    // instantiation against `S` under a RIGID-AWARE three-valued identity that
    // keys rigidity SYNTACTICALLY (isInScopeTypeParam — never null-decl
    // typeRefEqual, whose blindness would wrongly solve a rigid equation and
    // reopen the false-exemption hole). A leaf is NEVER exempt under a rigid `S`:
    // an undecidable rigid-identity component (`IntError: Result<int>` at
    // `Result<T>`) or a free leaf parameter sets `unattainable` (coverage
    // statically unattainable — requires a `_` arm, §19.6); a clean determined
    // solve (`Ok<T>: Result<T>` at `Result<T>` ⇒ `T`) is left for the name-match
    // to discharge, so the determined rigid shape stays exhaustive.
    LeafInstSolutions solveLeafInstantiationsRigid(TypeSymbol* leaf,
                                                   TypeSymbol* root,
                                                   const TypeRef* scrutTy);

    // §19.6 (v0.196, #1816 s5): CLAUSE (b) of the determination rule — FILTER
    // the ground solver's sole (Determined) solution `solution` by the leaf's
    // own + every chain-base's SCALAR `where`-constraints (`class` / `value` /
    // `notnull` / `new`; a type-bound `where Y: I` is s6). Returns true iff
    // `solution` PROVABLY violates such a constraint on some decl along the
    // `leaf` → `root` derivation chain — then σ is not a well-formed
    // instantiation (§14.2), the singleton solution set empties, and the caller
    // marks the leaf uninhabited/EXEMPT. Only a PROVABLE violation drops the
    // solution; a satisfied or undecidable constraint keeps it (never a false
    // exemption of an inhabited leaf).
    bool leafSolutionViolatesScalarConstraint(
        TypeSymbol* leaf, TypeSymbol* root,
        const std::unordered_map<std::string_view, TypeRef*>& solution);

    // §19.6 (v0.196, #1816 s5): does a FULLY-GROUND solved type PROVABLY satisfy
    // a `new` constraint (an accessible parameterless ctor / a well-defined
    // default)? Mirrors Resolver::argSatisfiesNew (§14.2 / §17.11). `decided` is
    // set false when unverifiable here — the `new` filter then keeps the
    // solution (never a false exemption).
    bool groundSatisfiesNew(const TypeRef* arg, bool& decided);

    // §19.6 (v0.196, #1816 s6): does a FULLY-GROUND solved type PROVABLY satisfy
    // a type-bound `where Y: I` (§14.2)? Discharged by declaration-based
    // subtyping (isSubtype) on an already-GROUND σ(Y) — a free parameter is
    // NEVER rescued by a census of the bound's conformers (the §19.6 census
    // prohibition). `decided` is set false when unverifiable here — the filter
    // then keeps the solution (never a false exemption; declines D8).
    bool groundSatisfiesTypeBound(const TypeRef* arg, const TypeRef* bound,
                                  bool& decided);

    // §5.1 / §19 (#1153): range-check each integer `case` pattern constant /
    // range-bound literal against an integer scrutinee's type. A literal that
    // does not fit was silently truncated to the scrutinee width at codegen
    // (litAtWidth) → wrong matches; reject it with the §5.1 narrowing
    // diagnostic instead. Engaged from both the CaseExpr and CaseStmt handlers.
    void checkCasePatternConstantRanges(const std::vector<ast::CaseArm>& arms,
                                        TypeRef* scrutTy);

    // §26.4 (v0.277, #3336): per-`where`-clause-item applicability test for
    // an extension block. Returns true ONLY on a PROVABLE violation
    // (decided = true && !ok); a satisfied/undecidable item keeps. The lookup
    // is intentionally a SILENT applicability filter (§26.5.1 "A `where`
    // clause is an *applicability* filter, never a ranking signal").
    bool extConstraintItemProvablyViolated(
        const ast::ConstraintItem& item, const TypeRef* arg);
    // §26.4 (v0.277, #3336): per-extension-block applicability test. Walks
    // every `where T: c1, c2` clause and drops the block on any provable item
    // violation. `binding` is the block-level type-param → concrete TypeRef
    // substitution built by collectFreeVarBinding.
    bool extWhereClauseProvablyViolated(
        const ast::ExtensionDecl& decl,
        const std::vector<std::pair<std::string, TypeRef*>>& binding);

    // §19.3 (#1847): validate a TOP-LEVEL bare-identifier `case`/`when` pattern.
    // The grammar leaves a single-/qualified-identifier pattern ambiguous (§A.7):
    // it is a TYPE TEST when the name resolves to a type (or a type parameter,
    // v0.182), a CONSTANT pattern when it resolves to a named `const`, and an
    // ENUM-CONSTANT pattern when it names a constant of the scrutinee enum. A
    // bare identifier that resolves to none of these — a runtime local/parameter/
    // field VALUE, or an entirely unresolved name — is neither a type nor a
    // constant and must be a compile error. Sema previously let it through and
    // codegen's NamedPattern handler returned nullptr ("deferred"): in statement
    // position the arm silently vanished (the mandatory `_` catch-all skipped,
    // §19.6); in expression position `case.end` was left unterminated (malformed
    // IR / ICE). Engaged for a top-level NamedPattern from both the CaseExpr and
    // CaseStmt handlers.
    void checkTopLevelNamedPattern(const ast::NamedPattern* np, TypeRef* scrutTy);

    // §19.3 (#1847): shared diagnostic for a top-level bare-identifier pattern
    // that resolves to a runtime VALUE (a local, parameter, or field) rather
    // than a type or constant.
    void reportNamedPatternIsValue(const ast::NamedPattern* np,
                                   std::string_view name);

    // §35.5 (v0.200, #2045): find a choice case by bare name on a choice
    // scrutinee's declaration. Returns null when `scrutTy` is not a choice
    // or no case with that name exists. Used by checkTopLevelNamedPattern
    // (payload-free accept / payload-carrying reject) and the CaseExpr /
    // CaseStmt DestructuringPattern handlers (positional payload matching).
    const ChoiceCaseSymbol* findChoiceCaseOnScrutinee(TypeRef* scrutTy,
                                                      std::string_view name);

    // #2219: when a `case` destructuring pattern binds a slot on a GENERIC
    // choice case (e.g. `case r of Good(v) => …` over `Result<int, string>`),
    // the case's payload field's TYPE may name a generic parameter `T` of the
    // choice declaration. That bare `T` must be substituted by the SCRUTINEE's
    // generic argument (`int`) so the slot binding is correctly typed and the
    // arms' common-type computation sees the substituted form. Walks `ty`
    // and substitutes every bare-name TypeRef whose declaration is a
    // GenericParamSymbol of `scrutTy->declaration` with the corresponding
    // entry from `scrutTy->genericArgs`. Recurses into Named genericArgs,
    // InlineArray element types, Function parameter/return types, and the
    // nullable wrapper. Returns `ty` unchanged when no substitution applies.
    // Mirrors (the field-walk part of) Resolver::applyGenericSubst +
    // buildBaseGenericSubst — local copy because the ExprTC worker has no
    // Resolver handle (#1621 / §31.2.1 split).
    TypeRef* substituteGenericCaseFieldType(const TypeRef* ty,
                                            const TypeRef* scrutTy);

    // Slice S21: does a callable with this return-type ascription
    // produce a value? No ascription → no (unit). An ascription that
    // resolves to the unit type → no. Anything else → yes.
    bool returnsValue(const ast::Type* rt);

    // async-readiness 3b (§30.1): compute the return-FORM context and the
    // result-assignability type for an `async` method's body by peeling the
    // task wrapper. `Task` → VoidMethod (no result); `Task<T>` / `OwningTask<T>`
    // → ValueMethod with result type T; everything else → VoidMethod.
    void asyncReturnCtxAndType(const ast::Type* rt, ReturnCtx& outCtx,
                               TypeRef*& outType);

    // Slice S21 (§21.7) return-statement form rules:
    //  - constructor / destructor: `return expression;` is an error.
    //  - value-returning method: bare `return;` is an error.
    //  - unit method: `return expression;` is an error.
    // The "every exit path returns" coverage rule needs flow analysis
    // and is left to a definite-return slice.
    void checkReturnForm(const ast::ReturnStmt& rs);

    // Slice T1: §5.1 numeric/char implicit-widening table. Returns
    // true iff `from` implicitly widens to `to` (both primitives,
    // distinct). Identity is handled by the caller.
    static bool primitiveWidens(PrimitiveType from, PrimitiveType to);

    // Slice T4: is `from` a subtype of `to` — i.e. does `to` appear in
    // `from`'s transitive base-type chain (base class or implemented
    // interface)? BFS, cycle-guarded.
    static bool isSubtype(TypeSymbol* from, TypeSymbol* to);

    // #2782 / #2787: does the INTERFACE `argTy` satisfy `where T:
    // IComparable<T>`? Delegates to sema::selfComparableIface — THE one
    // definition of this predicate (TypeResolution.hpp). This was one of three
    // hand-rolled copies until #2787 collapsed them; do not re-inline it.
    static bool selfComparableIfaceArg(const TypeRef* argTy,
                                       const TypeSymbol* icomparable) {
        return selfComparableIface(argTy, icomparable);
    }

    // §8.6 / §5.1 (#1311): does a primitive value type box into a builtin
    // generic interface it implements — IComparable<T>/IEquatable<T> where T
    // is the primitive's own keyword alias (stdlib §9.1–§9.5: every numeric
    // and char primitive implements both; Boolean implements IEquatable<bool>
    // only, NOT IComparable — §9.4)? Recognised by: `to` is a single-arg
    // IComparable/IEquatable whose argument is the SAME primitive as `fromPrim`
    // (no widening — Byte implements IComparable<byte>, not IComparable<int>).
    // Used by checkAssignable (storage boxing) and implicitlyConvertible
    // (argument-to-parameter boxing) so the two paths agree.
    static bool primitiveBoxesIntoBuiltinIface(PrimitiveType fromPrim,
                                               const TypeRef* to);

    // Slices T1/T4: assignability check (§5.1, §7, §12 subtyping).
    // Handles two cases and conservatively skips everything else
    // (mixed primitive/named, generics, null, Object target) so it
    // never false-positives while the type lattice is incomplete:
    //   - primitive → primitive: identity or §5.1 widening.
    //   - named → named (non-generic): identity or subclass/interface.
    // A non-nullable target accepting a nullable source is rejected.
    void checkAssignable(TypeRef* from, TypeRef* to,
                         SourceLocation loc, const std::string& what,
                         const ast::Expr* fromExpr = nullptr);

    // §5.1 "Constant narrowing" (#2925) — the ONE definition of the rule, for
    // every §5.1 conversion site. Verbatim (v0.240):
    //
    //   "**Constant narrowing.** An expression of integer type that is a
    //    **compile-time constant** ([§17.14]) implicitly converts to any
    //    integer type whose range contains its value: `var b: byte := 50;`,
    //    `Set(level: byte)` called as `Set(3)`, and enum constants of the
    //    underlying type ([§10.2]) are all well-typed by this rule. A constant
    //    whose value does not fit the target's range is a **compile error** at
    //    the conversion site — never a silent truncation. The rule applies to
    //    constants only; a run-time `int` value still requires the explicit
    //    `byte(x)` narrowing of [§5.2]. Collection-literal element typing
    //    ([§15.4]) and default-argument typing use this same rule."
    //
    // Re-types `fromExpr` to `to` when the rule applies and returns the new
    // source type; returns `from` unchanged otherwise (an out-of-range constant
    // and a non-constant both fall through to checkAssignable's §5.1
    // rejection — never a silent truncation). Called from checkAssignable
    // itself so the POPULATION is "every §5.1 conversion site", not a
    // hand-maintained list of statement kinds.
    TypeRef* narrowIntConstantToTarget(const ast::Expr* fromExpr,
                                       TypeRef* from, TypeRef* to);

    // §14.1 (#2926): does `t` mention a generic type parameter that is still
    // UNSUBSTITUTED at this point — one of the enclosing type's (currentType_)
    // or the enclosing method's (currentMethodDecl_)? Such a slot's real type
    // is only known at the instantiation, so `checkAssignable`'s contract —
    // "conservatively skips everything else … so it never false-positives
    // while the type lattice is incomplete" — says do not judge it here.
    // Uses the SUPERSET walk `refOutParamTypeMentionsTypeParam`, the same one
    // §22.3's ref/out deferral uses for the same reason (#1756: a narrower
    // guard is what made that check false-reject and get reverted four times).
    bool mentionsUnsubstitutedTypeParam(const TypeRef* t) const;

    // Slice S18: a condition must be `bool` (§21.2 if, §21.3 while,
    // §20 if-expression). Only flags when the condition has a resolved
    // primitive type that is not bool — conditions whose type isn't yet
    // inferred (unresolved identifiers, un-typed calls) are left alone
    // so this pass never produces false positives on incomplete typing.
    void checkConditionIsBool(const ast::Expr& cond, std::string_view ctx);

    // §21.5 (v0.201, #2051): reject a loop label that duplicates the label of an
    // enclosing loop in the same callable ("labels do not shadow"). No-op for an
    // unlabeled loop (empty `label`). Call before pushing onto loopLabelStack_.
    void checkLoopLabelShadow(std::string_view label, SourceLocation loc);
    // §21.5 (v0.201): resolve a `break`/`continue` target. `label` empty is the
    // unlabeled form (innermost loop, offset 0). A labeled form resolves against
    // the enclosing loop labels: unknown → error naming the labels in scope;
    // otherwise sets `offsetOut` to the nesting distance from the jump to the
    // target (0 = innermost) for codegen. Applies the §31.2 finally-boundary rule
    // to the resolved target (labeled or not). Precondition: loopDepth_ > 0.
    void checkLoopJumpTarget(std::string_view label, bool isBreak,
                             int& offsetOut, SourceLocation loc);

    // §20: type the `if` expression `x if c else y` with the COMMON type
    // of its two branches and record it on `e`. A branch whose type is
    // null (a bottom-typed `throw`/diverging expression) imposes no
    // constraint — the result type is the other branch's. When both
    // branches have resolved types, the common type is the one the other
    // widens/subtypes into; if neither converts to the other (e.g. a
    // primitive vs a class, as in the `throw E if c else x` shape), no
    // common type exists and a clean §20 type error is reported instead
    // of letting codegen build an ill-typed PHI and crash. Returns the
    // common TypeRef (already set on `e`), or null when none exists.
    void typeConditional(const ast::ConditionalExpr& cnd, const ast::Expr& e);

    // §19 + §20: type a `case` EXPRESSION with the COMMON type of its
    // (non-diverging) arm bodies and record it on `e`. Mirrors
    // typeConditional but folds over N arms: a bottom-typed (`throw` /
    // diverging) arm leaves its body's type null and imposes no constraint;
    // the result is the common type of the value-producing arms. When two
    // value arms have no common type (neither widens/subtypes into the
    // other), a clean §20 error is reported instead of letting codegen build
    // an ill-typed result PHI and crash. When EVERY arm diverges, the case
    // produces no value and no type is recorded.
    void typeCase(const ast::CaseExpr& cse, const ast::Expr& e);

    // §20: the pairwise common type of two branch/arm types — the one the
    // other widens (§5.1) or subtypes (§12) into. Returns null when neither
    // converts to the other (no common type). Shared by typeConditional and
    // typeCase so the `if`-expression and `case`-expression common-type
    // rules stay identical. Conservative (mirrors checkAssignable) so it
    // never false-positives while the type lattice is incomplete; an
    // Error-kind input yields the other operand (don't cascade), and an
    // `Object` operand is the universal common supertype.
    TypeRef* commonTypeOf(TypeRef* a, TypeRef* b);

    // §31.2.1: nearest common base of the types listed in a multi-type catch
    // arm binding (`var e: T1, T2, ...`). Returns the TypeRef to use as the
    // binding's static type: the most specific Named type that is a base of
    // every listed type. When no user-declared common base exists (e.g. all
    // types directly inherit a built-in like `Exception`), the built-in base
    // TypeRef is returned. Returns null if the types are empty or unresolved.
    TypeRef* catchArmNcb(
        const std::vector<std::unique_ptr<ast::Type>>& types);

    // #1621 / §31.2.1 / §20: the simple name of a Named TypeRef — its
    // TypeSymbol's name when declared, else the AST use-site's last
    // qualified-name part (for a built-in with no TypeSymbol, e.g.
    // `IOException`), else `importedName` (a synthetic built-in-ancestor
    // TypeRef minted by `builtinAncestorTypeRef`, which has neither). Empty
    // for a non-Named ref. Shared by `commonTypeOf`'s `convertsInto` and
    // `catchArmNcb`'s ancestor walk so both name a type identically.
    static std::string_view namedRefSimpleName(const TypeRef* ref);

    // #1621 / §31.2.1: mint a headless Named TypeRef standing for a built-in
    // exception ancestor name reached only through
    // `common::builtinExceptionParentOf` (no AST node names it directly —
    // e.g. `Exception` reached while walking up from `OverflowException`).
    // `declaration` stays null (it IS a built-in) and `importedName` carries
    // the name so `namedRefSimpleName` still reports it correctly.
    TypeRef* builtinAncestorTypeRef(std::string_view name);

    // §31.4 (v0.240, #2971): the type of `Exception.CauseChain` — a synthetic
    // `Array<ExceptionInfo>` over program-lifetime static astNodes (the pattern
    // `makeConcreteCollectionResult` uses): `Array` so `.Count` and indexing
    // resolve, an `ExceptionInfo` element so `chain[i].TypeName` type-checks.
    // Neither type has a user TypeSymbol.
    //
    // Shared rather than rebuilt: #2971 gave the SAME §31.4 member three more
    // resolution sites (a subclass's own static type, `this.`, `base.`) and a
    // second copy of this builder would be a second place for the element type
    // to drift from the receiver's.
    TypeRef* excCauseChainTypeRef();

    // #1621 / §31.2.1: the ancestor-name chain of `ty` (most-specific first,
    // `ty` itself included), walking the TypeSymbol base-type chain, the AST
    // TypeDecl's declared bases (built-in bases have no TypeSymbol and are
    // only visible here), AND — once a walked node is itself a built-in
    // Named type with no further AST bases — the built-in exception
    // hierarchy table (`common::builtinExceptionParentOf`), so a purely
    // built-in ancestor chain (e.g. `OverflowException` ->
    // `ArithmeticException` -> `Exception`) is still discoverable. Bounded:
    // a node is "visited" the first time its name (or, for an unnamed node,
    // its pointer) is seen, and the guard gates the WALK itself, so a §12.10
    // base-type cycle that reaches this walk during error recovery cannot
    // loop forever (#1624 precedent); a hard iteration cap is defense in
    // depth.
    std::vector<std::pair<std::string_view, TypeRef*>> namedAncestorChain(
        TypeRef* ty);

    // #1621 / §31.2.1: does `ty` (or one of its ancestors, per
    // `namedAncestorChain`) carry the simple name `ancestorName`? Used by
    // both `convertsInto` (does `from` subtype/widen into `to`, by name, when
    // one side is a built-in with no TypeSymbol to run `isSubtype` on) and
    // `catchArmNcb`'s Step 2 (does every OTHER listed type also have this
    // candidate ancestor?).
    bool hasNamedAncestor(TypeRef* ty, std::string_view ancestorName);

    // §11 / §12.9 / §5.1 (#1935): does the USER-declared type `from` actually
    // implement (directly or transitively) a BUILTIN interface whose simple name
    // is `builtinBaseName`? Interface conformance in Bezel is nominal — a type
    // conforms only by listing the interface among its bases — so a builtin base
    // absent from `from`'s transitive declared-base closure is PROOF `from` does
    // not conform. A builtin interface base carries a null declaration and is
    // therefore dropped from `baseTypeRefs` (TypeResolution records only bases
    // with a resolved TypeSymbol); it survives only on the SYNTACTIC AST base
    // list, and its own builtin supers (IList<T> → … → IEnumerable<T>) live only
    // in the hardcoded builtin interface hierarchy. This walk consults all three
    // — `baseTypeRefs`, the AST base list, and `builtinInterfaceAncestors` on any
    // builtin node — so a class implementing a more-derived builtin interface
    // still counts as implementing its builtin supers. Used by `checkAssignable`
    // to REJECT a user-declared source assigned to a builtin generic interface it
    // does not implement, instead of `variantGenericAssignable`'s conservative
    // user→builtin residual accept.
    bool userTypeImplementsBuiltinIface(TypeRef* from,
                                        std::string_view builtinBaseName);

    // §22.8 / §23.1 (v0.270, #3299/#3316): per-instance narrowing of the
    // readonly assignable context. The pre-fix code used a bare
    // `if (inConstructor_) return;` (and its `:=` / ref/out / property
    // siblings), letting any constructor body write any other instance's
    // readonly field. §22.8 v0.270: "The context is per-instance, not
    // per-type: an instance readonly field is assignable only through an
    // lvalue chain rooted at `this` — an explicit `this.F := e` or the
    // bare in-scope spelling that resolves to `this`'s storage — so a
    // constructor of the declaring type may NOT write another value's
    // readonly storage held in a parameter, local, or field." Returns
    // true iff the chain rooted at `target`'s base is `this` (explicit
    // or bare-implicit-this). Used by the four construction-window
    // carve-out sites to narrow the instance-readonly carve-out from
    // `inConstructor_` alone to `inConstructor_ && thisRooted`; the
    // static-readonly carve-out is preserved unchanged (per-instance vs
    // per-type does not reach it, v0.270 records the case as open).
    // Mirrors PureBodyWalker::targetRootSymbol's chain-walk shape but
    // adds the bare-implicit-this spelling check (FieldSymbol of
    // currentType_) the PureChecker walk does not.
    bool isThisRootedLValueTarget(const ast::Expr& target) const;

    // Slice S7: diagnose assignment to a readonly field outside the
    // declaring class's constructor. Tracks only the `this.<field>`
    // target shape; `other.f := X` would need cross-object readonly
    // tracking that we don't have yet (and rarely matters in practice).
    void checkReadonlyAssignment(const ast::AssignmentStmt& as);

    // §6.6 / A.6 (v0.184, #1690): reject an assignment/increment `target`
    // whose lvalue chain bases on a method-call result — grammar A.6
    // restricts an LValue's base to `Identifier | "this"`, and §6.6 states
    // "A.6's lvalue shape applies unchanged: a method-call result cannot
    // base the chain — extract it to a local first." Shared by
    // AssignmentStmt and IncrementStmt, which both consume grammar A.6's
    // `LValue` production.
    void checkLValueChainNotCallBased(const ast::Expr& target);

    // §23.3 / §23.1 / §13.6 / §17.6 (v0.240, #2868): the shared
    // property-or-indexer WRITE-TARGET check — no setter, a readonly
    // auto-property written outside its assignable context, or a setter that
    // is not accessible here. Called from BOTH statement forms that write
    // through grammar A.6's `LValue`: AssignmentStmt (`x := e`, `x op= e`)
    // and IncrementStmt (`x++` / `x--`). §17.6 states the increment operand
    // rule in the same terms the assignment target obeys — "The operand must
    // be an assignable lvalue (a local variable, a writable field, an indexer
    // slot, or a property with a setter …). Read-only locations cannot be
    // incremented." — so the two share ONE implementation rather than the
    // hand-maintained mirror that was found missing this check twice (#2309,
    // #2868). `incOp` is null for an assignment target and the operator token
    // ("++"/"--") for an increment target; it selects the diagnostic's verb
    // and adds the §17.6 citation, leaving every assignment message
    // byte-identical to what it was before the extraction.
    void checkPropertyWriteTarget(const ast::Expr& target,
                                  const SourceLocation& loc,
                                  const char* incOp);

    // §22.3 "ref and out argument locations" (v0.184, #1682): validates a
    // bare `ref`/`out` call argument's operand against the section's
    // assignable-storage-location categories — a mutable local variable, a
    // writable field, an indexer slot, or a property with an accessible
    // setter — and its declarative exclusions: no `using var` binding, no
    // readonly field/property outside its assignable context (§22.8/§23.1),
    // and no mutable static admitted only in the two forms of §22.12 rule 2.
    // The section's remaining exclusion — no location of OWNED state (an
    // owned local/parameter, an owned field, an owning-collection/owning-
    // inline-array slot) — is dataflow-dependent (whether a plain local
    // currently holds an owned value) and is checked separately by the
    // ownership pass (MethodOwnership::checkRefOutOwnedOperand), which
    // tracks live ownership state; this function only covers the
    // declaration-based categories. A no-op for every call-argument
    // modifier except the two BARE pre-declared forms "ref" and "out" —
    // "out var"/"out using var"/"out owned var" always declare a FRESH
    // local (no pre-existing-location question), and "owned"/"scoped" are
    // unrelated call-site markers (§22.3 / §28.14).
    void checkRefOutOperandCategory(const ast::CallArgument& a,
                                    std::string_view formalParamModifier = "");
    // Shared by the Identifier and MemberAccess cases of
    // checkRefOutOperandCategory: readonly-outside-context (§22.8) and
    // mutable-static-rule-2 (§22.12) for a field operand.
    void checkRefOutFieldOperand(FieldSymbol& fs, const ast::Expr& target,
                                 const char* kind);
    // Shared by the MemberAccess and Index cases of
    // checkRefOutOperandCategory: no-setter / readonly-auto-outside-context /
    // setter-visibility for a property (incl. indexer) operand — mirrors the
    // §23.1/§23.3/§13.6 assignment-target checks inline in visitStmt.
    void checkRefOutPropertyOperand(PropertySymbol& ps, const ast::Expr& target,
                                    const char* kind);

    // Slice S5: member lookup that walks the base-type chain.
    // BFS over the inheritance graph (no cycle detection — the spec
    // forbids cycles; sema's class-graph validation slice will catch
    // them). First match wins; ambiguity resolution between sibling
    // interfaces is a separate slice.
    Symbol* lookupMember(TypeSymbol* type, std::string_view name);

    // §26.5: extension-member lookup. Consulted ONLY when no instance member
    // matches (instance members win — an extension never shadows a real
    // member). Searches the module's ExtensionSymbols for one whose target
    // type equals `receiver` (or one of its ancestor types, per §26.5 step 2)
    // and that declares a member named `name`; returns that MemberSymbol, or
    // null. `receiver` is the receiver's resolved TypeRef (its nullability is
    // ignored — §26.2 forbids `extension int?`).
    // §26.5 (#714): when a generic-extension method is found (extension has
    // free type variables), the inferred binding (e.g. T → int) is written into
    // `outBinding` so the caller can record it for typeCall and codegen.
    // §26.5.1 rule 1 / §26.5 rule 4 (#1307): when more than one candidate
    // block is applicable (matches `receiver` and declares `name`), the
    // candidates are ranked by subtype specificity on their receiver-
    // substituted heads; a unique most-specific candidate wins, and a tie
    // among two or more equally-specific candidates is a compile error naming
    // each one (defining module, declared head incl. `where` clause,
    // declaration site) — `loc` is the diagnostic's location (the call site)
    // and `reportAmbiguity` lets a pure existence PROBE (see
    // extensionNameBlockedNote) suppress the diagnostic, since a probe must
    // not itself cause the ambiguity error it is only checking for.
    // fix #2328 (§26.5, v0.236): `arity` (default -1 = no filter) narrows
    // the candidate set to extension members whose parameter list has exactly
    // that many parameters — the per-call arity check the §26.5 step-2 lookup
    // previously skipped. A non-negative `arity` does NOT change the
    // specificity ranking: an arity-filtered ambiguity still raises rule 4,
    // and an arity-filtered unique winner is still preferred over an
    // arity-mismatched block. Bare-reference call sites (which have no
    // arity — they produce a Function value) leave it at the default.
    //
    // Parameter order is preserved with the original (receiver, name, outBinding,
    // loc, reportAmbiguity) signature plus the new `arity` at the END, so the
    // existing call sites — which pass `nullptr` for outBinding at the third
    // slot — keep working without edits. The new arity-filtered path used by
    // the §26.5 call-site fix in ExprTypecheck_Calls.cpp passes both an arity
    // AND a non-null outBinding.
    Symbol* lookupExtensionMember(
        const TypeRef* receiver,
        std::string_view name,
        std::vector<std::pair<std::string, TypeRef*>>* outBinding = nullptr,
        SourceLocation loc = SourceLocation{},
        bool reportAmbiguity = true,
        std::ptrdiff_t arity = -1);

    // §26.5 step 1 (v0.183): when an instance member of `name` name-blocks
    // extension lookup for `receiver` and the committed instance call then
    // fails (no applicable overload, wrong arity, or inaccessible), the
    // spec requires the diagnostic to "note a same-named extension where
    // one exists" — a name-blocked extension was never even considered, so
    // without this note a caller cannot tell their extension was shadowed
    // rather than simply not found. Returns an empty string when no
    // same-named extension matches `receiver` (append nothing), else a
    // clause of the form "; note: ..." ready to append to a diagnostic
    // message.
    std::string extensionNameBlockedNote(const TypeRef* receiver,
                                         std::string_view name);

    // §26.1 / §26.5: bind a resolved extension member to the member-access
    // expression — set its exprSymbol and the access's exprType. For a method
    // this is the §27.6 method-reference function type (the CallExpr handler
    // reads the MethodSymbol's decl for a call's return type); for a computed
    // property it is the property's declared type.
    void resolveExtensionMember(const ast::MemberAccessExpr& ma,
                                Symbol* member);

    // §2.7 rule 1 (#2097): resolve a bare name. When `memberBeforeModuleType`
    // is set, an accessible instance member of the enclosing type outranks a
    // same-named MODULE-level declaration (a top-level type) — the implicit-
    // `this.member` reading, used only in VALUE position; locals/params still
    // shadow the member. The default (false) keeps the historical order
    // (lexical scopes, incl. module, then the enclosing-type member as a
    // fallback), which the member-access-HEAD and pattern callers rely on so a
    // dotted head like `Color.Red` / `Data.M()` still resolves its head to the
    // top-level type.
    Symbol* lookupName(std::string_view name,
                       bool memberBeforeModuleType = false);

    // §16.4 / §19.6 (issue #738): per-`case` pattern legality checks shared by
    // the expression and statement case forms — reject `step` in a range
    // pattern (stride has no membership meaning, §16.4) and an unguarded `_`
    // catch-all in a non-last arm (§19.6). Both are clean diagnostics that
    // replace a downstream codegen IR failure / silently-unreachable arms.
    void validateCaseArms(const std::vector<ast::CaseArm>& arms);

    // §19.3 (v0.167, #1371): every member of a `|` pattern-alternative list must
    // resolve to a CONSTANT — a type member is a compile error "in any
    // position", at arm level or inside a positional destructuring slot.
    // `checkAltMembersConstant` walks a pattern (recursing through
    // DestructuringPattern slots and, per §19.3 v0.183 / #1584, AltPattern
    // members) and reports a member that names a type; `altMemberNamesType`
    // resolves whether one member expression denotes a type, `altPartsNameType`
    // the shared dotted-name resolution over an already-split QualifiedName.
    void checkAltMembersConstant(const ast::Pattern* p);
    bool altMemberNamesType(const ast::Expr* c);
    bool altPartsNameType(const std::vector<std::string_view>& parts);

    // §31.1 (issue #739; v0.243 / proposal 0080 / #3046): the operand of `throw`
    // must be an exception — an instance of `Exception` or a class deriving from
    // it. A primitive/string/record/enum/function thrown value also miscompiles
    // (codegen treats it as an exception-object pointer → SIGSEGV); reject every
    // non-`Exception`-family operand with a clean diagnostic naming the
    // requirement and the remedy. No-op when the value's type is not yet
    // resolved.
    void checkThrowableType(const ast::Expr& value);

    // §31.2.1 (v0.243, proposal 0080, #3046): every CLASS type named in a catch
    // arm pattern must be `Exception` or a class deriving from it — any other
    // class type is provably dead (single inheritance + every throwable is
    // `Exception`-derived), and `Object` is rejected for the opposite reason
    // (universally matching; the catch-all's spelling is the bare `Exception`
    // arm). An INTERFACE-typed arm stays admissible. A generic type parameter
    // used as an arm type must carry an `Exception`-derived class bound.
    // `fallback` anchors the diagnostic when the arm type's own span is the
    // degenerate 0:0 every `ast::Type` carries (Parser::parseType sets none).
    void checkCatchArmType(const ast::Type& armTy, SourceLocation fallback);

    // §31.1 / §31.2.1 / §31.4 (v0.243, #3046): the ONE `Exception`-family
    // predicate — a positive list over `common::isBuiltinExceptionName` (the
    // built-ins, which carry no TypeSymbol) and `sema::builtinExceptionBaseOf`
    // (the class-ancestor walk that works for local AND imported classes,
    // #2950). Shared by the throw-operand rule, the arm-type rule, and the
    // generic-bound rule so all three answer identically.
    bool isExceptionDerivedClass(const TypeRef* ty);

    // §14.2 / §31.2.1 (v0.243, #3046): is `name` a generic type parameter of the
    // enclosing method, type, or extension? Only such a declaration-less Named
    // ref is subject to the arm-type generic-bound rule (every user class,
    // local or imported, carries a TypeSymbol).
    bool isEnclosingTypeParamName(std::string_view name) const;

    // §14.2 / §31.2.1 (v0.243, #3046): does the enclosing declaration's
    // constraint set bound `paramName` by an `Exception`-derived CLASS —
    // directly or through a `where T: U` chain? Decided from the declaration
    // alone, never from an instantiation, exactly as §31.2.1 requires.
    bool typeParamHasExceptionBound(std::string_view paramName);

    // §5.1 (issue #738): fold `e` to a compile-time integer constant, resolving
    // `const` identifiers through the typecheck scope. Used to decide whether an
    // int initializer/value may implicitly narrow to a narrower integer target
    // (a literal/arithmetic/const may; a variable or call result may not).
    std::optional<long long> foldIntConstExprTC(const ast::Expr& e);

    // §17.14 (v0.184, #1688): does `e` contain a `/` or `mod` node whose RHS
    // folds to a compile-time zero, using the SAME identifier resolution as
    // foldIntConstExprTC (lexical-scope `lookupName`, not a type's member-
    // const list)? Called when foldIntConstExprTC(e) returns nullopt for a
    // local `const` initializer, to give the specific div/mod-by-zero
    // diagnostic instead of silently accepting the expression (which then
    // compiled and deferred the fault to a runtime DivideByZeroException).
    bool localConstDivByZeroTC(const ast::Expr& e);

    // §6.5 / §17.14 (v0.242, #3045): the shift count's domain is
    // `0 .. width−1` of the LEFT (shifted) operand's type — the operand's own
    // SOURCE type, because §6.5 keeps the count out of §6.2's promotion ("a
    // narrower left operand is *not* widened to match a wider count (`byte <<
    // int` yields `byte`, not `int`)"), so `byte << k` has domain `0..7`.
    // A count that is a compile-time constant (§17.14) outside the domain is a
    // COMPILE ERROR reported here; a non-constant count raises
    // `OverflowException` at runtime (§31.4; codegen's emitShiftCountInDomain).
    // `leftTy` is the left operand's (or, for `<<=`/`>>=`, the target's) type;
    // a nullable `T?` carries the underlying `T`'s domain (§7.11 lifts the
    // operator where `T` defines it). `opSpelling` is the operator as written
    // (`<<`, `>>`, `<<=`, `>>=`) so the compound forms name themselves.
    // `fallbackLoc` is used when the count expression carries no span (a
    // unary-minus node, `x << -1`, has an unset span — the diagnostic would
    // otherwise point at 0:0).
    void checkShiftCountDomain(const TypeRef* leftTy,
                               const ast::Expr& countExpr,
                               std::string_view opSpelling,
                               SourceLocation fallbackLoc);

    // §15.1 (issue #734): fold a LOCAL-`const` inline-array size onto its
    // resolved TypeRef. Member/module consts are folded earlier (type
    // resolution); a local const is only in scope during this pass, so when a
    // local declaration's type is `[N]T` with N still unfolded (arraySizeValue
    // < 0) and N resolves to in-scope local consts, set arraySizeValue so
    // codegen builds `[N x T]` rather than a flat scalar. No-op otherwise.
    void foldLocalConstArraySize(TypeRef* declType);

    // §12.7: collect the flow-narrowings that an `if`/`elif` condition
    // establishes for one branch. `forTrueBranch` selects the controlled
    // (then) block (`x is T` narrows there; `x is not T` does not) versus the
    // `else` branch (`x is not T` narrows there; `x is T` does not). Each
    // narrowing maps the tested variable's resolved symbol to the test type's
    // resolved TypeRef. `and`-joined tests contribute all their narrowings to
    // the true branch (§12.7). The condition must already have been visited so
    // operand symbols and the test type are resolved. Entries are written into
    // `narrowings_`; the caller saves/restores the map for block scoping.
    void collectNarrowings(const ast::Expr* cond, bool forTrueBranch);

    // §12.7 helper: if `cond` is a `<simple var> is T` / `is not T` test that
    // narrows in the requested branch, record the narrowing for the variable's
    // symbol. Returns true iff a narrowing was applied.
    bool narrowFromTypeTest(const ast::Expr* cond, bool forTrueBranch);

    // §7.4 / §8.4 helper: if `cond` is a `<simple var> <> null` / `= null`
    // test that narrows in the requested branch (`<> null` narrows the true
    // branch, `= null` narrows the else branch — and `is not null` / `is null`
    // desugar to these by the parser), record the variable's symbol in
    // `nullNarrowed_` (known non-null). Returns true iff a narrowing was
    // applied. (issue #418)
    bool narrowFromNullTest(const ast::Expr* cond, bool forTrueBranch);

    // §7.4 (v0.214, #2085/#2116) helper: if `cond` is a
    // `<readonly-field path> <> null` / `= null` test whose operand is a
    // trackable readonly-field access path (see readonlyFieldPathOf), record
    // its (rootId, fieldId) key in `fieldNarrowed_` for the narrowing branch.
    // Excluded inside the field's declaring-type construction window (the rule
    // does not apply there — §7.4/§22.8). Returns true iff a field-narrowing
    // fact was recorded.
    bool narrowFromFieldNullTest(const ast::Expr* cond, bool forTrueBranch);

    // §7.4 (v0.214, #2085/#2116): the FIELD-ONLY establishment walk used to
    // (re)seed `fieldNarrowed_` for a branch context without touching
    // `nullNarrowed_`/`narrowings_` — the same condition shapes
    // collectNarrowings covers (a single null test, `and`-composition for the
    // true branch), field facts only. Used by the if-statement walk to give an
    // elif CONDITION its correct entry state (entry + preceding conditions'
    // false-branch facts) before it is visited — the legacy sets skip that
    // reset and leak the preceding branch BODY's state into the condition's
    // gates (pre-existing local hole, filed separately from lane #2116).
    void collectFieldNarrowings(const ast::Expr* cond, bool forTrueBranch);

    // §7.4 (v0.214, #2085/#2116): if `e` denotes a trackable nullable
    // `readonly`-field access path — exactly ONE field off an admitted root
    // (`this`/`base`/bare implicit-this, a NON-GENERIC type name for a static
    // readonly, or a local/parameter for `local._f`) — return its
    // (rootId, fieldId) narrowing key. `assignIds` selects the two modes:
    // TRUE (establishment) assigns fresh dense ids as needed; FALSE
    // (consultation/invalidation) only reuses ids handed out earlier — a path
    // whose ids were never assigned cannot be in `fieldNarrowed_`, so nullopt
    // is returned without mutating the id table (keeps consultation
    // side-effect-free). Recognition resolves symbols through the PURE
    // `ctx_.exprSymbol` side table stamped by the operand's earlier visit —
    // no lookupName (which can record §22.5 forward-param hits), no TypeRef
    // creation — so recognizing-and-declining leaves the compiler state
    // untouched (#2116 determinism audit).
    //
    // #2114: a `TypeName._s` root whose TypeName is GENERIC (spelled type
    // arguments, or a TypeSymbol with generic params) is NOT trackable: sema
    // shares one FieldSymbol across instantiations while codegen gives each
    // instantiation separate static storage (probe on record with #2114), so a
    // shared key would let `Cache<int>._s <> null` admit `Cache<string>._s.M`.
    // A BARE static field of the enclosing generic type stays trackable — one
    // body has a single instantiation context. Conservative decline-with-
    // reasons; lifting needs instantiation-aware keys (#2114 stays open).
    std::optional<std::pair<std::uint32_t, std::uint32_t>>
        readonlyFieldPathOf(const ast::Expr* e, bool assignIds);

    // §7.4 / #2116: dense-id accessors for `narrowFactIds_` (its ONLY
    // accessors — the table is never iterated). narrowFactIdOf assigns on
    // first use (deterministic first-tracking-use order); narrowFactIdIfTracked
    // is the read-only probe returning 0 for "never tracked" (0 is the ambient
    // root id, never assigned to a symbol).
    std::uint32_t narrowFactIdOf(const Symbol* s);
    std::uint32_t narrowFactIdIfTracked(const Symbol* s) const;

    // §7.4 (v0.214, #2085/#2116): the resolved TypeRef of a field symbol (body
    // field or positional-record parameter), or null when unresolved. Pure
    // side-table reads.
    TypeRef* fieldTypeOf(const FieldSymbol* fs);

    // §7.4 (v0.214, #2085/#2116): true iff `fs` carries the `readonly` modifier.
    static bool isReadonlyField(const FieldSymbol* fs);

    // §7.4 (v0.214, #2085/#2116): drop every readonly-field fact rooted at
    // `root` (a local that was reassigned, passed `ref`/`out`, or mutably
    // captured — the path's facts ride the root). A no-op for a symbol that was
    // never tracked. Ambient-rooted facts (rootId 0) are never dropped — a
    // readonly field has no writer outside its construction window (§7.4).
    void dropFieldFactsRooted(const Symbol* root);

    // §7.4 / #2116: replace `fieldNarrowed_` with its intersection with
    // `other` — the all-paths join used by the if-statement walk, the loop
    // join, and the case-arm join. Pure set algebra over integer keys:
    // membership of the result is independent of container iteration order.
    void intersectFieldFactsWith(
        const std::set<std::pair<std::uint32_t, std::uint32_t>>& other);

    // §22.4 (v0.183, #1585) caller-side narrowing: if `cond` is a call to a
    // method that declares a `when true`/`when false` out-nullability
    // postcondition, and the requested branch is the one where the result
    // equals the stated constant, record the out recipient in `nullNarrowed_`
    // (known non-null). Returns true iff any narrowing was applied. The call
    // was already visited (so `cond`'s callee resolved and out-recipient
    // symbols are bound), and the #1188 out-argument invalidation already
    // cleared the recipient — this re-establishes it on the matching branch.
    bool narrowFromOutPostcond(const ast::Expr* cond, bool forTrueBranch);

    // The resolved declared TypeRef of a local or parameter symbol, or null for
    // any other symbol kind (or when the type is unresolved). Used by
    // null-narrowing to compute the non-nullable form.
    TypeRef* declaredTypeOf(Symbol* sym);

    // §7.4 (issue #418): is `sym` a simple variable currently known non-null?
    // True when it is in `nullNarrowed_` OR has a §12.7 type-narrowing (which is
    // always to a non-nullable form) OR its declared type is non-nullable.
    bool isKnownNonNull(Symbol* sym);

    // §7.4 (issue #418): update `nullNarrowed_` for an assignment of a value of
    // `valueType` to the simple variable `sym` — a non-null value makes it
    // known-non-null; a (possibly-)null value clears that knowledge.
    void narrowOnAssignment(Symbol* sym, const TypeRef* valueType);

    // §7.4 (#2258): reduce the three live narrowing sets to their INTERSECTION
    // with one snapshot — the single primitive every all-paths join and every
    // "un-seed what this construct established" restore is built from. Keeping
    // the current sets' DROPS is the point: a restore written as an ASSIGNMENT
    // (`nullNarrowed_ = saved;`) RESURRECTS a fact the visited sub-expression
    // legitimately invalidated per §7.4 "How narrowing is invalidated" (a
    // `ref`/`out` argument, a call/`await` on a mutably-captured local), which
    // is a silent compile-ACCEPT of a null dereference. An intersection keeps
    // the drop and still discards whatever the construct itself SEEDED (absent
    // from the snapshot). Pure set/map algebra over integer keys and pointer
    // identity — order-independent, so it is deterministic across shard
    // processes (#2116).
    void intersectNarrowingSetsWith(
        const std::unordered_set<Symbol*>& otherNull,
        const std::unordered_map<Symbol*, TypeRef*>& otherNarrowings,
        const std::set<std::pair<std::uint32_t, std::uint32_t>>& otherField);

    // §7.4 (issue #2118): after a loop body (which may execute ZERO times),
    // reduce the current narrowing state to the intersection of what holds on
    // the zero-iteration path (`entryNull` / `entryNarrowings`, captured before
    // the body) and what the body left (the current `nullNarrowed_` /
    // `narrowings_`). A fact ESTABLISHED inside the body is dropped (absent from
    // the entry sets); an entry fact the body INVALIDATED is dropped (the body
    // walk removed it, so it is absent from the current sets); an entry fact the
    // body left untouched survives (present in both). Result membership is a
    // pure set intersection, independent of container iteration order and
    // emitting no diagnostic, so it is deterministic across shard processes
    // (#2116).
    // #2116: `entryField` joins `fieldNarrowed_` under the same
    // zero-iteration-path intersection rule.
    // #2152: `edges` folds each break/continue edge snapshot into the same
    // intersection — both edge kinds can reach the after-loop point (break
    // directly; continue via the cond re-check on cond-false), so an edge that
    // left a local possibly-null drops it from the after-loop state.
    void intersectNarrowingSetsAfterLoop(
        const std::unordered_set<Symbol*>& entryNull,
        const std::unordered_map<Symbol*, TypeRef*>& entryNarrowings,
        const std::set<std::pair<std::uint32_t, std::uint32_t>>& entryField,
        const LoopEdgeNarrowing& edges);

    // §7.4 (#2152): snapshot the live narrowing state at a `break`/`continue`
    // edge into the TARGET loop's collector. `targetOffset` is the nesting
    // offset resolved by checkLoopJumpTarget (0 = innermost loop). Bounds-
    // checked so an erroneous jump (unknown label, jump outside any loop)
    // snapshots nothing — `loopEdgeNarrowing_` runs parallel to
    // `loopLabelStack_`, whose size equals `loopDepth_`.
    void recordLoopEdgeNarrowing(int targetOffset);

    void visitTopDecl(const ast::TopDecl& d);

    // Param-symbol lookup for the in-progress method/ctor on currentType_.
    const std::vector<Symbol*>* paramSymbolsOf(const ast::Member& m) const;

    // §35.5 / §19.3 (#2047 follow-up): recover the scrutinee type of a `case`
    // whose scrutinee is `this` / `base` inside a CHOICE instance method. Sema
    // leaves `this` untyped for a named-type method (the same posture as a record
    // `this`), so `ctx_.exprType(this)` is null and the choice-case-first pattern
    // resolution would be skipped — letting a pattern head that names a case
    // (`Ok`) fall through to general name lookup and collide with a same-named
    // user CLASS (the §35 spec rule is "a case name resolves to a case pattern,
    // never a type test"). When `scrutTy` is null and the scrutinee is
    // `this`/`base` in a choice method, returns a Named TypeRef for the enclosing
    // choice; otherwise returns `scrutTy` unchanged. `this` stays globally
    // untyped (codegen recovers the runtime value on its own path).
    TypeRef* recoverChoiceThisScrutTy(const ast::Expr* scrutinee,
                                      TypeRef* scrutTy);

    // Build a method scope (params on top of module scope), then visit
    // body. Locals are registered into this scope as visitStmt walks
    // LocalDecls.
    // §22.16 (v0.203, #2053): when `clauses` is non-null, each clause's
    // expression is visited in the SAME method scope (parameters + this +
    // enclosing statics in scope; body locals are not — they have not been
    // registered yet), and each clause's expression is checked to be of
    // type `bool`. Clauses are visited independently of the body so a
    // clause never observes a body local.
    void runMethodBody(
        const std::vector<ast::Param>& params,
        const std::vector<std::unique_ptr<ast::Stmt>>* stmts,
        const ast::Expr* exprBody,
        const std::vector<Symbol*>* paramSyms,
        const std::vector<ast::ContractClause>* clauses = nullptr);

    // §22.4 (v0.183, #1585) callee-side check: at a point where the body
    // returns the value `returnValue` (a `return E;` or the expression body of
    // an expression-bodied method), verify that every out-nullability
    // postcondition param whose stated constant this path may produce is
    // provably non-null. Uses §7 narrowing (`collectNarrowings` of the returned
    // expression) so `return conn <> null;` discharges a `when true` clause and
    // a path returning the other constant carries no obligation. A no-op unless
    // the enclosing bool method declares postcondition params.
    void checkOutPostcondAtReturn(const ast::Expr* returnValue,
                                  SourceLocation loc);
    // §22.4 (v0.183, #1585): the out-nullability postcondition params of the
    // method body currently being walked — (param symbol, whenTrue) pairs.
    // Populated in `runMethodBody` only for a bool-returning method; cleared
    // across a nested lambda body so a lambda `return` never discharges the
    // enclosing method's clause. Saved/restored by `runMethodBody`.
    std::vector<std::pair<ParameterSymbol*, bool>> currentOutPostconds_;

    // §7.4 (v0.182, #1566): recompute `mutablyCapturedNames_` from a method body
    // (its statements and/or expression body). Syntactic full-body scan: records
    // the name of every local/parameter assigned or passed `ref`/`out` inside any
    // lambda, excluding a lambda's own parameters (which shadow an outer name).
    void computeMutablyCaptured(
        const std::vector<std::unique_ptr<ast::Stmt>>* stmts,
        const ast::Expr* exprBody);

    // §7.4 (v0.182, #1566): drop the null-narrowing (and any §12.7 type-
    // narrowing) of every currently-narrowed local whose name is in
    // `mutablyCapturedNames_`. Called at each control-transfer operation inside a
    // controlled block; a no-op when nothing in the body is mutably captured.
    void invalidateCapturedNarrowings();

    void visitMember(const ast::Member& m);

    // §22.12 (#747 R42): true iff `e` is a compile-time-constant expression
    // admissible as a `const` member initializer — a literal (not an
    // interpolated string), a unary/binary/conditional expression over
    // constants, or a reference to another const / enum constant. Must be
    // called AFTER the initializer is type-checked (it reads exprSymbol).
    bool isConstInitExpr(const ast::Expr& e);

    // §22.13 (#747 R42): reject a `static readonly` field whose initializer
    // reads a `static readonly` field of the same type declared LATER. Called
    // with the FieldDecl after its initializer is type-checked.
    void checkStaticReadonlyForwardRef(const ast::FieldDecl* fd);

    void visitStmt(const ast::Stmt& s);

    void visitExpr(const ast::Expr& e);

    // Slice 3h: resolve a CallExpr's return type from the callee's
    // resolved MethodSymbol.
    void typeCall(const ast::CallExpr& ce);

    void checkCallArity(const ast::CallExpr& ce, MethodSymbol* ms);

    // §2.2 / §2.5 / §2.6 step 3 (issue #328 inc 3): type-check a call to an
    // IMPORTED method (no ast::MethodDecl) against its reconstructed
    // ParameterSymbol signature — argument count + per-argument assignability.
    void checkImportedCallArity(const ast::CallExpr& ce, MethodSymbol* ms);

    // §22.5 argument-to-parameter binding. Maps a call's argument list
    // (positional, then named) onto a parameter list:
    //   - positional arguments fill parameters in order;
    //   - a named argument `name := expr` fills the parameter named `name`;
    //   - "named arguments may appear in any order after positional
    //     arguments" — a positional argument that follows a named one is an
    //     error;
    //   - an unknown parameter name, a parameter bound twice (positionally
    //     then by name, or by two named args), and a required (non-default)
    //     parameter left unfilled are each errors.
    // When `emit` is true the diagnostics above are reported (with `loc` /
    // `calleeDesc`); when false the routine is silent (used to test a
    // candidate's applicability during §22.6 overload resolution).
    struct BindResult {
        bool ok = false;            // binding succeeded with no violations
        // paramToArg[p] = index into `args` filling parameter p, or -1 when
        // the parameter takes its default (valid only when `ok`).
        std::vector<int> paramToArg;
    };
    // fix #3178 / #3173 (§22.5 / §2.5, v0.254): the binder takes a
    // `sema::FormalsView`, so the SAME rule and the SAME five diagnostic
    // strings serve a locally declared formal list and an IMPORTED callee's
    // reconstructed `ParameterSymbol` list. The `std::vector<ast::Param>`
    // overload is kept for the many callers that already hold one — it just
    // wraps `FormalsView::of`.
    BindResult bindArgsToParams(sema::FormalsView params,
                               const std::vector<ast::CallArgument>& args,
                               SourceLocation loc,
                               const std::string& calleeDesc, bool emit);
    BindResult bindArgsToParams(const std::vector<ast::Param>& params,
                               const std::vector<ast::CallArgument>& args,
                               SourceLocation loc,
                               const std::string& calleeDesc, bool emit) {
        return bindArgsToParams(sema::FormalsView::of(params), args, loc,
                                calleeDesc, emit);
    }

    // §5.1: is a value of static type `from` implicitly convertible to
    // `to`? Identity is NOT counted here (that is the "exact" relation,
    // tested with typeRefEqual); this returns true only for a non-trivial
    // §5.1 conversion: numeric/char widening, non-nullable reference →
    // nullable, or subtype → supertype. Conservative: returns false for
    // type pairs the lattice can't yet judge, so a candidate is treated as
    // inapplicable rather than spuriously applicable.
    bool implicitlyConvertible(const TypeRef* from, const TypeRef* to);

    // §22.6 per-argument match classification against a parameter type.
    enum class ArgMatch { NotApplicable, Convert, Exact };
    ArgMatch classifyArg(const TypeRef* argTy, const TypeRef* paramTy);

    // §22.6 type-based overload resolution over a set of candidate
    // callables sharing the call's arity and binding all-positional.
    enum class OverloadOutcome {
        Resolved,     // a unique winner: `winnerIndex` is set.
        Ambiguous,    // diagnostic emitted (candidates cited); no winner.
        Unreachable,  // diagnostic emitted (none cited); no winner.
        Undecidable   // could not judge (unresolved arg/param types): the
                      // caller should fall back to arity-based selection.
    };
    struct OverloadResult {
        OverloadOutcome outcome = OverloadOutcome::Undecidable;
        std::size_t winnerIndex = 0;  // valid only when Resolved
    };

    // `paramTypes[i]` holds candidate i's ordered parameter TypeRefs (each
    // may be null when unresolved). `argTypes` holds the call's positional
    // argument static types (null when unresolved). `fillsDefault[i]` is
    // true when candidate i's §22.5 binding left one or more of its
    // parameters to be filled by their declared defaults — used by §22.6's
    // defaulted-arity tie-break (v0.181): a candidate applicable without
    // filling any default is preferred over one that fills one or more,
    // provided it is better-or-equal at every supplied-argument position.
    // Emits the ambiguous / unreachable diagnostic itself when applicable.
    // `candNames` supplies a display string per candidate for the
    // ambiguous-case citation. `extraNote` (default empty), when non-empty,
    // is appended verbatim to an emitted diagnostic's message — used by
    // member-access call sites to carry the §26.5 step 1 name-blocked-
    // extension note (see extensionNameBlockedNote); callers outside the
    // member-access path (e.g. `new T(...)` constructor overloads) pass the
    // default and get no such suffix.
    OverloadResult resolveOverload(
        const std::vector<std::vector<const TypeRef*>>& paramTypes,
        const std::vector<const TypeRef*>& argTypes,
        const std::vector<bool>& fillsDefault,
        const std::vector<std::string>& candNames, SourceLocation loc,
        const std::string& calleeDesc,
        const std::string& extraNote = std::string(),
        const std::vector<const ast::Expr*>& argExprs =
            std::vector<const ast::Expr*>());

    // §22.6 method-overload resolution. Given the initially-resolved
    // MethodSymbol for a call and the call site, returns the MethodSymbol
    // the call resolves to: applies type-based §22.6 selection when the
    // name has multiple same-arity, all-positional candidates and arg/param
    // types are decidable; otherwise falls back to arity-based first-wins
    // (preserving the prior behavior on named-arg / modifier / undecidable
    // calls). Emits the ambiguous / unreachable diagnostics itself.
    MethodSymbol* resolveMethodOverload(MethodSymbol* initial,
                                        const ast::CallExpr& ce);

    // §22.6/§12.1 (#1090): collect every same-name method overload visible on
    // `start` and its base classes, most-derived first, with a more-derived
    // overload shadowing a less-derived one of the same signature (so an
    // override does not duplicate the method it overrides). Lets overload
    // resolution and the arity check see an overload declared in a base class
    // that `lookupMember`'s first-match BFS stopped short of.
    void collectHierarchyOverloads(TypeSymbol* start, std::string_view name,
                                   std::vector<MethodSymbol*>& out);

    // §14.5 (v0.184, #1686): a `ref`/`out` argument bound to a generic
    // method's type parameter must unify EXACTLY with the parameter's
    // substituted type — no conversion, and no join across conflicting
    // occurrences, at either the binding or the later applicability check.
    // Runs UNCONDITIONALLY (independent of whether the call's return type
    // needs a substitution) for a call to a generic method that has at
    // least one ref/out parameter whose declared type mentions a method
    // type parameter. Returns true when a §14.5 diagnostic was emitted, in
    // which case the caller must stop typing the call (no codegen for a
    // rejected call).
    bool checkGenericRefOutExactUnification(const ast::CallExpr& ce,
                                            const ast::MethodDecl* md);

    // Slices T5/T6/T7: shared check of an all-positional argument list
    // against a parameter list — argument count within [required,
    // total] (optional params lower the minimum), then per-argument
    // assignability. Callers must have already ruled out named args
    // and overload ambiguity.
    void checkArgsAgainstParams(
        const std::vector<ast::Param>& params,
        const std::vector<ast::CallArgument>& args, SourceLocation loc,
        const std::string& calleeDesc,
        const ast::Expr* classTarget = nullptr,
        TypeSymbol* classParent = nullptr,
        const ast::MethodDecl* calleeMethodDecl = nullptr);

    // Slice T7: check `new T(args)` against T's sole constructor (if
    // any). Zero declared constructors → the implicit default ctor
    // takes no arguments. Multiple constructors → overload resolution
    // is deferred, so skip.
    void checkNewExprArgs(const ast::NewExpr& ne, TypeSymbol* t);

    // fix #3141 (family F3 of umbrella #3152) — §9.1 / §22.5 / §5.1 / §2.8.3
    // rule 4: the positional-record arm of checkNewExprArgs for a record whose
    // positional `ast::Param`s do not exist in this compilation (an IMPORTED
    // record: `ModuleLoader` assigns no `Symbol::decl`, and #2516's synthetic
    // `Param` carries a null `type`). Applies the SAME two rules the local arm
    // gets from checkArgsAgainstParams — §22.5 argument-to-field binding, then
    // §5.1 per-argument assignability — over `recordPositionalFieldSymbols`
    // (the indexed field list, both sides) and `storageComponentType` (the
    // declared type, both sides). Named arguments bind by field name exactly as
    // §22.5 binds them by parameter name.
    //
    // ⚠ There is no modifier-based deferral. §22.10 admits only plain and
    // `owned` in a primary-constructor parameter list, so a modifier changes
    // neither which field an argument binds to nor that field's type; a
    // `ref`/`out` argument takes §14.5's exact-match rule instead of §5.1's,
    // exactly as on the local path. checkImportedNewExprArgs' modifier deferral
    // is right for IT — an imported CONSTRUCTOR may really declare `ref`/`out`
    // parameters — and importing it here left `new Named(ref n, 2)` compiling
    // to a SIGSEGV.
    void checkPositionalNewArgsFromSymbols(const ast::NewExpr& ne,
                                           TypeSymbol* t);

    // §7 / stdlib §9.6 (#2315): a primitive `string` receiver's builtin
    // instance methods (IndexOf/Contains/StartsWith/EndsWith/LastIndexOf/
    // Replace/Split/Insert/Substring/...) declare NON-nullable `string`/
    // `char`/`int` parameters, but the call has no MethodSymbol — `string`
    // is a primitive with no user TypeSymbol — so the ordinary
    // checkCallArity → checkArgsAgainstParams path never runs. An
    // un-narrowed `string?` argument was silently accepted and reached
    // codegen, where the runtime helper's accidental null guard
    // (`bz_str_indexof` opens with `if (!self_p || !sub_p) return -1;`)
    // converted the type error into a "not found"/wrong answer. That guard
    // is load-bearing only while every lowering avoids dereferencing the
    // operand (a new lowering already segfaulted on it once). Run the SAME
    // §7/§12 assignability check the user-method path runs
    // (checkArgsAgainstParams → checkAssignable) against the stdlib-§9.6-
    // declared parameter types, so a builtin string call rejects an
    // un-narrowed `string?` exactly as a user `method Take(x: string)`
    // does. `Equals(other: string?)` is the documented exception (§11.2
    // parameter-nullability widening) and stays admissible; the static
    // `IsNullOrEmpty`/`IsNullOrWhiteSpace` take `string?` and are not
    // reached here (this covers the INSTANCE-receiver path only).
    void checkBuiltinStringCallArgs(const ast::CallExpr& ce);

    // §7 / stdlib §4.x (#2582): the COLLECTION twin of #2315. A builtin
    // collection instance method that declares a NON-nullable `item: T` /
    // `query: T` / `key: K` parameter (Array<T>.IndexOf/Contains/Remove/
    // LastIndexOf/Insert, Set<T>/SortedSet<T>.Add/TryAdd/Contains/Remove,
    // Stack<T>/Queue<T>.Push/Enqueue/Contains, LinkedList<T>.AddFirst/
    // AddLast/AddBefore/AddAfter/Find/Contains/Remove, Dictionary<K,V>/
    // SortedDictionary<K,V>.ContainsKey/Contains/Remove, OwningDictionary<
    // K,V>.PopValue/TryPopValue, OwningSet<T>.Take/TryTake) has no
    // MethodSymbol — these are synthetic Named TypeRefs with no user
    // TypeSymbol — so the ordinary checkCallArity → checkArgsAgainstParams
    // → checkAssignable path never fires. An un-narrowed `int?` (or `T?`)
    // argument was silently accepted and reached codegen, where the
    // equality-compare lowering tripped an LLVM ICmpInst assertion
    // (`ICmpInst::AssertOK`: operand-type mismatch, exit 134 — there is no
    // runtime null guard to mask the type error here). A user
    // `method Take(x: int)` with the identical signature correctly rejected
    // it (§7), so the two paths disagreed. Run the SAME §7/§12
    // assignability check the user-method path runs against the
    // instantiated element/key type `recvTy->genericArgs[0]`, so a builtin
    // collection call rejects an un-narrowed `T?` exactly as a user method
    // does. The §7.4 narrowing exception (a `T?` proved non-null by a
    // null-check IS assignable) lives inside checkAssignable, which this
    // reuses. Only INSTANCE calls on the builtin collection families are
    // covered: the gate requires a Named TypeRef with no user `declaration`
    // and a recognized collection head name, which excludes static calls
    // and user types (a user `method IndexOf(item: T): int` has a
    // MethodSymbol → checkCallArity already ran). The owning family's
    // `owned item: T` mutators (Add/Push/Enqueue/Insert/AddFirst/…) carry
    // a §22.3 `owned` modifier at the call site and are skipped per that
    // section (their nullability is governed by §22.3, not §7); the
    // owning-family `Contains`/`IndexOf`/`Find`/`Take`/etc. take a PLAIN
    // borrow `item: T`/`query: T` and ARE covered. Null-conditional calls
    // (`coll?.IndexOf(n)`) have a NullCondMemberExpr callee (not a
    // MemberAccessExpr) and are not reached — filed separately.
    void checkBuiltinCollectionCallArgs(const ast::CallExpr& ce);

    // §7 / stdlib §3.3 (#2581): the ReadOnlyStringSpan twin of #2315. A builtin
    // ReadOnlyStringSpan instance method (IndexOf/Contains/StartsWith/EndsWith/
    // SequenceEqual, stdlib §3.3) declares NON-nullable `string`/`char` operands
    // but has no MethodSymbol (it is synthetically typed in the CallExpr block
    // at L6419), so checkCallArity → checkArgsAgainstParams → checkAssignable
    // never fired. #2315's gate keys on a Primitive-String VALUE receiver
    // exprType, which excludes the Named ReadOnlyStringSpan receiver, so the
    // span path was left open and an un-narrowed `string?`/`char?` was silently
    // accepted. This helper mirrors the user-method path against the §3.3-declared
    // non-nullable char/string kinds; the ReadOnlyStringSpan-Named operand
    // overload is not a primitive slot and is not covered here.
    void checkBuiltinSpanCallArgs(const ast::CallExpr& ce);

    // §7.1 / §7.4 / §23.4 (#2619): the INDEX-ARGUMENT twin of the CallExpr §7
    // gates above, and the generalization of #2613 (which closed this hole for
    // the single `ReadOnlyStringSpan` receiver). §23.4: "An indexer is a
    // property whose name is replaced by `[parameters]`" — an index argument is
    // therefore a parameter pass, and §7.1 states the compiler "enforces the
    // distinction at every assignment, parameter pass, and return". NO indexer
    // path in sema ran that check: the builtin receivers (Array<T>, string,
    // Dictionary<K,V>, the span/buffer families, inline arrays) are synthetic
    // Named/Primitive TypeRefs with no MethodSymbol, and the USER-declared
    // indexer path — which DOES resolve a PropertySymbol, so the
    // no-MethodSymbol explanation never applied to it — did not check its
    // arguments either. An un-narrowed `int?` index was silently accepted and
    // reached codegen, which emitted malformed IR (`SExt only operates on
    // integer`, `GEP indexes must be integers`, `Call parameter type does not
    // match function signature`) — an internal compiler error where §7 asks
    // for a clean diagnostic. Called once per IndexExpr, after every receiver
    // family has been typed (so a user indexer's resolved PropertySymbol is
    // already attached to the node by #2530). See the definition in
    // ExprTypecheck_Exprs.cpp for why it cannot false-reject.
    void checkIndexArgNullability(const ast::IndexExpr& ix, const ast::Expr& e,
                                  TypeRef* recvTy);

    // fix #2758 (§23.5, v0.241): "Index arguments are **positional-only**. The
    // named-argument form (`Identifier :=`, §22.5) is not admitted at an index
    // position — a compile error naming this rule." An IndexExpr's indices
    // share the call ArgList grammar (A.7 PrimarySuffix `[ ArgList ]`), so
    // `d[key := "k"]` PARSES; every consumer then maps `indices[k]` to
    // `indexerParams[k]` POSITIONALLY and the name is silently dropped. Called
    // from visitChildren's IndexExpr arm — once per node, before any receiver
    // family has been resolved, because the rule is a property of the index
    // ARGUMENT alone and holds for a receiver whose type never resolves.
    void checkIndexArgsPositional(const ast::IndexExpr& ix);

    // fix #2994 (§17.7 "Standard-library builtins have a closed constructor
    // surface", v0.241): `new T(...)` on a standard-library builtin resolves
    // only against a DECLARED constructor, never against a compiler-supplied
    // fallback. Called for every NewExpr — the explicit `new T(...)` form from
    // the NewExpr typing arm, and the §17.7 `new()` shorthand from the two
    // sites that give the shorthand its target type (a field initializer and a
    // typed local declaration), since those are the same construction with the
    // type taken from context. See the definition for the two arms and for why
    // §14.2's generic-body `new T()` ≡ `default(T)` equivalence is untouched.
    void checkBuiltinClosedConstructorSurface(const ast::NewExpr& ne);

    // fix #2994: is `name` a generic type PARAMETER of the enclosing method or
    // type? A type parameter resolves to a Named TypeRef with a null
    // declaration — exactly the shape a reserved builtin has — so a body
    // writing `new T()` under `where T: new()` is indistinguishable from
    // `new Guid()` by TypeRef shape alone. Consulted so a type parameter that
    // SHADOWS a builtin name (`class Box<Guid>`) is never claimed by the
    // closed-surface arms. Mirrors the §14.1 (#992) detection in the NewExpr
    // arm.
    bool isEnclosingGenericTypeParamName(std::string_view name) const;

    // §2.5 / §22.7 (issue #328 FINDING [7] B): check `new ImportedT(args)`
    // against an imported type's reconstructed constructor signatures (no
    // ast::ConstructorDecl; ParameterSymbol resolved types). Single-ctor,
    // all-positional case; overloads/named args deferred.
    void checkImportedNewExprArgs(const ast::NewExpr& ne, TypeSymbol* t,
                                  const std::vector<ConstructorSymbol*>& ctors);

    // §17.1: postfix `.` member access. The target's type determines
    // the lookup scope. For now we handle `this.member` (lookup in the
    // current type) and `<expr>.member` where the expression has a
    // resolved Named or Primitive-Object type. Generics and inherited
    // members from base types are deferred to a later slice.
    // The TypeSymbol a member-access target denotes (for `.` and `?.`):
    // `this`/`base` keywords, then the target's exprType, then a fallback
    // through its exprSymbol (local/parameter declared type, or a type name
    // for static access). Returns null when it can't be determined.
    TypeSymbol* memberTargetType(const ast::Expr* target);

    void resolveMemberAccess(const ast::MemberAccessExpr& ma);
    // fix #3177 (§14.1 / §2.8.3 rule 4 / §13 / §23 / §7.5, v0.254): the
    // #2884 + #2943 imported-property access declines, asked ONCE for both
    // the `.` and the `?.` spelling (they are distinct AST nodes and only
    // the first carried them). `loc` is the caller-resolved access site.
    void declineUnsupportedImportedPropertyAccess(PropertySymbol* ips,
                                                 SourceLocation loc);

    // per §14.1/§14.2: substitute a member's declared type against the
    // receiver's concrete generic arguments. When a field/property declared
    // with a generic-parameter type (`v: T`) is accessed off a receiver of a
    // concrete instantiation (`b: Holder<Inner>`), the access `b.v` has type
    // `Inner`, not the bare `T`. Returns `declared` unchanged when no
    // substitution applies (non-generic field, missing receiver args, etc.).
    // See issue #361 (D13): without this, `b.v.x` cannot resolve `.x` because
    // `b.v` was typed as the unresolved type parameter.
    TypeRef* substituteMemberType(TypeRef* declared,
                                  const ast::Expr* target,
                                  TypeSymbol* parent);

    // per §14.1/§14.2 + §A.7 (issue #391): return-target generic
    // type-argument inference. A generic method whose type parameter appears
    // ONLY in the return type (`Create<T>(): T`) cannot have T inferred from
    // arguments, and Bezel has no call-site type-argument syntax (§A.7
    // PrimarySuffix has no `<...>` form), so T must be solved from the call
    // site's *target* type — the same "target type" the spec already uses for
    // collection literals (§15.4) and the `new()` shorthand (§17.7).
    //
    // If `ce` resolves to a generic method with one or more type parameters
    // left unbound by its arguments, this unifies the method's declared return
    // type against `target` to solve them. On success it re-types the CallExpr
    // to the substituted return type and returns true; on failure (no
    // applicable generic method, the params are already bound by arguments, or
    // the return type does not unify with `target`) it returns false and
    // leaves the call's type untouched. `target` may be null, in which case
    // the routine only reports whether the call *needs* inference (via
    // `outNeedsInfer`) without solving anything — the caller uses that to emit
    // the "cannot infer" diagnostic in a no-target context (§15.4 / §17.7:
    // inference is not available for `var` without annotation).
    bool inferReturnTargetTypeArgs(const ast::CallExpr& ce, TypeRef* target,
                                   bool* outNeedsInfer);

    // Recursive unification of a declared return TypeRef against a concrete
    // target TypeRef, binding any type-parameter names in `paramNames` into
    // `solved`. Returns false on a structural mismatch that prevents solving.
    bool unifyReturnAgainstTarget(
        const TypeRef* declared, const TypeRef* target,
        const std::unordered_set<std::string_view>& paramNames,
        std::unordered_map<std::string_view, TypeRef*>& solved);

    // Build the substituted form of a declared return TypeRef, replacing each
    // type-parameter name in `solved` with its concrete TypeRef. Returns
    // `declared` unchanged when nothing applies.
    TypeRef* substituteReturnType(
        TypeRef* declared,
        const std::unordered_map<std::string_view, TypeRef*>& solved);

    // per §14.1 + §A.7 (issue #391): emit the "cannot infer the type argument"
    // diagnostic when `ce` calls a generic method whose return-only type
    // parameter cannot be solved because the call sits in a no-target context
    // (a bare expression/discard statement, or a `var` without annotation).
    void reportNoTargetInferenceIfNeeded(const ast::CallExpr& ce);

    // §24.3: the event visibility fence. Outside the declaring class, an
    // event member's static type is `IEvent<T>` / `IAsyncEvent<T>`, which
    // exposes only `Subscribe`. The producer-side operations `Raise` /
    // `RaiseAsync` / `Add` / `Remove` (and the `+=` / `-=` desugaring) live
    // on `EventSource<T>` / `AsyncEventSource<T>`, the in-class face. Given
    // an event member, the accessed operation name (one of the producer-side
    // names, or `+=` / `-=`), and the access location, this emits a
    // diagnostic when the access is from outside the declaring class. A
    // derived class may use the producer-side face only for a `protected`
    // event declared on a base class (§24.5). `Subscribe` is never flagged.
    void checkEventVisibilityFence(EventSymbol* es, std::string_view op,
                                   SourceLocation loc);

    // §13.1 member-visibility enforcement (issue #432). Checks that the
    // member `msym` accessed at `loc` is visible from the current type
    // context `currentType_`. Reports a diagnostic and returns true when
    // access is rejected; returns false when access is allowed (caller
    // continues normally). Handles all four levels:
    //   private   — declaring type only (§13.1).
    //   protected — declaring type + derived types (§13.1).
    //   internal  — same module only; cross-module import → rejected (§13.4).
    //   public    — always allowed.
    // The overload-suppression heuristic (a non-private sibling with the same
    // name → suppress the error) is applied for private and protected, matching
    // the original private-only check.
    // `receiver` (optional, default null): the dot-call's receiver TypeRef,
    // when known. §26.5 step 1 (v0.183): when supplied and access is
    // rejected, the diagnostic notes a same-named extension that matches
    // `receiver` where one exists (name-blocked by the instance member,
    // never consulted) — see extensionNameBlockedNote.
    bool checkMemberVisibility(const MemberSymbol* msym,
                               std::string_view memberName,
                               SourceLocation loc,
                               const TypeRef* receiver = nullptr);

    // §7.5 null-conditional member access `x?.M`. Resolves the member on
    // the target's (non-nullable) type exactly like `.`, but the result
    // type is ALWAYS nullable (the access yields null when the target is
    // null). The member's own type is looked up; its nullable form is the
    // expression's type, so member access chains (`a?.b?.c`) resolve.
    void resolveNullCondMember(const ast::NullCondMemberExpr& nm);

    // #3019 (§9.2/§9.3/§35.7): the use-site gate for a direct
    // `.Equals(...)`/`.GetHashCode()` (or `?.Equals(...)`/`?.GetHashCode()`)
    // call on a record/choice that is not auto-hashable and declares
    // no user `=`/`<>` overloads. Shared by the `.` path
    // (resolveMemberAccess) and the `?.` path (resolveNullCondMember) so the
    // two forms enforce the SAME check: §7.5 evaluates the member normally
    // when the receiver is non-null, so a `?.Equals` on a non-bearing record
    // is the same use-site violation as the `.` form (the gate is on the
    // member's availability, not the receiver's nullness). `targetType` is the
    // receiver's underlying record/choice TypeSymbol (nullable wrapper already
    // unwrapped by the caller); no-op when targetType is null or not a
    // record/choice. `member` is "Equals"/"GetHashCode"; `loc` is the use site.
    void checkEqualityMemberUseSite(TypeSymbol* targetType,
                                    std::string_view member,
                                    SourceLocation loc);

    // §7.5 / #751: result type of a builtin (string/primitive) RECEIVER's
    // property-like value member (e.g. `string.Length`, `string.IsAscii`) for
    // the null-conditional `x?.M` path. resolveMemberAccess types these via a
    // large inlined intrinsic block keyed on the MemberAccessExpr node, which
    // resolveNullCondMember cannot run — so `x?.Length` was left untyped and a
    // following `??` (lowerCoalesce) silently read 0. Returns the member's
    // NON-nullable result type, or null when unrecognized; the caller lifts it
    // to the nullable form. Only the property-like value members reachable as a
    // bare `?.` access are covered — the numeric-primitive intrinsics
    // (CompareTo, Abs, …) are all METHODS, reached through `?(`
    // (NullCondCallExpr), not this path.
    TypeRef* builtinValueMemberType(const TypeRef* recvTy,
                                    std::string_view member);

    // §17.1 result-type table for binary operators. Slice 3c covers the
    // common cases with no operator-overload resolution: equality and
    // relational ops produce bool; arithmetic / shift / bitwise produce
    // the left operand's type (when the operands type-match); logical
    // ops `and` / `or` produce bool; `??` produces the non-nullable
    // result type of the LHS; range ops produce a Range value (slice 3c
    // leaves Range as Error).
    // §25: the user-defined `static operator <op>` method that applies to a
    // binary expression, if either operand's type (class or record)
    // declares one. `=`/`<>` on classes are excluded (they invoke Equals,
    // §25.2).
    MethodSymbol* findBinaryOperator(const ast::BinaryExpr& be);

    // §25.2 / §12.2 (v0.181, #1562): collect every `operator <token>` overload
    // of the given arity declared on `ts` OR inherited from any of its base
    // classes. A derived-typed operand reaches its base's operators (there is
    // no user-facing upcast that could otherwise reach them); §12.2's per-token
    // no-hiding rule keeps a single inheritance chain duplicate-free. Walks the
    // `baseTypes` chain transitively with a seen-set (cycle-safe).
    std::vector<MethodSymbol*> collectInheritedOperators(TypeSymbol* ts,
                                                         std::string_view token,
                                                         std::size_t arity);

    void typeBinary(const ast::BinaryExpr& be);

    // §17.1 result-type table for unary operators (slice 3c).
    // §25: a user-defined unary `operator <op>` (one parameter) declared on
    // the operand's type. Overloadable prefix unary operators are `-`/`~`.
    MethodSymbol* findUnaryOperator(const ast::UnaryExpr& ue);

    void typeUnary(const ast::UnaryExpr& ue);

    // §28.8.1 / §28.11 (issue #423): if `operand` is `coll[i]` whose receiver
    // `coll` has an INTERFACE static type, return the interface type name;
    // otherwise the empty string. `move coll[i]` is admitted only on the
    // enumerated concrete owning receivers (§28.11) — an interface is not one of
    // them — so a non-empty result is rejected by `typeUnary`'s `move` branch.
    std::string moveThroughInterfaceSlotName(const ast::Expr& operand) const;

    // §28.11 / §4.15 (#871): if `operand` is `coll[i]` whose receiver `coll` has
    // a BORROWING-span static type (ArraySpan<T>/ReadOnlyArraySpan<T> — views into
    // a borrowing Array<T> — or any read-only span), return that span type name;
    // otherwise the empty string. Such spans' slots hold borrows with no
    // ownership to transfer, so `move span[i]` is rejected at compile time. The
    // OWNING `Span<T>` (over an `OwningBuffer`) is NOT matched here.
    std::string moveThroughBorrowingSpanSlotName(const ast::Expr& operand) const;

    // §19.3 / §15.3 (v0.184, #1665): if `operand` is `s[i]` where `s` is a
    // `case`/`when` pattern binding (a LocalSymbol whose `decl` is a pattern
    // `BindingPattern`, not a plain `var`-declared LocalDecl) over a MOVE-ONLY
    // owning-inline-array scrutinee, return the bound name; otherwise the
    // empty string. The binding is a borrow of the scrutinee's value — it
    // does not own the array — so `move s[i]` is rejected at compile time by
    // `typeUnary`'s `move` branch.
    std::string moveThroughBorrowedMoveOnlyArrayBindingName(
        const ast::Expr& operand) const;

    // §19.3 / §15.3 (v0.196, #1745): the whole-binding sibling of the above.
    // If `operand` is a bare identifier `s` naming a `case`/`when` pattern
    // binding (a LocalSymbol whose `decl` is a pattern `BindingPattern`) over a
    // MOVE-ONLY owning-inline-array scrutinee, return the bound name; otherwise
    // the empty string. The binding is a borrow of the scrutinee's value — it
    // does not own the array — so `move s` (moving the WHOLE binding) is
    // rejected by `typeUnary`'s `move` branch, just as `move s[i]` (a single
    // slot) is: laundering the whole borrowed value would duplicate ownership
    // the original scrutinee still holds.
    std::string moveThroughBorrowedMoveOnlyArrayWholeBindingName(
        const ast::Expr& operand) const;

    // §30.14 / §3.6 (#600): sendability derived-type property.
    // Returns true iff `rt` satisfies the §30.14 sendable table for a
    // Channel<T> element or Domain.Spawn argument/result.
    //
    // The `inProgress` set implements the coinductive recursion rule:
    // a class or named type being evaluated is assumed sendable until
    // proven otherwise.  Callers should pass an empty set on initial
    // call; recursive sub-calls share the same set.
    bool isSendable(const TypeRef* rt,
                    std::unordered_set<const TypeSymbol*>& inProgress) const;

    // Convenience overload with a private inProgress set (top-level call).
    bool isSendable(const TypeRef* rt) const {
        std::unordered_set<const TypeSymbol*> ip;
        return isSendable(rt, ip);
    }

    // §30.14 (#600): true iff the entry argument to Domain.Spawn is
    // capture-free: a static-method-reference (MemberAccessExpr whose
    // target is a TypeSymbol) or a non-capturing lambda (a LambdaExpr
    // whose body references no locals from the enclosing scope).
    bool isCaptureFreeEntry(const ast::Expr* entryExpr) const;

    // Helper: does the lambda body (recursively) reference any identifier
    // whose resolved symbol is NOT in `ownParamSyms` and is a local or
    // parameter symbol (i.e. captured from the outer scope)?
    bool lambdaBodyCaptures(
        const ast::LambdaExpr* lam,
        const std::unordered_set<const Symbol*>& ownParamSyms) const;

    // Walk the given expression tree and check if any IdentifierExpr
    // resolves to a symbol that is a local/parameter NOT in ownParamSyms.
    bool exprCaptures(const ast::Expr& e,
                      const std::unordered_set<const Symbol*>& ownParamSyms) const;
    bool stmtCaptures(const ast::Stmt& s,
                      const std::unordered_set<const Symbol*>& ownParamSyms) const;

    void visitChildren(const ast::Expr& e);
};

}  // namespace bezel::sema
