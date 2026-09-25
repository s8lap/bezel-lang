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

#include "common/BuiltinRegistry.hpp"  // common::BuiltinFormals (fix #3424 / #3425)

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
    // §22.16 contract-clause static-requires state. The `static` qualifier
    // on a clause demands the proven tier; sema's enforcement fires from
    // the IdentifierExpr handler and from the per-clause walker. The bit
    // is detected by pointer-matching `&e` against
    // `currentMethodDecl_->clauses[i].expr.get()` — only the TOP-LEVEL
    // visitExpr of a clause expression matches, never a recursive visit,
    // so a single guard at the top of visitExpr is sufficient. RAII so the
    // flag is restored on every return path through visitExpr (including
    // the IdentifierExpr branch's many early returns on a bare-name
    // identifier that is itself a clause expression, e.g. the
    // `static requires(myConst)` shape).
    struct ClauseStaticGuard {
        ExprTC* tc;
        bool saved;
        bool entered;
        ClauseStaticGuard(ExprTC* t, const ast::Expr& e);
        ~ClauseStaticGuard();
    };
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
    // method, type, or (fix #3617-family drain, R10) extension block?
    bool isInScopeTypeParam(std::string_view name) const;
    // §26.4 / #3083: does `target`'s argument list bind `name` as a
    // block-level extension type parameter (a bare single-segment
    // non-nullable identifier in one of `target`'s generic-argument
    // positions, at any nesting depth)? Promoted from an ExprTypecheck.cpp
    // file-static (fix #3617-family drain, R10) to a member so
    // ExprTypecheck_Calls.cpp's copies of the same "is this name an in-scope
    // type parameter" question can share it instead of re-deriving a second
    // walk of `ast::Type::genericArgs`.
    bool isExtensionTargetTypeParam(const ast::Type& target,
                                    std::string_view name) const;
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
    // narrow #4059 (§9.5, v0.295): set (saved/restored) around the visit of
    // a `WithExpr`'s `source` so `checkNewExprArgs`'s §9.1/§9.2 body-only-record
    // rejection (#4059) does not fire when `new T() with(F := v)` is parsed —
    // the `with(...)` clause IS the construction form (§9.5), so the implicit-
    // default premise ("no declared ctor → error") is moot when the with-clause
    // supplies every field. Reset to false across a lambda boundary by virtue
    // of being a per-visit bool on the visitor itself, mirroring `inBindAtom_`'s
    // pattern.
    bool inWithExprSource_ = false;
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
    // §22.16: a `static` clause qualifier demands the proven tier. The
    // clause loop in ExprTypecheck.cpp's runMethodBody sets the kind and
    // `inContractClause_` flags before calling visitExpr(*c.expr); this
    // third flag carries the `static` bit (the AST node has it; the
    // sema layer keeps a mirrored bool to avoid an AST header include).
    // Set / restored inside visitExpr via a clause-entry guard that
    // matches the clause expression by pointer (the only AST node whose
    // pointer matches `currentMethodDecl_->clauses[i].expr.get()` is the
    // top-level clause visit — sub-expression visits never match, so
    // the flag stays set throughout the recursive walk and restores
    // cleanly on return).
    bool currentClauseIsStatic_ = false;

    // per §24: when about to visit an event-handler lambda argument
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

    // fix #4806 (§12.7 / §22.3, v0.307): the bare `ref`/`out` operands whose
    // §12.7 narrowing the IdentifierExpr visit deliberately did NOT apply
    // (they name the DECLARED location). Read only by the two §22.3
    // exact-referent diagnostics, which append a copy-out hint when the
    // mismatching operand is in this map: the user almost certainly meant
    // the narrowed type, and the sound way to pass it is a fresh local.
    //
    // narrow #4806 (§12.7 / §22.3, v0.307): maps to the NARROWED TypeRef
    // captured at the moment the narrowing was suppressed (rather than a bare
    // presence set), because the call-argument visit that runs right after
    // this one unconditionally erases `narrowings_[sym]` for every ref/out
    // operand (§7.4 invalidation, ExprTypecheck_Exprs.cpp ~9192) — by the
    // time checkArgsAgainstParams builds the diagnostic, `narrowings_` no
    // longer has the entry. Capturing the type here, before that erasure, is
    // what lets the hint verify the copy-out it suggests actually type-checks
    // (narrow #4806's Note A).
    std::unordered_map<const ast::Expr*, TypeRef*> refOutSuppressedNarrowing_;

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

    // fix #3756 (§27.4 / §27.1, v0.286): true while visiting a lambda literal
    // that is a DIRECT call- or constructor-argument — a target-type context
    // genuinely exists there (the matched formal's declared function-type
    // slot), even when this argument-visiting pass cannot thread its exact
    // per-parameter type through `pendingLambdaParamType_(s)_` (e.g. a
    // callee whose OWN generic type parameter the slot is written in terms
    // of, resolved later by call-site monomorphization rather than by this
    // thread — fix #829/#505's exclusion; or a `new` constructor argument,
    // which has no `pendingLambdaParamTypes_` seeding path at all). Consulted
    // by visitExpr(LambdaExpr) so an unannotated parameter is rejected only
    // when NO target-type context exists at all (§27.4: "In top-level
    // contexts with no target type ... parameter types must be specified"),
    // never merely because this specific thread came up empty. Consumed
    // (and cleared) once per lambda so a nested lambda, or an argument
    // lexically after the lambda in the same call, does not inherit a stale
    // `true`.
    bool pendingLambdaArgContext_ = false;

    // issue #777 / §30.6: true when `t` is a Named `Task<…>` / `OwningTask<…>`
    // (the async task wrapper). Used to detect a forgotten `await`. STRICT
    // (non-nullable): use for §30.1 / §30.6's declared-wrapper checks where a
    // `Task?` is a different question.
    bool isTaskShapedRef(const TypeRef* t) const;

    // fix #4443 (v0.302, §17.13 / §28.3 Keying rule): the task-family
    // predicate with nullability STRIPPED. Used by §17.13's discard + anonymous-
    // binding halves and by §28.3's mint-announcing sites (calls-mint, function-
    // slot threading) where the rule keys on "is this a Task-family value".
    bool isTaskFamilyRef(const TypeRef* t) const;

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
        const ast::LambdaExpr& lam, F&& fn) const {
        if (lam.exprBody) fn(lam.exprBody.get());
        std::vector<const ast::Stmt*> stack;
        for (const auto& s : lam.stmts) stack.push_back(s.get());
        while (!stack.empty()) {
            const ast::Stmt* s = stack.back();
            stack.pop_back();
            if (!s) continue;
            if (auto* rs = dynamic_cast<const ast::ReturnStmt*>(s)) {
                if (rs->value) fn(rs->value.get());
                continue;
            }
            if (auto* bs = dynamic_cast<const ast::DoEndBlock*>(s)) {
                for (const auto& s2 : bs->body) stack.push_back(s2.get());
                continue;
            }
            if (auto* ifs = dynamic_cast<const ast::IfStmt*>(s)) {
                for (const auto& s2 : ifs->ifBranch.body)
                    stack.push_back(s2.get());
                for (const auto& el : ifs->elifBranches)
                    for (const auto& s2 : el.body) stack.push_back(s2.get());
                for (const auto& s2 : ifs->elseBody) stack.push_back(s2.get());
                continue;
            }
            if (auto* ws = dynamic_cast<const ast::WhileStmt*>(s)) {
                for (const auto& s2 : ws->body) stack.push_back(s2.get());
                continue;
            }
            if (auto* fs = dynamic_cast<const ast::ForStmt*>(s)) {
                for (const auto& s2 : fs->body) stack.push_back(s2.get());
                continue;
            }
            if (auto* ts = dynamic_cast<const ast::TryStmt*>(s)) {
                for (const auto& s2 : ts->body) stack.push_back(s2.get());
                for (const auto& arm : ts->catches)
                    stack.push_back(arm.body.get());
                for (const auto& s2 : ts->finallyBody) stack.push_back(s2.get());
                continue;
            }
            if (auto* cs = dynamic_cast<const ast::CaseStmt*>(s)) {
                for (const auto& arm : cs->arms)
                    if (arm.bodyKind == ast::CaseArmBodyKind::Block)
                        for (const auto& s2 : arm.stmts)
                            stack.push_back(s2.get());
                continue;
            }
            // LocalDecl, AssignmentStmt, ExpressionStmt, BreakStmt,
            // ContinueStmt, ThrowStmt, YieldStmt, DiscardStmt — carry
            // expressions, not returns; nested lambdas inside those
            // expressions have their own callable and are excluded.
        }
    }

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

    // per §24: the payload type of an event-handler call's callee, or null
    // when `callee` is not `<event>.Subscribe/.Add/.Remove`. Used to set
    // pendingLambdaParamType_ before visiting the handler argument.
    TypeRef* eventHandlerPayloadType(const ast::Expr* callee);

    // §21.9 / §17.13 / §2.11 (#949): true when `e` is an `<event>.Subscribe(...)`
    // call. Subscribe returns an `owned Subscription` (a non-trivial-dispose
    // class, stdlib §2.11) but sema does not (yet) type the call result, so the
    // type-based drop/discard checks cannot see it. Recognized structurally so a
    // dropped or discarded Subscription is the hard error §21.9 / §17.13 require.
    bool isEventSubscribeDrop(const ast::Expr* e);

    // §28.9 / §22.5 (issue #1684): does `t`'s class chain make it
    // non-trivial-dispose — a destructor, an owning inline array (§15.1.1),
    // or a non-static `owned` field (declared, or a captured `owned`
    // primary-constructor parameter, §22.10, or of a NON-trivial-dispose
    // type per §28.9's recursive clause), anywhere from `t`'s declaration up
    // through its base classes? A thin wrapper over
    // `common::isNonTrivialDisposeClass` since #2654. The paragraph that
    // used to stand here said the §21.9 bare-discard walk in `visitStmt`'s
    // ExpressionStmt arm was "duplicated rather than shared because that
    // walk is not exposed as a callable predicate" — that walk is gone,
    // collapsed onto the same shared predicate by fix #3588, so there is one
    // body and this claim no longer holds.
    //
    // fix #3647 (§28.9 / §3.6 rule 2, v0.286): takes a `TypeRef` (not a bare
    // `TypeSymbol`) and adopts the widened `TypeRef+SemaContext` overload,
    // so this wrapper's callers see the §28.9 recursive clause and §3.6
    // rule 2's per-instantiation substitution instead of the pre-#3632
    // conservative "any owned field disqualifies" reading.
    bool isNonTrivialDisposeClass(const TypeRef* t) const;

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

    // fix #4310 (§5.1/§11.2): the per-ELEMENT type-conformance sibling of
    // checkBorrowingFamilyLiteralElements above. `checkAssignable` performs
    // the §5.1 conversion check (including class→interface §11.2 nominal
    // conformance via `isSubtype`) for a plain variable declaration or an
    // ordinary call argument, but has zero handling for
    // `TypeRef::Kind::InlineArray` — every call site that types a collection
    // literal pre-sets the literal's OWN exprType to the target type
    // (`ctx_.setExprType`) before the general `checkAssignable(ctx_.exprType
    // (*initializer), target, …)` call that follows, so that call trivially
    // compares the target to itself and never descends into the literal's
    // element expressions. This walks the literal's elements (and, for a
    // dict-form literal at a keyed target, its keys) and runs the ordinary
    // §5.1 check on each one individually — recursing for a nested literal
    // element (`[1]Array<IHolder>`) so the same gap one level down is closed
    // too. `target` must already be resolved to the collection's own type
    // (`[N]T` or a §15.4 builtin-collection head); a non-collection or
    // unresolved target is a no-op.
    void checkCollectionLiteralElementAssignability(
        const ast::CollectionLiteralExpr& cl, TypeRef* target);

    // stdlib §5 (D7): the source element type T of a LINQ operator/aggregate
    // call whose lambda argument is `(T) -> …` (Where/Select/Count/Any/All/
    // First/OrderBy/…), or null otherwise. Used to set pendingLambdaParamType_
    // before visiting the lambda argument so an untyped LINQ lambda parameter
    // is typed as the element type (its body can then resolve `p.x`).
    TypeRef* linqLambdaElementType(const ast::Expr* callee);

    // stdlib §2.20 / spec §35.2: the parameter type of the function argument
    // of a compiler-provided `Result<T, E>` value-payload combinator —
    // `Map(f: (T) -> U)` / `AndThen(f: (T) -> Result<U, E>)` take T,
    // `MapError(f: (E) -> F)` takes E. Used to set pendingLambdaParamType_
    // before visiting the lambda argument, exactly as the LINQ/event-handler
    // threads above do. Null when the call is not such a combinator.
    TypeRef* resultCombinatorLambdaParamType(const ast::Expr* callee);

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
    //
    // fix #3995 (§17.11/§9.7/§3.6 rule 2, v0.294): `incoming` is the
    // instantiation-site substitution environment (`common::RecordSubstEnv`,
    // OwnershipFacts.hpp, #2145) composed IN from an enclosing record frame
    // — mirroring `common::isBorrowBearingRecordCommon`'s own `env`
    // parameter, which the §9.7 borrow-bearing walk threads through its
    // identically-shaped recursion (OwnershipFacts.cpp). Every external
    // caller passes the default `nullptr` (a top-level `default(T)` check
    // has no enclosing frame); the Record and InlineArray arms
    // (ExprTypecheck_Exprs.cpp) compose it internally when recursing into a
    // field/element, so a DELEGATING generic record
    // (`record Outer<T>(In: Inner<T>)`) resolves `Inner<T>`'s own `T` back
    // to the OUTER instantiation's argument, not just a record's OWN
    // directly-bare-parameter fields.
    bool defaultIllFormed(const TypeRef* t,
                          const common::RecordSubstEnv* incoming = nullptr);

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
        const void* owner;        // ast::MethodDecl / ast::TypeDecl, or (#3644)
                                   // ast::ExtensionDecl
        std::string param;        // the type-parameter name
        SourceLocation loc;       // the `default(T)` site (for the note)
    };
    struct GenericInstantiationSite {
        const void* owner;        // fix #3719: the ast::MethodDecl being
                                   // called (checkMethodGenericConstraints),
                                   // OR the ast::TypeDecl / ast::ExtensionDecl
                                   // being constructed
                                   // (recordDefaultObligationsForConstruction)
                                   // — must match the corresponding
                                   // DefaultTypeParamUse::owner's kind.
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
    // Record one type-argument binding of a generic instantiation — a
    // generic METHOD's own call site (ExprTypecheck_Calls.cpp), or, since
    // fix #3719, a generic CLASS/RECORD construction or the extension(s)
    // targeting it (recordDefaultObligationsForConstruction).
    void recordGenericInstantiationSite(const void* owner,
                                        std::string_view param, TypeRef* arg,
                                        std::string argName,
                                        SourceLocation loc);
    // §3.6 rule 2 join, run once at the end of the pass.
    void checkDeferredDefaultInstantiations();

    // fix #3719 (§17.11 / §3.6 rule 2, v0.294): every prior call to
    // `recordGenericInstantiationSite` came from a generic METHOD's own call
    // site (ExprTypecheck_Calls.cpp's `checkMethodGenericConstraints`), with
    // `owner` unconditionally the called method's `ast::MethodDecl*`. A
    // `default(T)` obligation recorded against a class-level
    // (`currentType_->decl`, a `TypeDecl*`) or extension-level
    // (`currentExtension_->decl`, an `ExtensionDecl*`, #3644) owner could
    // therefore never match any recorded instantiation site and was dead
    // code since #3044 (issue #3719).
    //
    // Called once per `new T<...>()` against a generic CLASS or RECORD `t`
    // (after its own constructor-argument check), this records ONE
    // instantiation site per class-level type parameter (owner = `t->decl`),
    // and, because an extension shares its target type's concrete binding at
    // the SAME construction rather than at any call site of its own (an
    // extension is never itself "instantiated"), one per LOCAL extension
    // whose target names `t` at matching arity, for each of the target
    // type's bare free-type-parameter POSITIONS (owner = that extension's
    // own `ExtensionDecl*`).
    //
    // Scope (recorded here, not silently narrowed): local (non-imported)
    // extensions only — an imported extension's body is type-checked in its
    // OWN defining module's separate ExprTC pass, so this importing module's
    // `defaultTypeParamUses_` can never hold an entry keyed by that decl
    // pointer regardless of what gets recorded here; and only a target
    // written with a BARE free-parameter identifier at the matching
    // position (§26.4) — a concrete or nested target argument at that
    // position (`extension Box<Array<T>>`) is not a per-instantiation
    // variable at that position and is not walked recursively here.
    void recordDefaultObligationsForConstruction(const ast::NewExpr& ne,
                                                 TypeSymbol* t,
                                                 const TypeRef* rt);

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

    // fix #4183 (§14.5 phase 1, §26.5, v0.296/v0.298): the BUILTIN
    // counterpart of collectAncestorInstantiationsOf above. A builtin
    // collection type (`Array<T>`, `IList<T>`, ...) has no TypeSymbol —
    // `declaration` is null — so collectAncestorInstantiationsOf's
    // `baseTypeRefs` walk finds nothing for it; its interface hierarchy is
    // instead modelled by `builtinInterfaceAncestorsOf`
    // (ExprTypecheck.cpp, §26.5's extension-target matcher) — the SAME
    // table checkAssignable's own `variantGenericAssignable` already
    // consults for ordinary `Array<int>` → `IReadOnlyList<int>`
    // assignability (v0.180/#1180). Exposed as a member here (that helper
    // has internal linkage, file-local to ExprTypecheck.cpp) purely so
    // unifyArgSubst's constructed-generic-type case
    // (ExprTypecheck_Calls.cpp) can reuse it — no second, drifting builtin-
    // hierarchy table. `classTy` must be a null-declaration Named builtin;
    // `ifaceName` is the target's simple builtin name (`"IReadOnlyList"`,
    // never alias- or declaration-bearing — builtins are never aliased).
    // Appends every reached ancestor whose name matches (`classTy` itself
    // counts, mirroring collectAncestorInstantiationsOf's "argTy IS the
    // declaration" case) to `out`; `owned` keeps the synthesised ancestor
    // TypeRefs alive for the caller's use of `out`.
    void collectBuiltinAncestorInstantiationsOf(
        const TypeRef* classTy, std::string_view ifaceName,
        std::vector<TypeRef*>& out,
        std::vector<std::unique_ptr<TypeRef>>& owned);

    // fix #4892 (§5.1 reference conversions, §12.9, v0.307): does the
    // user-declared Named type `from` reach an instantiation of the BUILTIN
    // generic interface head of `to` (null declaration, e.g. `IEnumerable<T>`)
    // that converts to `to` under §12.9? Walks `from`'s user ancestors with
    // substitution (as collectAncestorInstantiationsOf), their no-TypeSymbol
    // builtin bases, and the builtin hierarchy table from each. Returns true
    // (conservative accept) when no instantiation of the head is found — base
    // conformance is userTypeImplementsBuiltinIface's question, not this one's
    // — or when an unmodeled builtin base leaves the closure incomplete; false
    // only when every instantiation found is provably incompatible.
    bool userTypeBuiltinInstantiationConverts(
        const TypeRef* from, const TypeRef* to,
        const std::unordered_set<std::string_view>* freeNames);

    // §14.1/§14.5 phase 1 (#785, #850, #896, #4089/#4183): structurally
    // unify a value parameter's declared type (`pt`, AST) against the call
    // argument's resolved type (`argTy`), binding each method type
    // parameter named in `paramNames` to the matching component of the
    // argument's type — a bare `p: T` binds T; a generic-typed
    // `p: Pair<A, B>` recurses pairwise into the generic args; a
    // constructed `p: G<...>` also unifies against the UNIQUE instantiation
    // of G that argTy's declared type implements or extends, via
    // collectAncestorInstantiationsOf (§14.5's full sentence — see #4183);
    // a function-typed `p: (...) -> ...` unifies component-wise over equal
    // parameter counts only (§14.5/§22.6, #4089). A member function (not a
    // free function) because the constructed-generic-type case needs
    // collectAncestorInstantiationsOf, which is private. `conflicts`
    // (optional) receives the name of any type parameter whose second-
    // occurrence inferred binding is DIFFERENT from the first-occurrence
    // binding.
    void unifyArgSubst(
        const ast::Type* pt, TypeRef* argTy,
        const std::unordered_set<std::string_view>& paramNames,
        std::unordered_map<std::string_view, TypeRef*>& argSubst,
        std::unordered_set<std::string_view>* conflicts = nullptr);

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
    // arms' common-type computation sees the substituted form. Extended by
    // fix #3898 (§14.1 / §19.3, v0.294) to a GENERIC RECORD's positional
    // field type the same way — a record declaration carries `genericParams`/
    // instantiations carry `genericArgs` in the identical shape, so this one
    // walk serves both `scrutTy->declaration->kind`s (Choice or Record)
    // rather than a second, drifting copy for records. Walks `ty` and
    // substitutes every bare-name TypeRef whose declaration is a
    // GenericParamSymbol of `scrutTy->declaration` with the corresponding
    // entry from `scrutTy->genericArgs`. Recurses into Named genericArgs,
    // InlineArray element types, Function parameter/return types, and the
    // nullable wrapper. Returns `ty` unchanged when no substitution applies
    // (including when `scrutTy->declaration` is a class/interface/enum).
    // Mirrors (the field-walk part of) Resolver::applyGenericSubst +
    // buildBaseGenericSubst — local copy because the ExprTC worker has no
    // Resolver handle (#1621 / §31.2.1 split). #3938 (§19.3 / §14.1,
    // v0.294): NOW threaded into `bindNestedDestructuringNames` — the
    // four caller loops (CaseExpr x CaseStmt, record x choice-payload)
    // pre-populate the nested-pattern worklist with each pending nested
    // pattern's own already-substituted scrutTy (the parent slot's
    // substituted type), and the walk calls this helper to re-substitute
    // the next level's slot types against that scrutTy. Previously the
    // helper was not reached in `bindNestedDestructuringNames` (the walk
    // re-derived each nested pattern's record purely by NAME), so a
    // generic record slot nested one level down (or nested inside a
    // choice-case payload) wrongly admitted a range/relational pattern
    // over the substituted nullable type — see that function's comment
    // for the full rationale.
    TypeRef* substituteGenericFieldType(const TypeRef* ty,
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

    // fix #4655 (§5.1, v0.307): names the specific §5.1 "never implicit" /
    // "constant narrowing" sub-rule that rejects a Primitive-Primitive pair
    // `checkAssignable` has already determined is neither identity nor a
    // `primitiveWidens` widening — in the spec's own wording — or returns
    // empty when none of the closed-form rules names the pair. `fromExpr`
    // (nullable) is consulted for the constant-narrowing case; see the
    // definition for the precedence rationale.
    std::string primitiveNeverImplicitReason(const ast::Expr* fromExpr,
                                             PrimitiveType from,
                                             PrimitiveType to);

    // fix #4501 (§5.1/§6.2/§17.3/§8.4, v0.306): THE answer to "does §5.1
    // admit an implicit conversion between these two operand types?", asked
    // once for every binary operator that needs it. `typeBinary` carried TWO
    // copies of this question — one in the equality/relational arm, one in the
    // §6.2 arithmetic-promotion arm — and BOTH bailed out on any pair that was
    // not numeric-vs-numeric, which is how `string = 0`, `bool = 0`,
    // `bool = "x"` and `decimal = "x"` reached codegen. Reports and returns
    // true iff the pair is inadmissible; the caller then does its own
    // error-recovery typing.
    //
    // `allowLiteralAdoption` is the §5.1 constant-narrowing escape ("`uint >
    // 1000`"): the equality/relational arm has had it since #1204. fix #4512
    // (§5.1/§6.2, v0.306) enables it for the arithmetic arm too (`ulong +
    // 1000` now admits, matching `ulong > 1000`) and narrows the escape's own
    // predicates (`isLiteralDerivedExpr`, and the true-integer-only target
    // check inside this function) to §5.1's exact domain — see that fix's
    // citation trail for why the escape needed narrowing before it could be
    // safely enabled for a second call site.
    bool reportNoImplicitConversionBetweenOperands(const ast::BinaryExpr& be,
                                                   std::string_view op,
                                                   const TypeRef* lt,
                                                   const TypeRef* rt,
                                                   bool allowLiteralAdoption);

    // fix #4513 (§5.1/§6.2/§6.6, v0.306): the SAME question as above, asked
    // from a position with no `ast::BinaryExpr` to hand — the §6.6 compound-
    // assignment family check (ExprTypecheck.cpp), whose "operands" are an
    // AssignmentStmt's `target`/`value`. The BinaryExpr overload delegates
    // here; `leftExpr`/`rightExpr` are only consulted for the §5.1 constant-
    // narrowing escape's literal-derived test (`isLiteralDerivedExpr`) and may
    // be null (then the escape never applies from that side).
    bool reportNoImplicitConversionBetweenOperands(SourceLocation loc,
                                                   std::string_view op,
                                                   const TypeRef* lt,
                                                   const TypeRef* rt,
                                                   bool allowLiteralAdoption,
                                                   const ast::Expr* leftExpr,
                                                   const ast::Expr* rightExpr);

    // fix #4513 (§3.5/§6.6, v0.306): the §3.5 per-operand-type table's `bool`
    // exclusion from the arithmetic (`+ - * /`) and ordering (`< <= > >=`)
    // families — fix #4500's typeBinary gate, extracted so the §6.6 compound-
    // assignment family check can ask the IDENTICAL question about the SAME
    // (target, right-hand-operand) pair instead of falling through to the
    // §6.6 write-back diagnostic, which names a binary result type (`bool`)
    // the binary path no longer computes for that pair. `+` defers to string
    // concatenation when either operand is `string` (§4.6 is the more
    // specific authority there), exactly as the typeBinary gate does. Reports
    // and returns true iff the pair is excluded; unlike the BinaryExpr call
    // site, the caller here does its own consequence (typeBinary does
    // error-recovery typing; the compound-assignment check just stops).
    bool reportBoolOperandExcludedFromArithmeticOrOrdering(
        SourceLocation loc, std::string_view op, const TypeRef* lt,
        const TypeRef* rt);

    // Slice T4: is `from` a subtype of `to` — i.e. does `to` appear in
    // `from`'s transitive base-type chain (base class or implemented
    // interface)? BFS, cycle-guarded.
    //
    // fix #4306 (§26.4/§26.5, v0.298): widened from `private` to `public`
    // (bracketed rather than moved, to keep this declaration beside its
    // Slice T4 neighbours) so the free function
    // `selfCallSiblingWhereSatisfiedByCaller` (ExprTypecheck_Calls.cpp,
    // outside this class, in an anonymous namespace) can reuse this EXACT
    // authority for its own `type`-item subtype-entailment check, instead
    // of a second, drifting spelling of the same BFS (§6 in this run's
    // standing lane rules: "is there a fact the compiler ALREADY COMPUTES
    // that decides this? Prefer feeding the existing authority"). No other
    // change — same signature, same body, every existing call site
    // unaffected by widened (never narrowed) access.
public:
    static bool isSubtype(TypeSymbol* from, TypeSymbol* to);
private:

    // fix #3559 (stdlib §5, v0.285): the shared REMOVED-at-v0.284 diagnostic
    // text both `OfType<U>()` typing arms raise — unconditionally, since the
    // member no longer exists. ONE spelling, because two arms reach `OfType`
    // and a message that drifted between them would make a half-fix read as a
    // whole one. (Issue 3440's interim soundness predicate
    // `ofTypeIdentityRelabelIsSound` was deleted with the member: it decided
    // WHICH pairs to admit, and no pair is admitted now.)
    static std::string ofTypeRemovedMessage(const TypeRef* srcElem,
                                            const TypeRef* outElem);

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
    // `freeTypeParamNames` (#4199): the set of type-parameter names this
    // specific call is entitled to treat as a FREE/rigid variable rather than
    // a concrete type — e.g. the callee method's own still-being-inferred
    // generic parameters at a call-argument check, or the enclosing scope's
    // rigid parameters at a pattern-exhaustiveness check. Null (the default)
    // means NONE — the ground-truth-conservative default every call site
    // that is not explicitly reasoning about a specific free-variable set
    // gets, matching `isFullyGroundTypeRef`'s existing conservative stance.
    void checkAssignable(
        TypeRef* from, TypeRef* to, SourceLocation loc,
        const std::string& what, const ast::Expr* fromExpr = nullptr,
        const std::unordered_set<std::string_view>* freeTypeParamNames =
            nullptr);

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

    // §14.1 / §12.9 (#4199): the SET of type-parameter names in scope at the
    // current position — the enclosing type's (currentType_), the enclosing
    // method's (currentMethodDecl_), and an enclosing extension's target-type
    // parameters (§26.4/#3083, the same rule `isExtensionTargetTypeParam`
    // tests one name at a time). Ground truth for "is a Named TypeRef with a
    // null declaration a genuinely FREE/rigid type variable, or a
    // null-declaration stdlib builtin that merely has the same shape" — see
    // `isFreeGenericTypeVarRef` (ExprTypecheck.cpp): a bare Named TypeRef with
    // no declaration is NOT sufficient on its own (issue #4199 — ~50 stdlib
    // builtins, e.g. `Task`, `Exception`, `IDisposable`, resolve to exactly
    // that shape, per `isFullyGroundTypeRef`'s own comment a few hundred
    // lines above: "a null-declaration stdlib builtin ... treated as
    // non-ground" — the SAME ambiguity that function already resolves
    // conservatively).
    //
    // Deliberately a SEPARATE small scan from `mentionsUnsubstitutedTypeParam`
    // (not a shared refactor of it): that function answers a boolean
    // "mentions any" question via `refOutParamTypeMentionsTypeParam` and its
    // existing callers/behaviour are out of this fix's scope; this one needs
    // to RETURN the set so `variantGenericAssignable`'s recursion can test
    // membership at arbitrary nesting depth.
    std::unordered_set<std::string_view> currentScopeGenericParamNames() const;
    // Sibling of `isExtensionTargetTypeParam` (which tests one name) that
    // collects EVERY name the same walk would recognize, for
    // `currentScopeGenericParamNames`'s extension arm.
    void collectExtensionTargetTypeParamNames(
        const ast::Type& target,
        std::unordered_set<std::string_view>& out) const;

    // fix #4810 (§14.2): does the named type parameter carry ANY `where`-
    // clause constraint in the current enclosing type/method/extension
    // declaration? The #4810 arm rejects primitive-literal initializers into a
    // type-parameter-typed storage slot at the DECLARATION (per the issue
    // body's "unconstrained type parameter" wording), but the spec's
    // instantiation-time check (§14.2: "operations that copy T (assignment,
    // parameter passing by value, returning by value) are checked at
    // instantiation") deliberately keeps the CONSTRAINED case as a deferred-
    // to-instantiation conversion; the deferred-to-instantiation path is the
    // one an existing test (`generic_ref_out_exact_unification_1686__…`) pins
    // and the fix's narrow issuance must not collide with. Answering "does
    // ANY `where T: …` clause in scope name this param?" is enough — a clause
    // for a name means the parameter is constrained and we skip; absent any
    // clause the parameter is unconstrained and we fire.
    bool isCurrentScopeTypeParamConstrained(std::string_view name) const;

    // fix #4229 (§31.2.1 / §12.7 / §2.8.3 rule 4 / §17.4, §19.3, v0.300): does
    // `t` mention a free/rigid type parameter of the ENCLOSING scope anywhere —
    // the shape (`Named` + no decl + no args + non-empty head) used as a proxy
    // by the file-local `refMentionsTypeVariable` (~7448) is ALSO the shape
    // every null-declaration stdlib builtin (`Task`, `Exception`, `IDisposable`,
    // `Mutex`, `Semaphore`, `DateTime`, `TimeSpan`, `Guid`, `StringBuilder`,
    // `Random`, `File`, `Path`, `IntPtr`, `CString`, ~50 names — #4229) takes
    // after type resolution, so the structural guess returns TRUE for those
    // builtins and the #1927/#1939/#2006/#2012/#2018 erasure guards silently
    // admit `MyExc<Task>` arms / `Wrap<Task>` `is` tests / `var x: Wrap<Task>`
    // case bindings at a non-distinguishable runtime descriptor (the
    // §2.8.3 rule 4 confusion class). The fix: thread the actual declared
    // names in via `currentScopeGenericParamNames()` and reuse
    // `refOutParamTypeMentionsTypeParam`'s exact-match walk — the same authority
    // `mentionsUnsubstitutedTypeParam` (line ~3805) and
    // `variantGenericAssignable`'s `freeNames` set (#4199) already trust, so
    // a null-declaration builtin that isn't one of the enclosing scope's
    // generic parameters returns FALSE here, and the downstream guard proceeds
    // to its reification check (#2022's `typeArgRuntimeReified`).
    //
    // Used by the catch-arm surface (#1939, ExprTypecheck.cpp ~17400),
    // `typeTestInterfaceBindAmbiguous` (#1927, ExprTypecheck.cpp ~7691), the
    // plain `is`/`as` surface (#2006, ExprTypecheck_Exprs.cpp ~2409), and the
    // value-form case binding (#2018, ExprTypecheck_Exprs.cpp ~10360).
    bool refMentionsTypeVariable(const TypeRef* t) const;
    // Sibling of `refMentionsTypeVariable` for the value/expression `case`
    // handler — was `typeTestTargetMentionsTypeVariable` in
    // ExprTypecheck_Exprs.cpp's anonymous namespace, identical defect class.
    bool typeTestTargetMentionsTypeVariable(const TypeRef* t) const;

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

    // §17.6/§22.8 (v0.298, #4290): the shared readonly-FIELD write-target
    // check (construction-window carve-out + `readonly` modifier
    // rejection), factored out of `checkReadonlyAssignment` so BOTH
    // AssignmentStmt and IncrementStmt consult ONE implementation instead
    // of IncrementStmt having none at all — the same #2868 shape already
    // used for `checkPropertyWriteTarget`. `incOp` is null for an
    // assignment target and the operator token ("++"/"--") for an
    // increment target.
    void checkReadonlyFieldWriteTarget(const ast::Expr& target,
                                        const SourceLocation& loc,
                                        const char* incOp);

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
    // §9.4 (v0.236 / v0.307 "Value receivers are borrows"): in a record
    // method body (not ctor, not static), reject any store that would land
    // in the receiver's own storage — `this.F := e`, bare `F := e`, the
    // same chain passed as `ref`/`out`, and `++`/`--` over the same shape.
    // Called from checkReadonlyAssignment (every AssignOp), from
    // checkRefOutOperandCategory (ref/out arguments), and from the
    // IncrementStmt site. Returns true if a diagnostic was reported.
    bool checkRecordReceiverStore(const ast::Expr& target,
                                  std::string_view op,
                                  const SourceLocation& loc);

    // fix s12_7_0002 / s12_7_0003 (§12.7, v0.307): true iff `sym` (a local
    // or parameter) is CURRENTLY §12.7-narrowed (`narrowings_`), its
    // DECLARED type is the box-holding `Object`/interface location
    // (mirrors `narrowFromTypeTest`'s own `isBoxHoldingLocation` lambda so
    // the two agree by construction), and the narrowed type is a VALUE
    // type — i.e. narrowing put a §8.6 box in play, as opposed to an
    // ordinary reference-type narrowing, which §12.7 states is unaffected
    // ("Narrowing to a *reference* type is unaffected: no box is
    // involved, and writes through the narrowed binding are ordinary
    // reference-type writes").
    bool narrowedToBoxedValue(Symbol* sym);

    // fix s12_7_0002 / s12_7_0003 (§12.7, v0.307): §12.7's box-write rule,
    // positions (i) (assignment/compound-assignment/++/--) and (ii) (a
    // `ref`/`out` argument) — "A write whose store would land in the box
    // is a compile error ... an lvalue chain (grammar A.6 `LValue`)
    // extending the binding with a `.` or `[...]` hop." Walks `target`
    // exactly as `checkLValueChainNotCallBased` does (peeling
    // `.member`/`[index]`/postfix-`!` hops down to the chain's base) and,
    // when the base is a §12.7-narrowed-to-a-boxed-value binding AND at
    // least one `.`/`[...]` hop was peeled, reports the box-write error
    // and returns true. A hop count of zero (a bare `o := …` / `ref o`)
    // is the admitted bare-binding form and is left untouched — position
    // (i)'s bare-binding carve-out is `checkReadonlyAssignment`'s own
    // business, and (ii) never passes a bare identifier through this
    // function's hop gate in the first place.
    bool checkNarrowedBoxWrite(const ast::Expr& target, std::string_view verb,
                               const SourceLocation& loc);

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
    Symbol* lookupMember(TypeSymbol* type, std::string_view name) const;

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

    // §27.8 (v0.294; fix #4050/#4051): resolve a `TypeAliasSymbol` through to
    // the TypeSymbol its `aliasedType` names (a class/record/interface/enum).
    // The ONE fact this family of "does this WRITTEN NAME denote a type"
    // classification predicates needs, and — per the #4038/#4048/#4050/#4051
    // review chain — the shared seam those sites should all resolve through
    // rather than each hand-rolling the same three-line lookup (LANE_RULES
    // §6's "duplicated classification predicate" hazard: fix #4038's
    // `resolveMemberAccess` arm and fix #4048's `altPartsNameType` lambda are
    // two independent copies already; this is written once here and reused
    // by both of THIS commit's fixes rather than becoming a third and fourth
    // copy). `aliasSym->decl`'s `aliasedType` was already eagerly resolved by
    // `Resolver::resolveTopDecl` when the `type` declaration itself was
    // processed (TypeResolution.cpp) — the same fact fix #578's type-position
    // alias transparency also reuses.
    //
    // Returns null when: the alias is somehow declaration-less (should not
    // happen for a module-internal alias, §27.8 — defensive only); the
    // aliased type is not `TypeRef::Kind::Named` (a primitive, function
    // type, or bare generic-parameter target — none of those are the
    // TypeSymbol-based "type" this family recognizes); OR — #4047 — the
    // alias's `aliasedType` has not yet been resolved because it is declared
    // TEXTUALLY BEFORE the (possibly further-aliased) type it names, and
    // `Resolver::resolveTopDecl`'s single forward pass over `src.topDecls`
    // has not reached it yet. Callers inherit that same known-open ordering
    // gap; #4047 tracks it, not this helper.
    TypeSymbol* resolveAliasToTypeSymbol(TypeAliasSymbol* aliasSym);

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

    // fix #4801 (§5.2 / §17.14 / §6.3 / §6.4, v0.307): the floating/decimal
    // sibling of foldIntConstExprTC. Folds a §17.14 constant expression whose
    // static type is `float`, `double`, `decimal` or an integer type to its
    // value under the run-time semantics §17.14 requires: IEEE 754 for the
    // binary floats (a `float`-typed node computes in single precision, as
    // codegen does; overflow to ±Infinity and NaN are VALUES), and the
    // runtime's own exact decimal ops for `decimal`. Accepted forms: literals,
    // `const` references (local, class-level, qualified `C.K`, and the
    // primitive-wrapper constants `double.MaxValue` / `double.NaN` / …),
    // unary `+`/`-`, binary `+ - * /` (and `mod` on decimal), parentheses (no
    // AST node), and integer subtrees via foldIntConstExprTC. nullopt for
    // anything else, and for a subexpression that itself faults (a decimal
    // overflow or division by zero, a literal that failed §4.3/§4.4
    // formation) — the conversion-site rule then stays silent rather than
    // cascading.
    struct FoldedNumConst {
        enum class Kind { Int, Float, Double, Decimal } kind = Kind::Int;
        long long i = 0;          // Kind::Int (ULong values are declined)
        double d = 0.0;           // Kind::Float (exactly a float) / Double
        std::uint32_t dec[4] = {0, 0, 0, 0};  // Kind::Decimal, bz_decimal_t
    };
    std::optional<FoldedNumConst> foldNumConstExprTC(const ast::Expr& e,
                                                     int depth = 0);

    // fix #3617 (§17.14 / §15.1 / §22.12 / §2.8.3 rule 4, v0.286): the ONE
    // bare-identifier resolver the three ExprTC §17.14 constant folds share.
    // Resolves `name` through `lookupName` (the lexical scope chain plus the
    // enclosing type's member consts, including an IMPORTED base's) to a
    // compile-time integer, consulting BOTH sources: the AST initializer when
    // the symbol has one (`ast::LocalConstDecl` / `ast::ConstDecl`), and
    // `ConstMemberSymbol::intValue` — fix #3230's wire-carried value — when it
    // does not, which is every imported const (§2.5 reconstructs the symbol
    // with no AST). `depth` is the recursion depth `foldIntConst` bounds.
    //
    // It exists because this was three hand-copied lambdas and only ONE of them
    // (foldIntConstExprTC's) carried #3230's imported arm; see the definition
    // for what the other two silently answered.
    std::optional<long long> foldConstIdentTC(std::string_view name, int depth);

    // fix #4856 (§17.14 / §2.8.3 / §22.12, v0.307): a `const` reference
    // denotes that const's value, and the names in its initializer were bound
    // where it was DECLARED — visitMember's ConstDecl arm typechecks a class
    // const's initializer with `currentScope_` = the module scope and
    // `currentType_` = the declaring type, looking bare names up in value
    // position. The §17.14 folds used to follow the initializer and re-resolve
    // its names at the USE site, so a same-named local or member there
    // (`A.K := M1` read from a class that also declares `M1`) substituted the
    // wrong value. `inConstDeclScope(s, f)` runs `f` with lookup positioned as
    // that visit had it when `s` is a class const with a known declaring type
    // (otherwise `f` runs unchanged); `foldInDeclScope_` makes the folds'
    // bare-name lookups use the same value-position order the visit used.
    bool foldInDeclScope_ = false;
    template <class F>
    auto inConstDeclScope(const Symbol* s, F&& f) -> decltype(f()) {
        auto* cms = dynamic_cast<const ConstMemberSymbol*>(s);
        if (!cms || !cms->parent || !moduleScope_) return f();
        Scope* const saveScope = currentScope_;
        TypeSymbol* const saveType = currentType_;
        const bool saveFlag = foldInDeclScope_;
        currentScope_ = moduleScope_.get();
        currentType_ = cms->parent;
        foldInDeclScope_ = true;
        auto r = f();
        currentScope_ = saveScope;
        currentType_ = saveType;
        foldInDeclScope_ = saveFlag;
        return r;
    }

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

    // fix #4843 / §5.3 / §5.2 / §17.14 (v0.307): `T?(x)` carries "its
    // constant-operand compile error (§5.2, §5.4)" — `T?(x)` is `T(x)` for
    // §5.2's constant-operand rule. The non-nullable arm of the rule
    // already runs in `typeCall` for the `CallExpr` shape (`T(x)`); the
    // nullable arm runs through this helper from the `NullCondCallExpr`
    // handler in ExprTypecheck_Exprs.cpp (`T?(x)`, where `?(` parses as a
    // 2-char punctuator). `target` is the cast's destination primitive
    // type; `targetName` is its textual spelling (e.g. `"int"` for
    // `PrimitiveType::Int`) for the diagnostic; `operand` is the
    // conversion's single argument; `loc` is the source location for the
    // diagnostic. The decimal-fault arm of `foldNumConstExprTC` already
    // emits its own §17.14 error at the constant expression (#4844); this
    // helper adds the integer-narrowing and float/double/decimal → integer
    // checks so `uint?(-1)` and `int?(1.0e30)` are §5.2 compile errors at
    // the conversion site.
    void checkNullableCastConstantOperand(const ast::Expr& operand,
                                          PrimitiveType target,
                                          std::string_view targetName,
                                          SourceLocation loc);

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
    // the true branch (§12.7), and (fix #3877, v0.294) `or`-joined tests
    // contribute all their FALSE-facts to the false branch — the De Morgan
    // dual. The condition must already have been visited so operand symbols
    // and the test type are resolved. Entries are written into
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

    // §7.4 / §7.11 (v0.286, #3298): the ONE "is this operand read as
    // non-null here?" test. §7.11 reads an operand's STATIC type and states
    // that "an operand that §7.4 has narrowed non-null is a non-nullable
    // operand and the ordinary operator applies to it unchanged" — so every
    // §7.11 gate (binary arithmetic/bitwise/shift, relational, unary, the
    // #1575 `string?` ordering guard, the §17.5 `in`-over-a-range gate) has to
    // ask exactly this question. It was spelled FIVE times, each an independent
    // copy of the same lookup; a sixth copy is how the family drifts.
    //
    // §7.4 narrows TWO kinds of storage and this consults both:
    //   * a simple local or parameter — `nullNarrowed_` / `narrowings_`, via
    //     isKnownNonNull (`this`/`base` are never narrowable operands);
    //   * a `readonly` FIELD path (`this._f`, `TypeName._s`, `local._f`) —
    //     `fieldNarrowed_`, v0.214/#2085/#2116, admitted because a readonly
    //     field has no writer after construction and so no TOCTOU hazard.
    // A MUTABLE field or a property never narrows and is excluded by both.
    bool isNarrowedNonNullRead(const ast::Expr* e);

    // §7.11 (v0.286, #3298): the static type an operand is READ AT, which is
    // the declared type with the nullable flag dropped when
    // `isNarrowedNonNullRead` holds. Returns `declared` unchanged for every
    // other operand, so a caller can normalize both operands once and then
    // compute §6.2 promotion and the result type without a narrowing special
    // case of its own.
    TypeRef* operandStaticTypeAt(const ast::Expr* e, TypeRef* declared);

    // §19.3 / §16.6 / §7.11 (v0.286, #3298): reject a relational or range
    // pattern in `arms` when the scrutinee's static type is nullable. Shared by
    // the CaseExpr and CaseStmt sites — the two forms take the same rule and
    // the same diagnostic, and §19.3 keys the rejection on the pattern's KIND,
    // not on which bounds are written.
    void checkOrderingPatternsOverNullableScrutinee(
        const TypeRef* scrutTy, const std::vector<ast::CaseArm>& arms);

    // §19.3 (v0.294, #3692): the single-pattern walk `
    // checkOrderingPatternsOverNullableScrutinee` runs per arm — factored out
    // so a destructuring SLOT's sub-pattern can be checked against ITS OWN
    // declared type (which need not be the arm scrutinee's type) at all four
    // slot-resolution sites (CaseExpr x CaseStmt, record x choice payload)
    // without a fifth copy of the walk. §19.3: "Each slot's right-hand side is
    // a full pattern … a binding, `_`, a constant, `null`, a range, a
    // relational, …" — the relational/range rejection is the same rule the
    // arm form already has, keyed on the SLOT's declared type, not the
    // scrutinee's.
    void checkOrderingPatternOverNullableType(const TypeRef* ty,
                                              const ast::Pattern* pat);

    // fix #3938 (§19.3 / §14.1, v0.294): each pending nested
    // DestructuringPattern carries its own ALREADY-SUBSTITUTED scrutinee
    // TypeRef — the slot in the parent pattern whose substituted type is
    // this nested pattern's scrutinee type — so the walk can substitute
    // the next level's bare generic-parameter fields (`Outer(Box(1..5, 1),
    // 9)` against `Outer<int?>` needs the nested `Box<T>` slot seen as
    // `Box<int?>`, not the bare `Box<T>` template) the same way the four
    // caller loops already do via `substituteGenericFieldType(fieldTy,
    // scrutTy)`. #3898 left this walker untouched: it re-derived each
    // nested pattern's record purely by NAME and received no scrutinee
    // TypeRef at all, so a generic record slot nested one level down (or
    // nested inside a choice-case payload) wrongly admitted a
    // range/relational pattern over the substituted nullable type.
    struct PendingNestedDestructuring {
        const ast::DestructuringPattern* pat;
        TypeRef* scrutTy;
    };

    // #726 / §19.3 / #3909 / #3938 (§35.5, v0.294): the ONE "bind names
    // inside a NESTED DestructuringPattern sub-pattern" walk
    // (`Outer(Inner(v), b)`). Before #3909, this walk existed ONLY inline
    // in the two RECORD-scrutinee DestructuringPattern handlers (CaseExpr
    // in ExprTypecheck_Exprs.cpp, CaseStmt here) — copy-pasted between
    // them — and the two CHOICE-case-payload handlers had no equivalent
    // at all, so `Wrap(Inner(v)) => v` over a choice case left `v`
    // unresolved even though the identical shape over a record
    // (`Outer(Inner(v)) => v`) worked. Factored out here (mirroring the
    // #3692 checkOrderingPatternOverNullableType precedent just above,
    // the same "four call sites, one walk" shape) so the CHOICE handlers
    // gain the capability by calling this, rather than by copying the walk
    // a THIRD and FOURTH time — landing #3909 this way REDUCES the count
    // of raw copies (2 -> 0 hand-copies, 1 shared implementation) while
    // adding the missing choice-side capability.
    //
    // Before #3938 the function took `subPatterns` (the OUTER pattern's
    // own slot list) and built its own worklist by name-only lookup of
    // any nested `DestructuringPattern` inside `subPatterns`. That lost
    // the parent slot's substituted type — the very thing §19.3's
    // nullable-ordering-pattern clause keys on — so the substitution the
    // four caller loops already compute for their OWN top-level slot was
    // thrown away one level down. #3938 changes the signature to take
    // the worklist directly (each pending pattern pre-tagged with its
    // own substituted scrutTy) so the substitution threads through any
    // depth of generic-record nesting.
    //
    // #1281: sets each nested binding's declType from its inner record's
    // positional field so a member access on a nested class/record
    // binding in the arm body resolves (a null declType left
    // `Outer(Inner(x)) => x.Id` unresolved / miscompiled).
    void bindNestedDestructuringNames(
        Scope& armScope,
        std::vector<PendingNestedDestructuring> nestedWork);

    // §7.4 (issue #418): update `nullNarrowed_` for an assignment of a value of
    // `valueType` to the simple variable `sym` — a non-null value makes it
    // known-non-null; a (possibly-)null value clears that knowledge. `value`
    // is the assigned expression where the caller has it (see
    // `valueProvesNonNull` for why the type alone is not sufficient).
    void narrowOnAssignment(Symbol* sym, const TypeRef* valueType,
                            const ast::Expr* value = nullptr);

    // §7.4 (v0.286, #3298): does this assigned/initialising VALUE prove the
    // target non-null? The rule was spelled `!valueType->nullable` at all three
    // establishment sites, and that is wrong for a §5.3 constant-narrowed
    // literal: `var a: byte? := 3;` retypes the literal to the TARGET type
    // `byte?`, so the type test said "possibly null" about the constant 3 and
    // no narrowing was established. `int?`/`long?`/`double?` were unaffected
    // (their literals keep a non-nullable type), which made §7.4 — and every
    // rule reading it, §7.11's operator gate included — answer differently for
    // `byte?` than for `int?` at identical shapes. A literal is never `null`
    // except `NullLiteralExpr`, so consulting the expression settles it.
    bool valueProvesNonNull(const ast::Expr* value, const TypeRef* valueType);

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
    //
    // fix #3774 (§22.13 / §23.1, v0.286): the rule is storage-shaped, not
    // syntax-shaped — a `static readonly` AUTO-PROPERTY's initializer is
    // "a store to the backing field … in the declaring type's own static
    // initialization" (§23.1), i.e. one of the very stores §22.13 orders
    // "in textual declaration order". Both the SOURCE and the TARGET of the
    // forward reference are therefore taken over static-readonly fields AND
    // static-readonly auto-properties, which is why the core takes an
    // `ast::Member*` (the two decl kinds' common base) rather than a
    // `FieldDecl*`. The PropertyDecl overload passes `pd->autoInitializer`.
    void checkStaticReadonlyForwardRef(const ast::FieldDecl* fd);
    void checkStaticReadonlyForwardRef(const ast::PropertyDecl* pd);
    void checkStaticReadonlyForwardRefImpl(
        const ast::Member* srcDecl, std::string_view srcName,
        const std::vector<std::string_view>& srcMods, const ast::Expr* init);
    // fix #3774: is `s` a member of the current type whose STORAGE is written
    // by §22.13's textual-order static-initialization pass and declared
    // `readonly` — a `static readonly` field, or a `static readonly`
    // auto-property (§23.1's generated backing field)? Returns the symbol's
    // declared name when it is, empty otherwise.
    static std::string_view staticReadonlyStorageName(const Symbol* s);

    void visitStmt(const ast::Stmt& s);

    void visitExpr(const ast::Expr& e);

    // Slice 3h: resolve a CallExpr's return type from the callee's
    // resolved MethodSymbol.
    void typeCall(const ast::CallExpr& ce);

    // fix #4325 (§5.1 / §11.2, v0.298): `explicitSubst` is the METHOD's own
    // type-parameter → concrete-TypeRef binding for the EXPLICITLY
    // specified type arguments (`<TypeArg, …>`) at the call site, built by
    // `typeCall` (see ~L3464–L3490). Non-null only when the call wrote
    // `<…>`; threaded into `checkArgsAgainstParams` so the per-argument
    // §5.1 assignability check runs against the substituted parameter
    // type (e.g. `IHolder` from `MakeArr<IHolder>(new Widget3(s))`), not
    // the bare type parameter (`T`) — without this, a non-conforming
    // argument to an interface-typed explicit type argument silently
    // slipped past `checkAssignable`'s free-type-var fallback and
    // reached codegen as a SIGSEGV at the interface dispatch
    // (measured, identical to #4302's signature).
    void checkCallArity(
        const ast::CallExpr& ce, MethodSymbol* ms,
        const std::unordered_map<std::string_view, TypeRef*>* explicitSubst =
            nullptr);

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
    // fix #3583 (§22.8 step 0 / §22.10 / §22.5, v0.286): publish the §22.5
    // binding of a convenience constructor's `this(...)` chain arguments to
    // its class's PRIMARY constructor parameter list, so codegen's single
    // §22.8-step-0 body serves the chain site as well as the `new` site. See
    // the definition in ExprTypecheck_Calls.cpp.
    void publishPrimaryCtorChainBinding(const ast::ConstructorDecl& cd);
    // narrow #4791 (§12.8 / §22.5 / §22.6 / §22.10, v0.307): decide a
    // `this(...)` chain call the resolver's arity-only self-chain check
    // (#3793) deferred because a named argument names no parameter of the
    // containing constructor (`deferredNamedThisChain`, SymbolCollection.cpp).
    // Accepts only when exactly one OTHER constructor of the type binds the
    // call by §22.5 name binding, that one is §22.6-applicable to the typed
    // arguments, and its own chain edges cannot lead back to `cd`; rejects
    // with a specific diagnostic otherwise. See the definition in
    // ExprTypecheck_Calls.cpp.
    void checkDeferredNamedThisChain(const ast::ConstructorDecl& cd);
    // fix #4484 (§22.5 / §22.8 step 0 / §22.10, v0.306): the `base(...)` twin
    // of the above — the §22.5 binding of a derived constructor's leading
    // `base(...)` arguments to the BASE class's primary-constructor parameter
    // list, read by codegen's `prePopulateBasePositionalFields`. Called only
    // from `publishPrimaryCtorChainBinding`, which owns the chain-call
    // recognition both arms share. Since #4496 the binder runs with
    // `emit=true`, so this also CHECKS the call; since #4519 a ZERO-argument
    // `base()` reaches it too (§12.8's "requires the base class to have a
    // parameterless constructor"). See the definition in
    // ExprTypecheck_Calls.cpp.
    void publishBaseChainPrimaryBinding(const ast::CallExpr& chain);
    // fix #4549 (§12.8 / §22.5 / §22.7 / §22.10, v0.306): §12.8's
    // "which requires the base class to have a parameterless constructor",
    // as ONE predicate read by BOTH spellings of the same call — the
    // IMPLICIT chain (`visitTopDecl`) and the explicit zero-argument
    // `base()` (`publishBaseChainPrimaryBinding`). The two cannot drift
    // because there is one body; see the definition in
    // ExprTypecheck_Calls.cpp for what "parameterless" means for each of
    // the three constructor forms a class can carry.
    bool baseHasParameterlessConstructor(const TypeSymbol* base,
                                         SourceLocation loc);
    // fix #4549: the two spellings' diagnostics, reported from the two call
    // sites above. `implicitSpelling` picks the remedy §12.8 names for the
    // inserted chain ("otherwise the chain must be explicit") over the one
    // an explicit `base()` admits (supply the arguments); the predicate and
    // the sentence naming the defect are shared.
    void reportNoParameterlessBaseCtor(const TypeSymbol* base,
                                       SourceLocation loc,
                                       bool implicitSpelling);
    BindResult bindArgsToParams(const std::vector<ast::Param>& params,
                               const std::vector<ast::CallArgument>& args,
                               SourceLocation loc,
                               const std::string& calleeDesc, bool emit) {
        return bindArgsToParams(sema::FormalsView::of(params), args, loc,
                                calleeDesc, emit);
    }

    // fix #3424 / #3425 (§22.5, v0.283): the winning candidate of a built-in
    // callee's overload-aware parameter-name lookup — the names the call bound
    // against, and the binding itself.
    struct BuiltinBind {
        std::vector<std::string_view> names;   // the DECLARED parameter list
        BindResult bind;
    };
    // Run the SHARED §22.5 binder over the registry's CANDIDATE parameter-name
    // lists for a built-in callee (`common::builtinParamNameCandidates` /
    // `common::builtinCtorParamNameCandidates`) and return the first candidate
    // that binds. A built-in has no overload set sema can resolve against, so
    // "the call is valid if it binds against ANY declared overload's names" is
    // the strongest statement available — and it is sound for the identity
    // bindings the built-in call sites admit, because a call whose written
    // names match one overload's declaration order lowers positionally to
    // exactly that overload.
    //
    // On failure a diagnostic is reported and `false` is returned: the SHARED
    // binder's own wording when there is one candidate (so a built-in callee's
    // unknown name, double bind, positional-after-named and missing-required
    // read identically to a user-declared callee's), and a candidate-listing
    // message when there are several, because the shared binder can only name
    // one of them. Shared by the member (#3398) and constructor (#3425) paths
    // so the candidate rule has exactly one spelling.
    // per §22.5, fix #3424 / #3425
    bool bindBuiltinCandidates(
        const std::vector<common::BuiltinFormals>& cands,
        const std::vector<ast::CallArgument>& args, SourceLocation loc,
        const std::string& desc, BuiltinBind& out);

    // fix #3425 (§22.5 / §17.7 "The `new` operator", v0.283): the `new T(...)`
    // twin of #3398's `checkBuiltinNamedArgs`.
    //
    // The hole this closes. A built-in type's constructors are arms of
    // codegen's `lowerNew`, not `ConstructorSymbol`s sema can see (the same
    // observation `checkBuiltinClosedConstructorSurface` above is built on),
    // so no binder ever ran over a built-in `new` and the argument NAME was
    // discarded outright: every argument bound POSITIONALLY and any name was
    // accepted. MEASURED on ef339fa: `new StringBuilder(zzznotaparam := "ab")`
    // compiled and ran, and `new Array<int>(fill := 7, size := 3)` — whose two
    // arguments are distinguishable — printed `Count`=7 and `[0]`=3, i.e. it
    // constructed `Array<int>(7, 3)` where §22.5 requires `Array<int>(3, 7)`.
    // That is a silent wrong VALUE (and a wrong allocation size), not only a
    // missing rejection, so #3425's own "severity: lower than #3398's,
    // because the common built-in constructors take one parameter" understates
    // it: `Array<T>(size, fill)`, `Semaphore(initialCount, maxCount)`,
    // `Dictionary<K,V>(capacity, comparer)`, `FileStream(path, mode, access)`
    // and `DateTime(year, month, day)` all take two or more.
    //
    // The rule applied is `bindArgsToParams`'s — the SAME binder the member,
    // user and imported paths run — over the registry's declared constructor
    // names, so the four §22.5 violations draw the user path's wording. The
    // two implementation-limitation rejections #3398 states for members hold
    // here for the same reasons: a head the registry carries no constructor
    // names for is rejected (rejecting an unknown name needs a COMPLETE list),
    // and a binding that is not the identity permutation is rejected wherever
    // codegen lowers the constructor by `ne.args[i]` POSITION
    // (`common::isBuiltinCtorLoweredByName` names the two heads whose
    // `lowerNew` arm does bind by name, #3098). Lifting the reorder is #3423's
    // codegen work, of which this is the `new`-expression half.
    //
    // `head` is the resolved built-in type name, already established as NOT a
    // user type by the caller. // per §22.5 / §17.7, fix #3425
    void checkBuiltinCtorNamedArgs(const ast::NewExpr& ne, const std::string& head);

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
        // applicable[c] is true when candidate c's arguments were found
        // applicable by classifyArg (per §22.6 step 1, NotApplicable removes
        // the candidate). Used by the caller to avoid arityFallback selecting
        // an overload whose classifyArg result was NotApplicable.
        std::vector<bool> applicable;
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
    //
    // fix #4233 (§26.4/§26.5, v0.298): `extBinding` is the extension's own
    // free-type-variable → concrete-TypeRef binding (the same one
    // `lookupExtensionMember` computes and `ctx_.setExprExtBinding` records
    // on the call's MemberAccessExpr, keyed by `ctx_.exprExtBinding`) — non-
    // null only for a call to an extension method whose signature mentions
    // the extension's free var. When present, each parameter's declared type
    // is substituted through it (mirroring the return-type substitution
    // `ExprTC::typeCall` already runs for the same call) before the
    // assignability check, so `Test(x: W)` on a `Box<int>` receiver checks
    // the argument against `int`, not the unresolved `W` — closing both the
    // #4233 arity-diagnostic-for-a-correct-call defect AND a wrongly-typed
    // argument silently reaching codegen as a type-mismatched IR crash.
    //
    // fix #4325 (§5.1 / §11.2, v0.298): `explicitSubst` is the METHOD's own
    // type-parameter → concrete-TypeRef binding for the EXPLICITLY
    // specified type arguments (`M<T: IBar>.Foo(arg)` — `T → IBar`) — non-
    // null only for a call that wrote `<TypeArg, …>` at the call site. The
    // `typeCall` slice already substitutes it into the call's RETURN type
    // (`rt = substituteReturnType(rt, explicitSubst)`, ~L3490), but the
    // per-argument §5.1 assignability check below ran against the raw,
    // unsubstituted parameter type — so for `MakeArr<T>(x: T)` called as
    // `MakeArr<IHolder>(new Widget3(s))` the parameter was typed as the
    // bare `T`, `checkAssignable`'s free-type-var fallback silently
    // accepted the non-conforming `Widget3` against `IHolder`'s constraint
    // (§11.2 nominal-conformance via `isSubtype`), and codegen dispatched
    // the interface call through a null itable slot — a SIGSEGV
    // (measured, identical to #4302's crash signature). Substituting the
    // explicit type args into `paramTy` BEFORE the §5.1 check lets the
    // same `checkAssignable` authority that already rejects the
    // non-generic twin (`plain_call_argument_noconform_rejected_control`,
    // #4310's suite) reject this one too — `IHolder` is a Named with a
    // declaration, so `Widget3` is not a subtype and the diagnostic
    // fires.
    void checkArgsAgainstParams(
        const std::vector<ast::Param>& params,
        const std::vector<ast::CallArgument>& args, SourceLocation loc,
        const std::string& calleeDesc,
        const ast::Expr* classTarget = nullptr,
        TypeSymbol* classParent = nullptr,
        const ast::MethodDecl* calleeMethodDecl = nullptr,
        const std::vector<std::pair<std::string, TypeRef*>>* extBinding =
            nullptr,
        const std::unordered_map<std::string_view, TypeRef*>* explicitSubst =
            nullptr);

    // Slice T7: check `new T(args)` against T's sole constructor (if
    // any). Zero declared constructors → the implicit default ctor
    // takes no arguments. Multiple constructors → overload resolution
    // is deferred, so skip.
    void checkNewExprArgs(const ast::NewExpr& ne, TypeSymbol* t);

    // fix #4170 (§35.1/§35.2/§35.3, v0.295; mirrors #4163's §5.4/§10
    // enum rejection): neither an enum nor a choice has a constructor
    // for `new` to resolve to — an enum value comes from the int→enum
    // cast `E(n)` (§5.4/§10, #960), a choice's construction *is* case
    // selection, `Type.Case(args)` (§35.1/§35.2/§35.3, #4163). This is
    // the SINGLE authority both `rt`-resolution sites feed: the
    // explicit `new T(...)` NewExpr-typing arm (ExprTypecheck_Exprs.cpp)
    // and the two §17.7 typeless `new()` shorthand call sites in this
    // file (field-initializer and local-decl) — before #4170 the
    // shorthand sites had no Enum/Choice arm at all, so `new()`
    // target-typed to either kind was silently typed and reached
    // codegen exactly as the (#4163-fixed) explicit spelling once did:
    // a SIGSEGV for choice (no case discriminant reaches the
    // `case`-statement lowering) and an IR-verifier backstop for enum.
    // Reports the appropriate diagnostic and returns true iff `rt`
    // names an Enum or Choice (i.e. iff it rejected); callers should
    // treat a `true` return the same way the explicit arm's original
    // two `if` blocks did — diagnose and continue, since later checks
    // in the same NewExpr-typing block (e.g. §14.5 generic-arity) still
    // apply to the same node.
    bool rejectNewOnEnumOrChoice(const ast::NewExpr& ne, const TypeRef* rt);

    // fix s17_7_0003 (§17.7 / §15.1, v0.307; sibling of `rejectNewOnEnumOrChoice`
    // just above, same shape): inline arrays declare no constructors — §17.7,
    // verbatim: "`new [3]int(0, 1, 2)` is rejected by the type checker —
    // inline arrays do not declare constructors, and the `new` syntax has no
    // meaning on them." `rt->kind` for an inline-array type spelling (`[N]T`,
    // §15.1) is `TypeRef::Kind::InlineArray`, a kind every existing `new`-
    // classification predicate in this file guards past with
    // `rt->kind == TypeRef::Kind::Named` — so `new [3]int(0, 1, 2)` fell
    // through every one of them, typed cleanly with its args silently
    // discarded, instead of being rejected. Single authority for both the
    // explicit `new T(...)` arm (ExprTypecheck_Exprs.cpp) and the four
    // §17.7 shorthand call sites in ExprTypecheck.cpp, mirroring
    // `rejectNewOnEnumOrChoice`. Returns true iff `rt` is an inline-array
    // type (i.e. iff it rejected).
    bool rejectNewOnInlineArray(const ast::NewExpr& ne, const TypeRef* rt);

    // fix #4176 (§3.1/§3.2, §11.7, §12.5, §17.7, v0.298): sibling of
    // `rejectNewOnEnumOrChoice` just above, same shape — the single
    // authority for "does `new` construct a value of this NAMED
    // CLASS-SHAPED kind": an abstract class, a `closed` class, a `static`
    // class, and an interface all have no instance to mint. This logic
    // ("Slice S11") already existed inline in the explicit `new T(...)`
    // NewExpr-typing arm (ExprTypecheck_Exprs.cpp); #4176 found the two
    // §17.7 typeless `new()` shorthand call sites in this file
    // (field-initializer and local-decl) never reached it at all — their
    // own `rt`-resolution routed straight to `checkNewExprArgs` for
    // `TypeDeclKind::Class` and nothing else — so `w: ISomeInterface :=
    // new();` with no implementing class in scope reached codegen with no
    // vtable to install and SEGFAULTed on first virtual dispatch (exit
    // 139); `w: AbstractThing := new();` crashes identically through the
    // same shorthand gap. Reports the appropriate diagnostic and returns
    // true iff `rt` names one of the four non-instantiable kinds;
    // callers should treat a `true` return the same way the explicit
    // arm's original if/else-if chain did — diagnose and skip the
    // Class/Record constructor-arg check below, since there is no
    // constructor to check args against.
    bool rejectNewOnNonInstantiableKind(const ast::NewExpr& ne,
                                         const TypeRef* rt);

    // fix #4249 (§17.7, v0.306; third sibling of `rejectNewOnEnumOrChoice`
    // and `rejectNewOnNonInstantiableKind` above, same shape): the single
    // authority for "§17.7 does not admit the `new()` shorthand HERE".
    // §17.7's written-declared-type family gives its member (v) three
    // declared-result positions, and the shorthand's own subset takes only
    // the first: "The shorthand is permitted at the written-declared-type
    // positions (i), (ii), (iii) and the `return e;` statement of (v) ...
    // It is **not** permitted at an expression body or at a lambda's
    // ascribed return". #4208 wired position (iii) and the `return e;`
    // statement of (v), and gated that wiring on `!inLambda_` so the
    // carve-out held — but declining to TYPE the shorthand is not the same
    // as DIAGNOSING it. The untyped NewExpr simply fell through to codegen's
    // #2491 zero/null backstop, so both spellings of a lambda's ascribed
    // return — `lambda(): W => do return new(); end` (an `internal:` Warning,
    // measured) and `lambda(): W => new()` (no diagnostic at all, measured) —
    // compiled and then SEGV'd (exit 139, 60/60 runs) on the first member
    // read of the null they returned. That is the same silent-wrong-answer
    // class #4170/#4176 closed at the other shorthand sites, and #4208
    // removed from the method-return position, left standing at this one.
    // Reports the §17.7 exclusion at the operand's own span. `rt` may be
    // null (a lambda whose return type would arrive only by target
    // inference — outside the written-declared-type family entirely, per
    // §17.7's "A lambda whose return type arrives only by target inference
    // ... [is] outside the family"); it is used only to name the type in
    // the suggested explicit spelling.
    void rejectNewShorthandAtLambdaReturn(const ast::NewExpr& ne,
                                           const TypeRef* rt);

    // fix s17_7_0008 (§17.7, v0.307; fourth sibling of
    // `rejectNewShorthandAtLambdaReturn` just above, same shape — the OTHER
    // half of the SAME exclusion clause it closes): "It is **not** permitted
    // at an expression body (`=> new()`) or at a lambda's ascribed return."
    // #4249 closed the lambda's-ascribed-return half (both its block-form
    // `do return new(); end` and bare-arrow spellings); this closes the
    // expression-body half — a member (method/operator/property-getter/
    // accessor) whose body is `=> new()` with no target-type wiring at this
    // position (§17.7's written-declared-type family (v) grants the
    // shorthand only the `return e;` STATEMENT, not the expression-body
    // spelling of the same declared-result member). Runs in
    // `runMethodBody`'s `exprBody` branch (ExprTypecheck.cpp), BEFORE the
    // body visit — for a typeless NewExpr the visit types nothing and
    // reports nothing on its own, so without this call the shorthand fell
    // straight through to codegen's #2491 zero/null backstop exactly as the
    // two #4249 spellings did pre-fix. `rt` is the member's declared return
    // type (may be null; handled like the lambda sibling).
    void rejectNewShorthandAtExpressionBody(const ast::NewExpr& ne,
                                             const TypeRef* rt);

    // narrow #4059 further (§9.1 / §9.2 / §9.5, v0.295): true iff
    // `new T()` is permitted on this body record (i.e. NOT the
    // silent-zero-fill defect shape). Returns true when the record
    // (a) has at least one body field/auto-property WITH an `:=`
    // initializer — the field-init text IS the construction (§9.1), or
    // (b) has NO body fields or auto-properties at all (methods-only
    // body or literally-empty body — nothing to silently zero-fill).
    // Returns false only when the body record has at least one body
    // field/auto-property AND NONE of them carry an initializer — the
    // exact shape of the #4059 silent-zero-fill defect. Local records
    // read the AST decl (`FieldDecl::initializer` /
    // `PropertyDecl::autoInitializer`); imported records ask the wire
    // side table (`importedInstanceFieldInitText`,
    // `importedConstValue`). Defined in ExprTypecheck_Calls.cpp.
    bool recordAllowsZeroArgNew(const TypeSymbol& t);

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

    // fix #3623 (§5.1 / §22.5 / §22.10 / §22.8 step 0, v0.286): the §5.1
    // per-slot assignability check, factored out of `checkPositionalNewArgsFromSymbols`
    // so it runs identically for a `this(...)` chain to a primary constructor
    // — the §22.8 step-0 TWIN of `new T(...)`, see #3583 — and for the imported
    // record path. The chain site had no §5.1 check at all (#3583 deliberately
    // deferred it and filed this issue), so a `this("bad", 1)` against
    // `class B(n: int, s: string)` reached codegen and ran to SIGSEGV.
    // `target` is the resolved `TypeRef` of the call target — a `NewExpr`'s
    // `ne.type` for the `new T(...)` case, `nullptr` for the chain case — and
    // is used only for the generic-param substitution arm; bare non-generic-
    // param slots still run `checkAssignable` without it. `t` is the type
    // symbol the positional fields belong to (the new-target class, or the
    // enclosing class for a chain); `t->genericParams` drives the substitution
    // walk exactly as the inlined loop did. See the body in ExprTypecheck_Calls.cpp.
    // fix #4806 (§12.7 / §22.3, v0.307): suffix for the §22.3 exact-referent
    // diagnostic when the mismatching `ref`/`out` operand is a bare
    // identifier whose §12.7 narrowing was suppressed (it names the DECLARED
    // location). Empty otherwise. Names the copy-out remedy.
    std::string refOutNarrowedCopyOutHint(const ast::Expr& operand,
                                          const TypeRef* paramTy,
                                          std::string_view mod) const;

    void checkPositionalArgsAssignable(
        const std::vector<const Symbol*>& fields, const BindResult& b,
        const std::vector<ast::CallArgument>& args,
        const std::string& desc, const TypeRef* target, const TypeSymbol* t);

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
    // `IsNullOrWhiteSpace` takes `string?` and is not reached here (this
    // covers the INSTANCE-receiver path only) — its former sibling
    // `IsNullOrEmpty` was retired entirely (fix #3854, stdlib §9.6, v0.290).
    void checkBuiltinStringCallArgs(const ast::CallExpr& ce);

    // fix #3398 (spec §22.5 "Optional parameters and named arguments",
    // v0.283; stdlib §1 Conventions — *Window parameters*; §9.6; §3.2):
    // §22.5 BINDING for a call to a BUILT-IN member.
    //
    // The hole this closes. §22.5's binder ran for a user-declared callee
    // (`checkCallArity` → `bindArgsToParams`) and, since #3178, for an
    // imported one (`checkImportedCallArity` over the same binder). A
    // built-in member has NO `MethodSymbol` at all, so neither ran and the
    // argument NAME was discarded outright: every argument bound
    // POSITIONALLY, any name was accepted, and a reordered named call
    // silently computed a different value than the identical call on a
    // user-declared method — `"hello world".Remove(length := 6,
    // startIndex := 5)` compiled clean and evaluated `Remove(6, 5)`,
    // printing `"hello "` where §22.5 requires `"hello"` (#3398). That is a
    // wrong-answer bug, not only a missing-rejection bug.
    //
    // The rule applied here is `bindArgsToParams`'s — the SAME binder, over a
    // `FormalsView` of the parameter names `common::builtinParamNameCandidates`
    // carries — so the four §22.5 violations (unknown parameter name, a
    // parameter bound twice, a positional argument after a named one, a
    // required parameter left unsupplied) draw the same diagnostic strings a
    // user-declared callee draws. No second copy of the rule; the built-in
    // path only supplies the formals the registry now holds.
    //
    // TWO REJECTIONS this adds that a user-declared callee does not take, both
    // stated as implementation limitations rather than language rules:
    //
    //   1. A named argument on a built-in member the registry carries no
    //      names for is REJECTED. Rejecting an unknown name needs a COMPLETE
    //      name list for that member, so a partial table must either fall
    //      back to today's permissive positional binding or reject — and a
    //      loud rejection beats a wrong value.
    //   2. A named argument that binds to a parameter OTHER than its own
    //      position is REJECTED even where the names are known. §22.5 admits
    //      the reorder, but honouring it needs codegen to lower a built-in
    //      call through the binding rather than through `ce.args[i]`, while
    //      §22.5's *Evaluation order* forbids buying that by permuting the
    //      argument list (supplied arguments evaluate in call-site textual
    //      order). Reordering support is #3423; until it lands the call is
    //      rejected, never mis-evaluated. Widening the table past `string` /
    //      `Encoding` — and making its lookup overload-aware — is #3424; the
    //      `new T(...)` twin, still unfixed, is #3425.
    //
    // `calleeSym` is the call's resolved callee symbol. A non-null one means
    // the call is NOT a built-in member access — a user or imported method
    // (including an EXTENSION method on a built-in receiver, §26.5, which
    // resolves to a real `MethodSymbol` and whose named arguments
    // `checkCallArity` already binds) — and this check does not claim it.
    // per §22.5 / stdlib §1 / §9.6 / §3.2, fix #3398
    void checkBuiltinNamedArgs(const ast::CallExpr& ce, Symbol* calleeSym);

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

    // §24: the event visibility fence. Outside the declaring class, an
    // event member's static type is `IEvent<T>` / `IAsyncEvent<T>`, which
    // exposes only `Subscribe`. The producer-side operations `Raise` /
    // `RaiseAsync` / `Add` / `Remove` (and the `+=` / `-=` desugaring) live
    // on `EventSource<T>` / `AsyncEventSource<T>`, the in-class face. Given
    // an event member, the accessed operation name (one of the producer-side
    // names, or `+=` / `-=`), and the access location, this emits a
    // diagnostic when the access is from outside the declaring class. A
    // derived class may use the producer-side face only for a `protected`
    // event declared on a base class (§24). `Subscribe` is never flagged.
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
    // `recvTy` (fix #3906, §35.11, v0.294) is the receiver's OWN resolved
    // TypeRef when the caller has one (`ctx_.exprType` of the receiver
    // expression) — carrying the actual generic arguments of THIS
    // instantiation (`Pair<int>`, not bare `Pair`). When it names the same
    // declaration as `targetType` it is threaded into the choice classifier
    // so an unmarked bare-type-parameter payload is classified against the
    // substituted type per §35.11, not the uninstantiated declaration; a
    // caller with no such TypeRef (or one that does not resolve to
    // `targetType`) leaves the prior bare-declaration behavior unchanged.
    void checkEqualityMemberUseSite(TypeSymbol* targetType,
                                    std::string_view member,
                                    SourceLocation loc,
                                    const sema::TypeRef* recvTy = nullptr);

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

    // §4.14 (#4670-class, l4_14_0010): if `operand` is `buf[i]` whose receiver
    // has an `OwningBuffer<T>` static type and T is value-typed (a primitive,
    // record, enum, value-like or borrow-bearing choice, inline array of
    // values, function value, value builtin, or the unit type — anything
    // whose slot semantics dispatch on the value-presentation arm), return
    // the receiver's display name; otherwise the empty string. Such a slot
    // holds no ownership to transfer: the value-typed `OwningBuffer<T>`
    // presentation stores T directly (no nullability wrapper), so `move`
    // would read a plain value and pretend an ownership transfer happened.
    // Spec §4.14's *Value-typed T* bullet: "`move buf[i]` is not admitted;
    // values are read with the plain indexer and copied at the use site."
    // The class-typed presentation of `OwningBuffer<T>` (slots hold `T?`)
    // IS admitted by `move` and is NOT matched here.
    std::string moveThroughOwningBufferValueElementName(
        const ast::Expr& operand) const;

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

    // §28.3 / §28.8.1 (issue #4284): if `operand` is a read of a non-`owned`
    // FIELD (`recv.f` via MemberAccessExpr, or the bare-identifier implicit
    // `this.f` form), return the field's display name (the bare identifier for
    // the implicit-`this` form, the textual receiver.field for the explicit
    // form); otherwise the empty string. A class-typed field with no `owned`
    // annotation holds a borrow (§28.3: "An unmarked read of a class-typed
    // binding never consumes…"), so `move <field>` is rejected at compile
    // time — accepting it would classify the read as an owned transfer, the
    // program's reclaim-eligibility walk would treat the moved-from slot as a
    // fresh owned value, and disposing both the `move` result and the original
    // owner would free the same backing storage twice (a heap-use-after-free
    // measured in the issue's repro under `--sanitize=address`). The leaf
    // field's `owned` modifier is the single authority here — the receiver's
    // ownership status (a borrow of a non-owned field is also a borrow) is
    // irrelevant; only the field-shape "this slot owns storage" qualifies it
    // for a `move` transfer. The legal forms `move obj.f` / `move this.f` /
    // `move _f` / `move obj.f!` / `move this.f!` / `move _f!` over an
    // `owned`-modified field are NOT matched (the modifier check excludes them),
    // and `move obj.f!` over a STATIC `owned` field is rejected elsewhere by
    // the #2021 receiver classification.
    std::string moveOnNonOwnedFieldName(const ast::Expr& operand) const;

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
    bool isCaptureFreeEntry(const ast::Expr* entryExpr);

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
