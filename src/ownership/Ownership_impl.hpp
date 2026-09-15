#pragma once

// Private implementation header for the per-method ownership analyzer
// (cf. codegen/LlvmBackend_impl.hpp).
//
// Declares the `MethodOwnership` worker class whose method definitions are
// distributed across Ownership.cpp and Ownership_Checks.cpp. This header is
// included ONLY by those implementation files; the public surface stays in
// Ownership.hpp (checkOwnership). Keeping the class out of the public header
// preserves the encapsulation the previous anonymous-namespace placement
// gave, while allowing the definitions to live in separately-compiled units.

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <vector>

#include "ast/Node.hpp"
#include "flow/CFG.hpp"
#include "ownership/OwnershipState.hpp"
#include "sema/SemaContext.hpp"
#include "sema/Symbol.hpp"

namespace bezel::common {
struct RecordSubstEnv;  // §9.7/#2145 instantiation-site substitution env
                        // (common/OwnershipFacts.hpp); pointer-only here
}  // namespace bezel::common

namespace bezel::ownership {

// §22.10 / §22.3 / §2.5 (#1627, #2659, #3176): a resolved CALLEE's formal list,
// abstracted over every source one can have — a local `ast::Param` vector (a
// user constructor, a method, an `operator`, a class primary constructor's
// positional fields, a choice case's payload), a `ConstructorSymbol`, or a
// `MethodSymbol`. Ownership_Checks.cpp holds the full derivation and the two
// resolvers (`resolveCtorParams` / `resolveCalleeParams`); the definition lives
// here because `MethodOwnership::markOutOwnedArgsCore` takes it.
//
// The point of the view is that NO consumer open-codes
// `dynamic_cast<const ast::MethodDecl*>(sym->decl)` — that cast is null for
// every non-generic imported member, so each open-coded copy silently answered
// "this callee has no formals" across a module boundary. #2659 measured that as
// a double free on the constructor path; #3176 measured it again on the method
// path (`AddressSanitizer: heap-use-after-free`) plus a leaked `out owned`
// recipient. Every accessor here consults the AST source first and the symbol
// source second, so a member both of them describe can never get two answers.
struct CalleeFormals {
    // Local: a user `constructor(...)`'s params, or a class PRIMARY
    // constructor's positionalFields (#1627 — those live on the TypeDecl and
    // are never a ConstructorSymbol member).
    const std::vector<ast::Param>* declParams = nullptr;
    // Imported (or local-with-decl): the resolved ConstructorSymbol. Read
    // through sema::formalParamModifier / formalParamName, which prefer the
    // ast::ConstructorDecl when it exists and fall back to ParameterSymbol.
    const sema::ConstructorSymbol* ctorSym = nullptr;
    // fix #3176: the resolved MethodSymbol — an ordinary method, a free
    // function, an extension method or an `operator`. Read through the
    // MethodSymbol overloads of the same accessors, which prefer the
    // ast::MethodDecl / ast::OperatorDecl when it exists and fall back to the
    // reconstructed ParameterSymbol list, which is all an imported method has.
    const sema::MethodSymbol* methSym = nullptr;
    // fix #3141 (family F3 of umbrella #3152): a class PRIMARY constructor's
    // parameters, as the FieldSymbols they become. This is the SAME list
    // `declParams` names for a local class — a local positional FieldSymbol's
    // `posParam` points into `td->positionalFields` — reached through the
    // symbol table so that it survives an import, where there is no
    // `ast::TypeDecl` to index at all (§2.8.3 rule 4).
    std::vector<const sema::Symbol*> posFields;
    // True when the formals came from a class PRIMARY constructor rather than a
    // declared `constructor(...)`. The distinction matters for exactly one
    // consumer: a primary-ctor parameter BECOMES A FIELD, so a plain
    // function-typed one is RETAINED by the constructed object and is therefore
    // not a §28.14 "verified call-bounded" position. Every other consumer
    // deliberately treats the two alike — that unification is what #1627 and
    // #3141 are.
    bool primaryCtor = false;

    bool valid() const {
        return declParams || ctorSym || methSym || !posFields.empty();
    }
    std::size_t size() const {
        if (declParams) return declParams->size();
        if (!posFields.empty()) return posFields.size();
        if (ctorSym) return sema::formalParamCount(*ctorSym);
        return methSym ? sema::formalParamCount(*methSym) : 0;
    }
    std::string_view modifier(std::size_t i) const {
        if (declParams) return i < declParams->size() ? (*declParams)[i].modifier
                                                      : std::string_view{};
        if (!posFields.empty()) {
            // A positional parameter's `owned` marker lives on the `ast::Param`
            // locally; the metadata carries a positional field's MODIFIERS only
            // through `MemberSymbol::modifiers`, which SymbolCollection leaves
            // empty for a local positional field — so the marker reaches the
            // wire from neither side and an imported positional field can only
            // answer "" here (#3164, filed with the fix shape). Consult the
            // Param when it carries the syntax (#2516's synthetic import Param
            // has a null `type` and no modifier), then the symbol's own list, so
            // this reader needs no change once #3164 populates it.
            if (i >= posFields.size()) return {};
            if (auto* fs =
                    dynamic_cast<const sema::FieldSymbol*>(posFields[i])) {
                if (fs->posParam && fs->posParam->type)
                    return fs->posParam->modifier;
                for (std::string_view mod : fs->modifiers)
                    if (mod == "owned") return mod;
            }
            return {};
        }
        if (ctorSym) return sema::formalParamModifier(*ctorSym, i);
        return methSym ? sema::formalParamModifier(*methSym, i)
                       : std::string_view{};
    }
    std::string_view name(std::size_t i) const {
        if (declParams)
            return i < declParams->size() ? (*declParams)[i].name
                                          : std::string_view{};
        if (!posFields.empty())
            return i < posFields.size() ? posFields[i]->name
                                        : std::string_view{};
        if (ctorSym) return sema::formalParamName(*ctorSym, i);
        return methSym ? sema::formalParamName(*methSym, i)
                       : std::string_view{};
    }
    // fix #3176: the formal's declared type, for the §28.14 gates that ask
    // "is this formal function-typed" rather than "what modifier does it
    // carry". Returns the syntactic node when a source carries one and the
    // already-resolved TypeRef otherwise (an imported formal has no
    // `ast::Type`), so a caller resolves `declType` through SemaContext and
    // falls back to `resolvedType`. Never both non-null for one formal.
    const ast::Type* declType(std::size_t i) const {
        if (declParams) return i < declParams->size()
                                   ? (*declParams)[i].type.get() : nullptr;
        if (!posFields.empty()) {
            if (i >= posFields.size()) return nullptr;
            if (auto* fs = dynamic_cast<const sema::FieldSymbol*>(posFields[i]))
                return fs->posParam ? fs->posParam->type.get() : nullptr;
            return nullptr;
        }
        if (ctorSym) return sema::formalParamDeclType(*ctorSym, i);
        return methSym ? sema::formalParamDeclType(*methSym, i) : nullptr;
    }
    const sema::TypeRef* resolvedType(std::size_t i) const {
        if (declParams || !posFields.empty()) return nullptr;
        if (ctorSym) return sema::formalParamResolvedType(*ctorSym, i);
        return methSym ? sema::formalParamResolvedType(*methSym, i) : nullptr;
    }
    // fix #3769 (§22.5 / §2.5, v0.286): this same formal list as the ONE
    // §22.5 BINDING view `ExprTC::bindArgsToParams` runs over
    // (`sema::FormalsView`, #3178/#3173). The binding facts — how many formals
    // there are, each one's NAME, and whether each MAY BE LEFT UNBOUND — are
    // exactly what an admission test needs, and `hasDefault` in particular has
    // no reader on `CalleeFormals` at all: this view is where it already lives,
    // for a local `ast::Param` list, a §22.10 positional-field list, an
    // imported `ConstructorSymbol` and a `MethodSymbol` alike. Constructing the
    // view here rather than restating those four arms is what keeps the
    // owned-sink resolver's "which constructor does this call reach?" answer
    // from drifting away from sema's "may this call omit that argument?".
    //
    // NON-OWNING, like every `FormalsView`: the returned view borrows this
    // `CalleeFormals`' `posFields` vector (and the AST/symbol lists the other
    // arms point at), so it must not outlive the `CalleeFormals` it came from.
    // The source order is the one every accessor above uses — AST first,
    // symbol second — so a formal both sources describe cannot get two answers.
    sema::FormalsView bindingView() const {
        if (declParams) return sema::FormalsView::of(*declParams);
        if (!posFields.empty()) return sema::FormalsView::of(posFields);
        if (ctorSym) return sema::FormalsView::of(*ctorSym);
        if (methSym) return sema::FormalsView::of(*methSym);
        return {};
    }
};

// §28.3 K3.4 (#747 R42 / fix #3949 / fix #3971): is `e` a TOP-LEVEL consuming
// form — the shape a read-checking walker must NOT run the broad `checkRead`
// over, because a more specific move / await-consume check owns the diagnostic
// there and a read-check run first would steal it (and, for the dispose half,
// weaken the ERROR to a use-after-dispose WARNING, which turns a rejected
// program into an accepted one)?
//
// ONE spelling, THREE callers.  `handleLocalDecl` has made this exact decision
// since #747 (`var y := move x;`); `handleAssignment` since #3949 (`y := move
// x;`); `checkRead`'s own `NewExpr` argument walk since #3971 (`new T(move x)`
// — measured: without the guard the precise "cannot move from disposed local
// `x` (§28.3)" ERROR degrades to §28.2's WARNING and the program is ACCEPTED,
// the exact #3941 shape).  It was file-local to Ownership.cpp until #3971
// needed the third caller; a second, drifting copy of a decision an existing
// site already makes is this codebase's dominant defect generator, so the
// predicate moved here rather than being restated.
// // per §28.3 K3.4, v0.294, #747, #3949, #3971
inline bool isTopLevelConsumingForm(const ast::Expr& e) {
    if (const auto* ue = dynamic_cast<const ast::UnaryExpr*>(&e))
        return ue->op == "move" || ue->op == "await";
    return false;
}

// Per-method ownership analyzer.
//
// Slice O3a: the analysis runs as a forward dataflow over the method
// body's control-flow graph (`bezel::flow::CFG`) rather than the
// original syntactic statement walker. The per-statement transfer
// functions (handleLocalDecl / handleDischarge / handleAssignment /
// handleReturn and the move/dispose/read helpers in Ownership_Checks.cpp)
// are unchanged; the driver feeds each CFG basic block's straight-line
// statement sequence through them, and merges predecessor exit states at
// block entry using the §28.3 asymmetric meet (`joinInto`).
//
//   - if/elif/else joins flow through real CFG merge blocks (the merge
//     carries the originating `IfStmt` so the clash diagnostic lands at
//     the if's span);
//   - loops are iterated to a fixpoint (O3b): back-edges and the zero-trip
//     header→after skip edge participate in the meet, so the analysis is
//     sound for the never-executed loop path;
//   - try/catch/finally is expanded into the real F2 exception-edge CFG
//     (O11-real, `treatTryAsOpaque=false`). The driver applies §28.3
//     "Exception-path discharge" per edge: when state flows along an
//     exception (unwinding) edge — a `throw`, or a throwable call inside a
//     try body — every live owned local is auto-disposed by the implicit
//     try/finally, so it is not reported as leaked on that edge.
using BlockState = std::unordered_map<sema::Symbol*, LocalOwnership>;

// fix #3564 / #3565 (§28.9 / §31.1 / §27.5, v0.286): WHICH SCOPE an escape
// check is being asked about. The escape checks below (`checkClosureEscapes
// OnReturn`, `checkBorrowStoreEscape`) were written for a METHOD body, whose
// dataflow inputs (`states_`, `localDepth_`, `recordBorrowLocalSource_`, the
// method's parameter sets) all describe the method's own scope. The
// lambda-body drivers (`checkLambdaBodyEscapes`, issue 3546's traversal fix
// extended by 3564/3565) run the SAME checks over a LAMBDA body, whose scope
// is a different one — so they say so with this argument rather than growing
// a second, drifting copy of each rule. See `checkOneLambdaBodyEscapes`
// (Ownership.cpp) for what is seeded, and the two check declarations below
// for which legs each scope admits and why. // per §28.9, §31.1, v0.286
enum class EscapeScope {
    // The enclosing method's own body: every leg, method-scope inputs.
    Method,
    // One lambda body, with `states_` / `localDepth_` saved, cleared and
    // re-seeded from that body alone. Legs whose inputs this seeding does not
    // supply are skipped rather than allowed to answer about the wrong scope.
    LambdaBody,
};

// fix #3746 (§28.3 *Captured owned bindings*, v0.286): WHICH owned bindings a
// borrow-source query counts as a source whose life ends inside THIS frame.
//
// §28.3 states the two kinds as ONE category — "a captured owned binding — an
// owned local or `owned` parameter of the enclosing body" — and *Auto-drop
// (affine)* gives the parameter its scope end explicitly: "for an `owned`
// parameter, at the end of the method". `checkClosureEscapesOnReturn`'s §28.9
// leg has always answered for BOTH (it reads `states_`, which `run()` seeds
// with the `owned` parameters, and it has a `ParameterSymbol` arm), while
// `ownedLocalBorrowSource` answered for locals only. Two channels of ONE escape
// rule disagreeing on one axis is the defect #3746 measured, so the kind is an
// ARGUMENT of the one query rather than a second query beside it. See
// `ownedLocalBorrowSource` for which caller passes which, and why.
// // per §28.3, §28.9, v0.286
enum class OwnedSourceKinds {
    // Owned scope-bound LOCALs only — the pre-#3746 answer. Kept for the
    // channels whose verdict is a declaring-BLOCK depth comparison (an `owned`
    // parameter has no declaring block, and no block of this frame outlives the
    // frame, so there is nothing to compare and nothing to reject) and for the
    // §28.8 transfer-shaped positions.
    LocalOnly,
    // …and an `owned` parameter of this frame, whose value THIS method disposes
    // at its end. NOT `out owned`: that parameter's storage is the CALLER's
    // frame and the value written is transferred out, so this frame disposes
    // nothing and a borrow of it is not bounded here.
    LocalOrOwnedParam,
};

// §28.8.1 (#1125) / §28.6 bullet 2 (#2518): if `e` is a leading force-unwrap
// `x!` (PostfixBangExpr), return its operand `x`; otherwise return `e`
// unchanged. Lets the §28.6 / §28.8.1 field-read recognizers see through the
// null-forgiving `!` so the force-unwrap spellings (`return _f!`, `move _f!`)
// are classified identically to their bare forms. Defined in
// Ownership_Checks.cpp; shared by both Ownership.cpp and Ownership_Checks.cpp.
const ast::Expr* lookThroughForceUnwrap(const ast::Expr* e);

// §28.8.1 (#867): the per-block dataflow lattice for the field-restoration
// check. A `move` of an owned field leaves the field moved-from; the field
// must be RESTORED (reassigned a value, incl. `null` for a nullable owned
// field) on EVERY control-flow path from the move to method exit. Earlier the
// moved-from facts lived in a flat member map mutated linearly as statements
// were visited, so a `move` restored on only SOME paths was wrongly accepted:
// the map is not part of `BlockState`, so it never participated in the CFG
// meet at joins. `FlowState` bundles the locals lattice WITH the field-moved-
// from facts so both flow through `runDataflow`/`computeEntry` together (the
// locals merged via `joinInto`, the fields via `joinFieldsInto`). The field
// facts merge with the OPPOSITE polarity to the locals:
// a field is moved-from at a join iff it is moved-from on ANY incoming path
// (equivalently RESTORED iff restored on ALL paths) — an unrestored field on
// even one path must be reported at exit. // per §28.8.1, #867
using FieldMovedFrom = std::unordered_map<std::string_view, bool>;

// §28.8.1 (#867): the full per-block dataflow state — the locals lattice plus
// the field-moved-from facts, carried together so both participate in the CFG
// meet (the locals via the §28.3 `joinInto`, the fields via the OR-over-paths
// `joinFieldsInto`).
struct FlowState {
    BlockState locals;
    FieldMovedFrom fields;
};

// §28.3 *Bare-type-parameter returns* (v0.194, #1801 S6): the generic context a
// callee needs to recognize a bare-type-parameter return POSITIONALLY and to
// classify its potential source set. A method's declared return type is a "bare
// type parameter" when its single-part head names either one of the method's own
// generic parameters (`method Sel<T>(...): T`) or one of the ENCLOSING type's
// (`class Box<T> ... method Get(...): T`) — the name-set membership test that
// distinguishes a bare parameter from a 0-arity builtin sharing the null-
// declaration Named shape. The `where T: value` exclusion (spec item 5) needs
// both levels' constraints. Threaded from checkMember, where the MethodDecl and
// its enclosing TypeDecl are in scope; every pointer is null for constructors,
// destructors, operators, and lambdas (no bare-type-parameter return rule
// applies), and a non-generic method leaves the vectors empty. // per §28.3
struct GenericReturnContext {
    const std::vector<ast::GenericParam>* methodParams = nullptr;
    const std::vector<ast::GenericParam>* enclosingParams = nullptr;
    const std::vector<ast::GenericConstraint>* methodConstraints = nullptr;
    const std::vector<ast::GenericConstraint>* enclosingConstraints = nullptr;
    // §28.3 v0.235 (#1666, #2381): for a method declared inside an EXTENSION
    // block, the block-level type parameters ride the target type's generic
    // argument list (an `ExtensionDecl` carries no `genericParams` vector), so
    // they are collected here as synthetic `GenericParam`s and `enclosingParams`
    // is pointed at this vector — letting `computeBareReturnContext` recognize a
    // bare-`T` return that names the extension-block-level parameter (the v0.235
    // iteration-variable ruling's must-reject shape is an extension method on
    // `IEnumerable<T>`). Empty for type-member and static methods. Owns the
    // `string_view` names (they point into the AST, which outlive the pass).
    std::vector<ast::GenericParam> extensionEnclosingParams;
    // §28.3 *Bare-type-parameter returns* (v0.194, #1801 S10; #1885): true when
    // this method IMPLEMENTS or OVERRIDES a generic declaration whose declared
    // return type is a bare type parameter (`class VChooser: IChooser<C> ...
    // method Pick(a: C, b: C): C`), so the classification is inherited even
    // though this method's own signature names concrete types. Callers dispatch
    // through the generic signature and re-establish the borrow against their
    // own arguments (the caller-side dual is findInheritedBareReturnDecl), so
    // the instance parameter-borrow-return prohibition (checkInstanceBorrowReturn)
    // must NOT fire on such a body. Computed in checkMember, where the enclosing
    // type's base types are reachable. // per §28.3 v0.194
    bool returnInheritsBareTypeParam = false;
    // §28.3 (v0.196; #1910): when `returnInheritsBareTypeParam` is true, the
    // positions (in this method's parameter list) that correspond to a SOURCE
    // parameter in the inherited declaration — a plain parameter whose type
    // mentions the returned type parameter. The S10 carve-out in
    // checkInstanceBorrowReturn admits parameter-sourced borrow returns ONLY
    // at these positions; a parameter at a non-source position (e.g. one
    // whose inherited counterpart is `Other` instead of `T`) carries a borrow
    // whose source is not re-established at every caller and must still be
    // rejected. Empty when `returnInheritsBareTypeParam` is false. // per §28.3
    // v0.196
    std::vector<std::size_t> inheritedBareTypeParamSourcePositions;
};

// §28.1 (v0.196, #1817/#1860): true when a bare-type-parameter `owned` slot's
// SUBSTITUTED type is a value/runtime-managed kind whose marker is erased (no
// disposal obligation) — a primitive incl. string, a record/enum/value-builtin,
// a non-owning inline array. Conservative in the leak-safe direction: a still-
// generic bare-T (Named, no declaration, non-value-builtin) is NOT erased, so
// the definition-side boundary keeps every obligation and only the concrete-
// substitution boundary erases. Defined in Ownership.cpp near producerState;
// shared here so the out-owned RECIPIENT gate in Ownership_Checks.cpp
// (markOutOwnedArgs, #1860) reuses the exact classification E1 applied to the
// owned return + owned-param seeding. // per §28.1 v0.196
bool isValueSubstitutionErased(const sema::TypeRef* rt);

class MethodOwnership {
public:
    MethodOwnership(sema::SemaContext& ctx) : ctx_(ctx) {}

    // Run the linear walk. `body` is the method/ctor/lambda body's
    // statement list. `paramSyms` (slice O10) is the matching list of
    // ParameterSymbols — `owned` params are introduced as Owned locals
    // at method entry per §28.8 and must reach a discharge point on
    // every normal-exit path (same rule as `var x := producer-of-owned;`).
    //
    // Async-readiness 3c: `isAsync` is true when the enclosing method/lambda
    // is declared `async`. It gates the additive at-await-site Pattern A / B
    // checks (§30.5) and the across-suspend SuspendLocal annotation — all of
    // which fire ONLY on async bodies, so a non-async body's diagnostics are
    // byte-for-byte unchanged. // per §30.5, §30.1
    //
    // §28.3 (v0.180, #1547): `isStatic` is true when the enclosing method is
    // declared `static`. It gates checkStaticBorrowReturn, which rejects a
    // static method returning a borrow with no receiver to source it from
    // (a static method has no `this`). False (the default) for constructors/
    // destructors/operators, where the check does not apply.
    void run(const std::vector<std::unique_ptr<ast::Stmt>>& body,
             const std::vector<sema::Symbol*>* paramSyms = nullptr,
             bool isAsync = false, bool isDestructor = false,
             const ast::Type* returnType = nullptr, bool returnOwned = false,
             bool isStatic = false,
             const GenericReturnContext* genericCtx = nullptr,
             bool isGenericBody = false);

    // fix #3751: the CONSTRUCTOR whose body is being analysed, when the body
    // is a constructor's (null for a method / destructor / operator / lambda /
    // field-initializer run). It is the only channel through which the
    // ownership pass can know which class — and which sibling constructors — a
    // `this(...)` / `base(...)` chain in this body can target; §22.10's
    // no-self-chain exclusion needs the symbol itself, not just its class.
    // Set by `checkMember`'s ConstructorDecl arm (Ownership.cpp) before `run`.
    void setEnclosingCtor(const sema::ConstructorSymbol* cs) {
        enclosingCtor_ = cs;
    }

    // §28.3 / §5.11 (#1007): apply ONLY the borrow-returned-through-owning-type
    // check to an expression-bodied method `M(): T => exprBody` (which the full
    // block dataflow above is not run on). Seeds the parameters so a borrowed
    // param is classified correctly, then runs `checkBorrowReturnedThroughOwning
    // Type(exprBody)` and — v0.180, #1547 — `checkStaticBorrowReturn(exprBody)`
    // when `isStatic` — nothing else, so no unrelated diagnostics are
    // introduced on expression bodies. // per §28.3, §5.11
    // `isAsync` (v0.190, SB01-B / #1786): threaded so the Task-family callee-
    // return check (`checkBorrowReturnedThroughOwningType`) can exempt an
    // `async` expression body — its `=> E` body is the AWAITED value (type T),
    // not a task, and an async body auto-satisfies the owned-frame obligation.
    // Without it, `async method N(v): Task<int> => v` (v0.190) misfires: the
    // block-body path already passes `methodIsAsync(*md)`, this path did not.
    void runExprBodyOwningReturnCheck(
        const ast::Expr& exprBody,
        const std::vector<sema::Symbol*>* paramSyms,
        const ast::Type* returnType, bool returnOwned,
        bool isAsync = false,
        bool isStatic = false,
        const GenericReturnContext* genericCtx = nullptr);

    // #926 / §28.6 (v0.153): a property/indexer GETTER may not return a freshly-
    // produced owned value — a getter is a borrow-producing position (properties
    // and indexers do not carry `owned`), so a fresh owned value would escape with
    // no owner and leak. checkGetterFreshOwnedReturn reports when `e` is a fresh
    // owned producer (producerState == Owned). scanGetterReturns / -Stmt walk a
    // block-form getter accessor body for `return <fresh owned>` statements (nested
    // in if/while/for/try/do). These run a NARROW check only (not the full
    // ownership pass), so accessor bodies surface no other new diagnostics.
    void checkGetterFreshOwnedReturn(const ast::Expr& e);
    void scanGetterReturns(const std::vector<std::unique_ptr<ast::Stmt>>& stmts);
    void scanGetterReturnsStmt(const ast::Stmt* s);

    // #1484 / #2168 / §23.1 / §28.6: declaration initializers that store a
    // freshly-produced owned value (producerState == Owned) into NON-`owned`
    // backing storage are errors — the value is left with no owner and leaks.
    // checkFieldInitializer covers explicit fields and preserves the §28.5
    // `owned`-field path. checkAutoPropertyInitializer covers an auto-property's
    // generated backing field, which has no `owned` declaration form (§23.1).
    // Both checks exempt static storage (§22.13), and are NARROW checks only.
    //
    // `enclosingKind` (issue #1251) is the kind of the type that DECLARES `fd`.
    // §9.7's storage bullet is scoped to "an instance field of a **class**", so
    // the borrow-bearing record / span / choice leg must not fire on a RECORD's
    // own component initializer — a record component of borrow-bearing type is
    // exactly what §9.7's transitivity clause contemplates as legal. Null (the
    // default) keeps the leg enabled, the conservative reading for a caller
    // that cannot name the declaring type. // per §9.7
    void checkFieldInitializer(const ast::FieldDecl& fd,
                               const ast::TypeDecl* enclosingType = nullptr);
    void checkAutoPropertyInitializer(const ast::PropertyDecl& pd);

    // §28.3 SB01-B (v0.190, #1919/#1926): the callee-side borrow-return
    // prohibition on a plain (non-`async`) `Task`-family return applies to
    // LAMBDA / function-value bodies wherever they appear — the spec is explicit
    // that it governs "a non-`async` method OR lambda". These pre-passes drive
    // the rejection over every lambda in a body/expression that is not otherwise
    // ownership-analysed for it. #1919 wired them into run() (block method/ctor/
    // operator bodies) and runExprBodyOwningReturnCheck (`=>` method bodies);
    // #1926 additionally wires them into the field-initializer, property/indexer
    // accessor (get AND set), and expression-bodied-operator entry points in
    // checkMember, so they are public. The internal collectors and the single-
    // lambda classifier stay private. // per §28.3 SB01-B (v0.190)
    //
    // `expectedReturnFnType` (#1929): the enclosing DECLARED return function
    // type of `body` (a method's return type, a block getter's property type),
    // used as the effective-return fallback for a lambda returned DIRECTLY
    // (`return <lambda>;`) — its funcReturn is left un-inferred in return
    // position, so the classifier was inert on it before. // per §28.3 SB01-B
    void checkLambdaTaskFamilyBorrowReturns(
        const std::vector<std::unique_ptr<ast::Stmt>>& body,
        const sema::TypeRef* expectedReturnFnType = nullptr);
    // `topExpected` (#1926): the expected function type at `e`'s own position
    // (a field's / getter's / operator's declared type), used as the effective-
    // return fallback for the TOP-LEVEL lambda when the type-checker left its
    // funcReturn un-inferred (it is null off the typed-local #829 hint path).
    void checkLambdaTaskFamilyBorrowReturnsExpr(
        const ast::Expr& e, const sema::TypeRef* topExpected = nullptr);

    // fix #3546 (§31.1, v0.283): §31.1 states the catch-arm binding "may not
    // be ... captured by a closure that may outlive the arm", and "Exception
    // instances themselves cannot escape the arm that caught them". The RETURN
    // channel of that rejection lives in `checkClosureEscapesOnReturn`, which
    // the dataflow drives from `visitStmt`'s `ReturnStmt` arm — and ownership's
    // statement walks deliberately never enter a lambda body (a lambda body
    // lives inside a `LambdaExpr`, an *expression*). So a `return <closure
    // capturing ex>` inside a `try`/`catch` that itself sits inside a lambda
    // body was never classified: it compiled clean and segfaulted at runtime,
    // while the byte-identical method-level spelling was rejected. These
    // drivers reach exactly those returns and feed the SAME check — no second
    // spelling of the rule, and no second spelling of "is this decl a catch-arm
    // binding". Mirrors `checkLambdaTaskFamilyBorrowReturns{,Expr}`'s
    // collect-every-lambda-then-walk-its-own-returns shape. // per §31.1 v0.283
    // fix #3564 / #3565 (§28.9 / §31.1, v0.286): the traversal hole #3546
    // named was WIDER than its own §31.1 return leg, and the two survivors are
    // the same hole through two more channels:
    //   * #3565 — the §31.1 ASSIGNMENT channel (`f := lambda() => ex...;`
    //     inside a lambda body, `f` declared outside the arm). Rejected at
    //     method level by `checkBorrowStoreEscape`'s `Dest::OuterLocal` arm
    //     since fix #3520; inside a lambda body it compiled clean and
    //     heap-use-after-freed (measured rc=139 / ASan UAF on 5d052b3).
    //   * #3564 — the §28.9 owned-scope-bound-local RETURN channel (`using var
    //     r := new R(); return lambda() => r.V();` inside a lambda body).
    //     Rejected at method level by `checkClosureEscapesOnReturn`'s §28.9
    //     leg; inside a lambda body it compiled clean and leaked its 56 bytes
    //     while the returned closure read storage the lambda invocation owns.
    // Both are closed by giving this driver the lambda body's OWN scope
    // (`checkOneLambdaBodyEscapes` swaps `states_` / `localDepth_` for a seed
    // built from that body alone, exactly as `applyUsingVarBorrowPinInLambdas`
    // swaps `states_` for its pin pre-pass) and then feeding the SAME two
    // checks with `EscapeScope::LambdaBody`. No third spelling of either rule
    // and no new predicate: `isCatchBorrow`, `localIsOwnedObligation`,
    // `collectLocalDepths`, `scanReturnsIn` and `forEachNestedStmt` are all
    // pre-existing authorities. // per §28.9, §31.1, v0.286
    void checkLambdaBodyEscapes(
        const std::vector<std::unique_ptr<ast::Stmt>>& body);
    void checkLambdaBodyEscapesExpr(const ast::Expr& e);
    void checkOneLambdaBodyEscapes(const ast::LambdaExpr& lam);

    // fix #3564: seed `states_` with the owned scope-bound locals declared in
    // ONE lambda body, so `checkClosureEscapesOnReturn`'s §28.9 leg (and
    // `checkBorrowStoreEscape`'s owned-source legs) can answer about that
    // body. Resolution goes through `findLocalSymbol` (node-keyed, so it
    // matches a `LocalConstDecl` too) and `localIsOwnedObligation` — the
    // existing both-forms authority — rather than a `dynamic_cast<const
    // ast::LocalDecl*>` walk, which would have been an eighth instance of the
    // #3591 LocalDecl-only family. Called only inside
    // `checkOneLambdaBodyEscapes`'s save/clear/restore window.
    void seedLambdaBodyOwnedLocals(
        const std::vector<std::unique_ptr<ast::Stmt>>& body);

    // fix #3565: fire `checkBorrowStoreEscape` on every `AssignmentStmt`
    // reachable in one lambda body without crossing a nested lambda — the
    // assignment-channel sibling of `scanCatchBorrowClosureReturns`. Rides
    // `forEachNestedStmt` (the #3656 single statement grammar), so a new
    // statement kind reaches it with no edit here.
    void scanLambdaBodyBorrowStores(const ast::Stmt& s);

    // fix #3799 (§28.11, v0.286): the expression-bearing walker companions
    // used by `scanLambdaBodyBorrowStores` to drive
    // `checkCollectionBorrowStoreEscape` from inside lambda bodies. Mirrors
    // `collectBodyLambdas` / `collectBodyLambdasExpr` shape-for-shape (the
    // #3565 / #1919 walkers), so every expression position those admit is
    // visited here. Never crosses into a nested lambda — a nested lambda's
    // own stores are walked when the flat lambda list in
    // `checkOneLambdaBodyEscapes` reaches it, with ITS body's seed.
    void forEachBorrowStoreCallInStmt(
        const ast::Stmt& s,
        const std::function<void(const ast::CallExpr&)>& fn) const;
    void forEachBorrowStoreCallInExpr(
        const ast::Expr& e,
        const std::function<void(const ast::CallExpr&)>& fn) const;

    // #2166 / §28.10 (v0.220): the `using var` borrow-pin classification
    // (`handleLocalDecl`'s `usingVarSourceIsBorrow` flag — see the #2128/#2137
    // derivation above) is set by the dataflow's `visitStmt` walk for every
    // `LocalDecl` reached IN-LINE in a method body. A `LocalDecl` inside a
    // LAMBDA body is reached only by the lambda's own dataflow, which the
    // ownership pass does NOT run (lambdas carry their body as an `Expr`;
    // `checkCapturedOwnedConsumeInStmt` / `checkLambdaTaskFamilyBorrowReturns`
    // are the only lambda walks, and neither classifies the local-decl layer).
    // Codegen (`registerUsingDisposal`, `classOfLocal`) then treats the typed
    // `using var` as an adoption and registers a scope-end dispose — double-
    // freeing the borrow whose real owner also disposes it (heap-use-after-
    // free under ASan for the field-read shape; the same shape family as
    // #2128/#2137, just in lambda position).
    //
    // Run the using-var borrow classification for lambda-body LocalDecls at
    // a structural pre-pass (before the dataflow) — collect every LambdaExpr
    // in `body`, walk each lambda body with a FRESH state map (lambda bodies
    // are independent scopes), and call `handleLocalDecl` on every LocalDecl.
    // `handleLocalDecl` already sets `usingVarSourceIsBorrow` based on the
    // shared predicates (no codegen re-derivation needed); the only thing
    // missing for lambda bodies was the walk. // per §28.10 v0.220, #2166
    void applyUsingVarBorrowPinInLambdas(
        const std::vector<std::unique_ptr<ast::Stmt>>& body);

    // fix #2674 / §28.3 / §28.9 (v0.282): the four OWNERSHIP-pass-set
    // end-of-statement reclaim flags — DiscardStmt::disposeOwnedValue
    // (#1009), disposeOwnedCollectionValue (#1909), disposeOwnedBuilder-
    // Value (#2655), disposeInlineOwnedArray (#1016) — are set inside
    // MethodOwnership::visitStmt's DiscardStmt arm, which the dataflow
    // drives for every METHOD-body statement. Lambda bodies are reached
    // only by narrow checks (never by visitStmt), so the four flags are
    // silently absent inside every lambda body — every `_ := new T();`
    // / `_ := owning-collection-proc;` / `_ := [N owned]T();` discard in a
    // block-bodied lambda leaks at lambda exit. This pre-pass mirrors
    // `applyUsingVarBorrowPinInLambdas`'s shape — collect every LambdaExpr
    // in `body`, walk each lambda body with a fresh state map, and for
    // each `DiscardStmt` apply the SAME four flag-setting predicates
    // visitStmt's DiscardStmt arm applies. Runs BEFORE the dataflow so
    // the flags are stamped by the time codegen reads them. `discardOut-
    // OwnedVar` (#1387/#1651) is intentionally NOT set here — it gates on
    // `lookupState` returning Owned, and the lambda body's dataflow state
    // is empty under the ownership pass, so the gate cannot fire today.
    // // per §28.3, §17.13, §28.9, #2674
    void applyLambdaBodyDiscardStmtFlags(
        const std::vector<std::unique_ptr<ast::Stmt>>& body);
    void applyLambdaBodyDiscardStmtFlagsBody(const ast::LambdaExpr& lam);
    void applyLambdaBodyDiscardStmtFlagsStmt(const ast::Stmt& s);

    // #1066 / §9.7 (v0.153) + §28.3: a borrow-bearing record may not be
    // constructed from a TEMPORARY bound to one of its borrow-carrying fields —
    // the temporary's lifetime ends at the construction expression, so the record
    // immediately outlives that borrow's source (a dangling borrow). Reject at
    // `new Record(...)` when the record is borrow-bearing and a positional arg
    // bound to a borrow-carrying field is a temporary (not a named place). Keeps
    // long-lived-borrow construction (`var xs := […]; new Bag(xs)`) legal.
    void checkRecordFromTemporary(const ast::NewExpr& ne);
    // fix #4344 (§9.7 / §28.3, v0.298): the `with(name := value, …)` non-
    // destructive-update sibling of `checkRecordFromTemporary`. Same rule, same
    // reject, but the `WithExpr::assigns` list is name-keyed (`InitAssign{name,
    // value}`), so each override is matched to its borrow-carrying field by
    // walking the source record's `members` and matching `name` (not by
    // positional indexing as `NewExpr::args` does). The sema phase already
    // validated every `InitAssign.name` against the source record's declared
    // members (ExprTypecheck_Exprs.cpp:10843-10872), so an unresolvable name
    // is impossible here. The predicate deliberately OMITS the inline-array
    // arm of `fieldTypeIsBorrowCarrying`: an inline-array-of-references
    // literal moves its elements into the new record at the `with(...)`
    // boundary (the record OWNS the elements), so there is no §9.7 violation
    // there — see `checkRecordWithFromTemporary`'s implementation comment
    // for the asymmetry rationale and #4330's
    // `positional_field_conforming_accepted_control` test pin.
    void checkRecordWithFromTemporary(const ast::WithExpr& we);
    // True when `ft` is a borrow-carrying field type (a class/interface
    // reference — including the BUILTIN heap collections Array/Dictionary/… that
    // carry no user TypeDecl — an inline array of references, or a transitively
    // borrow-bearing record). Used by checkRecordFromTemporary; intentionally
    // distinct from isBorrowBearingRecord's own predicate (which is left
    // unchanged, so the #360 return-escape behaviour does not shift).
    bool fieldTypeIsBorrowCarrying(const sema::TypeRef* ft) const;
    // True when `e` is a temporary (not a named place — identifier / member
    // access / index slot — whose storage can outlive the construction).
    bool exprIsTemporary(const ast::Expr& e) const;
    bool rhsIsVacuousBorrowPin(const ast::Expr& e) const;
    bool rhsIsDisposableClassType(const sema::TypeRef* rt);
    // §28.9 (v0.237) trivial-dispose discharge-warning exemption (#2654):
    // is `ld` a plain `var` bound to `new T(...)` where T is a trivial-dispose
    // CLASS? Consulted ONLY to suppress the §28.3 auto-drop WARNING — the
    // `autoDropAtScopeEnd` flag and the emitted scope-end free are unaffected.
    // fix #2666: the inline-array-field arm of the trivial-dispose predicate
    // is now tested INSIDE `common::isNonTrivialDisposeClass` itself, so the
    // `classChainHasOwningInlineArrayField` companion walk this method used
    // to consult (#2654) is removed — see `localIsTrivialDisposeNewBinding`'s
    // header comment for the rationale and the #2666 walk-through.
    bool localIsTrivialDisposeNewBinding(const ast::LocalDecl& ld);

    // §28.8.1 / fix #3586 — test-only accessor: true when `s` was recorded
    // as lexically inside a `try` by collectStmtsInsideTry. Exposed so the
    // walker-coverage regression tests for fix #3586 can verify the CaseStmt
    // arm directly; the consumer of `stmtsInsideTry_` (§28.8.1's deferred-
    // firing path) currently skips a whole `case` on purpose, so the gap is
    // latent and otherwise untestable. Read-only — the walker is the only
    // mutator. // per §28.8.1
    bool isStmtInsideTry(const ast::Stmt* s) const {
        return stmtsInsideTry_.count(s) > 0;
    }

private:
    sema::SemaContext& ctx_;
    std::unordered_map<sema::Symbol*, LocalOwnership> states_;
    // fix #4141 (§21.5/§28.3, v0.298): pending `break`/`continue` arm
    // end-states to be JOINED into the IMMEDIATELY enclosing loop's
    // post-loop join, rather than folded into the if/case's own join
    // (which would be a false-reject — see `armDivertsToEnclosingLoop`).
    // Each entry is a snapshot of `states_` at the point of an unlabeled
    // `break`/`continue` (offset 0) that terminates an if/case arm
    // inside this walker; the immediately enclosing `While`/`For`
    // handler pops and joins every entry pushed during its body's walk.
    // Labeled forms targeting an OUTER loop (offset > 0) are deferred to
    // #4146's Stage 2 ("lambda bodies through the real CFG") — this
    // walker has no mechanism to skip past intervening loop constructs
    // when joining, and the over-conservative fallback (folding the arm
    // into the immediate join) is sound-direction only.
    std::vector<BlockState> pendingDivertedStates_;
    // Async-readiness 3c: true while analysing an `async` method/lambda body.
    // Gates the at-await-site Pattern A/B checks and the SuspendLocal
    // annotation so they never affect a synchronous body. // per §30.5
    bool isAsync_ = false;
    bool isGenericBody_ = false;
    // §28.8.1 (#763): true while analysing a DESTRUCTOR body. Inside a
    // destructor the field-restoration rule does NOT require reassigning a
    // moved-from owned field — the object is being torn down and no future read
    // of the field occurs ("the field-restoration rule does not require
    // reassignment inside a destructor"). Gates the field-moved-from-at-exit
    // check so a `move this.f` whose taken value is otherwise discharged is not
    // spuriously rejected. // per §28.8.1
    bool isDestructor_ = false;
    // §28.3 / §28.6 / §5.11 (#1007): the enclosing method's DECLARED return type
    // and whether it was declared `owned`. An owning-collection / owning-wrapper
    // return type (`OwningArray<T>`, `OwningTask<T>`, `Shared<T>`, …) names ELEMENT
    // ownership (§5.11); the CONTAINER-ownership axis is the separate `owned`
    // qualifier. A method declared `: OwningArray<T>` (no `owned`) that hands back
    // a BORROW (a non-owned param, a borrowed local, a plain field read) launders
    // that borrow into a value the caller tracks as owned — a double-free. The
    // return check (handleReturn) consults these to reject that case. Null/false
    // for constructors, destructors, operators, lambdas, and non-owning returns.
    // // per §28.3, §28.6, §5.11
    const ast::Type* returnType_ = nullptr;
    bool returnOwned_ = false;
    // §28.3 (v0.180, #1547): true when the enclosing method is `static` (and
    // therefore has no `this` / receiver). A static method's borrow-return
    // options are strictly narrower than an instance method's: the borrow-
    // lifetime rule admits only a receiver-sourced borrow (an instance
    // method's `this`/its fields) or an extension method's receiver-parameter
    // borrow — neither of which a static method has. checkStaticBorrowReturn
    // consults this flag. Extension methods are never `static` (§26.3 rejects
    // a `static` modifier inside an extension block), so `isStatic_` unambigu-
    // ously means "no receiver at all," not merely "no `this`-keyword access."
    // False for constructors/destructors/operators (no return-borrow rule
    // applies to them here) and for instance methods. // per §28.3
    bool isStatic_ = false;
    // §28.3 *Bare-type-parameter returns* (v0.194, #1801 S6): true when this
    // method's declared return type is a bare type parameter (method- or
    // enclosing-type-level, non-`owned`) that is NOT `where T: value`-constrained.
    // Gates checkBareTypeParamReturn's callee-side admission. Computed once by
    // computeBareReturnContext. // per §28.3 v0.194
    bool returnIsBareTypeParam_ = false;
    // §28.3 (v0.194 S10; #1885): true when this method implements/overrides a
    // generic declaration whose return type is a bare type parameter, so its
    // return classification is inherited (callers dispatch through the generic
    // signature and re-establish borrows against their own arguments). Gates OFF
    // checkInstanceBorrowReturn — a parameter-sourced borrow return from such a
    // body is admitted, exactly as the bare-T rule admits it. Set from the
    // GenericReturnContext computed in checkMember. // per §28.3 v0.194
    bool returnInheritsBareTypeParam_ = false;
    // §28.3 (v0.196, #1910): when `returnInheritsBareTypeParam_` is true, the
    // positions in this method's parameter list that correspond to a SOURCE
    // parameter in the inherited declaration. checkInstanceBorrowReturn uses
    // these to admit parameter-sourced borrow returns ONLY at source
    // positions — a non-source parameter return is still rejected, because
    // its borrow's source is not re-established at every caller. Empty when
    // `returnInheritsBareTypeParam_` is false (no carve-out to gate). Set
    // from the GenericReturnContext computed in checkMember. // per §28.3
    // v0.196
    std::vector<std::size_t> inheritedBareTypeParamSourcePositions_;
    // §28.3 (v0.194, #1801 S6): true when the bare-type-parameter return's
    // POTENTIAL SOURCE SET is structurally empty — a static method with no plain
    // (non-`owned`) parameter whose declared type mentions the returned parameter.
    // With an empty set the factory regime governs the callee body (owned/fresh
    // returns admitted, static-storage rejected — checkStaticBorrowReturn and the
    // factory rule), so checkBareTypeParamReturn is a no-op. Only a NON-empty set
    // (an instance receiver or a plain source parameter) admits the receiver- /
    // parameter-sourced borrow returns. Defaults true (a no-op) so a non-bare-T
    // method is never affected. // per §28.3 v0.194
    bool bareReturnSourceSetEmpty_ = true;
    // §28.3 v0.194 *Bare-type-parameter returns* Callee side, per-instantiation-
    // category admission (#2064): true when the returned parameter is constrained
    // `where T: class` at its own level, so every substitution is class/interface
    // and the body-side reject is decidable at the declaration. False for a
    // mixed-domain parameter (no class constraint) — value substitutions are
    // spec-vacuous and the body is admitted at the declaration; the per-call-site
    // class-substitution check is the recorded residual. Defaults false (the
    // check is a no-op) so a non-bare-T method is never affected.
    // // per §28.3 v0.194, §3.6 rule 2
    bool bareReturnAllClassInstantiations_ = false;
    // §29.3 / §29.8 (#945): true when this method's body uses `yield` (it is an
    // iterator method). A SYNC iterator (sawYield_ && !isAsync_) whose `owned`
    // parameter is never moved out gets §29.8 state-machine auto-dispose instead
    // of the §28.3 normal-exit drop reject; per #1521, the SAME auto-dispose
    // applies whenever `isAsync_` alone is true (a plain, non-iterator `async`
    // method — §30.1's frame-transfer + terminal-state dispose) — see the
    // `sawYield_ || isAsync_` gate in run(). Set at the start of run().
    bool sawYield_ = false;
    // §29.8 (#945): symbols (params or locals) discharged — moved-from or
    // disposed — on AT LEAST ONE path during the dataflow. Recorded by
    // markMovedByName / markDisposedByName. Used by the end-of-method check to
    // distinguish an iterator's owned parameter that is NEVER discharged (safe to
    // auto-dispose unconditionally) from one discharged on some path (kept as a
    // reject — the state machine cannot statically know whether the body moved it
    // out, so auto-disposing would risk a double-free). Monotone (grows only), so
    // it is fixpoint-safe across the dataflow's block re-evaluations.
    std::unordered_set<sema::Symbol*> everDischarged_;
    // fix #4314 (§28.3 / §28.6 / §28.9, v0.298): local symbols whose own
    // DECLARING scope contains a plain bare-identifier reassignment
    // (`sym := E;`) with `producerState(E) == Owned` — the "Pattern 2"
    // (§K1.7/K3.11) idiom of a local that becomes owned via reassignment
    // rather than its own initializer. Populated ONCE per `run()` by
    // `collectReassignedOwnedLocals`, called BEFORE
    // `applyUsingVarBorrowPinInLambdas` (see that call site) so the set is
    // fully populated before the lambda-body pre-pass consults it via
    // `isReassignedOwnedLocalInEnclosingFrame` — unlike `ld->
    // becomesOwnedViaReassign` (set by the MAIN dataflow, which runs AFTER
    // this pre-pass; see that predicate's declaration comment for the
    // measured ordering trap this set exists to avoid). Deliberately NOT
    // cleared/rebuilt per lambda the way `states_` is: this is a per-METHOD
    // static fact, independent of which lambda is currently being walked.
    std::unordered_set<sema::Symbol*> reassignedOwnedLocals_;
    // §28.3 (#1900): locals whose slot DEFINITELY holds a fresh value at the
    // current program point — i.e. the LAST state-changing operation on the local
    // was an UNCONDITIONAL reassignment (`x := new T()` at straight-line scope,
    // not inside a conditional branch). Tracks the consume-and-restore idiom
    // (`s.Take(owned x); x := new Res();`) and the reset-a-resource idiom
    // (`x.Dispose(); x := new Res();`), where the slot is provably fresh on every
    // path reaching scope end despite everDischarged_ being set. A discharge
    // (markMovedByName / markDisposedByName) REMOVES the symbol from the set so
    // a subsequent conditional discharge cannot silently re-poison the local.
    // Used as an additional accept clause in the scope-end auto-drop gate
    // alongside `everDischarged_ == 0` — never as a REPLACEMENT, so the monotone
    // everDischarged_ still protects the conditional-discharge double-free the
    // original gate was designed to catch. NOT monotone: grows on reassign and
    // shrinks on discharge (it represents a current-point fact, not a
    // path-history).
    std::unordered_set<sema::Symbol*> definitelyRestoredLocals_;
    // §28.3 (#1257): every plain (non-`using`) `var` local that a LocalDecl
    // transfer classified as Owned at its declaration. Recorded so the end-of-
    // method auto-drop can flag a nested-scope owned local that is NOT present in
    // the function-exit `endState` — one declared in an inner block that exits
    // normally before the function reaches a throw-only exit (its scope ended, so
    // it is not live at the function exit, and when the sole exit is a throw the
    // exit state may be empty entirely). Such a local is never flagged by the
    // endState loop, and codegen's fallback unwind-only disposal cannot fire (the
    // inner scope was popped before the outer throw unwinds) → leak. Monotone
    // (grows only) like `everDischarged_`, so it is fixpoint-safe.
    std::unordered_set<sema::Symbol*> everOwnedPlainLocals_;
    // §29.2 / §28.3 v0.157 (#1329): every owned iteration variable seeded by
    // seedOwnedIterVar for the CURRENT method. Used by the post-endState
    // break-body check: a seeded iter var absent from endState AND never
    // discharged (everDischarged_.count == 0) leaked via a break-only body —
    // the break edge is not a loopEdge so item=Owned is not propagated to the
    // after block via unionKeys=true. Monotone (grows only), fixpoint-safe.
    std::unordered_set<sema::Symbol*> seededIterVarSyms_;
    // §30.5 / §29.2 (#1888): every owned local (in particular a drained
    // `OwningTask<T>` for-loop iteration variable) that a Pattern B `await`
    // consumed — recorded by recordAwaitOperandConsume. The await IS the
    // discharge ("task discharged by the await; no further obligation", §30.5),
    // so the §29.2 owned-iter-var auto-drop must STILL flag the iter var for a
    // body-end task disposal (the frame is never freed by the await itself —
    // codegen frees a bound task via a scope/body-end fire-and-forget, exactly as
    // a plain `var t := M(); await t;` does) but WITHOUT the spurious must-use
    // warning, matching the warning-free plain-local await. Monotone, fixpoint-safe.
    std::unordered_set<sema::Symbol*> awaitDischargedIterVars_;
    // §28.3 / #1387: every `out owned var` INLINE local whose declaring call
    // appears in a WHILE-LOOP CONDITION (block.isLoopHeader == true). While-
    // condition items are potentially overwritten on each iteration (the slot is
    // shared across iterations via ensureOutVarSlots' alreadyMapped check), so
    // an intermediate value that was not disposed before the next iteration leaks.
    // Auto-drop can only dispose the FINAL slot value → incomplete. These symbols
    // are excluded from the §28.3 affine auto-drop (undischarged keeps the hard
    // error). If-condition and statement-level `out owned var` locals are NOT in
    // this set — they are written at most once, making auto-drop exactly-once-safe.
    // Monotone (grows only), fixpoint-safe.  // per §28.3, §22.4
    std::unordered_set<sema::Symbol*> loopCondOutOwnedSyms_;
    // §28.3 / §28.11 (#1466): every PRE-DECLARED nullable out-owned recipient —
    // `var item: T? := null; coll.TryX(out item);` — i.e. the reachable plain
    // `out item` call-site form at an `out owned` parameter (distinct from the
    // inline `out owned var` / `out using var` forms). markOutOwnedArgs marks
    // such a local Owned unconditionally (conservative for the false path where
    // TryX returned false and the local stays null). The end-of-scope check
    // consults this set to give an undischarged recipient a null-guarded AUTO-
    // DROP (§28.3 warning + scope-end reclaim) instead of the hard discharge
    // error — a recipient is provably null on the Try* false path, so forcing an
    // explicit discharge there would over-reject the idiomatic empty/false
    // pattern (the #1466 REDO reason). Monotone (grows only), fixpoint-safe.
    std::unordered_set<sema::Symbol*> preDeclaredOutOwnedRecipients_;
    // §28.3 (v0.180) consume-site freeze: per-local READ-use source offsets,
    // precomputed once by collectBorrowUses before the dataflow. A borrow-holder
    // is *live* at a consume site (a `move`, an `owned`-argument transfer, or a
    // drop-on-overwrite reassignment) when it has a reachable later use; the
    // freeze query looks for a use textually after the consume, plus the
    // loop-back-edge cases below. Keyed by the read identifier's resolved symbol;
    // SourceLocation::offset (0-based buffer index) is a total source order.
    std::unordered_map<const sema::Symbol*, std::vector<std::uint32_t>>
        borrowUseOffsets_;
    // §28.3 (v0.180) consume-site freeze: the [lo, hi] source-offset extent of
    // every `while` / `for` loop body (bounded by the identifier offsets it
    // contains — loop-statement spans are not populated by the parser). A holder
    // use textually *before* a consume but inside the SAME enclosing loop
    // re-executes after the consume via the back edge, so it keeps the borrow
    // live — the sound closure of the textual "used-after" query over loops.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> loopBodySpans_;
    // Transient stack of the loop bodies currently being scanned by
    // collectBorrowUses (innermost last); accumulates each loop's identifier
    // extent. Populated and drained during the pre-pass only.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> loopScanStack_;
    // §28.3 (v0.186, #1726) consume-site freeze: for each local of borrow-
    // bearing-record type (a span, canonically — isBorrowBearingRecordOrSpan),
    // the source binding(s) whose borrow the record carries, computed at the
    // local's declaration as the identifier root(s) of its initializer chain
    // (`buf.AsSpan()` / `s.Slice(r)` → `buf`/`s`; a `new Rec(a, b)` record →
    // the borrow-carrying arg roots). The spec (v0.186): "A live local of
    // borrow-bearing record type ... counts as a live local borrow of every
    // carried borrow's source ... for as long as `s` is live." These record
    // locals carry NO LocalOwnership.source (they are value-typed records;
    // setting one would perturb the §9.7 escape-on-return / field-store checks,
    // which the spec assigns to §9.7's own escape bullets, not to this freeze).
    // So the freeze consults this map IN ADDITION to states_. A root may itself
    // be another record local — the query resolves transitively.
    //
    // §28.3 (#1741): each carried source records the source-offset WINDOW
    // [from, until) during which the local views it, so a straight-line
    // reassignment (`s := other.AsSpan();`) retires the old source and installs
    // the new one at the reassignment offset R — the old window closes at R (a
    // use of `s` after R no longer keeps the old source live, fixing the stale
    // over-rejection) and the new source opens [R, MAX) (a use of `s` after R
    // now keeps the NEW source live, closing the symmetric under-rejection).
    // A declaration opens [initOffset, MAX). Populated by
    // collectRecordBorrowSource / re-collected on reassignment in the same
    // pre-pass as borrowUseOffsets_. // per §28.3, §4.19, §9.7
    struct RecordBorrowSource {
        const sema::Symbol* source;
        std::uint32_t from;   // source offset at/after which the local views it
        std::uint32_t until;  // source offset before which it views it (MAX=open)
    };
    std::unordered_map<const sema::Symbol*, std::vector<RecordBorrowSource>>
        recordBorrowLocalSource_;
    // issue #4225 (§28.3/§28.7/§30.14 v0.180): the sibling window table for a
    // PLAIN identifier alias holder (`var y := x;` / `y := x;` where the
    // source is a class/interface-typed local, tracked via
    // `states_[holder].source`, NOT a borrow-bearing record/span/choice
    // value). `recordBorrowLocalSource_` is documented above as being for
    // locals that carry "NO LocalOwnership.source" — a plain alias holder
    // does carry one, and checkConsumeFreeze's states_-based holder loop
    // already reads it to decide which holder currently aliases `consumed`.
    // What that loop lacked was a WINDOW bound on "later use": once `y` is
    // rebound away from `x` (`y := new Res(1);`), any further touch of `y`
    // was still counted as a live use of the OLD alias, because
    // `borrowUseOffsets_` is a whole-body pre-scan with no notion of when an
    // alias retires. This map closes that gap with the exact window
    // discipline `recordBorrowLocalSource_` already uses (#1741): each entry
    // is the offset range over which `holder`'s tracked source equals a given
    // symbol, closed at a later unconditional rebind and reopened (or left
    // closed) whenever the rebind's RHS is itself a bare identifier naming a
    // different symbol — a purely STRUCTURAL match, not `borrowSourceForExpr`
    // (which cannot run in this pre-pass at all; see
    // collectAliasBorrowSource's comment for why). It does not need to be
    // authoritative about "is this really a borrow": it is consulted only
    // for a holder checkConsumeFreeze's states_-based loop has ALREADY
    // confirmed, from the live `states_[holder].source`, aliases `consumed`
    // right now — this map only answers WHEN that confirmed alias retires.
    // Kept SEPARATE from
    // `recordBorrowLocalSource_` rather than merged into it: that map's
    // second checkConsumeFreeze loop has no "holder still alive" guard
    // (unneeded there, since record/span locals never reach Disposed/
    // MovedFrom) — merging would need that guard added defensively, whereas
    // here the existing states_-based loop already performs it for the
    // holder before ever consulting this map. // per §28.3, #1741
    std::unordered_map<const sema::Symbol*, std::vector<RecordBorrowSource>>
        aliasBorrowSourceWindows_;
    // §28.3 (#1741): conditional-nesting depth of the freeze pre-pass walk. The
    // method body and unconditional `do…end` blocks are depth 0; each if / elif
    // / else / while / for / try-catch-finally body adds one. A reassignment's
    // precise old-source retirement (window close) is applied ONLY at depth 0,
    // where the rebind is unconditional and definitely retires the old view; a
    // conditional rebind might not execute, so the old source stays live on the
    // not-taken paths and the window is left open (sound over-rejection, the
    // pre-#1741 behavior). // per §28.3
    unsigned borrowUseCondDepth_ = 0;
    // §28.3 / §28.7 (v0.180, #1465): reassigned owned locals whose drop-on-
    // overwrite codegen implements (disposeDisplacedOwnedClassLocal — plain owned
    // class/interface locals; collections/wrappers/tasks excluded). For these the
    // displaced value is disposed at each overwrite, so the FINAL value is safely
    // auto-dropped at scope end (§28.3); they are NOT excluded from affine
    // auto-drop the way other reassigned locals are. This closes the #1465
    // over-rejection (reassigning an owned local and letting it auto-drop).
    std::unordered_set<const sema::Symbol*> reassignedDropCovered_;
    // §28.3 / §28.9 (#861): block-nesting depth of each local's declaration,
    // keyed by its `LocalDecl` AST node (== `LocalSymbol::decl`). The method
    // body is depth 0; each nested do-end / if-branch / loop / try-arm body adds
    // one. Populated at the start of run(). Used by checkBorrowStoreEscape to
    // decide whether a closure assigned to a plain local escapes: the target
    // outlives the captured source iff its declaring block is strictly
    // shallower (smaller depth) — sound because both are in scope at the
    // assignment, so their blocks lie on a single nesting path.
    std::unordered_map<const ast::Node*, int> localDepth_;
    // fix #3727 (§28.3 *Borrow lifetime rules* / §28.9, v0.286): the depths
    // above, SEALED as they stood the FIRST time `collectLocalDepths` ran on
    // this analyzer instance — i.e. the frame that lexically ENCLOSES every
    // lambda body this instance will later check.
    //
    // Why a second copy of the same map is not a second AUTHORITY: it is
    // written by `collectLocalDepths` itself, in one place, from the same walk;
    // nothing recomputes a depth and nothing decides "which block encloses
    // which" a second way. What it preserves is the INPUT that
    // `checkOneLambdaBodyEscapes` (Ownership.cpp) deliberately clears: that
    // driver saves, clears and re-seeds `localDepth_` from ONE lambda body so
    // the §31.1 depth comparison answers about that body — which is exactly
    // right for a source declared INSIDE it, and leaves a source (and target)
    // declared OUTSIDE it with no depth at all (issue 3727: a measured
    // heap-use-after-free).
    //
    // Sealing on the FIRST call is what makes an entry's depth meaningful:
    //   * from `run()` the first call is line ~221's `collectLocalDepths(body)`
    //     over the whole METHOD body, long before `checkLambdaBodyEscapes` runs
    //     at the bottom — so the seal is the method frame;
    //   * on the instances that never call `run()` at all (a property /
    //     operator / accessor `=> E` body: `mo.checkLambdaBodyEscapesExpr(...)`
    //     on a fresh `MethodOwnership`) the first call is the OUTERMOST lambda
    //     body `collectBodyLambdas` yields — again a frame enclosing every
    //     later one it can be consulted about.
    // A miss is always safe: both readers require a POSITIVE hit for BOTH the
    // target and the source before comparing (`enclosingFrameDepthOf` returns
    // -1 otherwise), and a decl recorded in some UNRELATED frame (a sibling
    // lambda's body) can never be named from the body being checked — it is not
    // in scope there — so a hit for both operands proves both live in the one
    // sealed frame, on a single nesting path, with comparable depths.
    // // per §28.3, §28.9, v0.286
    //
    // fix #3743 (§28.3 / §28.9 / §27.5, v0.286): the seal is a CHAIN of frames,
    // not one frame. #3727 sealed only the frame `collectLocalDepths` walked
    // first, and that walk deliberately never enters a `LambdaExpr` (the #3656
    // statement grammar), so an owned local declared in an ENCLOSING LAMBDA's
    // own body was a key of no map at all and `checkBorrowStoreEscape` returned
    // before any comparison — issue 3743, a measured SIGSEGV one frame further
    // out than 3727's. Each entry therefore carries WHICH frame declared it
    // (`frameLevel`: 0 is the frame walked first, +1 per enclosing-lambda
    // nesting step) beside its block depth WITHIN that frame, and the escape
    // comparison is lexicographic on the pair. That is the same "strictly
    // encloses" question the single-frame form asked, extended to the one axis
    // it could not express: descending into a lambda body is a scope step
    // exactly as descending into a `do ... end` is, and along the ancestor
    // chain of ANY assignment both operands are in lexical scope, so their
    // (frameLevel, blockDepth) pairs lie on one strictly-increasing path — the
    // single-nesting-path argument above, one level up.
    struct EnclosingFrameSite {
        // -1 in BOTH fields is the DECLINE answer (`enclosingFrameSiteOf`'s
        // miss), never a guess — see that declaration.
        int frameLevel = -1;
        int blockDepth = -1;
    };
    std::unordered_map<const ast::Node*, EnclosingFrameSite>
        enclosingFrameLocalDepth_;
    bool enclosingFrameDepthsSealed_ = false;
    // fix #3727: the enclosing frame's OWNED-obligation facts, seeded lazily
    // from `enclosingFrameLocalDepth_` through the same `localIsOwnedObligation`
    // authority `seedLambdaBodyOwnedLocals` uses (Ownership.cpp). Built at most
    // once per instance because both of its inputs are settled by the time a
    // lambda-body escape pass can ask: the depth map is sealed, and
    // `applyUsingVarBorrowPinInLambdas` (#2166) has already run at the top of
    // `run()`. // per §28.3, §28.9, v0.286
    std::optional<BlockState> enclosingFrameOwnedStates_;
    // Slice O8 / §28.8.1 (#867): the CURRENT block's `move this.field`
    // tracker — the field's name maps to its moved-from state. The transfer
    // functions (recordMovesIn / handleDischarge mark; handleAssignment clear)
    // mutate this map exactly as they mutate `states_` for locals. The CFG
    // driver (runDataflow) seeds it from the block's merged entry field facts
    // before each block and snapshots it into the block's `FlowState` exit
    // after, so the facts participate in the §28.8.1 join (moved-from on ANY
    // path → reported at exit). fieldMoveLoc_ stores the move-site location for
    // diagnostics (path-insensitive: last move site per field name wins; it
    // only fixes the reported error location). // per §28.8.1, #867
    FieldMovedFrom fieldMovedFrom_;
    std::unordered_map<std::string_view, SourceLocation> fieldMoveLoc_;

    // §28.8.1 "Exception paths are not exempt" (#3080): the statements lying
    // lexically inside a `try` of THIS method — its body, a catch arm body, or the
    // finally body. `checkThrowableWhileFieldMovedFrom` deliberately does NOT fire
    // on them: an exception raised inside a `try` need not leave the method, and
    // the catch arm may itself restore the field, so a throwable expression there
    // is not necessarily an unrestored *exit* and firing would reject legal
    // programs. That leaves the in-`try` case a residual hole (#3241 R1): deciding
    // it needs
    // one exception edge per throwable SITE — the precise F2 the CFG builder
    // explicitly defers ("A more precise F2 would emit one edge per throwable call
    // site; for now we over-approximate", src/flow/CFG.cpp:544) — because the one
    // edge it does emit leaves the try body's END block, a program point AFTER the
    // restoring assignment has already run. Filled by collectStmtsInsideTry before
    // the dataflow, so the answer is stable across the fixpoint passes.
    // // per §28.8.1, #3080
    std::unordered_set<const ast::Stmt*> stmtsInsideTry_;

    // §28.8.1 (#3241 R2): a parallel map from in-`try` statement to the
    // IMMEDIATE enclosing TryStmt. The set above answers "is this in any
    // try?" (used by the original R1 skip); this map answers "which try?"
    // (used by the R2 deferred-check firing to know whose catch arms to ask).
    // Populated by collectStmtsInsideTryStmt on the same recursive pass that
    // fills the set, with the IMMEDIATE enclosing try tracked as the call
    // descends. A statement lexically inside a nested try is associated
    // with the INNERMOST enclosing try (the one whose catch arms are the
    // relevant resolution set for the §28.8.1 exception edge). // per
    // §28.8.1, #3241 R2
    std::unordered_map<const ast::Stmt*, const ast::TryStmt*> stmtsInsideTryByStmt_;

    // §28.8.1 (#3241 R2): deferred in-`try` throwable checks. When a
    // throwable in a try body sees a moved-from field, we cannot decide if
    // it's a violation until we know whether the catch arms restore the
    // field — the dataflow's exitState for the catch arm's last block is
    // empty in the current build (pre-existing #3241 R1 residue: the CFG
    // throwability edge is one over-approximated end-of-body edge, not per
    // throwable site, so the at-exit OR-merge does not see the catch arm's
    // restored state for the in-`try` case). So the fire uses a SYNTACTIC
    // check on the catch arm body — does it contain a top-level
    // restoring assignment to the field? — which is conservative in the
    // safe direction (a catch arm that conditionally restores would be
    // reported as "may not restore"; an under-approximation that misses
    // rather than invents a violation, matching the R1 in-`try` skip's
    // safety posture). Each entry is the throwable's site + the moved
    // fields at that site + the IMMEDIATE enclosing try. Populated during
    // the dataflow and the final emit pass; report()'s suppressDiag_ gate
    // emits the diagnostic exactly once at the final pass. // per §28.8.1,
    // #3241 R2
    struct PendingThrowableInTryCheck {
        const ast::TryStmt* enclosingTry;
        const ast::Expr* throwable;
        std::vector<std::string_view> moved;
    };
    std::vector<PendingThrowableInTryCheck> pendingThrowableInTryChecks_;

    // §28.3 / #892: for loop-header Branch blocks, the ownership state
    // BEFORE the condition expression is evaluated. A while condition
    // `s.TryPop(out owned item)` marks `item` Owned in the post-condition
    // exit state (which flows to the body-entry on the true edge). But on
    // the false (skip) exit — when TryPop returns false and `item` was not
    // written — the local should still carry its pre-condition state (e.g.
    // null/Disposed from the prior body iteration), not the Owned state that
    // the post-condition exit has. This map stores, for each loop header,
    // the locals (and fields) state captured just before processBlock calls
    // handleDischarge on the condition so computeEntry can use it when
    // folding the header→after skip edge. Populated by processBlock; consumed
    // by computeEntry's foldPred lambda. // per §28.3, #892
    std::unordered_map<const flow::CFGBlock*, FlowState> loopSkipExitState_;

    // §28.3 / #1407: for each non-loop if/elif Branch block whose condition
    // declares `out owned var` inline locals, the set of those symbols. On the
    // FALSE edge (TryPop returned false → the local was NOT written), those
    // symbols retain their pre-condition state — null/absent, NOT the Owned
    // state handleDischarge marks unconditionally. foldPred in computeEntry
    // demotes them to PolyNull on the false edge so a discharge inside the
    // then-branch alone correctly satisfies the obligation:
    //   then-branch exit: Disposed ⊓ false-edge: PolyNull = Disposed (OK)
    // vs. the pre-fix wrong meet:
    //   then-branch exit: Disposed ⊓ false-edge: Owned = Owned (spurious).
    // Monotone (grows only), fixpoint-safe. // per §28.3, #1407
    std::unordered_map<const flow::CFGBlock*,
                       std::unordered_set<sema::Symbol*>> ifCondOutOwnedSyms_;

    // Slice O3b: the loop fixpoint re-processes loop bodies until the
    // per-block states stabilize. The transfer functions emit diagnostics
    // as a side effect; running them while iterating to the fixpoint would
    // emit each diagnostic once per pass. So the fixpoint iterations run
    // with diagnostics suppressed, and a single final pass
    // (suppressDiag_ == false) over the stabilized entry states emits each
    // diagnostic exactly once. Every transfer-function report routes
    // through `report()` so the flag is honoured in one place.
    bool suppressDiag_ = false;
    void report(Severity sev, SourceLocation loc, std::string msg) {
        if (suppressDiag_) return;
        ctx_.diagnostics().report(sev, loc, std::move(msg));
    }

    void visitStmt(const ast::Stmt& s);

    // Categorize the initializer expression to determine the local's
    // initial ownership state (slice O1 producer table — minimal).
    OwnershipState producerState(const ast::Expr& e);

    // §21.1 (v0.201), fix #2709: THE authority for "would §21.1 reject this as
    // a `const` local's initializer?". Exactly two callers, and they are the
    // whole point of it existing:
    //   * visitStmt's `LocalConstDecl` arm, as the OUTER GATE on the whole
    //     rejection — the branches inside it only select a message;
    //   * handleLocalDecl's #2073 vacuous-`using var` lint guard, whose two
    //     messages both END in "use `const x := …;`" and so may fire only
    //     where that remedy actually compiles (#2695).
    // Routing the DECISION (not merely the disjuncts) through one function is
    // what makes them incapable of disagreeing. #2695 established that
    // property for the `producerState == Owned` arm alone, leaving §21.1's
    // other two named categories matched by SEPARATELY MAINTAINED tests on the
    // two sides — the drain-alias producer (closed only by coincidence: both
    // sides happened to spell the same predicate) and the builtin
    // `owned`-returning materializers (not matched on the const side at all,
    // which is #2709's leak).
    //
    // The `LocalConstDecl` arm's message-selecting branches are made TOTAL by
    // a trailing `else` carrying §21.1's general wording, so a disjunct added
    // here without a matching branch still REJECTS (with a less specific
    // message) instead of silently accepting — the accept-and-leak shape
    // #2709 was filed for.
    //
    // #2749 item 2 added the FOURTH disjunct, `common::
    // isBuiltinArrayRangeSliceIndex` — §28.3's "Range slice `arr[r]` on
    // `Array<T>` → Owned" row, an `ast::IndexExpr` that neither `producerState`
    // (no IndexExpr arm) nor the two CallExpr disjuncts could see. It was the
    // SAME live remedy loop #2709 fixed, reachable through the more idiomatic
    // spelling: the spec makes `arr[r]` and `arr.Slice(r)` one operation, so
    // rejecting only the second was an inconsistency #2709 introduced. Note the
    // residual it does NOT fix: `using var s := arr[r]` still leaks (#2753) —
    // that is a disposal defect, not a diagnostic one.
    //
    // It is DIAGNOSTIC-ONLY. It never feeds a disposal, a retain, or a
    // dataflow state — `producerState` remains the sole input to those, and is
    // unchanged. // per §21.1, §28.3, §28.10, §29.11, #2695, #2709, #2749
    bool constInitializerRejected(const ast::Expr& e);

    // SB01-A (§28.3 "Task-family calls produce owned frames", v0.190, SB-01,
    // #1786): the GENERIC GUARD for the caller-side task-family classification.
    // True when the callee `md`'s DECLARED (pre-substitution) return type is a
    // bare type parameter — of `md` itself OR of its enclosing type — so a
    // `Task`-family CALL classification must NOT fire (a bare-T return at
    // `T = Task<int>` is governed by *Bare-type-parameter returns*, #1801, not
    // by this rule). Mirrors the recognizer collectBareTypeParamCallBorrowSource
    // uses (method-level via the shared fact, enclosing-type via the callee
    // symbol's parent generic params).
    bool callDeclaredReturnIsBareTypeParam(const ast::CallExpr& ce,
                                           const ast::MethodDecl& md);

    // §28.3 arm-meet (v0.184, #1661): reject an if-expression / expression-
    // position `case`/`when` whose arms mix an owned value with a borrow.
    void checkArmMeetMixed(const ast::Expr& e);

    // fix #4286 (§28.3 / §28.6, v0.298): `fromLambdaBody` mirrors the
    // established `fromLambdaBody` convention (`checkCollectionBorrowStoreEscape`
    // / `checkIndexerSetBorrowStoreEscape`, fix #3799) — true ONLY when this
    // call is driven from `applyUsingVarBorrowPinInLambdaStmt` (the #2166
    // lambda-body pre-pass), never from the method-level `visitStmt` call site.
    // Gates a RETRY of the bare-identifier alias check (see the definition)
    // against the enclosing frame's owned-obligation seed, needed because a
    // lambda body's own `states_` is cleared and rebuilt from scratch per
    // lambda and so never contains a CAPTURED owned local from an enclosing
    // frame. Defaulted so every pre-existing call site is unaffected.
    void handleLocalDecl(const ast::LocalDecl& ld, bool fromLambdaBody = false);

    // §17.15 / K8.5 (v0.216, #2098): process a binding atom `(var x := E;
    // body)` in evaluation order — each clause through handleLocalDecl (the
    // clause IS a §21.1 declaration: classification, marker validation,
    // dangling-borrow checks, states_ registration), then the body through
    // handleDischarge (so K3 consuming positions inside the atom discharge
    // clause bindings), then finishBindAtom (the K8.5 boundary). Guarded by
    // bindAtomsProcessed_ so exactly ONE walker processes a given atom per
    // block visit — checkRead and handleDischarge both dispatch here, and a
    // statement's transfer functions may reach the same expression through
    // either; the guard is cleared per processBlock so the fixpoint
    // iterations re-run the transfer (states must be recomputed per visit).
    void processBindAtom(const ast::BindExpr& bx);

    // §17.15 / K8.5 / K8.4 (v0.216, #2098): the atom's boundary. For each
    // clause (reverse order is codegen's job; this is the decision):
    //   * cross-check the dataflow's end-state against the SHARED disposal
    //     predicate (common::bindClauseNeedsAtomDisposal — the one authority
    //     codegen also consults) and reject on any disagreement: a
    //     CONDITIONAL consume (discharged on some path, Owned at the atom's
    //     end — K7.4's per-path disposal is not implemented for atoms in
    //     this version), or an owned-producer form outside the shared
    //     predicate's decidable set (a Task-family mint, a `move`, a
    //     `??`-owned — decline-with-reasons rather than a silent leak);
    //   * run the K8.4 escape check (checkBindAtomBodyEscape);
    //   * erase the clause bindings from states_ / everOwnedPlainLocals_ —
    //     the atom is their boundary; the end-of-method scope machinery
    //     must never see them (no must-use warning attaches, §17.15).
    void finishBindAtom(const ast::BindExpr& bx);

    // §17.15 / K8.4 (v0.216, #2098): reject a body whose VALUE borrows an
    // owned binding of the atom — "a borrow sourced by an atom binding may
    // not escape the atom — not in the body's value, not by store, not by
    // capture in an escaping closure — rejected, never lifetime-extended"
    // (§28.3 named-temporaries, K8.4 applied at the atom boundary). Covers
    // the peeled body value's borrow roots (a bare clause read, a member/
    // index chain rooted at a clause, a borrow-returning instance call on a
    // clause receiver, if/case arms thereof — resolved TRANSITIVELY through
    // this atom's borrow clauses) and a body-value lambda capturing a
    // clause. Self-contained (classifies clauses via the shared
    // bindClauseIsOwnedProduction, not the dataflow), so the
    // expression-body path can run it too. Gated on suppressDiag_ by the
    // caller.
    void checkBindAtomBodyEscape(const ast::BindExpr& bx);

    // §17.15 / K8.5 (#2098, review F2): the `=>`-body STRUCTURAL siblings of
    // finishBindAtom's staged rejections. An expression-bodied member runs
    // no ownership dataflow, but codegen lowers the K8.5 boundary
    // unconditionally via the shared predicate — so the three staged bounds
    // must hold there too, decidable without states_:
    //   (a) an owned producer OUTSIDE the shared predicate's decidable set
    //       (producerState says Owned, bindClauseIsOwnedProduction does not
    //       claim it — a Task-family mint, a `move`, a `??`-owned): codegen
    //       would register no boundary disposal — a silent leak; reject.
    //   (b) a CONDITIONAL consume (consumed per the scan but not on the
    //       straight-line evaluation — bindClauseConsumedUnconditionally):
    //       codegen would suppress the boundary disposal while the
    //       not-selected path leaks (or an IIFE-selected path double-frees);
    //       reject.
    //   (c) a closure ANYWHERE inside the atom that references an
    //       owned-production clause binding (collectLambdaIdentifiers,
    //       symbol-resolved): the dataflow's #1958 captured-consume scan
    //       cannot run here, so the reference is conservatively rejected in
    //       `=>` position (a read capture is legal in a BLOCK body, where
    //       the precise scan runs; the remedy is a block body).
    // Called from runExprBodyOwningReturnCheck's atom walk only — block
    // bodies keep finishBindAtom's precise dataflow cross-check.
    void checkBindAtomExprBodyStaged(const ast::BindExpr& bx);

    // §17.15 (#2098): per-block-visit dedup for processBindAtom (see above).
    // Cleared at the start of every processBlock so fixpoint iterations
    // re-run the atom's transfer functions.
    std::unordered_set<const ast::Expr*> bindAtomsProcessed_;

    // §28.3 K3.4/K7.4 (v0.209, #2080): the arm-position transfer marker
    // `owned x` — consume-if-selected. processMarkedArmConstruct processes an
    // if-expression / expression-`case`/`when` that CONTAINS marked arms:
    // each value arm is walked from the construct-entry state; a marked arm
    // consumes its binding on that arm's path (handleMarkedArmTransfer —
    // named owned plain LOCAL of class/interface type in this first landing;
    // the K5.1 freeze and the moved-from machinery ride markMovedByName);
    // after the arms, the binding joins per K7.4 — MovedFrom when EVERY
    // non-absorbing value arm consumed it, else Owned + maybeConsumed — and
    // its LocalDecl is flagged dischargedByArmTransfer so codegen's
    // null-guarded scope-end disposal implements the per-path boundary
    // (the marker lowering nulls the slot on the selected path). The K8.3
    // scrutinee freeze — a `case` arm result marking the SCRUTINEE's own
    // binding — is rejected here (the scrutinee borrow is live through the
    // selected arm's completion). Constructs WITHOUT marked arms keep the
    // pre-v0.209 walk byte-identically. Deduped per block visit
    // (markedConstructsProcessed_, cleared with bindAtomsProcessed_).
    void processMarkedArmConstruct(const ast::Expr& e);
    void handleMarkedArmTransfer(const ast::MarkedTransferExpr& mt);
    // fix #4006 (§28.3 K3.4, v0.246 proposal 0090/#3056): a bare `move
    // <identifier>` used directly as an if/case arm's OWN value is the
    // spec's CURRENT canonical spelling of the K3.4 marked arm transfer
    // (§7.7/§28.3's own coalescing example, `(if a <> null then move a else
    // move b)`) — the implementation was never migrated for ARM POSITION:
    // only the legacy `owned x` marker (`MarkedTransferExpr`) routed through
    // `processMarkedArmConstruct`'s per-path-safe disposal
    // (`dischargedByArmTransfer`). `handleMarkedArmMoveTransfer` is this
    // spelling's entry point (mirrors `handleMarkedArmTransfer`, sharing its
    // validate+consume core via `consumeMarkedArmTransfer` — no second
    // spelling of that decision); `isBareMoveArmValue` is the shared
    // recognizer both `exprContainsMarkedTransfer` and
    // `processMarkedArmConstruct`'s per-arm dispatch ask, so the two
    // predicates cannot drift about what counts. A `move` NESTED inside a
    // larger arm expression (`new Holder(move a)`, #3990's own shape) does
    // NOT qualify — only the arm's own, unwrapped value.
    static bool isBareMoveArmValue(const ast::Expr& e);
    void handleMarkedArmMoveTransfer(const ast::UnaryExpr& mv);
    // The validate+consume core `handleMarkedArmTransfer` and
    // `handleMarkedArmMoveTransfer` both funnel through: `name`/`loc` are the
    // bound identifier and the diagnostic anchor, `resolvedSym` is the
    // sema-resolved symbol if the caller has one (falls back to a name scan,
    // exactly as `handleMarkedArmTransfer` always did), and `spellingKeyword`
    // ("owned" or "move") is spliced into the two diagnostics that name what
    // the user actually wrote.
    void consumeMarkedArmTransfer(std::string_view name, SourceLocation loc,
                                   sema::Symbol* resolvedSym,
                                   std::string_view spellingKeyword);
    // True when `e` contains a MarkedTransferExpr OR a bare `move <id>` arm
    // value (see `isBareMoveArmValue`) in an arm position of any nested
    // if/case construct (the gate for processMarkedArmConstruct — a
    // construct without either spelling keeps the old walk).
    static bool exprContainsMarkedTransfer(const ast::Expr& e);
    std::unordered_set<const ast::Expr*> markedConstructsProcessed_;
    // Per-ARM accumulation of the bindings consumed by marked transfers
    // while that arm was walked (nested constructs append theirs — a
    // consume-if-inner-selected is still maybe-consumed from outside).
    // Saved/cleared/collected around each arm by processMarkedArmConstruct.
    std::vector<sema::Symbol*> armTransferConsumed_;

    // Walk an expression and side-effect any `move` operations it
    // contains. Used by handleLocalDecl so `move`-in-initializer
    // properly transitions the source's state.
    void recordMovesIn(const ast::Expr& e);

    // fix #3990 (§28.3 K7.4, v0.294): fold a marker-free if/case construct's
    // per-arm exit states — each computed by the caller from an ISOLATED copy
    // of the pre-construct baseline, so a move recorded in one arm is
    // invisible to another — via the SAME meet the real CFG `if`-join uses
    // (`joinInto`, shared rather than re-spelled). `ends` holds only the
    // NON-absorbing arms' exit states (K7.5's `throw` / `null`-literal arms
    // are walked by the caller for their own diagnostics but never joined —
    // control does not resume after the construct on that path). Leaves
    // `states_` at the meet, or restores `baseline` unchanged when every arm
    // absorbed.
    void joinMarkerFreeArmEnds(std::vector<BlockState>&& ends,
                                BlockState&& baseline, SourceLocation joinLoc);

    void recordMovesInArgs(const std::vector<ast::CallArgument>& args,
                           const ast::Expr* callee = nullptr);

    // fix #4095 (§9.7 / §22.7 / §22.8, v0.295): a record constructor's leading
    // `this(...)` chain argument that is a fresh owned producer (`new T(...)`)
    // destined for a NON-`owned` chain-target formal is rejected up front. The
    // shape produces a dangling borrow: the chain target stores the producer's
    // reference into the record's borrow-carrying field, but the producer's
    // frame ends the moment the chain call returns, so the record outlives its
    // borrow source. The narrow check rejects ONLY `ast::NewExpr*` arguments
    // whose bound formal is not `owned`; `this(42)` / `this(n)` (literal or
    // named owned binding), `this(owned x)` (the legal `owned` transfer), and
    // `this(move x)` are all unaffected. Runs at the top of the constructor
    // body's analysis, gated on the declaring type being a RECORD. See
    // Ownership_Checks.cpp for the diagnostic text and §9.7's no-temporary-
    // source rule. // per §9.7, §22.7, §22.8, v0.295
    void checkRecordChainFreshProducerArgs(
        const std::vector<std::unique_ptr<ast::Stmt>>& body);

    // fix #3751 (§22.10 / §22.3 / §28.8, v0.286): the formals of a `this(...)`
    // / `base(...)` constructor CHAIN call's target, resolved by the SAME
    // `resolveCtorFormals` a `new T(args)` site resolves through and against
    // the target `emitChainCall` lowers to. See the definition in
    // Ownership_Checks.cpp for the measured defect this closes. Answers an
    // invalid view for anything that is not a chain call, so a caller can use
    // it as a fallback when `resolveCalleeParams` finds no symbol.
    //
    // fix #3769 (§22.5, v0.286): takes the ARGUMENT LIST, not just its size —
    // §22.5 admission needs each argument's NAME to bind it, and a call may now
    // omit a defaulted trailing parameter.
    CalleeFormals resolveChainCtorParams(
        const ast::Expr* callee,
        const std::vector<ast::CallArgument>& args) const;

    // fix #3751: `resolveCalleeParams` with the chain fallback applied — the ONE
    // answer to "what formal list does this call reach?" shared by the two
    // owned-sink classifiers (`recordMovesInArgs` and `isImplicitOwned`).
    CalleeFormals calleeOrChainFormals(
        const ast::Expr* callee,
        const std::vector<ast::CallArgument>& args) const;

    // Detect `local.Dispose()` patterns and mark the local as Disposed.
    // Also detects `local` consumed as a `return` value (handleReturn
    // path) and `move local` (UnaryExpr op=move on bare identifier).
    //
    // Slice O2: before marking, check the current state — re-disposing
    // or re-moving an already-Disposed/MovedFrom local is a diagnostic
    // (the §28.3 "read of moved-from / disposed" rules).
    // §28.8: check if argument at position `pos` in `args` is an implicit
    // owned-IN transfer based on the callee's formal parameter list — true
    // only for a plain `owned` formal (the argument is consumed). An
    // `out owned` formal is the owned-OUT direction (the argument local
    // RECEIVES ownership) and is NOT an implicit owned sink (#860).
    bool isImplicitOwned(const std::vector<ast::CallArgument>& args,
                         std::size_t pos, const ast::Expr* callee);

    void handleDischarge(const ast::Expr& e);

    // Recursively descend an expression and, at each IdentifierExpr that
    // names a tracked local, verify the local's ownership state is
    // readable. `selfTarget=true` for the `x` in `x.Dispose()` — we
    // tolerate that as the discharge use site; the post-marking handles
    // it. For everything else, MovedFrom / Disposed is a diagnostic.
    void checkRead(const ast::Expr& e, bool selfTarget);

    void handleAssignment(const ast::AssignmentStmt& as);

    // §28.3 / §28.7 / §28.10 (v0.180, #1965): handle an assignment-position
    // `owned` transfer marker `y := owned x;` (as.valueHasOwnedMarker). The
    // assignment analog of the binding marker (#1962): the source must be a
    // named owned binding (validated + moved-from by checkAndMoveBindingOwnedMarker),
    // the target must be an owned-state or polymorphic-null local (the state-match
    // rule — a borrowed-state target is rejected), the displaced owned value is
    // dropped-on-overwrite (§28.7 — the consume-site "Overwrite while borrowed"
    // freeze applies), and the target adopts Owned. Called from handleAssignment.
    void handleMarkedOwnedAssignment(const ast::AssignmentStmt& as);

    // §15.3 (#1549): a straight-line, program-order pre-pass (mirrors codegen's
    // MoveOnlyArrayTransferWalker's borrow/transfer classification of an
    // identifier occurrence, but for diagnostic purposes rather than
    // disposal-eligibility) that tracks, per move-only inline-array LOCAL
    // name, which slots are provably moved-from at each point in the body.
    // At a whole-value reassignment `arr := newValue;` reachable while some
    // slot is not provably moved-from, emits the §15.3-mandated warning
    // naming the at-risk slots. Runs before the CFG dataflow, like the other
    // syntactic pre-passes below (collectLocalDepths etc.) — it does not
    // participate in branch-join precision (a conditionally-executed
    // `move arr[i]` is treated as if it always executes, the same
    // conservative simplification codegen's walker already makes for
    // disposal eligibility), so it may under- or over-warn on branchy code;
    // the mandate is a best-effort warning, not a soundness-critical check
    // (#1385 tracks the real fix: disposing the displaced slots so nothing
    // leaks in the first place). // per §15.3
    void moveOnlyArrayLeakScan(const std::vector<std::unique_ptr<ast::Stmt>>& body);
    void moveOnlyArrayLeakScanStmt(const ast::Stmt* s);
    void moveOnlyArrayLeakScanExpr(const ast::Expr* e, bool borrow);
    // `move arr[CONST_I]` with a compile-time-constant index proves slot
    // CONST_I moved-from; a non-constant index proves nothing (conservative).
    void moveOnlyArrayNoteSlotMoved(const ast::IndexExpr& ix);
    // A slot store `arr[CONST_I] := RHS;`: `null` proves the slot empty (no
    // owned value to leak — same standing as a proven-moved slot); any other
    // RHS re-populates the slot with a live owned value (at risk again).
    void moveOnlyArrayNoteSlotStore(const ast::AssignmentStmt& as);
    // A bare array-local identifier read in a non-borrow (owned-consuming)
    // position transfers the WHOLE array out of `name` — nothing is left in
    // this binding to leak, so it drops out of tracking.
    void moveOnlyArrayNoteWholeTransfer(std::string_view name);
    // The check point: `arr := newValue;` where `arr` names a move-only
    // inline-array local. Warns when some slot is provably still live, then
    // resets tracking to reflect the fresh (fully-populated) RHS value.
    void moveOnlyArrayCheckReassignment(const ast::AssignmentStmt& as);
    bool isMoveOnlyInlineArrayType(const sema::TypeRef* rt,
                                   long long& sizeOut) const;
    // §15.3 (#1899) bullet 1 "Local-to-local assignment": a bare-identifier read
    // of a move-only inline-array LOCAL in a transfer (non-borrow) position —
    // e.g. the initializer of `var b := a;` — MOVES the whole array: "After the
    // move, `x` is in moved-from state and may not be read until reassigned."
    // Mark the source local moved-from in the flow state (`states_`) so a later
    // read/move/dispose of it is the use-after-move compile error §15.3 mandates.
    // A move-only value has no borrow reading, so an unmarked bare-id read at a
    // value position can only be this move (copies are forbidden). The same
    // applies to a move-only-array PARAMETER source (§15.3 "Parameter passing",
    // #1922): a plain move-only-array parameter is not seeded into `states_` at
    // method entry, so it is seeded lazily at the move site (as a live Borrowed
    // binding) before marking, giving a parameter the identical use-after-move
    // rejection a local gets. A no-op when `src` is not such an identifier or its
    // source is neither a tracked live local nor a move-only-array parameter (a
    // MovedFrom/Disposed/Poisoned source was already diagnosed by the preceding
    // checkRead). // per §15.3 bullet 1
    void noteMoveOnlyArrayWholeMoveRead(const ast::Expr& src);
    struct MoveOnlyArraySlots {
        long long size = 0;
        std::unordered_set<long long> movedSlots;  // provably moved-from
    };
    std::unordered_map<std::string, MoveOnlyArraySlots> moveOnlyArrayLive_;
    // fix #3263 (§15.3 v0.260 / §28.7 v0.260): a name that has been the source
    // of a whole-value move-out — `var b := a;`, a return of `a`, a discard
    // sink of `a`, an argument passing of `a` (a move-only param/argument is
    // a move, #1922) — distinct from per-slot `move a[i]` moves. The leak
    // scan calls `moveOnlyArrayNoteWholeTransfer(name)` to populate the
    // erased set's complement; `moveOnlyArrayCheckReassignment` reads this
    // set to mark a later `a := X;` with `wholeValueLhsMovedFrom` so codegen
    // suppresses the drop-on-overwrite pass (defect 2 — the stale alloca
    // pointers now belong to the receiver, not to `a`).
    std::unordered_set<std::string> moveOnlyArrayWholeValueMovedFrom_;

    void handleReturn(const ast::Expr& e);

    // §28.3 / §28.6 / §5.11 (#1007): reject returning a BORROW through an
    // owning-collection / owning-wrapper return type (`OwningArray<T>`,
    // `OwningTask<T>`, `Shared<T>`, …) declared WITHOUT `owned`. Such a return
    // type names element ownership, not container ownership: a method that hands
    // back a borrowed value (a non-owned param, a borrowed local, a plain field
    // read) launders it into a value the caller tracks as owned — a double-free
    // (#1007). A fresh producer (`new`, `move`, a call returning owned, an owned
    // local/param being transferred out) is Owned, not a borrow, and is
    // unaffected. Fires from handleReturn; a no-op for non-owning return types.
    void checkBorrowReturnedThroughOwningType(const ast::Expr& e);

    // §28.3 SB01-B (v0.190, #1919): the internal machinery behind the two public
    // checkLambdaTaskFamilyBorrowReturns* entry points (declared above). The two
    // collectors walk a body/expression gathering every LambdaExpr (mirroring the
    // scopedPreScan traversal); checkOneLambdaTaskFamilyBorrowReturn applies the
    // callee-side plain-`Task` borrow-return rejection to a single lambda — the
    // effective return type read from the lambda's inferred function `TypeRef`
    // (`funcReturn->headName()`), the async exemption from `lam.isAsync`. The
    // whole pass is syntactic, independent of the enclosing method's dataflow.
    // // per §28.3 SB01-B (v0.190)
    void collectBodyLambdas(
        const ast::Stmt& s,
        std::vector<const ast::LambdaExpr*>& out) const;
    void collectBodyLambdasExpr(
        const ast::Expr& e,
        std::vector<const ast::LambdaExpr*>& out) const;
    // #2166 / §28.10 (v0.220): per-statement walker for the lambda-body
    // using-var borrow-pin pre-pass. Mirrors `collectBodyLambdas` shape-for-
    // shape — under-collecting merely misses a pin (conservative over-adopt,
    // detectable as an ASan trip), so completeness is the bar. Calls
    // `handleLocalDecl` on every LocalDecl reached (the predicate that sets
    // `usingVarSourceIsBorrow`); recurses into nested lambdas via
    // `applyUsingVarBorrowPinInLambdaBody`.
    void applyUsingVarBorrowPinInLambdaStmt(const ast::Stmt& s);
    // #2166 / §28.10 (v0.220): per-lambda body walker — saves/restores
    // `states_` and `fieldMovedFrom_` (the lambda body is its own scope) and
    // descends into the lambda's stmts and any nested lambdas.
    void applyUsingVarBorrowPinInLambdaBody(const ast::LambdaExpr& lam);
    // fix #3571 (§28.3 SB01-B / §31.1, v0.286): the ONE traversal reaching
    // every `return E;` statement in a body without crossing a nested lambda
    // (a pure statement walk never enters a lambda body, which lives inside a
    // LambdaExpr expression) — the 7 statement forms `ReturnStmt`, `IfStmt`
    // (`ifBranch.body` + `elifBranches` + `elseBody`), `WhileStmt`, `ForStmt`,
    // `DoEndBlock`, `TryStmt` (`body` + `catches[].body` + `finallyBody`), and
    // `CaseStmt` (`arms[].stmts`). Previously hand-copied three times —
    // `collectReturnDirectLambdas`, the local `scanReturns` inside
    // `checkOneLambdaTaskFamilyBorrowReturn`, and `scanCatchBorrowClosureReturns`
    // — differing only in the leaf action; `fn` is that leaf, invoked once per
    // `return`'s VALUE expression (never called when `rs->value` is null, i.e.
    // a bare `return;`). One place to add a statement form; one place to decide
    // a future `CaseArmBodyKind::Expression` arm (this walk, like all three
    // copies before it, reaches only `arm.stmts`, not `arm.exprBody` — #3571).
    void scanReturnsIn(
        const ast::Stmt& s,
        const std::function<void(const ast::Expr&)>& fn) const;
    // §28.3 SB01-B (#1929): collect the lambdas that are the DIRECT value of a
    // `return E;` in a block body (mirroring the return traversal), so the block
    // driver can apply the enclosing declared return type only to those.
    void collectReturnDirectLambdas(
        const ast::Stmt& s,
        std::unordered_set<const ast::LambdaExpr*>& out) const;
    // fix #4005 (§28.3, v0.294): the local-decl-DIRECT sibling of
    // collectReturnDirectLambdas immediately above — see its definition
    // (Ownership.cpp) for the full rationale.
    void collectLocalDeclDirectLambdaTargets(
        const ast::Stmt& s,
        std::unordered_map<const ast::LambdaExpr*, const sema::TypeRef*>&
            out) const;
    // fix #4035 (review follow-up on #4005): the shared leaf both the
    // LocalDecl and LocalConstDecl arms of collectLocalDeclDirectLambdaTargets
    // call — records `declType` for `init` when `init` is directly a
    // LambdaExpr, OR recurses through a `(if ... then ... else ...)`
    // ConditionalExpr's `thenValue`/`elseValue` (which right-nests an elif
    // chain as a further ConditionalExpr in `elseValue`, §20/A.7) applying the
    // SAME declared type to whichever arm(s) are themselves directly a
    // LambdaExpr. See Ownership.cpp for the full rationale.
    void recordLocalDeclDirectLambdaTarget(
        const ast::Type* declType, const ast::Expr* init,
        std::unordered_map<const ast::LambdaExpr*, const sema::TypeRef*>&
            out) const;
    // fix #4034 (§28.3, v0.294): the CALL-ARGUMENT-direct sibling of
    // collectReturnDirectLambdas / collectLocalDeclDirectLambdaTargets. A
    // lambda literal passed DIRECTLY as a call argument to a
    // `() -> owned T`-typed parameter has its type dictated by the CALLEE's
    // FORMAL SIGNATURE — a target position neither sibling collector can see
    // (there is no ReturnStmt or LocalDecl at an argument position at all),
    // which is why #4005's review carved this out as needing "callee-
    // signature threading, a materially different mechanism" rather than a
    // third `collectLocalDeclDirectLambdaTargets`-shaped statement-only
    // walker. Unlike the other two, a call site can appear at ANY expression
    // nesting depth (`1 + Foo(lambda() => b)`, `Bar(Baz(lambda() => b))`), so
    // this pair mirrors `collectBodyLambdas`/`collectBodyLambdasExpr`'s FULL
    // statement+expression traversal (the only walker in this file that
    // already reaches a lambda at any nesting depth, including inside a
    // found lambda's own body — #4033's shape falls out of that traversal
    // for free, with no separate re-invocation needed) rather than the
    // narrower 7-statement-form walk `collectLocalDeclDirectLambdaTargets`
    // uses. See Ownership_Checks.cpp for the definitions and for why they
    // live there (the CalleeFormals / resolveCalleeParams / resolveCtorParams
    // / boundFormalIndex machinery they delegate to — CLAUDE.md §6, no
    // second argument-to-formal binding rule — is private to that TU).
    void collectCallArgDirectLambdaTargets(
        const ast::Stmt& s,
        std::unordered_map<const ast::LambdaExpr*, const sema::TypeRef*>&
            out) const;
    void collectCallArgDirectLambdaTargetsExpr(
        const ast::Expr& e,
        std::unordered_map<const ast::LambdaExpr*, const sema::TypeRef*>&
            out) const;
    // fix #4034: the shared leaf the CallExpr / NullCondCallExpr / NewExpr
    // arms of collectCallArgDirectLambdaTargetsExpr call. `formals` is
    // already resolved by the caller (calleeOrChainFormals for a CallExpr/
    // NullCondCallExpr — so a `this(...)`/`base(...)` chain call's target is
    // reached exactly as recordMovesInArgs/markOutOwnedArgs already reach it
    // — or resolveCtorParams for a NewExpr); this leaf only maps each
    // DIRECT-LambdaExpr argument to its BOUND formal's resolved function
    // type via boundFormalIndex (the SAME §22.5 binder every other owned-
    // sink classifier in this file uses, so a named/reordered argument list
    // is not mis-mapped) and CalleeFormals::declType/resolvedType (the same
    // declType-then-resolvedType order verifiedCallBoundedFormalAt's
    // formalIsPlainFunction uses — an imported formal has no ast::Type at
    // all). A formal that does not resolve to a Function-kind type records
    // nothing.
    void recordCallArgDirectLambdaTargetsFromFormals(
        const CalleeFormals& formals,
        const std::vector<ast::CallArgument>& args,
        std::unordered_map<const ast::LambdaExpr*, const sema::TypeRef*>&
            out) const;
    // fix #4074 (§28.3, v0.294): the INVOKED-FUNCTION-VALUE sibling of
    // recordCallArgDirectLambdaTargetsFromFormals immediately above.
    // `calleeOrChainFormals` (and the `resolveCalleeParams` it wraps)
    // requires `ctx_.exprSymbol(callee)` to be a MethodSymbol /
    // ConstructorSymbol / ChoiceCaseSymbol, or a this(...)/base(...) chain
    // target — a plain parameter/field/local whose own STATIC TYPE happens
    // to be a function type resolves to none of those, so
    // `calleeOrChainFormals` answers invalid for it and #4034's mechanism
    // never sees the call. This reads the callee EXPRESSION's own resolved
    // type instead, mirroring the EXISTING authority for the identical
    // "does this function-VALUE callee's type carry information about
    // argument position N" question — checkScopedAdmission's function-value
    // arm (`ft->funcParams[argPos]`, Ownership_Checks.cpp, fix #1420/#1439)
    // — rather than inventing a second spelling of "read the callee's own
    // Function TypeRef" (CLAUDE.md §6). Deliberately does NOT go through
    // `boundFormalIndex`'s §22.5 named-argument binding, for the same
    // reason that existing arm doesn't either: that binding is anchored on
    // a DECLARED formal list with a compiler-verified name/arity
    // (`FormalsView`), whereas a function-VALUE's `TypeRef::FuncParam::name`
    // is optional call-site-facing metadata with no such verification —
    // extending named-argument binding to it is new scope this fix does not
    // open. Deliberately narrow in a second sense too: unlike
    // `CalleeFormals`, which many other owned-sink / scoped / call-bounded
    // classifiers key off (recordMovesInArgs, markOutOwnedArgsCore,
    // scopedFormalAt, verifiedCallBoundedFormalAt, borrowSourceForExpr,
    // catchBorrowAmongNonOwnedCtorArgs — see this file's CalleeFormals
    // header comment), this does not touch `CalleeFormals`,
    // `resolveCalleeParams`, or `calleeOrChainFormals` themselves — it
    // feeds ONLY the lambda-owned-target collector below, so none of those
    // other consumers' move-marking is affected (LANE_RULES §14/§15,
    // #4042's double-free lesson: a classifier change can surface in a
    // caller that keys move-marking off it).
    void recordCallArgDirectLambdaTargetsFromFuncValue(
        const ast::Expr* callee,
        const std::vector<ast::CallArgument>& args,
        std::unordered_map<const ast::LambdaExpr*, const sema::TypeRef*>&
            out) const;
    // fix #4075 (§28.3 / §23.4 *Indexers*, v0.294): the INDEXER sibling of
    // recordCallArgDirectLambdaTargetsFromFormals. An indexer access
    // (`obj[args]`) resolves through a DIFFERENT lookup than a call/`new` —
    // it has no callee EXPRESSION at all (`ast::IndexExpr` carries a
    // `target` and an index `ArgList`, not a callee), so
    // `calleeOrChainFormals`/`resolveCalleeParams` — both keyed on a callee
    // expression — can never see it, and #4034's mechanism never fires for
    // an indexer argument.  Reads the resolved indexer `PropertySymbol`
    // ExprTypecheck's own §23.4 branch already attaches to the IndexExpr
    // node (`ctx_.setExprSymbol(&e, ps)`, ExprTypecheck_Exprs.cpp fix
    // #2530) — the SAME fact `checkIndexArgNullability`'s
    // `declaredIndexParam` lambda (ExprTypecheck_Exprs.cpp, fix #2619)
    // already reads for the identical "declared type of index parameter k"
    // question — rather than re-deriving indexer overload resolution here
    // (CLAUDE.md §6). Binds POSITIONALLY, mirroring that same precedent:
    // §23.5 "Index arguments are positional-only" (fix #2758) — a named
    // argument at an index position is a compile error, so there is no
    // §22.5 bound-formal question to ask here at all, unlike a call. Feeds
    // ONLY the lambda-owned-target collector below, exactly like the
    // function-value sibling above — it does not touch `CalleeFormals` or
    // any of its other consumers.
    //
    // fix #4084 (§28.3 / §23.4 / §14.1, v0.294) NARROWS THAT MIRROR for the
    // GENERIC-INDEXER axis: `checkIndexArgNullability`'s `declaredIndexParam`
    // lambda calls `substituteMemberType` to substitute the receiver's
    // concrete generic type arguments into the declared `T` (so a
    // `Box<() -> owned Res>` indexing `property [mk: T]` reports the
    // resolved `mk`-slot type as `() -> owned Res`, not the bare parameter
    // `T`); this fix reads each slot's post-substitution TypeRef from a
    // per-IndexExpr cache the typecheck pass populates from that exact
    // `substituteMemberType` call site. Pre-fix, the resolver read the RAW
    // declared type straight off `pd->indexerParams` / `ps->indexerParams`
    // (the unresolved `T`, `kind == GenericParam`), so a generic indexer
    // with a BARE-type-parameter declared slot and a function-typed
    // concrete instantiation bypassed the `pt->kind == Function` gate and
    // #4075's defect class reopened through generics — measured 40-byte
    // LSan leak on the issue's exact repro. Mirroring the function-value
    // sibling above, which avoids the same re-derivation by reading
    // `ctx_.exprType` (an existing per-expression cache); the per-indexer-
    // parameter-slot cache here is the indexer-specific equivalent the
    // typecheck pass's existing per-IndexExpr work did not previously
    // publish. The raw-read fallback below the cache branch preserves
    // pre-fix behavior for any case the typecheck pass didn't reach (e.g.,
    // a no-`recvTy` early-out in `checkIndexArgNullability`), so this fix
    // does not regress #4075's existing non-generic surface.
    void recordCallArgDirectLambdaTargetsFromIndexer(
        const ast::IndexExpr& ix,
        std::unordered_map<const ast::LambdaExpr*, const sema::TypeRef*>&
            out) const;
    void checkOneLambdaTaskFamilyBorrowReturn(
        const ast::LambdaExpr& lam,
        const sema::TypeRef* expectedFnType = nullptr);

    // fix #4005 (§28.3 *Borrow lifetime rules* / *Concrete-slot function
    // bodies*, v0.294): the OWNED-target sibling of
    // checkOneLambdaTaskFamilyBorrowReturn immediately above. That function
    // explicitly declines a target whose `funcReturnOwned` is true ("An
    // explicit `owned` target return is a different, explicit transfer
    // contract, not this rule's plain-`Task` case") — but nothing else in
    // the lambda-body-checking machinery ever picks up the owned-target case
    // afterward, so `var mk: () -> owned Res := lambda() => b;` (`b` a
    // captured BORROW) compiled with zero diagnostics and double-freed at
    // runtime: `c := mk()`'s scope-end auto-drop disposes the object once
    // (tracked Owned off the declared `owned` slot) and `b`'s own `using
    // var` scope-end disposal disposes it again — the identical #3993 shape,
    // reached through a function value instead of a named method/operator
    // return. Resolves effRet/effOwned exactly as the sibling function does
    // (the lambda's own inferred function TypeRef first — the #829 typed-
    // local hint — falling back to `expectedFnType` for a lambda whose type
    // is dictated by its position but left un-inferred there), fires only
    // when `effOwned` is true, and applies the SAME exclusions
    // checkOwnedReturnDirectBorrow (#3993, Ownership_Checks.cpp) applies to
    // a named method's return: `move E` (an owned transfer, not a borrow),
    // a bare owned-field read (a legal receiver-sourced transfer), a bare
    // catch-bound identifier (its own unconditional rejection elsewhere),
    // and a bare force-unwrap of a named OWNED place. Reuses
    // `ownedArgSourceIsBorrow` — the single existing borrow-source
    // authority — via a FRESH `MethodOwnership`, exactly as the sibling
    // function does, so the lambda's own parameters resolve independently
    // of the enclosing dataflow. Driven from the same two entry points as
    // the sibling function (checkLambdaTaskFamilyBorrowReturns /
    // -Expr), so it reaches a lambda anywhere in a body a lambda can appear
    // — a local-decl initializer, a return, a field/property/operator
    // position — the moment those drivers are wired for that body (which
    // fix #4004 does for operators in the same commit).
    // // per §28.3 *Borrow lifetime rules*, *Concrete-slot function bodies*
    void checkOneLambdaOwnedReturnDirectBorrow(
        const ast::LambdaExpr& lam,
        const sema::TypeRef* expectedFnType = nullptr);

    // §28.3 (v0.180, #1547): reject a BORROW returned from a STATIC method
    // through a plain (non-owning) class/interface return type declared
    // WITHOUT `owned`. The borrow-lifetime rule admits only two forms of
    // borrow-returning source: a receiver-sourced borrow from an INSTANCE
    // method (`this`, a bare owned-field read, or a borrow reached through
    // the receiver's fields/indexers), or an extension method's receiver-
    // parameter borrow — a static method has neither. The spec's own
    // canonical example is `static method Pick(a: Res, b: Res): Res return
    // a;`: `a` is a borrowed parameter (source: the caller's frame) with no
    // source proven to outlive the call, so returning it must be rejected;
    // this callee-side prohibition is what makes the caller-side sourceless-
    // return owned-assumption (*Factory methods*, §28.3) sound. Scoped to
    // plain return types only: an owning-collection / owning-wrapper return
    // (`OwningArray<T>`, `Shared<T>`, …) is already rejected uniformly for
    // both static and instance methods by
    // checkBorrowReturnedThroughOwningType above, so this does not re-check
    // those (isOwningTypeNameHead heads are skipped to avoid double-
    // reporting). Gated on isStatic_ — a no-op for instance/extension
    // methods, constructors, destructors, and operators. Uses
    // ownedArgSourceIsBorrow to classify the returned expression: a fresh
    // producer (`new`, an owned-returning call, a `move`), a static property
    // read (process-lived storage, §28.3 producers table — and, resolving to a
    // property rather than a FieldSymbol, not classified a borrow at all), or
    // anything the classifier cannot positively identify as a borrow is left
    // unflagged — conservative, so a legal factory return is never
    // over-rejected. **Static-storage re-exposure is property-getter-only
    // (v0.196, #1815).** A static method returning a bare read of a `static`
    // `owned` field (`exprIsStaticMemberRead`) IS now flagged: v0.187 admitted
    // such a read returned from either a static property getter or a static
    // method, and v0.196 RETIRES the static-method arm — nothing
    // signature-visible distinguishes a storage-returning static method from a
    // factory, so every caller classifies the call as the sourceless-factory
    // case (§28.3 *Factory methods*) and would mint a second owner for the
    // borrowed-out storage. Re-exposure is the static PROPERTY getter (whose
    // `T.P` read is the sound caller-side dual); property getters never reach
    // this function (checkMember's PropertyDecl branch routes them to
    // checkGetterFreshOwnedReturn / scanGetterReturns), and a static-method
    // return of a static PROPERTY read stays admitted because
    // ownedArgSourceIsBorrow does not classify a property read as a borrow.
    // Fires from handleReturn (block bodies) and runExprBodyOwningReturnCheck
    // (`=>` expression bodies). // per §28.3, §28.6
    void checkStaticBorrowReturn(const ast::Expr& e);

    // §28.3 (v0.196, #1885) — the INSTANCE-method sibling of
    // checkStaticBorrowReturn. Through a plain (non-`owned`) class/interface
    // return type, an instance method may return only a *receiver-sourced*
    // borrow (`this`, a bare owned-field read, or a borrow reached through the
    // receiver's fields/indexers — §26.7 accesses an extension receiver as
    // `this`, so extension methods are covered). A borrow whose root is a plain
    // parameter (`return a;` / `return h._inner;`) is rejected: every caller
    // attributes an instance non-`owned` return to the RECEIVER (§28.3 producers
    // table), which outlives it, while the borrow aliases the parameter's object
    // — an object the caller may dispose while the return still aliases it
    // (use-after-free). This callee-side prohibition is what makes the caller-
    // side receiver-attribution sound (#1499). A no-op for static methods
    // (checkStaticBorrowReturn), owning-type returns
    // (checkBorrowReturnedThroughOwningType), bare-T returns
    // (checkBareTypeParamReturn admits parameter-sourced borrows, §28.3 v0.194),
    // and receiver-sourced returns. Fires from handleReturn and
    // runExprBodyOwningReturnCheck. // per §28.3, §26.7
    void checkInstanceBorrowReturn(const ast::Expr& e);

    // §28.3 *Bare-type-parameter returns* (v0.194, #1801 S6) — the CALLEE-side,
    // per-instantiation-category admission that SUPERSEDES checkStaticBorrowReturn's
    // vacuous bare-T behaviour (isBorrowableType is false for an abstract bare
    // `T`, so the static-borrow check never fired). When the declared return type
    // is a bare type parameter (method-level or enclosing-type-level, non-`owned`,
    // not `where T: value`) AND the call's potential source set is NON-EMPTY (an
    // instance receiver, or a plain — non-`owned` — parameter whose type mentions
    // the returned parameter), the return-path admission of the §3.6 rule-2 pattern
    // applies: `null`, and a borrow whose source outlives the call (the receiver,
    // a plain source parameter, or static storage — every caller re-establishes
    // these against its own receiver and arguments), are ADMITTED; a fresh owned
    // value, or a borrow of an owned local / `using var` / `owned` parameter, is
    // REJECTED at the return site (declare `owned T`). The check is VACUOUS at the
    // abstract declaration for the ADMIT patterns (a plain-parameter passthrough
    // `return a;` and every §11.8 `Sum<T>`-style `owned T` accumulator stay
    // accepted) and at value / runtime-managed substitutions (the reject's remedy,
    // `owned T`, is erased there, §28.1 v0.196); it bites the unsound owned-escape
    // patterns, which are wrong at every class / interface instantiation. With an
    // EMPTY source set the factory regime governs the body instead (checkStatic-
    // BorrowReturn / the factory rule) — this check is a no-op there. Fires from
    // handleReturn (block bodies) and runExprBodyOwningReturnCheck (`=>` bodies).
    // // per §28.3 v0.194, §3.6 rule 2
    void checkBareTypeParamReturn(const ast::Expr& e);
    // §28.3 (v0.194, #1801 S6): precompute `returnIsBareTypeParam_` and
    // `bareReturnSourceSetEmpty_` from the declared return type, the static-ness,
    // the parameter symbols (for the source set), and the threaded generic context
    // (for positional recognition and the `where T: value` exclusion). Runs once
    // at the top of run() / runExprBodyOwningReturnCheck(), before any return is
    // classified. // per §28.3 v0.194
    void computeBareReturnContext(
        const std::vector<sema::Symbol*>* paramSyms,
        const GenericReturnContext* genericCtx);
    // §28.3 (v0.194, #1801 S6): the shared reject diagnostic for a bare-type-
    // parameter return of a value the caller cannot own (a fresh owned value or a
    // borrow of an owned local / `using var` / `owned` parameter). // per §28.3
    void reportBareTypeParamOwnedReturn(const ast::Expr& e);
    // §28.3 v0.235 (#1666, #2381): returns the ForStmt whose iteration variable
    // `e` is, when that loop's source is statically an INTERFACE-TYPED
    // enumeration (`IEnumerable<T>` / `IAsyncEnumerable<T>`) — the v0.235 ruling's
    // must-reject shape (the element's backing is unprovable, so the variable's
    // source is conservatively the loop's implicit enumerator, a scope-bound
    // local). Returns null for a non-iteration-variable expression, an iteration
    // variable over a CONCRETE collection (Array<T> …, collection-backed, the
    // chain element slot → collection → receiver, admitted), a range/span/
    // inline-array/string source, or the owning heads (IOwningEnumerable/…, which
    // seed the iteration variable OWNED and are rejected by the owned-local arm).
    // // per §28.3, §29.1, §29.2 v0.235
    const ast::ForStmt* borrowedIterVarFromInterfaceSource(const ast::Expr& e);

    // §28.3 / §28.6 (v0.180 #1547; role changed v0.196 #1815): true when `e` is
    // a bare identifier or member access resolving to a STATIC field or property
    // member (a `modifiers` list containing "static" on the resolved
    // MemberSymbol). It once EXEMPTED a static method's read of static storage
    // from checkStaticBorrowReturn; v0.196 retires that exemption (the
    // static-method static-storage re-exposure arm), so the helper now SELECTS
    // the diagnostic instead: a flagged borrow that IS a static-member read
    // (`_instance` / `Other._instance`) gets the message naming the
    // static-property remedy and the `T.P()` -> `T.P` call-site migration; every
    // other borrow return keeps the ordinary no-receiver message. (A static
    // PROPERTY read is never a flagged borrow here — ownedArgSourceIsBorrow
    // resolves it to a property, not a FieldSymbol — so it never reaches the
    // selection.)
    bool exprIsStaticMemberRead(const ast::Expr& e) const;

    // §28.6 *Borrowed owning collections* (#1008 Part C): reject an ownership-
    // transferring / disposing operation (Dispose / Drain / Pop* / Dequeue /
    // owning Add / Insert / Remove* / Clear) on a BORROW of an owning collection.
    // A borrow grants read access only; the owner disposes the container and its
    // elements, so a borrower's Dispose would double-free it. The current
    // scope's OWNED owning collections — a `this`-field, or a local/param with
    // Owned state — keep their full API.
    void checkBorrowedOwningCollectionCapability(
        const ast::MemberAccessExpr& callee);

    // §28.6 (#1005): reject returning an OWNED FIELD's `move`d value through a
    // method return type that is NOT declared `owned`. The leak is the `move`
    // form: `move this.f` / `move f` transfers the owned field's value out
    // (§28.8.1), nulling the field, so the moved value escapes a non-`owned`
    // (borrow) return with no owner. A BARE read (`return this.f;` / `=> f;`) is
    // NOT flagged: per §28.6's "Methods returning `this`" rule a non-`owned`
    // class return type yields a legal BORROW of the field (the field's owner —
    // the receiver's destructor — still disposes it; the owning-collection
    // `return _transactions` leak is the separate #1007 case). An `owned` return
    // type is the legal transfer and is exempt. Scoped to owned FIELD `move`s
    // only; owned-LOCAL returns (`var r := new T(); return r;`) are a separate
    // concern (#1006a) and are NOT handled here. Fires from handleReturn (block
    // bodies) and runExprBodyOwningReturnCheck (`=>` expression bodies).
    // // per §28.6
    void checkOwnedFieldReturn(const ast::Expr& e);

    // fix #4311 (§30.5 "Result lifetime and task disposal", Pattern A result
    // slots kind 1, §28.3 *Producers*, v0.298): the ownership-pass sibling of
    // `fix #4285`'s codegen check (`detectAsyncPatternAKind`,
    // LlvmBackend_Async.cpp), which rejects a Pattern-A `Task<T>` (class T)
    // async producer's non-`await` `return` of a `MemberAccessExpr` /
    // `NullCondMemberExpr` / `IndexExpr` (an unconditional borrow) but
    // DELIBERATELY did not extend to a bare `IdentifierExpr` — that shape may
    // legitimately name a still-owned local or `owned` parameter, and telling
    // it apart from a BORROWED one needs this pass's own per-local flow
    // state (`states_`/`lookupState`), which codegen does not carry. Rejects
    // a bare identifier naming: a local currently tracked `Borrowed`
    // (`lookupState`); a plain (non-`owned`) parameter; or an implicit-`this`
    // field read (a `FieldSymbol` — the same bare-identifier spelling
    // `exprIsOwnedFieldRead` recognizes, §28.6 — is the SAME unconditional
    // borrow #4285 rejects via the explicit `this.f` / `obj.f` spelling, just
    // reached through the unqualified name). Admits: a local tracked `Owned`
    // (a plain `var` — a genuine kind-1 fresh-owned source) and an `owned` /
    // `out owned` parameter. Scoped to the PLAIN `Task` head only, never
    // `OwningTask` (Pattern B) — mirrors `checkBorrowReturnedThroughOwningType`'s
    // `plainTaskInSyncBody` gate's documented two-pattern split. Fires from
    // handleReturn (block bodies) and runExprBodyOwningReturnCheck (`=>`
    // expression bodies). // per §30.5, §28.3, §28.6
    void checkAsyncPatternABorrowedIdentifierReturn(const ast::Expr& e);

    // fix #4311: shared diagnostic for
    // `checkAsyncPatternABorrowedIdentifierReturn`, worded identically to
    // `fix #4285`'s codegen diagnostic (LlvmBackend_Async.cpp) so the two AST
    // shapes of the same §30.5 kind-1 violation read as ONE rule to the user.
    void reportAsyncPatternABorrowedIdentifierReturn(const ast::Expr& e);

    // fix #4349 (§30.5 Pattern A / §28.3 Producers, v0.298): the LAMBDA-BODY
    // sibling of `checkAsyncPatternABorrowedIdentifierReturn`. The latter is
    // wired only into `handleReturn` / `runExprBodyOwningReturnCheck` (the
    // entry points for a top-level METHOD's own return statements / expression
    // body) and is never invoked for a lambda's own body — lambda bodies are
    // walked by the separate `checkLambdaTaskFamilyBorrowReturns{,Expr}`
    // pre-pass (SB01-B), a DIFFERENT, narrower question (whether the lambda
    // VALUE itself is a borrow of a task-family object, not whether the
    // lambda's OWN async body borrows its produced T). Consequence: an
    // `async lambda` that is itself a Pattern-A `Task<T:class>` producer and
    // returns a bare `IdentifierExpr` naming a borrowed local compiles clean
    // and reproduces the EXACT §30.5 kind-1 heap-use-after-free #4311 exists
    // to close — just reached through a lambda instead of a method. (The
    // sibling codegen check from #4285 IS reached correctly for the same
    // lambda body — codegen's `detectAsyncPatternAKind` reaches lambda
    // bodies via the shared `lowerReturnStmt` / `emitAsyncLambda` path. Only
    // the ownership-pass bare-identifier check #4311 added fails to reach
    // lambdas.) Drive the check over each lambda's own body with the
    // LAMBDA's own `isAsync` / return type — mirroring
    // `checkOneLambdaOwnedReturnDirectBorrow`'s shape (uses a fresh
    // `MethodOwnership sub` to classify the local's own initializer
    // independently of the enclosing dataflow). Fires from
    // `checkLambdaTaskFamilyBorrowReturns` (block-body driver) and
    // `checkLambdaTaskFamilyBorrowReturnsExpr` (`=>` expression-body driver),
    // the same two entry points the sibling lambda pre-passes use. Scoped
    // identically to `checkAsyncPatternABorrowedIdentifierReturn`: PLAIN
    // `Task` head only (never `OwningTask` — Pattern B), and only when the
    // result T is a class with a real disposal obligation (§28.9 —
    // `isNonTrivialDisposeClass`). // per §30.5, §28.3, §28.6
    void checkOneLambdaAsyncPatternABorrowedIdentifierReturn(
        const ast::LambdaExpr& lam,
        const sema::TypeRef* expectedFnType = nullptr);

    // §28.3 / §5.11 (#1007): the name-head of an owning-collection / owning-
    // wrapper type (the set whose value the caller must discharge). True for
    // `OwningArray`/`OwningDictionary`/…/`Shared`/`Weak`/`OwningTask`/etc.
    static bool isOwningTypeNameHead(std::string_view head);
    // §28.6 / §5.11 (#1008 Part C): the owning COLLECTIONS only (OwningArray,
    // OwningDictionary, OwningSet, OwningStack, OwningQueue, OwningLinkedList) — the
    // Part C borrow-return / capability rules apply to these; the owning
    // wrappers keep their consumer-owns semantics.
    static bool isOwningCollectionNameHead(std::string_view head);

    // §28.6 / §28.11: the BORROWING-family collections (Array, Dictionary,
    // Set, Stack, Queue, LinkedList, and the v0.136 sorted variants
    // SortedSet/SortedDictionary — §28.11's borrowing-family list). Derived
    // from the SAME name registries codegen/ownership already use for
    // collection dispatch (BuiltinRegistry.hpp), filtered to the non-"Owning"-
    // prefixed entries, rather than a new hand-maintained list.
    static bool isBorrowingCollectionNameHead(std::string_view head);

    // §28.6 "Who may call Dispose()" (v0.213, axiom A8 retired, #2081): true
    // when `e` is an element read — a bare indexer-get `coll[i]`, a for-loop's
    // default-iteration variable, or a field read off one of those (the
    // dictionary-entry `.Key`/`.Value` shape, or any other per-iteration field
    // access) — whose STRUCTURAL SOURCE is statically a borrowing-family
    // collection (isBorrowingCollectionNameHead). Such an element is a borrow:
    // the collection is a transit point, not the element's owner. Through
    // v0.212 the keyed-transit exception (axiom A8) ADMITTED disposing these;
    // v0.213 retired A8 (the #2075 corpus sweep found no load-bearing client),
    // so checkDisposeReceiverOwnership now REJECTS every shape this recognizes.
    // Default iteration over an OWNING collection also produces element borrows
    // (§28.11) but fails the borrowing-family test here — it is rejected by the
    // owning-collection arms of checkDisposeReceiverOwnership instead — and an
    // IEnumerable<T>/enumerator/lazy-query source likewise falls through to the
    // tracked-borrow arm.
    bool disposeReceiverIsBorrowingCollectionElement(const ast::Expr& e) const;

    // §28.6 "Who may call Dispose()" (v0.213, axiom A8 retired, #2081). See the
    // .cpp definition for the full contract and the recorded-gap note. Rejects a
    // `.Dispose()` / `?.Dispose()` / `!.Dispose()` call for the GENUINE-borrow
    // receiver FORMS identifiable by symbol resolution — `this`, a non-`owned`
    // (or foreign-`owned`) field read, a non-`owned` parameter, a catch/pattern
    // binding, an IEnumerable/enumerator-sourced for-loop variable, an
    // owning-HEAP-collection element, and (v0.213, A8 retired) a borrowing-
    // family collection element (indexer-get, iteration variable, dictionary-
    // entry field — disposeReceiverIsBorrowingCollectionElement). It does NOT
    // reject a plain local, a bare call result, an inline-array slot, or anything
    // classified only by the `Borrowed` lattice state: that state conflates a
    // genuine borrow with an owned disposable (enumerator/subscription), so
    // rejecting on it over-rejects — those genuine-borrow shapes are left
    // unenforced (the recorded gap). `loc` is the diagnostic anchor (the receiver's own span —
    // the enclosing MemberAccessExpr's span is not reliably populated by the
    // parser). A no-op when the receiver's own static type is an owning
    // COLLECTION — that axis is fully owned by the sibling
    // checkBorrowedOwningCollectionCapability, called earlier in the same
    // CallExpr handler; running both would double-report the same call.
    void checkDisposeReceiverOwnership(const ast::Expr& recv,
                                        SourceLocation loc);

    // A returned value is only a *borrow* of its local root when it has
    // reference type; a value-typed projection (e.g. `r.V` where V is an
    // int, or `r.Size()` returning int) is a copy and cannot dangle.
    // Generic type parameters (Named with no declaration) are treated as
    // non-borrowable: the ownership checker runs on generic code without
    // knowing the concrete instantiation (e.g. Box<int>.Value has sema type
    // T which looks like an unresolved Named), and conservatively treating T
    // as borrowable produces false positives for value-type instantiations.
    // A missed borrow for T=class is acceptable at this stage (§14).
    static bool isBorrowableType(const sema::TypeRef* rt);

    // §28.3 borrow-escape gate: true when the returned expression could
    // be a borrow aliasing into its identifier root (so it must be checked
    // against `using var` scope). False when the returned value is a
    // value-typed copy (an int/bool/enum/record-by-value projection),
    // which can never outlive its source. Prefers sema's resolved expr
    // type (`isBorrowableType`); for a bare property/field access that
    // sema left untyped — most notably a built-in collection's value-
    // typed `Length`/`Count`/`IsEmpty` projection, which sema types only
    // on the call form — it falls back to recognizing the spec/stdlib
    // value-typed projection by member name (still a copy, never a borrow).
    bool returnedExprMayBorrow(const ast::Expr& e) const;

    // Walk the return value's expression and, for each path that ends
    // in a local-identifier root, verify the root isn't a `using var`.
    void checkBorrowEscapesOnReturn(const ast::Expr& e);

    // §28.3 closure-escape (issue #360, D1): a returned lambda whose
    // captured set includes an owned (`using var` or owned-producing)
    // method-scope local of reference type. Capturing such a local does
    // not transfer ownership; the capture is a borrow whose source is the
    // local's enclosing scope, and the returned closure outlives that
    // scope — a use-after-dispose. Walks the returned expression for any
    // LambdaExpr and diagnoses each offending capture.
    //
    // `scope` (issue 3546, §31.1, v0.283; widened by fix #3564, v0.286):
    // `EscapeScope::LambdaBody` says the caller is
    // `checkOneLambdaBodyEscapes`, asking about ONE LAMBDA BODY rather than
    // the enclosing method. In that scope:
    //   * the §31.1 caught-exception leg runs — `isCatchBorrow` reads a
    //     sema-set flag (`sema::LocalSymbol::isCatchBinding`, issue 3521), so
    //     it classifies the captured identifier exactly from anywhere;
    //   * the §28.9 owned-local leg runs (fix #3564) — the caller has cleared
    //     `states_` and re-seeded it from the lambda body's own owned
    //     obligations, so `isOwnedTrackedLocal` now answers about the right
    //     scope. A method-scope local resolves to nothing there, deliberately:
    //     a closure returned from a lambda leaves only that lambda, and
    //     whether the LAMBDA outlives the method local is the escape question
    //     this pre-pass cannot answer (issue 3564 names it; the SIGSEGV it
    //     still costs is measured and filed as issue 3702);
    //   * the §27.5 ref/out leg is SKIPPED — `isRefOutParam` would resolve the
    //     enclosing METHOD's parameter, and whether a closure escaping the
    //     lambda escapes the method call is exactly the open scoping question
    //     issue 3564 records as unmeasured;
    //   * the §9.7 borrow-bearing record / choice / span leg is SKIPPED —
    //     `recordBorrowLocalSource_` is method-scope provenance and is not
    //     re-seeded, so it too would answer about the wrong scope.
    //
    // ⚠ That exactness is about CLASSIFYING the identifier, NOT about escape.
    // A `return` inside a lambda leaves only THAT LAMBDA, and whether the
    // lambda itself outlives the arm needs escape analysis this pre-pass does
    // not have. The driver is therefore deliberately CONSERVATIVE: it rejects
    // a returned closure over a catch binding anywhere in a lambda body,
    // including the case where the lambda provably does not outlive the arm
    // (`var mk := lambda(): () -> int => do return lambda() => Use(ex); end;
    // var g := mk(); total := g();` — memory-safe, ASan-clean, and rejected).
    // §31.1's own wording is "captured by a closure that MAY outlive the arm",
    // so over-approximating "may" is a defensible reading, but it IS a choice
    // the spec did not make for us and it costs a legal program. Issue 3569
    // tracks narrowing it to lambdas whose own value escapes.
    void checkClosureEscapesOnReturn(
        const ast::Expr& e, EscapeScope scope = EscapeScope::Method);

    // Per-statement driver for `checkOneLambdaBodyEscapes`:
    // fires `checkClosureEscapesOnReturn` on every `return E;` reachable in one
    // lambda body without crossing a nested lambda. fix #3571: a thin
    // `scanReturnsIn` wrapper — was its own copy of the traversal.
    void scanCatchBorrowClosureReturns(const ast::Stmt& s);


    // §28.3 (v0.184, #1958) "Captured owned bindings": inside a closure a
    // captured owned binding — an owned local or `owned` parameter of the
    // enclosing body — is uniformly in BORROW state; a marked transfer
    // (`M(owned x)` / `move x`), an explicit `.Dispose()`, an announced discard
    // (`_ := x`), a reassignment, and every other owned-consuming use of it are
    // compile errors ("a closure cannot consume, reassign, or dispose a
    // captured owned binding"). checkClosureEscapesOnReturn only fires on the
    // ESCAPE (return) path; this walk fires regardless of escape, so an
    // in-place-invoked closure that consumes a captured owned binding — which
    // would dispose the value once in the closure body and again at the owner's
    // scope end (double-dispose / use-after-free) — is rejected. Invoked once
    // per statement from visitStmt (final emit pass only). The `insideLambda`
    // flag is false at method top level (the ordinary consuming forms handled by
    // the main pass are untouched) and becomes true on entering any lambda body,
    // so a consuming form is diagnosed exactly when it is lexically inside at
    // least one closure (a nested closure counts — the binding is still an
    // enclosing-method owned binding there). A captured owned binding that is
    // merely READ (a field access, a non-consuming method call) is never a
    // consuming form and stays accepted.
    void checkCapturedOwnedConsumeInStmt(const ast::Stmt& s);
    void scanCapturedOwnedConsumeStmt(const ast::Stmt& s, bool insideLambda);
    void scanCapturedOwnedConsumeExpr(const ast::Expr& e, bool insideLambda);
    // True when `e` is a bare identifier resolving to an enclosing-method owned
    // binding (a local/`owned` parameter currently in the Owned state — the same
    // key checkClosureEscapesOnReturn resolves against `states_`). A lambda's own
    // parameter or a lambda-body-local owned binding is NOT in `states_`, so it
    // is correctly excluded (a closure may consume what it owns itself).
    bool isCapturedOwnedBinding(const ast::Expr& e);
    void reportCapturedOwnedConsume(std::string_view name, SourceLocation loc,
                                    const char* verb);

    // fix #3755 (§28.3 *Captured owned bindings* / §27.5 / §28.8.1, v0.286):
    // the READ half of the §28.3 captured-owned-binding rule, the sibling of
    // the CONSUME half `checkCapturedOwnedConsumeInStmt` above.
    //
    // §28.3 *Captured owned bindings*: "Inside a closure, a captured owned
    // binding — an owned local or `owned` parameter of the enclosing body —
    // is uniformly in **borrow state**: reads borrow". A capture is therefore
    // a READ of the binding, performed where the closure is created (§27.5:
    // "Captured variables are heap-allocated in a closure object"). §28.8.1:
    // "a moved-from *binding* is simply unreadable until reassigned, with no
    // restoration obligation." Composing the two: capturing a binding that is
    // MovedFrom at the literal's position is the same error `checkRead`'s
    // IdentifierExpr arm reports for a direct `p.V()` after `move p` — but
    // `checkRead` never descends into a lambda literal's body, so the capture
    // form was unchecked and compiled into a read of freed/garbage memory
    // (#3755, measured: a garbage `int`, no diagnostic, rc=0).
    //
    // No new state table: this consults the SAME `states_` MovedFrom fact the
    // direct read consults, at the SAME program point the consume half uses
    // (statement entry — the point the closure is created), and enumerates
    // the captures with the SAME `collectLambdaIdentifiers` free-variable
    // walk every other capture check uses. Resolution is BY SYMBOL
    // (`ctx_.exprSymbol`), like `isCapturedOwnedBinding`, so a lambda
    // parameter or lambda-body local that shadows the outer name resolves to
    // its own symbol, is absent from `states_`, and is correctly not
    // reported. Only the MovedFrom state is consulted — the Disposed /
    // maybe-consumed / Uninitialized legs of `checkRead` are deliberately NOT
    // replicated here (see the commit body's exclusion diff).
    //
    // `lamPath` is the literal's branch path — see `stmtBranchPath_` below for
    // what a branch path is and why the per-statement sets are scoped by one.
    using BranchPath = std::vector<std::pair<const void*, unsigned>>;
    // Two paths can co-execute unless they sit in DIFFERENT arms of some
    // branch node they share. Paths are built by nesting, so a shared node
    // always appears at the same depth with the same prefix, which is what
    // makes the lockstep comparison exact.
    static bool branchPathsCoExecute(const BranchPath& a, const BranchPath& b) {
        const std::size_t n = a.size() < b.size() ? a.size() : b.size();
        for (std::size_t i = 0; i < n; ++i) {
            // Different branch CONSTRUCTS at the same depth are siblings —
            // both run, so they co-execute.
            if (a[i].first != b[i].first) return true;
            // Same construct, different arm: mutually exclusive.
            if (a[i].second != b[i].second) return false;
        }
        // One path is a prefix of the other: the shorter one is unconditional
        // relative to the longer, so it runs whenever the longer does.
        return true;
    }
    void reportMovedFromCapturesIn(const ast::LambdaExpr& lam,
                                   const BranchPath& lamPath);
    // fix #3755 (review follow-up, §22.5 *Evaluation order* / §28.3, v0.286):
    // the same-STATEMENT leg. `checkCapturedOwnedConsumeInStmt` runs on the
    // states at STATEMENT ENTRY, so a consume performed by another part of the
    // SAME statement is invisible to the MovedFrom test above:
    //
    //     Go(move p, lambda(): int => p.V())   // accepted; reads freed memory
    //     Go(lambda(): int => p.V(), move p)   // accepted; reads freed memory
    //
    // BOTH orders were measured to compile rc=0 and read freed storage (plain
    // build printed garbage; `--probe --asan` reported a heap-use-after-free),
    // so the defect is NOT the argument order — it is that the statement
    // transfers the captured binding's value away at all. §22.5's *Evaluation
    // order* ("A call's supplied argument expressions are evaluated in
    // call-site textual order — left to right as written") makes the first
    // spelling a plain read-of-moved-from at the literal; the second is
    // §28.3's consume-site freeze ("A borrow is *live* at a consume site when
    // some path from the site reaches a later use of it — … through a closure
    // value that is itself still live"), whose implementation sees only
    // closures held by LOCALS and therefore misses an argument-position
    // closure temporary. One order-independent rule covers both: a lambda
    // literal may not capture a binding this statement consumes.
    //
    // `stmtConsumedBindings_` is that statement's consume set, `stmtPendingLambdas_`
    // its lambda literals; the walk fills both and the driver drains the
    // second against the first AFTER the walk, so the answer does not depend
    // on which side the walk reached first. Both are cleared per statement.
    //
    // PATH SCOPING (second review follow-up). A flat per-statement set is
    // WRONG for the two EXPRESSION-level mutually-exclusive-branch nodes:
    //
    //     var r: () -> int := (if flag then Consume(move p)
    //                                   else lambda(): int => p.V());
    //
    // is legal and safe — the arms never both run, so on the `else` path `p`
    // was never moved — yet a flat set pools the `move` from one arm with the
    // lambda from the other and falsely rejects it. Statement-level control
    // flow needs no such treatment: `scanCapturedOwnedConsumeStmt` returns
    // early for it at the top level (each nested statement gets its own
    // `visitStmt` -> `checkCapturedOwnedConsumeInStmt` call with fresh state),
    // so IfStmt/WhileStmt/ForStmt/CaseStmt/TryStmt arms are already isolated.
    // Only `ConditionalExpr` (the §20 if-atom, elif chains nesting inside its
    // `elseValue`) and `CaseExpr` are walked as one expression here.
    //
    // Enumerated from the AST: those two are the WHOLE set of expression nodes
    // with mutually exclusive alternatives. The short-circuiting BinaryExpr
    // forms (`??`, `and`, `or`) and the null-conditional accesses have a
    // conditional RIGHT side but an unconditional LEFT one, so their parts CAN
    // co-execute and flat pooling is already the exact answer for them.
    //
    // So each consume and each pending lambda carries the branch path it sits
    // on — the stack of (branch node, arm index) it is nested under — and a
    // consume is paired with a lambda only when the two paths CAN co-execute.
    // A condition / scrutinee runs on every path through its construct and so
    // carries the ENCLOSING path, not an arm's; a `case` arm's guard likewise
    // (a guard may run and its arm still not be selected), which is the
    // conservative direction — it can only add rejections, never lose one.
    BranchPath stmtBranchPath_;
    std::vector<std::pair<sema::Symbol*, BranchPath>> stmtConsumedBindings_;
    std::vector<std::pair<const ast::LambdaExpr*, BranchPath>>
        stmtPendingLambdas_;
    // Cheap gate: false when the statement contains no lambda literal at all,
    // in which case nothing is recorded and the new leg is provably inert.
    bool stmtHasLambda_ = false;
    // Records `e` (a bare identifier naming a dataflow-tracked binding) as
    // consumed by the statement being walked. Non-identifier operands and
    // untracked symbols are ignored.
    void recordStatementConsumedBinding(const ast::Expr& e);
    // The one insertion point for a resolved consumed symbol; stamps it with
    // the branch path currently being walked.
    void recordStatementConsumedSymbol(sema::Symbol* sym);
    // THE one spelling of "does this argument CONSUME its operand?" — a marked
    // `owned r`, or a bare identifier bound to a plain `owned` formal. Shared
    // by `reportArgConsumeCapturedOwned` (the #1958 in-closure consume reject)
    // and the statement-consume recorder, so the two can never drift.
    static bool argConsumesBareBinding(const ast::CallArgument& a,
                                       bool ownedSinkParam) {
        return a.modifier == "owned" || (a.modifier.empty() && ownedSinkParam);
    }
    // fix #3755: per-method de-duplication for the walk above. A nested
    // lambda's identifiers are collected by BOTH its own and its enclosing
    // lambda's `collectLambdaIdentifiers` call, and a block may be re-visited,
    // so report each offending capture SITE at most once. Keyed by AST site,
    // not by symbol, so two distinct captures of the same binding each get
    // their own diagnostic. Deliberately does NOT poison the `states_` record
    // (unlike `checkRead`'s direct-read arm): this walk runs from a pre-pass
    // driver that is invoked before the statement's own transfer functions,
    // and mutating the dataflow record from there would perturb every later
    // consumer of that binding's state.
    std::unordered_set<const ast::IdentifierExpr*> reportedMovedFromCaptures_;
    // True when the argument at source index `argIdx` of a call whose callee is
    // `callee` is a CONSUMING owned sink — a user-declared plain `owned` formal,
    // or a builtin owned-sink position (Domain.Spawn, channel Send, …). Mirrors
    // the owned-sink half of recordMovesInArgs (an `out owned` / ref / out /
    // scoped / borrow formal is NOT a consuming sink). Used to reject the
    // UNMARKED consume `Sink(r)` of a captured owned binding inside a closure —
    // §22.3's call-site `owned`-marker requirement only fires at method top
    // level, so the (more natural) unmarked spelling reaches the closure body
    // unrejected (#1958). §22.5 (#1973): `args` is threaded so the classification
    // keys off the BOUND parameter (named-arg binding), not the source position.
    bool calleeArgIsOwnedSink(const ast::Expr* callee,
                              const std::vector<ast::CallArgument>& args,
                              std::size_t argIdx);
    // Reports the §28.3 v0.184 transfer error when call argument `a` consumes a
    // captured owned binding — either the MARKED form (`owned r`) or the UNMARKED
    // bare identifier flowing into an owned parameter (`ownedSinkParam`). A bare
    // identifier passed to a borrow parameter is not a consume and is accepted.
    void reportArgConsumeCapturedOwned(const ast::CallArgument& a,
                                       bool ownedSinkParam);

    // Free-variable walk of a lambda body. Appends to `out` every
    // IdentifierExpr in the body (and in nested lambdas) — sema has
    // already attached each identifier's resolved Symbol via exprSymbol,
    // and a captured method-scope local resolves to the same LocalSymbol
    // key the dataflow tracks in `states_`, so resolving against `states_`
    // by symbol identifies exactly the captured method-scope locals.
    void collectLambdaIdentifiers(
        const ast::LambdaExpr& lam,
        std::vector<const ast::IdentifierExpr*>& out) const;
    void checkTransitiveCapturesDisposed(
        const ast::LambdaExpr& lam,
        std::set<const ast::LambdaExpr*>& visited);
    void collectIdentifiersIn(
        const ast::Expr& e,
        std::vector<const ast::IdentifierExpr*>& out) const;
    void collectIdentifiersInStmt(
        const ast::Stmt& s,
        std::vector<const ast::IdentifierExpr*>& out) const;

    // fix #4271 (§28.3 / §28.6 / §28.7, v0.298): SEED `states_` with entries
    // from `outerStates` for every LocalSymbol this lambda body CAPTURES
    // (declared outside the body). The lambda-body pre-pass clears `states_`
    // on entry so the dataflow it stands in for doesn't leak lambda-local
    // bookkeeping into the enclosing scope; that clear, however, also
    // discards the outer-scope state for captured locals, so the
    // lambda-body AssignmentStmt / .Dispose() arms' `for (auto& [sym, st] :
    // states_) if (sym->name != tid->name) continue;` and `lookupState(name)`
    // lookups find NOTHING for them and the bookkeeping (producer-state
    // adoption, disposeAliasSource clear, becomesOwnedViaReassign flag,
    // disposeReclaimsReassignedBorrow stamping) silently does not run — a
    // fresh owned value reassigned to a captured local inside the lambda and
    // then explicitly `.Dispose()`'d leaks the heap block. Restored to
    // `outerStates` after the body walk so the enclosing scope's state is
    // untouched.
    void seedCapturedOuterLocals(
        const ast::LambdaExpr& lam,
        const BlockState& outerStates);
    // fix #4271: derive the declaration-time `LocalOwnership` of a captured
    // LocalSymbol from its `LocalDecl` AST and insert into `states_`. See
    // `seedCapturedOuterLocals`'s declaration for why this runs in the
    // pre-pass (BEFORE the dataflow populates `outerStates`).
    void seedCapturedLocalFromDecl(sema::LocalSymbol* ls);

    // §9.7 borrow-bearing-record escape (issue #360, D2): a returned
    // expression whose static type is a borrow-bearing record that
    // carries a borrow of a scope-bound (`using var` / owned) local.
    // Handles the direct `return new Rec(...localUsingVar...)` form.
    void checkRecordBorrowEscapesOnReturn(const ast::Expr& e);

    // §28.9 / §9.7 INDIRECT escape (issue #360, second slice): a returned
    // *bare local* whose tracked borrow-source aliases a still-owned
    // scope-bound local. This covers the tractable indirect forms where the
    // alias was bound into a local one statement earlier:
    //   var b := usingVarLocal;            return b;   // direct re-borrow
    //   var b := usingVarLocal.refField;   return b;   // field-access borrow
    //   var h := new Holder(usingVarLocal); return h;  // record holder
    //   var f := () => usingVarLocal.V;     return f;  // closure holder
    // In each case `b`/`h`/`f`'s LocalOwnership.source was populated at the
    // declaration/assignment site by borrowSourceForExpr (below), pointing at
    // the owned source local; returning the holder lets the borrow outlive
    // that source. Deeper data-flow (store-to-field of an escaping object,
    // multi-hop alias chains) is deferred. Only fires when the source local
    // is still in the Owned state at the return point.
    void checkIndirectEscapeOnReturn(const ast::Expr& e);

    // §28.3 *Bare-type-parameter returns* (v0.194, #1801 S2): given a holder
    // local bound from a bare-type-parameter call (`var m := Pick(a, b);`),
    // return the first carried borrow source that is still a LIVE escape-bound
    // scope-bound obligation at `atOffset` — an owned scope-bound local
    // (§28.9/§9.7) or a catch binding (§31.1) — or nullptr if none. Such a
    // holder carries the UNION of the call's sources in recordBorrowLocalSource_
    // (not in states_.source), so returning it, or storing it into an owned
    // field / out-param / longer-lived local, escapes every contributing source
    // that is still a live owned obligation. A carried source that OUTLIVES the
    // return (a parameter, a returned-through source) is not escape-bound, so it
    // is never returned here — the same guard the single-source paths apply. The
    // view-window is honoured precisely for the holder's DIRECT sources (a
    // straight-line reassignment that dropped the borrow closes it); a transitive
    // hop through an intermediate bare-T / record local uses the full range
    // (sound over-approximation, matching recordBorrowConsumedWindow). // per
    // §28.3, §28.9, §9.7, v0.194
    sema::Symbol* carriedEscapeBoundSourceLiveAt(const sema::Symbol* holder,
                                                 std::uint32_t atOffset) const;

    // §28.3 *Bare-type-parameter returns* (v0.194, #1801 S3/S10/S11/S13). The
    // unified CALLER-side classification of a call whose result is an
    // argument-source-union borrow of a bare type parameter — covering:
    //   * a user method whose declared (pre-substitution) return is a bare type
    //     parameter of the method or its enclosing type (S1);
    //   * a user method that IMPLEMENTS or OVERRIDES a generic declaration whose
    //     return type is a bare type parameter — the source set is read
    //     POSITIONALLY from the generic declaration (S10);
    //   * the intrinsic selector calls `Math.Min`/`Max`/`Clamp` (stdlib §2.2),
    //     whose result borrows all their operands (S11);
    //   * the intrinsic fold `Aggregate(seed, fold)` (stdlib §5), whose result
    //     borrows {receiver, seed} (S13).
    // `sources` is the resolved potential source set (borrowable class/interface
    // roots) — the receiver plus each T-reaching plain argument bound to a live
    // binding; `unboundOwnedTemp` names a T-reaching source position bound to an
    // UNBOUND OWNED TEMPORARY (a fresh producer / `move` result), which bounds the
    // result to the full expression and is the S3 laundering shape. `recognized`
    // is true when the call is such a borrow at a class/interface instantiation
    // (a value/runtime-managed instantiation, or the empty-set factory case, is
    // not recognized). // per §28.3 v0.194
    struct BareBorrowCallInfo {
        bool recognized = false;
        std::vector<sema::Symbol*> sources;
        const ast::Expr* unboundOwnedTemp = nullptr;
    };
    BareBorrowCallInfo analyzeBareBorrowCall(const ast::CallExpr& ce);
    // §28.3 (v0.194, #1801 S10): the generic declaration (interface/base-class
    // method) that `implMd` implements or overrides whose declared return type is
    // a bare type parameter, or nullptr. Found by walking the callee type's base
    // types/interfaces for a same-name, same-arity method with a bare-type-
    // parameter return; the source set is then read positionally from it. // per
    // §28.3 v0.194
    const ast::MethodDecl* findInheritedBareReturnDecl(
        const ast::CallExpr& ce, const ast::MethodDecl& implMd);
    // §28.3 (v0.194, #1801 S3): reject a launder of a fresh owned value out
    // through a bare-type-parameter borrow result — an unbound owned temporary
    // argument bounds the result to the full expression, so binding / storing /
    // returning it is an escape. Fired from handleLocalDecl, handleReturn, and
    // handleAssignment on the escaping expression `e`. A no-op unless `e` is such
    // a call with a temporary source. // per §28.3 v0.194
    void checkBareBorrowTempLaunder(const ast::Expr& e);
    // #2169 / §28.3 v0.226: reject a `Drain()` call bound to a local when the
    // receiver is a non-stable-named place (a temporary). The drain view
    // aliases the receiver's array (stdlib §4.5 v0.195), so the binding's
    // lifetime extends a borrow past the temporary's K8 boundary — a
    // temporary escape per §28.3. Fired from handleLocalDecl only (a `Drain()`
    // expression-statement or argument is consumed inside its own full
    // expression, no escape). The `bindingLoc` is the LocalDecl's own span
    // begin — the parser-stamped binding site, distinct from the synthetic
    // expr nodes' default span (the receiver's span is not a useful target
    // here: a single Drain call may live across many lines and the binding
    // is what the user writes; reporting at the binding is the prevailing
    // diagnostic position). The #2146 stable-named-receiver case is the
    // §28.10 borrow-pin recognition; this is its temporary-receiver twin,
    // currently REJECTED because the codegen-side adoption (iterator-format
    // dispose slot-1 → owning-collection cascade) lives in the locked
    // iterator-emission paths. // per §28.3, stdlib §4.5 v0.195, #2169
    void checkDrainOnTemporary(
            const ast::CallExpr& ce, SourceLocation bindingLoc);
    // #2175(a) / §28.3 v0.226 (spec "Returning a deferred-activation frame"):
    // the RETURN-SITE twin of checkDrainOnTemporary. `return recv.Drain();`
    // over a scope-bound owned local / `using var` / temporary escapes the
    // receiver's scope — the drain view aliases the array (stdlib §4.5
    // v0.195) and is disposed at the callee's exit. Pre-fix the code let it
    // through (runtime benign by accident — the caller's adoption registers
    // no dispose for a declaration-less IOwningEnumerable type); the spec
    // is unambiguous ("a scope-bound local, a `using var`, a temporary —
    // is rejected at the return site"). The check is narrow: ONLY a builtin
    // `recv.Drain()` call on an owning-collection head where `recv` is a
    // tracked scope-bound owned local or `using var`. A `this`-rooted chain
    // is admitted (the receiver outlives the call); a parameter is
    // admitted; a non-Drain call is not affected. // per §28.3 v0.226,
    // stdlib §4.5 v0.195, #2175
    void checkReturnDrainFromScopeBoundReceiver(const ast::Expr& e);
    // §28.3 (v0.194, #1801 S5): reject a loop-carried bare-type-parameter
    // accumulation — `prev := Math.Max(prev, cur)` — where a contributing source
    // is a scope-bound owned local declared in a block the assignment target's
    // block strictly encloses (a source that does not outlive the loop back
    // edge). Reuses the localDepth_ / isEscapeBoundLocal machinery of
    // checkBorrowStoreEscape. // per §28.3 v0.194
    bool checkBareBorrowLoopCarriedEscape(const ast::AssignmentStmt& as);
    // §28.3 (v0.194, #1801 S3/S4): the LocalSymbols currently bound to a
    // bare-type-parameter argument-source-union borrow — the result is a BORROW,
    // never owned, so a `using var` over it is a §28.10 borrow-pin and an explicit
    // `.Dispose()` of it is rejected (you do not own it). Populated at the binding
    // site. // per §28.3 v0.194
    std::unordered_set<const sema::Symbol*> bareBorrowHolders_;

    // §28.3 (#386 cases 1+2) INDIRECT escape through an assignment. When the
    // assignment target is a *field of an object* (`obj.f := …`, including
    // `this.f := …`) or an *out / ref / out-owned parameter* (`p := …`), and
    // the RHS borrows an owned scope-bound LOCAL, the borrow is stored into a
    // location that outlives that local — a use-after-dispose:
    //   - A field of any object outlives a method-scope local; §28.3 forbids
    //     assigning a borrow to any field ("class-typed fields that hold a
    //     borrow are not expressible in this version").
    //   - An out / ref parameter's storage is the caller's frame, which
    //     outlives this method; writing a borrow of a local through it lets the
    //     borrow outlive its source (§28.3 borrow-lifetime rules; §22.3 `out`
    //     produces a borrow location for the caller).
    // The source is restricted to an owned *local* (not an owned parameter):
    // assigning an owned parameter into an owned field is the §28.8 ownership-
    // transfer pattern, not a borrow store, so it must not be flagged here.
    // Borrows whose source is a plain parameter are likewise not flagged — the
    // parameter's source outlives this method (the "borrow in, borrow out"
    // pattern). Called from handleAssignment before the RHS state transition.
    // Returns true when it reported a diagnostic, so the owned-field borrow
    // check (issue #402) can skip a borrow source #386 has already flagged and
    // avoid a duplicate diagnostic for the same store. // per §28.3
    // `scope` (fix #3565, §31.1, v0.286): `EscapeScope::LambdaBody` says the
    // caller is `scanLambdaBodyBorrowStores`, running this check over ONE
    // lambda body with `states_` / `localDepth_` re-seeded from that body
    // alone. Every SOURCE-based leg then answers about the lambda body: a
    // method-scope local is absent from both maps, so the `td >= 0 && sd >= 0`
    // depth guard and `isOwnedTrackedLocal` both decline it. The one leg that
    // consults NO source — §9.7's type-based "a borrow-bearing record / span /
    // choice may not be stored in an instance field of a class" — is therefore
    // skipped in that scope: it is a different rule with a different input,
    // and reaching it inside a lambda body is its own (still open) gap, filed
    // as issue 3701 rather than smuggled in here. // per §31.1, §9.7, v0.286
    bool checkBorrowStoreEscape(const ast::AssignmentStmt& as,
                                EscapeScope scope = EscapeScope::Method);

    // §9.7 (issue #1251): true when an assignment TARGET classified as a field
    // write names storage owned by a CLASS or INTERFACE instance, as opposed to
    // a component of a RECORD value. §9.7's storage bullet is scoped to "an
    // instance field of a **class**" — a class instance's lifetime is not
    // bounded by any borrow source's scope, which is the whole premise of the
    // rejection. A record is a VALUE: its components are reached only through
    // the record value itself, which is subject to §9.7's rules at its own use
    // sites, and §9.7's transitivity clause explicitly contemplates a record
    // whose field is borrow-carrying (that is what MAKES it borrow-bearing).
    // Rejecting a record's own component store would make a borrow-bearing body
    // record unconstructible — `record SpanBox public v: ArraySpan<int>;
    // constructor(x: ArraySpan<int>) v := x; end constructor;` — which the
    // equivalent positional form (§9.1's synthesized primary constructor) is
    // free to do. Resolves the target's member symbol (sema attaches it to both
    // `obj.f` and the bare implicit-`this` form) and reads its declaring type's
    // kind; an unresolvable target keeps the rejection, the conservative
    // reading. // per §9.7, §9.1
    bool storeTargetIsClassField(const ast::Expr& target) const;

    // §28.11 "Storing a borrow" (v0.180, #1539) — the borrowing-collection
    // store rule. Storing a class-typed borrow into a BORROWING collection
    // (`Array<T>`, `Dictionary<K, V>`, `SortedDictionary<K, V>`, `Set<T>`,
    // `Stack<T>`, `Queue<T>`, `LinkedList<T>`) "follows the same shape as
    // §28.3's borrowed-field rule": parameter-, receiver-, and field-SOURCED
    // borrows are admitted; a borrow of a scope-bound LOCAL stored into a
    // collection that provably outlives that scope — the collection is a
    // field, is reachable from one, escapes the method (the receiver or a
    // parameter), or is a local of strictly wider scope — is rejected at the
    // store site. Owning collections (`OwningArray<T>`, …) take an `owned`
    // argument and are governed by the ownership-transfer rules (§28.8)
    // elsewhere, not this rule, so they are excluded. Called from
    // handleDischarge for every CallExpr; scans a builtin storing method
    // (Add/Insert/TryAdd/AddFirst/AddLast/AddBefore/AddAfter/Push/Enqueue)
    // whose receiver is a borrowing collection.
    //
    // fix #3799 (§28.11, v0.286): `fromLambdaBody` is the signal from the
    // lambda-body pre-pass (`scanLambdaBodyBorrowStores`, the #3565 driver).
    // When true, the function additionally retries the borrow-source query
    // against the ENCLOSING frame (mirroring `checkBorrowStoreEscape`'s #3727
    // / #3744 retry leg) so a borrow of a captured owned local — declared in
    // the enclosing method, not in this lambda body — is correctly identified
    // as a scope-bound source, exactly as the assignment-channel sibling does.
    // Default false preserves every existing call site (visitExpr and the
    // method-level dataflow). // per §28.11
    void checkCollectionBorrowStoreEscape(const ast::CallExpr& call,
                                          bool fromLambdaBody = false);

    // §28.11 "Storing a borrow" (hole closed #1884) — the indexer-set sibling of
    // checkCollectionBorrowStoreEscape. §28.11 lists "Add, Insert, indexer-set"
    // as the store operations of a borrowing collection, so `coll[i] := value`
    // is a borrow store exactly like `.Add(value)`: a borrow of a scope-bound
    // owned local stored through it into a collection that provably outlives
    // that scope (a field, reachable from one, escaping the method, or an outer
    // local) is rejected. The IndexExpr assignment target was classified by
    // neither checkBorrowStoreEscape (member/field/out-param/outer-local
    // targets) nor checkCollectionBorrowStoreEscape (storing-method calls).
    // Owning-collection / inline-array indexer sets TAKE owned (a §28.8 transfer)
    // and are excluded. Called from handleAssignment; returns true when it
    // reported a diagnostic.
    //
    // It is also the indexer half of §15.4's fresh-store warning, for BOTH of
    // that section's store positions — "the indexer-set, key and value positions
    // alike" (#3395): the value always, and the index when the head is a
    // dictionary, whose `:=` upsert retains an absent key.
    //
    // fix #3799 (§28.11, v0.286): `fromLambdaBody` mirrors the same retry as
    // `checkCollectionBorrowStoreEscape` above — see that function's comment
    // for the rationale. Default false preserves every existing call site
    // (handleAssignment / the method-level dataflow). // per §28.11, §15.4
    bool checkIndexerSetBorrowStoreEscape(const ast::AssignmentStmt& as,
                                          bool fromLambdaBody = false);

    // §15.4 fresh-store warning (v0.243, the taken #1813 follow-on) — THE one
    // authority for it, and the reason it is a function rather than two inline
    // arms: §15.4 states the rule once for "`Add`/`Insert` and the indexer-set,
    // key and value positions alike", so the two store sinks the compiler has
    // (`checkCollectionBorrowStoreEscape`'s storing-method CALLS and
    // `checkIndexerSetBorrowStoreEscape`'s `coll[i] := value`) must answer it
    // with ONE predicate and ONE message. #3364's whole cost was a second
    // predicate derived for one arm of a classification that already had an
    // authority.
    //
    // §15.4: "Because the retention is statically certain to outlive the value,
    // the compiler **must warn** when a store-position argument on a
    // borrowing-family collection … is a **fresh unbound owned temporary of
    // class type** (§28.3): a `new` expression, a collection literal, an
    // `owned`-returning call result, a `move` result, or a `Task`-family call
    // result … The warning does not narrow the admission: the program remains
    // accepted, no `using var` is demanded, and the recorded literal-only-
    // rejection asymmetry above stands unchanged."
    //
    // FRESHNESS IS `producerState`, NOT A NEW SHAPE LIST. producerState is the
    // §28.3 producers-table authority already used for exactly this question at
    // checkDisposeReceiverOwnership ("a fresh owned producer … or a `move`
    // result — producerState already classifies a class-typed `move` operand as
    // Owned (§28.8.1), so this single check covers both the spec's 'unbound
    // owned temporary' and 'a `move` result' admissions"), and it carries the
    // `new`/owned-call/marked-transfer/arm-meet/Task-family arms too. The one
    // §15.4 form it has no arm for is the collection literal, which is added
    // here as an explicit shape (a literal has no producerState arm because it
    // is not a general expression, §15.4).
    //
    // `isDictionaryKeyPosition` selects the Dictionary-KEY message variant,
    // which omits the owning-family remedy: `OwningDictionary`'s key position is
    // borrowing exactly like `Dictionary`'s, so naming it there would send the
    // reader to a remedy that does not work (the asymmetry
    // checkBorrowingFamilyLiteralElements already prints for a rejected literal
    // key). // per §15.4, §28.11, §28.3
    void warnSec154FreshStore(const ast::Expr& arg, std::string_view storeSpelling,
                              std::string_view recvName, std::string_view collHead,
                              bool isDictionaryKeyPosition);

    // §28.3 / §28.5 / §28.8 (issue #402): true when `target` resolves to an
    // `owned` field — an owned storage location. Reuses the owned-field
    // recognizer `exprIsOwnedFieldRead` (a real FieldSymbol carrying the
    // `owned` modifier; a property setter is not a FieldSymbol, so a property
    // target is correctly excluded). Used by handleAssignment to (a) require
    // an OWNED value on the RHS of an owned-field assignment and (b) record the
    // ownership transfer INTO the field (move-out of an owned local, discharge
    // of a consumed owned parameter). // per §28.3, §28.5, §28.8
    bool targetIsOwnedField(const ast::Expr& target) const;

    // §28.11 (issue #780): true when `target` is an owning inline-array slot
    // `a[i]` — an index into a `[N owned]T?` / `[N owned]T` value. Such a slot
    // is an owned storage location with the SAME discipline as an `owned` field:
    // its indexer-set "takes owned" (§28.11 element slot policy), so a borrow
    // RHS is the double-free hazard and a bare owned-local read is a move INTO
    // the slot. Used by handleAssignment to route the slot write through
    // handleOwnedFieldAssignment (with the owning-array-slot diagnostic
    // wording). // per §28.11, §28.3
    bool targetIsOwningInlineArraySlot(const ast::Expr& target) const;

    // #1020 / §28.6 (v0.153): true when `target` resolves to a NON-`owned` field
    // (a FieldSymbol without the `owned` modifier) — a BORROW field that does not
    // own its referent. Used by handleAssignment to reject a freshly-produced
    // owned value (producerState == Owned) stored into it: the field is not an
    // owner, so the fresh value would leak. A bare borrow read of a parameter or
    // local (producerState == Borrowed) is the legitimate back-reference store and
    // is not flagged. The `owned`-field case is handled by targetIsOwnedField.
    bool targetIsNonOwnedField(const ast::Expr& target) const;

    // §28.3 / §28.5 / §28.8 (issue #402): handle an assignment whose LHS is an
    // `owned` field. An owned field may only receive an OWNED value (§28.3
    // producers: `new`, `move`, an owned-returning call, a consumed `owned`
    // parameter). A BORROW RHS — a bare borrow parameter, a borrowed local, a
    // bare (un-`move`d) owned-field read, or `this` — would give the field's
    // owner disposal responsibility for an object whose real owner also
    // disposes it: a double-free. This reports that hazard (the field-write
    // analogue of #395 / #398). When the RHS is a legal owned value, it instead
    // records the transfer IN: a bare owned-LOCAL read moves out of the local
    // (move-on-read into an owned slot, §28.3); a bare owned-PARAMETER read
    // discharges the parameter (the §28.8 `_resource := resource;` pattern).
    // `move` / `new` / call RHS side-effects are already recorded by the
    // preceding recordMovesIn, so this records only the bare-identifier case.
    // `borrowAlreadyFlagged` is the #386 result, so a borrow source already
    // diagnosed there is not re-reported. `intoOwningArraySlot` (issue #780)
    // selects the owning inline-array-slot diagnostic wording (§28.11) instead
    // of the owned-field wording; the ownership semantics are identical, so the
    // borrow-reject and the move-/discharge-recording paths are shared.
    // // per §28.3, §28.5, §28.8, §28.11
    void handleOwnedFieldAssignment(const ast::AssignmentStmt& as,
                                    bool borrowAlreadyFlagged,
                                    bool intoOwningArraySlot = false);

    // Like the owned-source half of borrowSourceForExpr but restricted to an
    // owned binding whose life ends inside this frame. `kinds` selects WHICH
    // (see OwnedSourceKinds above): the default `LocalOnly` is an owned
    // scope-bound *local* (a `using var` or `var x := <owned producer>`) and
    // nothing else, which is what leaves the §28.8 owned-parameter-into-owned-
    // field transfer alone; `LocalOrOwnedParam` (fix #3746) adds an `owned`
    // parameter, which §28.3 *Captured owned bindings* names in the same breath
    // as the local and *Auto-drop (affine)* disposes "at the end of the method".
    // Returns that binding's symbol, or null. Used by checkBorrowStoreEscape /
    // checkCollectionBorrowStoreEscape / checkIndexerSetBorrowStoreEscape to
    // flag exactly the use-after-dispose hazard — a borrow of a value that dies
    // before the destination does. // per §28.3, §28.8, §28.9
    sema::Symbol* ownedLocalBorrowSource(
        const ast::Expr& rhs,
        OwnedSourceKinds kinds = OwnedSourceKinds::LocalOnly);

    // fix #3706 (§28.3 clause 1 / §28.9 / §28.11 / §29.2 "Iterator yield
    // ownership", v0.294): a TYPE/SHAPE-based recognizer, independent of
    // `states_`, for "is `e` a bare read of a `var` / `using var` / §21.1
    // `const` local bound directly to a fresh, non-dict `[...]` collection
    // literal of the builtin `Array<T>` head?" K1.5 classifies a reference-
    // typed collection literal as an OWNED atom, so per §28.3 clause 1 an
    // unmarked read of such a local stored into ANY field is the same
    // borrow-escape this file rejects for every other owned scope-bound
    // local — but `isBorrowableType` (this file) classifies EVERY
    // `Array<T>`-typed identifier non-borrowable (a declaration-less Named
    // TypeRef, not the recognized builtin-exception carve-out), so
    // `ownedLocalBorrowSource` / `borrowSourceForExpr` above structurally
    // cannot see one as a live borrow source regardless of the local's
    // `states_` state — true of a plain `var`, a `using var`, AND a `const`
    // alike (measured: all three compile clean with no diagnostic before
    // this fix). This recognizer is the narrow, independent leg
    // `checkBorrowStoreEscape` needs so the store is rejected anyway. See
    // the .cpp for the full derivation and why it is sound in both
    // `EscapeScope::Method` and `EscapeScope::LambdaBody` (it consults no
    // `states_` / `localDepth_` entry, mirroring the §9.7 record/span/choice
    // leg). Returns the local's symbol, or null.
    sema::LocalSymbol* borrowingArrayLiteralLocalFieldStoreSource(
        const ast::Expr& e) const;

    // Compute the owned scope-bound source local that the initializer/RHS
    // expression `rhs` borrows from, or null if it borrows nothing owned.
    // Used to populate LocalOwnership.source so an aliasing local can be
    // recognized as an escape when it is later returned. Recognizes exactly
    // the three borrow-binding shapes the indirect check above relies on:
    //   - a re-borrow rooted at an Owned local (`r` or `r.f` / `r[i]`),
    //   - a borrow-bearing-record construction whose arg aliases an Owned
    //     local (`new Holder(r)`),
    //   - a closure capturing an Owned local (`() => r.V`).
    // Returns the FIRST owned source found. A value-typed result (e.g. an
    // `int` re-borrow `var n := r.V;`) is never reported because the source
    // is only taken when the alias carries a reference/record/closure borrow
    // — the per-shape gating below ensures a value-typed local is never
    // flagged. Conservative: returns null when it cannot prove an owned
    // source, so it never manufactures a false escape.
    sema::Symbol* borrowSourceForExpr(const ast::Expr& rhs);

    // §28.3 (v0.180) consume-site freeze pre-pass. Walk the method body once,
    // recording every READ-use of a local into borrowUseOffsets_ and the extent
    // of every loop body into loopBodySpans_, so the freeze query can answer
    // "is this borrow-holder still live at the consume site?" without a second
    // dataflow. Called once at the start of run(). // per §28.3
    void collectBorrowUses(const std::vector<std::unique_ptr<ast::Stmt>>& body);
    void collectBorrowUsesStmt(const ast::Stmt& s);
    // §28.3 (v0.186, #1726): if `ld` declares a local whose initializer produces
    // a borrow-bearing record / builtin span view (isBorrowBearingRecordOrSpan),
    // record the source binding(s) it carries into recordBorrowLocalSource_ (the
    // identifier root of the initializer chain, or the borrow-carrying arg roots
    // of a `new Rec(...)` construction). Called from the freeze pre-pass at each
    // LocalDecl / LocalConstDecl. // per §28.3
    // fix #3689 (§21.1, v0.286): widened from `const ast::LocalDecl&` to
    // `const ast::Node&` — §21.1 explicitly admits a `const` binding a
    // borrow-bearing record/span value (a record constructor call, `AsSpan()`,
    // …), and the body only ever reads `ld`'s initializer expression and passes
    // `ld` on to `findLocalSymbol` (already `const ast::Node&`), so this is a
    // pure widening. See the header comment at the LocalConstDecl call sites in
    // collectBorrowUsesStmt for the measured false-ACCEPT this closes.
    void collectRecordBorrowSource(const ast::Node& ld);
    // §28.3 *Bare-type-parameter returns* (v0.194, #1801): a DISJOINT sibling of
    // collectRecordBorrowSource. When `ld`'s initializer is a call whose callee's
    // declared (pre-substitution) return type is a bare type parameter (of the
    // method or its enclosing type) and the call's potential source set is
    // NON-empty (receiver + T-reaching plain borrowable args), record each source
    // root into recordBorrowLocalSource_ with window [initOffset, MAX). The
    // result is a borrow carrying the UNION of its sources' liveness, so the
    // existing checkConsumeFreeze scan freezes consumption (move / owned-arg
    // transfer / drop-on-overwrite) of any contributing source while the result
    // has a reachable later use. Keyed on the bare-T-call recognizer, never on
    // the record predicate, so span/record freeze is untouched. // per §28.3
    // fix #3689 (§21.1 / §28.3 v0.194, v0.286): widened from
    // `const ast::LocalDecl&` to `const ast::Node&`, the same rationale as
    // collectRecordBorrowSource above — a `const m := Pick(a, b);` binding a
    // bare-type-parameter call result is §21.1-admissible and carries the same
    // union borrow a `var` does.
    void collectBareTypeParamCallBorrowSource(const ast::Node& ld);
    // §28.3 (#1741): re-collect the carried source of a borrow-bearing-record /
    // span local `s` when it is REASSIGNED (`s := <producer>;`) at source offset
    // `reassignOffset`. The old source windows are closed at that offset and the
    // new producer's source windows opened from it, so a later use of `s` keeps
    // the NEW source live, not the stale one. Called only at depth 0 (an
    // unconditional rebind). // per §28.3
    void recollectRecordBorrowSourceOnReassign(const sema::Symbol* lsym,
                                               const ast::Expr& producer,
                                               std::uint32_t reassignOffset);
    // issue #4225: follow a chain of PLAIN alias holders already recorded in
    // aliasBorrowSourceWindows_ EARLIER in this same forward pre-scan pass
    // (only statements already visited have entries, since the scan is a
    // straightforward single forward pass) to its ultimate root — the
    // state-independent equivalent of borrowSourceForExpr's own "multi-hop
    // alias chain" resolution (Shape 1, Ownership_Checks.cpp), which cannot
    // run here (states_ is not live yet). Needed for soundness, not just
    // precision: without it, `var w := x; y := w;` would record `y`'s window
    // against `w` instead of `x`, so `aliasBorrowConsumedWindow(y, x)` would
    // find nothing and checkConsumeFreeze's holder loop would silently
    // MISS-reject a later consume of `x` while `y` (via `w`) still has a
    // reachable use — the false ACCEPT this issue's own steering warned
    // against. Returns `sym` itself when it carries no currently-open
    // tracked alias (the base case: an unaliased identifier, or a plain
    // owned root). `seen` guards a cycle (structurally not expected, cheap
    // to defend). // per §28.3, #4225
    const sema::Symbol* resolveAliasChainRoot(const sema::Symbol* sym) const;
    // issue #4225 (§28.3/§28.7/§30.14 v0.180): sibling of
    // collectRecordBorrowSource for a PLAIN identifier alias of a
    // class/interface-typed source (`var y := x;`) — see
    // aliasBorrowSourceWindows_'s comment for why this is a separate map
    // rather than a widening of collectRecordBorrowSource. Records a
    // STRUCTURAL match only (the initializer is a bare identifier naming
    // symbol S) — NOT borrowSourceForExpr, which reads live states_ and
    // cannot run in this pre-pass at all (states_ has no entries yet when
    // collectBorrowUses runs; see this function's own definition comment for
    // the measured failure of an earlier draft that tried). Called from the
    // freeze pre-pass at each LocalDecl / LocalConstDecl. // per §28.3, #4225
    void collectAliasBorrowSource(const ast::Node& ld);
    // issue #4225 / #4250 (send-back — measured heap-use-after-free):
    // re-collect a plain alias holder's tracked-source window when it is
    // REASSIGNED (`y := <producer>;`) at source offset `reassignOffset` —
    // the plain-alias sibling of recollectRecordBorrowSourceOnReassign.
    // FAILS CLOSED BY DEFAULT: only two RHS shapes are trusted to retire
    // (close-without-reopening, or close-then-reopen) a window —
    //   (1) self-assignment (the RHS names `lsym` itself) is a no-op: it
    //       touches nothing, since assigning a holder to its own current
    //       value cannot change what it aliases;
    //   (2) a bare identifier naming a DIFFERENT symbol closes then opens a
    //       new window against the chain-resolved root (unchanged from the
    //       original #4225 fix);
    //   (3) a `new` expression closes without reopening (the one shape this
    //       fix's own regression tests rely on).
    // EVERY OTHER RHS shape — `this.w`, `x.Field`, `w[0]`, `w!`, a call, and
    // any future shape nobody has written a case for — leaves every open
    // window untouched, so the old unbounded, conservative query stays in
    // force for that holder/source pair. #4250 measured that the prior
    // "close unconditionally, reopen only for a bare identifier" default
    // silently retired a window real dataflow still considered open, for
    // both self-assignment (a live heap-use-after-free) and member-access
    // RHS (`borrowSourceForExpr`'s own Shape 1 recognizes
    // MemberAccessExpr/NullCondMemberExpr/IndexExpr as alias-producing;
    // this state-free pre-pass cannot evaluate those live-states_ gates to
    // know whether a given instance actually does, so it must not guess).
    // Called only at depth 0 (an unconditional rebind), matching
    // recollectRecordBorrowSourceOnReassign's own §1741 depth-0 restriction:
    // a conditional rebind might not execute, so the old window must stay
    // open on the not-taken path (sound over-rejection). // per §28.3,
    // #4225, #4250
    void recollectAliasBorrowSourceOnReassign(const sema::Symbol* lsym,
                                              const ast::Expr& producer,
                                              std::uint32_t reassignOffset);
    // issue #4225: the source-offset window [from, until) during which
    // `holder`'s tracked alias (via aliasBorrowSourceWindows_) is `consumed`,
    // unioned across entries the same way recordBorrowConsumedWindow does —
    // or nullopt if `holder` was never tracked as aliasing `consumed` at all.
    // ⚠ KNOWN SCOPE LIMIT vs. recordBorrowConsumedWindow's record-local case:
    // no multi-hop chain resolution. `var b := r; var c := b;` records `c`'s
    // tracked source as `b` (the immediate RHS identifier), NOT the ultimate
    // owned root `r` — a consume of `r` while `c` (but not `b`) has a later
    // use falls back to the unbounded, sound-over-rejecting default in
    // checkConsumeFreeze, rather than this window. Only the direct,
    // single-hop alias this issue's repro exercises is covered; chained
    // aliasing is left for a follow-up. // per §28.3, #4225
    std::optional<std::pair<std::uint32_t, std::uint32_t>>
    aliasBorrowConsumedWindow(const sema::Symbol* holder,
                              const sema::Symbol* consumed) const;
    // §28.3: collect the identifier-root source symbols the borrow-bearing
    // producer `e` carries (the initializer chain root, or every arg root of a
    // `new Rec(...)`) into `srcs`, excluding the local `self` itself (a
    // self-derivation `s := s.Slice(r)` yields no new root — the local keeps the
    // source it already held). // per §28.3
    void collectRecordBorrowRoots(const ast::Expr& e, const sema::Symbol* self,
                                  std::vector<const sema::Symbol*>& srcs);
    // §35.5 / §9.7 (v0.220, #2180): a case-arm payload BINDING of a borrow-
    // bearing carrier (a borrow-bearing record / span / choice payload) is "a
    // borrow whose source is the scrutinee's source" (§35.5 / §19.3). Such a
    // binding is a pattern `BindingPattern`, NOT a `LocalDecl`, so
    // collectRecordBorrowSource never records its provenance and the escape leg
    // could not resolve its carried source. This pre-pass walks every case
    // statement / expression (including control-flow-nested and arm-nested
    // ones) and records `armBinding -> scrutineeRoot` into recordBorrowLocalSource_,
    // so carriedEscapeBoundSourceLiveAt resolves the binding's source through the
    // scrutinee's OWN carried provenance (the choice local's roots, collected by
    // collectRecordBorrowSource). Runs after collectBorrowUses so the scrutinee
    // provenance is already in place. // per §35.5, §9.7 (v0.220)
    void collectCaseArmBorrowSources(
        const std::vector<std::unique_ptr<ast::Stmt>>& body);
    void collectCaseArmBorrowSourcesStmt(const ast::Stmt& s);
    void collectCaseArmBorrowSourcesExpr(const ast::Expr& e);
    void registerCaseArmBorrowSources(const std::vector<ast::CaseArm>& arms,
                                      const ast::Expr* scrutinee);
    // §28.3 (v0.186, #1726): true when the borrow-bearing-record local `recLocal`
    // transitively carries a borrow of `consumed` — a DFS over
    // recordBorrowLocalSource_ following record-local roots to a binding root.
    // Used by checkConsumeFreeze to reject a consume of `consumed` while a live
    // span over it survives. // per §28.3
    bool recordBorrowAliasesConsumed(const sema::Symbol* recLocal,
                                     const sema::Symbol* consumed) const;
    // §28.3 (#1741): the source-offset window [from, until) during which
    // `recLocal` (directly or transitively) views `consumed`, or nullopt if it
    // never does. A DIRECT source match yields that entry's precise view-window;
    // a match reached TRANSITIVELY through an intermediate record local yields
    // the coarse full range [0, MAX) (over-approximation — reassignment of an
    // intermediate span is rare and over-freezing there is the safe direction).
    // Multiple contributions are unioned to [min from, max until). // per §28.3
    std::optional<std::pair<std::uint32_t, std::uint32_t>>
    recordBorrowConsumedWindow(const sema::Symbol* recLocal,
                               const sema::Symbol* consumed) const;
    // §28.3 (#1741): like borrowHolderUsedAfter, but the surviving use must also
    // fall inside the source-view window [winFrom, winUntil) — a use of a
    // reassigned span after its old source's window has closed no longer keeps
    // that (now-retired) source live. // per §28.3
    bool borrowHolderUsedAfterInWindow(const sema::Symbol* holder,
                                       std::uint32_t consumeOffset,
                                       std::uint32_t winFrom,
                                       std::uint32_t winUntil) const;
    // Record each identifier read inside `e` into borrowUseOffsets_ (and into any
    // enclosing loop's identifier extent). `attributeOffset` != 0 forces every
    // recorded use to that source offset instead of the identifier's own — used
    // for interpolated-string holes, whose sub-parsed identifiers carry offset 0
    // (their real location is the enclosing `$"..."` node). // per §28.3
    void recordBorrowUseExpr(const ast::Expr& e,
                             std::uint32_t attributeOffset = 0);
    // Extend every loop currently on loopScanStack_ to include `off`. // per §28.3
    void noteLoopScanOffset(std::uint32_t off);
    // True when the borrow-holder `holder` has a reachable use after the consume
    // at `consumeOffset` — a use textually after it, or (loop back edge) a use
    // inside a loop body that also contains the consume. // per §28.3
    bool borrowHolderUsedAfter(const sema::Symbol* holder,
                               std::uint32_t consumeOffset) const;
    // §28.3 (v0.180) consume-site freeze: reject consuming the owned binding
    // named `consumedName` at `consumeLoc` while a LIVE local borrow of its
    // current value exists (a plain alias, a `using var` pin, or a closure that
    // captured it). The diagnostic names the consumed binding, the borrow, and
    // the borrow's surviving use. No-op when no live borrow exists (the
    // unborrowed transfer / drop-on-overwrite stays legal). // per §28.3, §28.7, §30.14
    void checkConsumeFreeze(std::string_view consumedName,
                            SourceLocation consumeLoc);
    // §28.3 / §28.7 (v0.180, #1465): true when the reassigned owned local `sym`
    // is a plain owned class/interface local whose drop-on-overwrite codegen
    // implements — so its FINAL value is safely auto-droppable at scope end.
    bool localDropOnOverwriteCovered(const sema::Symbol* sym);

    // True when `sym` is a tracked local currently in the Owned state — the
    // scope-bound discharge obligation that a borrow may not outlive. Used by
    // borrowSourceForExpr to decide whether a re-borrow / capture aliases an
    // owned source. (A parameter without `owned` enters Borrowed, never Owned,
    // so plain-parameter borrows are correctly not treated as escapes — see
    // the §9.7 param-borrow control.)
    bool isOwnedTrackedLocal(sema::Symbol* sym) const;

    // fix #4156 (§28.2 / §28.9, v0.294): true when `sym` is a tracked local
    // currently in the Disposed state — an explicit `.Dispose()` call has
    // already run and dominates this program point (the same fact
    // `checkRead`'s IdentifierExpr arm consults for its own §28.2
    // dominating-Dispose warning). Sibling of `isOwnedTrackedLocal` above.
    // Currently called ONLY from `checkIndirectEscapeOnReturn` (the
    // `return f;` bare-holder-of-a-closure escape check), to recognize that
    // a closure capturing `sym` by reference and escaping the method will
    // read the ALREADY-disposed cell on every future invocation — the gap
    // #4156 filed: capturing an owned local that is STILL Owned at the
    // return already errors (the arm just below), but a local the
    // enclosing scope explicitly disposed BEFORE the return fell through
    // silently, because it is no longer Owned and `isEscapeBoundLocal` never
    // considered Disposed a live hazard. A same-shaped extension of
    // `checkClosureEscapesOnReturn` (the direct-lambda-literal sibling) was
    // written and reverted — every probe found it pre-empted by
    // `reportMovedFromCapturesIn`'s existing #3771 Disposed leg, so that
    // function does NOT call this helper; see the investigation note beside
    // its `st.state != OwnershipState::Owned` gate. Deliberately NOT folded
    // into `isEscapeBoundLocal` itself: that predicate is shared by the
    // §9.7 span/record-borrow escape checks and the bare-type-param-call
    // escape check, whose "already disposed/moved ⇒ no live borrow to
    // dangle" reasoning (see `checkIndirectEscapeOnReturn`'s header comment)
    // is correct for THOSE shapes — a stale reference alias is a different,
    // already-diagnosed hazard (`checkRead`'s own Disposed-arm fires on the
    // re-borrowing read itself). Only a closure's capture is a standing
    // ALIAS to a SHARED, MUTATED cell whose read is deferred to an
    // unknowable future call outside this function's dominance analysis, so
    // the call site gates this check on a function-typed holder
    // specifically. // per §28.2, §28.9
    bool isDisposedTrackedLocal(sema::Symbol* sym) const;

    // Issue #392 / §31.1: true when `sym` is a catch-bound exception variable
    // (`var ex: T` in a catch arm pattern). Per §31.1 "Exception ownership" the
    // binding is a BORROW of the in-flight exception whose lifetime is the arm
    // body: it may be passed to other methods (a borrow for the call's duration)
    // and read, but may NOT be assigned to an `owned` field, returned as
    // `owned`, or "captured by a closure that may outlive the arm" — the
    // exception instance cannot escape the arm that caught it (the bare re-raise
    // `throw;` is the one allowed exception). The binding is never declared via
    // a `LocalDecl`, so it is not in `states_`; it carries no discharge
    // obligation (the runtime owns and disposes the exception), so seeding it
    // into `states_` as Owned would mis-fire the end-of-method undischarged
    // check. The escape machinery consults this predicate via `isCatchBorrow` /
    // `isEscapeBoundLocal` to treat an escape of the binding (or of a local that
    // re-borrows it) exactly as it treats an escape of an owned scope-bound
    // local.
    //
    // fix #3521 (§31.1 / §31.2.1, v0.283): reads `LocalSymbol::isCatchBinding`,
    // which sema sets where it already anchors the binding. This REPLACES a
    // per-method `catchBorrows_` set populated by a syntactic AST pre-pass
    // (`collectCatchBorrows`), the second of #3419's three spellings of this
    // classification. The pre-pass recursed only into if/while/for/do-end/try,
    // so a catch arm nested in a `case` arm body or inside a lambda body was
    // INVISIBLE to it and every §31.1 escape check silently did not fire there
    // (measured on b43196a). Reading sema's flag makes the answer independent of
    // the walker's statement-form coverage.
    bool isCatchBorrow(sema::Symbol* sym) const;

    // True when `sym` is a scope-bound local whose borrow may NOT escape this
    // method body: either an owned scope-bound local (`isOwnedTrackedLocal`)
    // or a catch-bound exception variable (`isCatchBorrow`). The escape checks
    // use this in place of `isOwnedTrackedLocal` so the catch binding's borrow
    // is rejected on every channel that already rejects an owned local's
    // borrow (store-to-field, out/ref param, multi-hop alias, closure
    // capture). // per §28.3, §31.1
    bool isEscapeBoundLocal(sema::Symbol* sym) const;

    // §28.3 / §28.9 (#861): populate `localDepth_` with each LocalDecl's
    // block-nesting depth, and (fix #3520) each catch arm's BINDING depth.
    // Called once at the start of run().
    void collectLocalDepths(
        const std::vector<std::unique_ptr<ast::Stmt>>& body);
    void collectLocalDepthsStmt(const ast::Stmt& s, int depth);

    // fix #3727 (§28.3 *Borrow lifetime rules* / §28.9, v0.286): the
    // `enclosingFrameLocalDepth_` reader. Returns the sealed enclosing frame's
    // block-nesting depth of the declaration node `declNode`, or -1 when that
    // frame does not declare it. -1 is the DECLINE answer, never a guess: a
    // for-loop induction variable (`LocalSymbol::decl` is the `ForStmt`), a
    // pattern binding (a `BindingPattern`) and an inline `out var` are not
    // `localDepth_` keys in any frame, so they read -1 here exactly as a decl
    // belonging to some other frame does — the #3707 review's lesson
    // ("`td < 0` is not proof of *declared outside*") applied to this map by
    // deciding membership POSITIVELY. // per §28.3, §28.9, v0.286
    EnclosingFrameSite enclosingFrameSiteOf(const ast::Node* declNode) const;

    // fix #3743 (§28.3 *Borrow lifetime rules*, v0.286): "does the block that
    // declares `outer` strictly enclose the block that declares `inner`?",
    // asked of two sealed frame-chain sites. A shallower FRAME wins outright (a
    // lambda body is scoped inside the block that declares the lambda, so every
    // binding of an enclosing frame outlives every binding of an inner one);
    // within ONE frame the pre-existing block-depth comparison decides. Both
    // sites must be positive hits — a miss on either declines, exactly as the
    // single-frame form did. // per §28.3, §28.9, v0.286
    static bool frameSiteStrictlyEncloses(const EnclosingFrameSite& outer,
                                          const EnclosingFrameSite& inner);

    // fix #3743: seal the frame CHAIN. Called once, from `collectLocalDepths`,
    // right after that function has sealed the frame it was handed (level 0):
    // walks the lambda bodies nested inside `body` and seals each one's own
    // `LocalDecl` depths under its lambda-nesting level. Frame membership and
    // depth are recorded by the ONE authority that already records them
    // (`collectLocalDepthsStmt`, run over each frame's statements with
    // `localDepth_` swapped aside), and the set of lambda bodies is the ONE
    // authority that already enumerates them for the escape driver
    // (`collectBodyLambdas` / `collectBodyLambdasExpr`) — so a lambda this seal
    // covers is exactly a lambda body `checkOneLambdaBodyEscapes` will check,
    // and neither the depth rule nor the lambda grammar is respelled here.
    // // per §28.3, §28.9, v0.286
    void sealEnclosingFrameChain(
        const std::vector<std::unique_ptr<ast::Stmt>>& body);
    void sealOneEnclosingFrame(
        const std::vector<std::unique_ptr<ast::Stmt>>& stmts, int frameLevel);

    // fix #3727 (§28.3 *Borrow lifetime rules* / §28.9 / §27.5, v0.286): ask
    // `ownedLocalBorrowSource` — the SAME authority, not a second spelling of
    // it — about the ENCLOSING frame rather than the lambda body currently
    // being checked, by installing that frame's owned-obligation facts in
    // `states_` for the duration of the one query and restoring them after.
    //
    // Needed because `isOwnedTrackedLocal` (the leaf `borrowSourceForExpr`'s
    // owned legs bottom out in) is a `states_` lookup, and `states_` inside
    // `EscapeScope::LambdaBody` holds ONLY the checked body's own seeded
    // obligations — so an owned local declared in the enclosing method (or an
    // enclosing lambda) resolves to NO borrow source at all and
    // `checkBorrowStoreEscape` returns before any depth comparison. Called
    // only after the ordinary, body-scoped query has already come back empty.
    // // per §28.3, §28.9, §27.5, v0.286
    //
    // fix #3744 (§28.3 *Captured owned bindings* / *Auto-drop (affine)*,
    // v0.286): `kinds` is passed straight through to `ownedLocalBorrowSource`,
    // so the retry admits exactly the source kinds its CALLING CHANNEL admits —
    // one axis, decided once, by #3746's argument rather than a second one
    // here. `LocalOrOwnedParam` additionally installs the frame's `owned`
    // PARAMETERS in the swapped-in `states_`: `enclosingFrameLocalDepth_`'s
    // keys are declaration NODES and a `ParameterSymbol::decl` is null, so a
    // frame seeded from that map alone holds no parameter and the widened
    // channel would still resolve nothing for `owned p` (measured — the
    // parameter twin of issue 3744's repro compiles rc=0 and SIGSEGVs both
    // before and after fix #3746). The `owned`-ness question is asked of
    // `symbolIsOwnedObligation`, the predicate #3746 added for exactly it.
    // // per §28.3, §28.9, §27.5, v0.286
    sema::Symbol* enclosingFrameOwnedLocalBorrowSource(const ast::Expr& rhs,
                                                       OwnedSourceKinds kinds);

    // fix #4286 (§28.3 "Ownership state of locals" / §28.6 "Who may call
    // Dispose()", v0.298): answers "is `sym` a LOCAL whose OWN declaration is
    // a fresh owned production" — a purely DECLARATION-SITE fact, usable from
    // the EARLY lambda-body pre-pass this fix's call sites run in.
    //
    // Deliberately NOT built on `enclosingFrameOwnedLocalBorrowSource` above,
    // and NOT the `enclosingFrameOwnedStates_`/`localIsOwnedObligation` seed
    // that helper uses — TWO independent, both-measured reasons:
    //  1. `localIsOwnedObligation` answers "will this declaration need a
    //     SYNTHESIZED end-of-scope auto-drop" (`ld->autoDropAtScopeEnd` /
    //     `ld->becomesOwnedViaReassign`) — both flags are set by
    //     Ownership.cpp's OWN dataflow, `autoDropAtScopeEnd` only by the
    //     end-of-method auto-drop sweep (Ownership.cpp ~line 921) that runs
    //     at the very END of `run()`. This fix's call site
    //     (`applyUsingVarBorrowPinInLambdas`, #2166) runs at the TOP of
    //     `run()` — well before that sweep, and `autoDropAtScopeEnd` is
    //     FALSE outright for a local (this issue's own repro `a`) that is
    //     later explicitly `.Dispose()`d, since an explicitly-discharged
    //     local needs no synthesized auto-drop.
    //  2. Even queried at the right TIME, `enclosingFrameOwnedLocalBorrowSource`
    //     bottoms out in `ownedLocalBorrowSource` -> `borrowSourceForExpr`,
    //     whose Shape 1 gates on `returnedExprMayBorrow` (`isBorrowableType`),
    //     which — per `disposeAliasSource`'s OWN header comment
    //     (OwnershipState.hpp) — answers FALSE for every compiler-builtin
    //     collection (Array/Dictionary/Set/LinkedList/...; a Named TypeRef
    //     with a null `declaration`, the same shape an unresolved generic
    //     parameter has). That gate is EXACTLY why `disposeAliasSource`
    //     exists as a separate field rather than a reuse of `.source` in the
    //     first place (fix #4236's own header comment).
    // MEASURED: reusing `enclosingFrameOwnedLocalBorrowSource` for this fix
    // left every one of this issue's own repro programs (`var a := new
    // Array<int>(2); ... var b := a; b.Dispose();` inside a lambda, and the
    // `using var`/`const` twins) compiling clean — the fix appeared to do
    // nothing, for reason 1 above (the `Array<int>` repro type ALSO trips
    // reason 2, independently).
    //
    // `producerState` has neither dependency — it classifies an EXPRESSION
    // (`new T(...)`, an owned-returning call, `move`, …) as Owned/Borrowed
    // purely from its AST shape and static type, available at any point.
    // It is the SAME classifier `handleLocalDecl` itself uses for its FIRST-
    // CUT classification of a local's own declaration (`lo.state =
    // producerState(*ld.initializer);`) — this helper asks that identical
    // question about the CAPTURED symbol's declaration instead. Restricted
    // to a `LocalSymbol` (an `owned` PARAMETER capture is a deliberately-
    // unclosed sibling gap — see the commit body). // per §28.3, §28.6, v0.298
    bool isOwnedTrackedLocalInEnclosingFrame(sema::Symbol* sym);

    // fix #4314 (§28.3 / §28.6 / §28.9, v0.298): sibling of
    // `isOwnedTrackedLocalInEnclosingFrame` above, for the "Pattern 2" idiom
    // the spec itself names (§K1.7/K3.11 worked example — "the same
    // declaration `var x: Stream;` leaves `x` borrowed in Pattern 1 and owned
    // in Pattern 2 — the ASSIGNMENTS decide, not the declaration"): a captured
    // local that becomes owned via a later plain REASSIGNMENT rather than its
    // own declaration initializer (`var a: Array<int>; a := new
    // Array<int>(2);`). `isOwnedTrackedLocalInEnclosingFrame` is blind to
    // this — it only ever inspects `ld->initializer`, which is null here.
    //
    // Answers via `reassignedOwnedLocals_` — a STATIC AST scan of this
    // method's own top-level `body`, computed ONCE by
    // `collectReassignedOwnedLocals` and populated BEFORE
    // `applyUsingVarBorrowPinInLambdas` runs (see the call site in `run()`).
    // Deliberately NOT `ld->becomesOwnedViaReassign` — measured (issue
    // #4314) that flag is stamped by `handleAssignment` as part of the MAIN
    // per-statement dataflow walk over the method body, which runs strictly
    // AFTER this fix's early lambda-body pre-pass; at the point this
    // predicate is consulted the flag is exactly as unset as
    // `ld->autoDropAtScopeEnd` was measured to be for #4286's own rejected
    // `enclosingFrameOwnedLocalBorrowSource` reuse — the identical
    // dataflow-timing trap, one level over. `collectReassignedOwnedLocals`
    // has no such dependency: it is a pure AST walk using `producerState`
    // (the same classifier `isOwnedTrackedLocalInEnclosingFrame` uses),
    // available at any point regardless of when it runs.
    //
    // Restricted to a `LocalSymbol` (an `owned` PARAMETER has no bare-
    // identifier reassignment shape of this kind reachable the same way —
    // out of scope here exactly as it is for #4286/#4304).
    bool isReassignedOwnedLocalInEnclosingFrame(sema::Symbol* sym);

    // fix #4314: populates `reassignedOwnedLocals_` — see that member's
    // declaration and `isReassignedOwnedLocalInEnclosingFrame` above for the
    // full rationale. A BOUNDED forward scan of `stmts` (this method's own
    // top-level body): recurses into every nested CONTROL-FLOW statement
    // list (if/elif/else, while, for, do…end, try/catch/finally, case arms —
    // the same statement-kind coverage `collectBodyLambdas` already
    // establishes as the completeness bar for this file), but deliberately
    // does NOT descend into any `LambdaExpr`'s own body: `AssignmentStmt` is
    // never embedded inside an expression in this grammar (only inside a
    // statement list), so the only way a lambda-interior reassignment could
    // reach this scan is by explicitly walking into `LambdaExpr::stmts` —
    // which this function does not do. A reassignment happening inside a
    // DIFFERENT lambda's own frame reassigning the SAME captured-by-
    // reference local is a separate, materially harder case (that lambda's
    // own frame is the one #4272's fenced reassignment arm, ~line 9518,
    // already governs) left to a follow-up issue rather than folded in here.
    void collectReassignedOwnedLocals(
        const std::vector<std::unique_ptr<ast::Stmt>>& stmts);
    // fix #4314: single-statement primitive `collectReassignedOwnedLocals`
    // above loops over — see that function's definition (Ownership.cpp) for
    // why this split mirrors `collectBodyLambdas`'s own base-case/loop shape.
    void collectReassignedOwnedLocalsStmt(const ast::Stmt& s);

    // fix #4321 (§28.3 / §28.6 / §28.9, v0.298): closes the sibling gap
    // #4314 itself documented as deferred — a captured local's DECLARATION
    // and REASSIGNMENT both live inside a DIFFERENT (outer) lambda's own
    // body, not this method's top-level statement tree, and only a lambda
    // nested inside THAT outer lambda aliases+disposes it:
    //
    //   var f := lambda() => do
    //     var a: Array<int>; a := new Array<int>(2);
    //     var inner := lambda() => do var b := a; b.Dispose(); end;
    //   end;
    //
    // `collectReassignedOwnedLocals(body)` is bounded to the METHOD's own
    // top-level `body` by design (see its declaration above) — `a`'s
    // reassignment lives inside `f`'s own `stmts`, one frame deeper, so it
    // is never found and `inner`'s alias-check retry sees an empty set.
    //
    // Called ONCE from `run()`, immediately after `collectReassignedOwnedLocals
    // (body)`, ADDITIONALLY reuses `collectBodyLambdas` (the SAME flattening
    // authority `applyUsingVarBorrowPinInLambdas` itself uses) to enumerate
    // every `LambdaExpr` reachable from `body` AT EVERY NESTING DEPTH —
    // `collectBodyLambdasExpr`'s own `LambdaExpr` case recurses into a found
    // lambda's body to collect further-nested lambdas too, so the flat list
    // already includes `inner` as well as `f`. For each lambda in that list,
    // scans ITS OWN top-level `stmts` with the UNCHANGED
    // `collectReassignedOwnedLocals` — so `f`'s own reassignment of `a` is
    // found by scanning `f->stmts` directly, and both scans feed the SAME
    // `reassignedOwnedLocals_` set `isReassignedOwnedLocalInEnclosingFrame`
    // already consults.
    //
    // SOUNDNESS OF ONE FLAT SET ACROSS FRAMES (deliberately NOT keyed per
    // lambda-frame): `reassignedOwnedLocals_` is keyed by `sema::Symbol*`,
    // one per DECLARATION, never by name. `isReassignedOwnedLocalInEnclosingFrame`
    // is only ever consulted with `rsym = ctx_.exprSymbol(*rid)` for an
    // identifier occurrence that SEMA has already resolved according to real
    // lexical scoping — so `rsym`'s own declaring scope is necessarily an
    // ANCESTOR of (or equal to) the frame the identifier occurs in; sema
    // cannot have resolved it to a symbol declared in a sibling or unrelated
    // lambda (that would be a name-resolution error, a different diagnostic
    // entirely, reported long before ownership analysis runs). So a lambda's
    // OWN locally-declared symbol can only ever be looked up from a scope
    // nested inside that lambda (itself or a lambda nested within it) —
    // membership in one flat set answers "was `rsym` reassigned to an owned
    // value anywhere within its own accessible lexical extent", which is
    // exactly the question this predicate needs answered, regardless of
    // which physical frame recorded it. No frame-keyed map is needed for
    // correctness; this mirrors #4314's own choice not to key
    // `everDischarged_` (Ownership_impl.hpp) per frame for the same reason.
    //
    // Deliberately does NOT touch `applyUsingVarBorrowPinInLambdaStmt`'s own
    // fenced `AssignmentStmt` arm (~line 9518, the #4272 mechanism) — that
    // arm updates `states_` DYNAMICALLY, scoped to the lambda currently being
    // walked, to answer a DIFFERENT, narrower question (is THIS identifier,
    // read from THIS SAME frame, currently Owned in `states_`); this function
    // is a parallel, independent STATIC fact-gathering pass that only ever
    // feeds `isReassignedOwnedLocalInEnclosingFrame`, so the two cannot
    // observe or corrupt each other's state.
    void collectReassignedOwnedLocalsInLambdaFrames(
        const std::vector<std::unique_ptr<ast::Stmt>>& body);

    // fix #4304 (§28.3 "Captured owned bindings" / §28.6, v0.298): sibling of
    // `isOwnedTrackedLocalInEnclosingFrame` above, for an `owned` PARAMETER
    // captured by a lambda body rather than a local:
    //
    //   public static method Run(owned p: Array<int>): int
    //     var f := lambda() => do var b := p; b.Dispose(); end;
    //
    // `isOwnedTrackedLocalInEnclosingFrame`'s `dynamic_cast<sema::LocalSymbol*>`
    // returns null for a `ParameterSymbol` root and answers false outright —
    // an `owned` parameter has no `LocalDecl`/initializer to classify via
    // `producerState` in the first place.
    //
    // Answers via `symbolIsOwnedObligation` (Ownership_Checks.cpp; #3746) —
    // restricted HERE to its `ParameterSymbol` arm only
    // (`dynamic_cast<sema::ParameterSymbol*>` first; never falls through to
    // its `LocalSymbol` arm). That restriction is deliberate, not
    // incidental: `symbolIsOwnedObligation`'s `LocalSymbol` arm bottoms out
    // in `localIsOwnedObligation`, which is exactly the seed #4286 measured
    // and rejected for the identical reason `isOwnedTrackedLocalInEnclosingFrame`'s
    // own declaration comment documents above (`ld->autoDropAtScopeEnd` is
    // set only at the very END of `run()`'s main dataflow, well after this
    // fix's early lambda-body pre-pass runs, and reads false regardless for
    // an explicitly-`.Dispose()`d local) — calling `symbolIsOwnedObligation`
    // unrestricted would silently reintroduce that exact dataflow-timing
    // trap for a captured LOCAL while only fixing the PARAMETER axis.
    //
    // `symbolIsOwnedObligation`'s OWN `ParameterSymbol` arm
    // (`ps->modifier == "owned"`) has no such dependency: `modifier` is a
    // plain AST/symbol-table field set during parsing/sema, available
    // unconditionally from before `run()` even begins — so restricting to
    // that one arm carries none of the timing risk the unrestricted call
    // would. `out owned` is excluded by that same arm (§28.3/§22.3: the
    // caller's frame disposes an `out owned` value, not this one), matching
    // `isOwnedTrackedLocalInEnclosingFrame`'s own `LocalSymbol`-only
    // restriction for the sibling axis. // per §28.3, §28.6, §22.3, v0.298
    bool isOwnedParameterInEnclosingFrame(sema::Symbol* sym);

    // Issue #392 / §31.1: a returned bare catch-binding identifier
    // (`return ex;`) lets the in-flight exception instance escape the arm
    // whose body the runtime disposes it after — a use-after-free. Diagnoses
    // that direct form. Indirect returns through a holder local
    // (`var b := ex; return b;`) and returned closures capturing `ex` are
    // handled by the existing escape machinery, which `isCatchBorrow` /
    // `isEscapeBoundLocal` extend to recognize the catch binding as a source.
    void checkCatchBorrowEscapesOnReturn(const ast::Expr& e);

    // §31.1 / §17.12 (issue #1569, v0.182): the `throw` operand is consumed as
    // an OWNED value transferred to the runtime's exception machinery — so a
    // BORROWED class reference is not a valid operand: a catch-arm binding
    // re-thrown (`throw ex;`), a non-`owned` parameter, a field read, or `this`
    // each still has a live owner, and letting the runtime dispose it after the
    // arm is a use-after-free / double-dispose. `ownedArgSourceIsBorrow`
    // classifies the operand exactly as for an `owned` sink; when it is a
    // borrow, this diagnoses and directs to the bare re-raise `throw;`
    // (§31.2.4) or a fresh cause-chained instance (§31.4). A `new`/call, an
    // owned local/param transferred out, and a `move`d owned value are owned
    // producers and are not flagged. Called from the ThrowStmt handler
    // (`visitStmt`) and the ThrowExpr handler (`handleDischarge`), BEFORE the
    // operand's own discharge walk. // per §31.1, §17.12
    void checkThrowOperandNotBorrowed(const ast::Expr& operand);

    // §31.1 (issue #430): scan the constructor arguments of `ne` for a catch
    // borrow in a NON-OWNED parameter position. Returns the first catch-borrow
    // symbol found, or null. Only BARE identifier arguments are examined (value
    // projections like `ex.Message` are MemberAccessExprs and are skipped).
    // Owned-parameter positions are excluded to avoid double-reporting with the
    // existing #404 `checkOwnedArgSource` check. // per §31.1, §28.3
    sema::Symbol* catchBorrowAmongNonOwnedCtorArgs(const ast::NewExpr& ne) const;

    // §31.1 (issue #430): emit a §31.1 diagnostic when the NewExpr `ne` is in
    // an ESCAPING position (throw or return from a catch arm) and
    // `catchBorrowAmongNonOwnedCtorArgs` finds a catch borrow in a non-owned-
    // param argument. The escaping constructed object may store the borrow in
    // a field, leaving a dangling reference after the arm completes. Called
    // from the throw handler (`visitStmt`) and `handleReturn`. // per §31.1
    void checkCatchBorrowEscapesViaNewExpr(const ast::NewExpr& ne,
                                           SourceLocation loc);

    // §27.5 / §28.6 (#1217): true when `sym` is a plain `ref` or `out`
    // parameter (ParameterSymbol with modifier "ref" or "out", excluding
    // "out owned" — that is an ownership-transfer parameter and is already
    // handled by the owned-parameter escape machinery). A ref/out param is
    // stored as a pointer to the CALLER's stack frame; capturing it in a
    // closure that escapes the method produces a dangling pointer. // per §27.5
    bool isRefOutParam(sema::Symbol* sym) const;

    // §27.5 / §28.6 (#1217): reject a lambda stored to a FIELD or OUT-PARAM
    // (both escaping locations that outlive the method call) when the lambda
    // captures a ref/out parameter. The captured pointer is to the caller's
    // stack frame and is valid only for the duration of the call; storing
    // the closure in a field or through an out-param lets it outlive that
    // frame — a use-after-free. Called from handleAssignment AFTER the
    // existing checkBorrowStoreEscape, so it fires ONLY when the target is a
    // field or out-param (field = any MemberAccessExpr or bare FieldSymbol
    // identifier; out-param = the "p := …" assignment through an out/ref
    // parameter that is itself the destination). // per §27.5, §28.6
    void checkRefOutLambdaCaptureOnStore(const ast::AssignmentStmt& as);

    // §9.7: true when `rt` is a record type that holds at least one
    // borrow-carrying field of a non-runtime-managed type (a class/
    // interface reference field, an inline array of reference elements,
    // or another borrow-bearing record). String/function/boxed and
    // value-typed fields do not make a record borrow-bearing. STATIC
    // fields are excluded (§9.2's "static members are ignored" exemption,
    // reused here) and delegates to isBorrowBearingRecordImpl with a fresh
    // visited set. // per §9.7, #1611
    bool isBorrowBearingRecord(const sema::TypeRef* rt) const;

    // Recursive worker for isBorrowBearingRecord (#1611). `seen` holds the
    // record TypeRefs on the CURRENT DFS path (inserted on entry, erased on
    // exit) so a self- or mutually-referential record type (e.g. a legal §9.2
    // `Origin`-style static field whose type is the enclosing record) is
    // visited at most once per path instead of recursing without bound (keyed
    // on the TypeRef, not the TypeDecl: two instantiations of one generic
    // record on a chain — `Wrap<Wrap<ArraySpan<int>>>`, #2145 — are distinct
    // subjects, while a genuinely cyclic chain revisits the SAME cached field
    // TypeRef; path — not visited-set — semantics so one field TypeRef shared
    // by SIBLING frames re-classifies under each frame's own env, the #2145
    // diamond). `env` is the §9.7/#2145 instantiation-site substitution
    // environment for a generic record's bare-parameter fields; null =
    // declaration-site behaviour. Mirrors common::isBorrowBearingRecordCommon
    // exactly (#1595 lockstep). // per §9.7
    bool isBorrowBearingRecordImpl(
        const sema::TypeRef* rt,
        std::unordered_set<const sema::TypeRef*>& seen,
        const common::RecordSubstEnv* env) const;

    // §9.7's borrow-carrying-field kinds applied to ONE resolved field type in
    // substitution environment `env` (the split-out former field lambda of
    // isBorrowBearingRecordImpl, #2145 — a substituted field type must be
    // re-classified in the environment carried by its substitution entry).
    // Also the single field predicate isNullSourceRecordExpr consults, so the
    // null-source recognizer and the classifier cannot drift. // per §9.7
    bool recordFieldIsBorrowCarrying(
        const sema::TypeRef* ft,
        std::unordered_set<const sema::TypeRef*>& seen,
        const common::RecordSubstEnv* env) const;

    // §9.7 (v0.180, #1540): true when `e` is provably the **null-source
    // form** of a borrow-bearing record — a direct `new Rec(...)`
    // construction where every argument bound to a borrow-carrying
    // positional field is a literal `null` (recursing into a nested
    // borrow-bearing record argument). This is a conservative, purely
    // syntactic recognizer: a record value read back from a local,
    // parameter, or field is never treated as null-source here even if it
    // happens to hold one at run time — matching the reject-not-trust
    // posture the field-store and collection-store rules below apply to
    // every OTHER case. Used to exempt the stdlib "sentinel" shape (e.g.
    // `CancellationToken.None`) from the otherwise-unconditional storage
    // rejection. // per §9.7
    bool isNullSourceRecordExpr(const ast::Expr& e) const;

    // §35.4 / §9.7 (v0.226, #2209): true when `e` is provably the
    // **null-source form** of a borrow-bearing choice (§35.4 with "record"
    // read as "record or choice" per §9.7's umbrella clause). A choice case
    // construction `Choice.Case(args…)` is null-source iff either the case
    // is payload-free (e.g. `Opt.None`, `Box.Empty` — no borrow to be
    // sourced) or every borrow-carrying payload argument is a literal
    // `null` (recursing into nested null-source record/choice arguments,
    // same shape as isNullSourceRecordExpr). A choice value read back from
    // a local/parameter/field is never treated as null-source even if it
    // happens to hold one at run time — matching the reject-not-trust
    // posture of the parallel record rule. Exempts the analogous stdlib
    // sentinel shape (`CancellationToken.None`, via its choice form) from
    // the otherwise-unconditional storage rejection. // per §35.4, §9.7
    bool isNullSourceChoiceExpr(const ast::Expr& e) const;

    // §30.1 / §9.7 (v0.179, #1438/#1448): true when `rt` is borrow-bearing for
    // the async-signature and across-suspension rules — a user borrow-bearing
    // record (isBorrowBearingRecord, above) OR a builtin span *view* over an
    // Array / OwningBuffer (`ArraySpan` / `ReadOnlyArraySpan`, stdlib §4.15;
    // `Span` / `ReadOnlySpan`, stdlib §4.14 — each carries a borrow of its
    // backing reference-typed store, but has no user Record TypeDecl so the
    // predicate above misses it). The stdlib-certified `CancellationToken`
    // (§30.8, the single enumerated exception) is a value builtin with no
    // Record decl and is not in the span list, so it is correctly NOT flagged;
    // likewise the string-backed span `ReadOnlyStringSpan` (string is
    // runtime-managed, §9.7). // per §30.1
    bool isBorrowBearingRecordOrSpan(const sema::TypeRef* rt) const;

    // §35.4 / §9.7 (v0.220, #2180): true when `rt` is a BORROW-BEARING choice
    // (its §35.4.2 classification — any case has a plain borrow-carrying payload
    // field). §9.7 (v0.220): "Borrow-bearing choices are subject to every rule
    // of this section, with 'record' read as 'record or choice' throughout" —
    // including the closure-capture escape bullet. isBorrowBearingRecordOrSpan
    // is false for a choice (a choice is not a record or a span view), so the
    // capture-escape legs OR this in to cover the choice CARRIER axis. Wraps the
    // shared classifier common::classifyChoiceType (the same §35.4 authority the
    // sendability, disposal-codegen, and lint paths consult), so a value-like or
    // owning choice is correctly NOT flagged here. // per §35.4, §9.7 (v0.220)
    bool isBorrowBearingChoice(const sema::TypeRef* rt) const;

    // §30.1 (v0.179, #1438): an `async` method / async iterator may not declare
    // a parameter, result, or (for an async iterator) element type that is a
    // borrow-bearing record (isBorrowBearingRecordOrSpan). An async method
    // captures its parameters into a heap state-machine frame that outlives the
    // synchronous call and lives until the task settles, so a captured borrow
    // has no stack frame whose unwinding bounds it. Runs once per body at the
    // top of run(), gated on `isAsync_`. `CancellationToken` is exempt (handled
    // by the predicate). // per §30.1
    void checkAsyncSignatureBorrowBearing(
        const std::vector<sema::Symbol*>* paramSyms,
        const ast::Type* returnType);

    // §30.1 / §29.4 (v0.179, #1448): a local of borrow-bearing-record type
    // whose live range crosses a suspension point (`await`, `await using`,
    // `await for`, or `yield return` in an iterator) is rejected unless every
    // borrow it carries has a frame-resident source — an owned local, a
    // `using var`/`await using var`, or an `owned` parameter of the same body
    // (locations the state-machine frame itself owns, which therefore survive
    // every suspension). A caller-side source (a plain parameter, `this`, a
    // field through `this`, or an enclosing capture) does not qualify: its
    // owner may dispose it while the frame is suspended. Runs a source-order
    // walk over the body; gated on `isAsync_ || sawYield_`. // per §30.1, §29.4
    void checkBorrowBearingLocalsAcrossSuspension(
        const std::vector<std::unique_ptr<ast::Stmt>>& body);

    // Info recorded at a borrow-bearing-record local's declaration for the
    // across-suspension check. `frameResident` is computed once from the
    // initializer's borrow source (an owned/using/owned-param source survives
    // suspension). // per §30.1
    struct SuspCandidate {
        std::string_view name;
        bool frameResident = false;
    };
    // Helpers for checkBorrowBearingLocalsAcrossSuspension. `walkSuspStmts`
    // takes raw statement pointers so a nested body, a `do…end` block, or a
    // single catch-arm body can all be walked through the one function.
    void walkSuspStmts(
        const std::vector<const ast::Stmt*>& stmts,
        std::unordered_map<const sema::Symbol*, const ast::Node*>& crossed);
    void reportSuspCross(const sema::Symbol* sym, const ast::Node* suspNode);
    // §30.7 (v0.184, #1677): report an `await using var` binding held live
    // across a `yield return` whose static type has no synchronous disposal
    // fallback (a no-destructor IAsyncDisposable class such as `TaskScope`, or
    // an interface type). Emitted at the yield, naming the binding. // per §30.7
    void reportAwaitUsingYield(std::string_view name,
                               const ast::Node* yieldNode);
    // True when `init`'s borrow source is a frame-resident owner (per §30.1).
    bool suspSourceIsFrameResident(const ast::Expr& init);
    // Collect the identifiers read anywhere in `e` (no descent into lambdas).
    void collectReadIdentifiers(
        const ast::Expr* e,
        std::vector<const ast::IdentifierExpr*>& out) const;
    // True when `e` (recursively, no lambda descent) contains an `await`.
    bool exprContainsAwait(const ast::Expr* e) const;
    // True when any statement in `stmts` (recursively, incl. nested blocks) is
    // a suspension point: an `await` expr, an `await using` decl, an `await
    // for` loop, or a `yield return`. // per §30.1, §29.4
    bool blockContainsSuspension(
        const std::vector<std::unique_ptr<ast::Stmt>>& stmts) const;
    // The per-body candidate map + de-dup set for the across-suspension walk.
    std::unordered_map<const sema::Symbol*, SuspCandidate> suspCands_;
    std::unordered_set<const sema::Symbol*> suspInScope_;
    std::unordered_set<const sema::Symbol*> suspReported_;
    // §30.7 (v0.184, #1677): `await using var` bindings currently in scope whose
    // static type has no synchronous disposal fallback (symbol → binding name),
    // and the per-binding de-dup set for the across-`yield return` reject. // per §30.7
    std::unordered_map<const sema::Symbol*, std::string_view>
        awaitUsingYieldInScope_;
    std::unordered_set<const sema::Symbol*> awaitUsingYieldReported_;

    // Find the identifier at the root of a chain like `x.f.g[i].M(...)`.
    // Returns null if the root isn't a bare identifier.
    const ast::IdentifierExpr* identifierRoot(const ast::Expr& e);

    // §28.3 join. Per the spec the join is asymmetric: matching states
    // carry forward (with their auxiliary data, e.g. Owned's discharge
    // obligation), but mismatched owned-vs-borrowed is rejected (the
    // spec's "silently demoting an owned obligation to a borrowed state
    // at the join would leak the resource"). Mutates `into` in place.
    //
    // `emitClash` controls whether the owned-vs-borrowed mismatch reports
    // a diagnostic. It is true for the CFG merge-block meet (reproducing
    // the old branch-join diagnostic) and false for the synthetic meet at
    // the function exit used only to derive the end-of-method
    // undischarged-owned state (the old walker never met at exit, so
    // emitting there would introduce a new diagnostic — O3a forbids that).
    //
    // Slice O3b: `unionKeys` controls the domain. When false (the if/elif/
    // else merge), only locals present in `into` are met — a local declared
    // inside one branch is out of scope after the merge and is dropped.
    // When true (the loop back-edge and zero-trip skip merges), locals
    // present in only one operand are carried into the result, preserving a
    // §28.3 discharge obligation that exists on one incoming path but not
    // the other (the zero-trip leak case). The carried-in state is the
    // operand's state met against "absent" — Owned/Uninitialized survive so
    // the end-of-method / read checks still see the hazard.
    void joinInto(std::unordered_map<sema::Symbol*, LocalOwnership>& into,
                  const std::unordered_map<sema::Symbol*,
                                           LocalOwnership>& other,
                  SourceLocation joinLoc, bool emitClash = true,
                  bool unionKeys = false);

    // Slice O3a/O3b CFG driver: run the forward dataflow over `cfg`,
    // seeding the entry block with `entry`. Loops are iterated to a
    // fixpoint (O3b): the back-edge and the zero-trip header→after skip
    // edge participate in the meet, so the analysis is sound for the
    // never-executed loop path (§28). Returns the meet of all states
    // reaching the CFG exit (used as the end-of-method undischarged-owned
    // check). Diagnostics for reads/moves/disposals and branch-join
    // clashes are emitted on the single final (non-suppressed) pass.
    //
    // §28.8.1 (#867): the returned FlowState also carries the field-moved-from
    // facts merged over every path reaching exit (OR-over-paths), which run()
    // reads for the field-restoration check.
    FlowState runDataflow(const flow::CFG& cfg, const FlowState& entry);

    // §28.8.1 (#867): merge `other`'s field-moved-from facts into `into` with
    // OR-over-predecessors — a field moved-from on ANY incoming path is
    // moved-from at the join (restored only when restored on ALL paths). A
    // field absent from one operand is treated as RESTORED (false) on that
    // path: a field that was never moved on a path is not moved-from there.
    void joinFieldsInto(FieldMovedFrom& into, const FieldMovedFrom& other);

    // Process one CFG block's straight-line statements, mutating
    // `states_` (which the caller has set to the block's entry state).
    void processBlock(const flow::CFGBlock& block);

    // fix #3591 (§21.1, v0.286): widened from `const ast::LocalDecl&` to
    // `const ast::Node&` — the body only ever compares `ls->decl == &ld` by
    // pointer identity, never reads a LocalDecl-specific field, so admitting
    // a `LocalConstDecl` (or any other decl-anchor node, e.g. a `BindClause`)
    // is safe by construction; every existing call site passes a
    // `LocalDecl&`, which upcasts implicitly, so this is source-compatible.
    sema::LocalSymbol* findLocalSymbol(const ast::Node& ld);

    // §29.2 (#834): seed the iteration variable of `fs` as Owned at the loop
    // body entry when its iterable resolves to an owning enumerable, so the
    // §28.3 discharge check rejects an undischarged iteration variable.
    void seedOwnedIterVar(const ast::ForStmt& fs);

    // §29.2 (#834): true when `fs`'s iterable has static type
    // `IOwningEnumerable<T>` / `IOwningAsyncEnumerable<T>` (owning iteration).
    bool iterableIsOwningEnumerable(const ast::ForStmt& fs);

    // §29.2 (#834): the LocalSymbol ExprTypecheck created for `fs`'s iteration
    // variable (its `decl` is `&fs`); null when none exists.
    sema::LocalSymbol* findForLoopVarSymbol(const ast::ForStmt& fs);

    LocalOwnership* lookupState(std::string_view name);

    void markDisposedByName(std::string_view name, SourceLocation loc);

    // fix #4246 (§28.3 / §28.6, v0.298): stamp `id` (an explicit
    // `.Dispose()`/`?.Dispose()` receiver) with `disposeReclaimsReassigned-
    // Borrow` when the dataflow's OWN current state for this local is
    // Owned — MUST be called before the caller transitions that state via
    // `markDisposedByName` (this reads `states_` as it stands at the call
    // site, not after). See IdentifierExpr's field comment for why this is
    // a per-occurrence fact rather than a per-declaration one.
    void markExplicitDisposeReclaimIfReassignedBorrow(
        const ast::IdentifierExpr& id);

    void markMovedByName(std::string_view name, SourceLocation loc);

    // §22.3 (v0.169, #1348): true when `e` is a NAMED OWNED BINDING — a bare
    // identifier (other than `this`/`base`) resolving to a tracked owned local
    // or `owned` parameter currently in the Owned state, so passing it into an
    // `owned` sink leaves the binding moved-from. This is the SOLE argument
    // form on which the call-site `owned` marker (`M(owned x)`) is REQUIRED;
    // every other owned-value form (a `new` expression, a call returning
    // `owned T`, an operator result, a `move obj.f`) is a *fresh producer* that
    // invalidates no live binding and stays unmarked. Reuses the existing
    // moved-from state tracking rather than re-classifying the expression.
    // fix #3751: see setEnclosingCtor above.
    const sema::ConstructorSymbol* enclosingCtor_ = nullptr;

    bool isNamedOwnedBinding(const ast::Expr& e);

    // §22.3 (v0.169, #1348): report the "missing marker" error — a named owned
    // binding `name` is being transferred into an `owned` sink without the
    // required call-site `owned` marker. Emits the spec-stated fix-it.
    void reportMissingOwnedMarker(std::string_view name, SourceLocation loc);

    // §22.3 (v0.169, #1348): report a "spurious marker" error — an `owned`
    // call-site marker written where it is rejected. `onFreshProducer` selects
    // the fresh-producer wording (`new`/owned-returning call/operator/`move
    // obj.f`); otherwise the non-`owned`-parameter-position wording.
    void reportSpuriousOwnedMarker(SourceLocation loc, bool onFreshProducer);

    // §28.3 / §28.10 (v0.180, #1962): validate and record the binding-position
    // `owned` transfer marker — `var y := owned x;` / `using var y := owned x;`.
    // The marked initializer must be a NAMED OWNED BINDING (an owned local or
    // `owned` parameter currently Owned); when it is, the source is marked
    // moved-from (the binding takes over the disposal obligation). The marker on a
    // borrow, a fresh producer, or any other right-hand side is rejected with a
    // spec-stated diagnostic — mirroring the call-site marker's rules (§22.3).
    // Called from handleLocalDecl for either `var` or `using var`.
    void checkAndMoveBindingOwnedMarker(const ast::Expr& init);

    // §28.3 / §28.10 (v0.180, #1962): report a rejected binding-position `owned`
    // marker. `onFreshProducer` selects the fresh-producer wording (`new`,
    // owned-returning call, operator, `move obj.f`); otherwise the borrow /
    // not-a-named-owned-binding wording.
    void reportBindingOwnedMarkerRejected(SourceLocation loc, bool onFreshProducer);

    // §28.8.1 (#897 #898): record a `move this.field` / `move _field` (qualified
    // OR bare owned-field move). This is the field analogue of markMovedByName
    // for locals: BEFORE marking the field moved-from, it CHECKS the current
    // (path-sensitive, #867) `fieldMovedFrom_` fact — if the field is already
    // moved-from on the path reaching this move site, the move has no ownership
    // to transfer and is a use-after-move; report a §28.8.1 error. Otherwise
    // mark the field moved-from and record the move-site location. The check
    // routes through `report()` so the loop-fixpoint suppression (#867) emits
    // each diagnostic exactly once on the converged final pass — so a loop whose
    // body moves the field but restores it only conditionally is rejected at the
    // move site on the moved-from back-edge path (#898), while a move-restore-
    // move sequence and a loop with unconditional restore are accepted.
    void recordFieldMove(std::string_view name, SourceLocation loc);

    // §28.8.1 / §28.8 (issue #1999, extended #2005): in the bare-statement
    // discharge walk (handleDischarge), reject `M(move recv.f!)` / `new
    // T(move recv.f)` where the move source is an owned field `f` of a NON-`this`
    // receiver `recv`. Two receiver shapes are covered, each with its own tailored
    // diagnostic: (a) a STABLE IDENTIFIER receiver — an owned local / `using var` /
    // owned parameter, or a borrow of a class instance (#1999); (b) a TEMPORARY
    // receiver — a call result / fresh value not bound to a stable identifier
    // (`getBox().f`, #2005). `ma` is the peeled owned-field read `recv.f`; `loc`
    // is the move site. Returns true (arg now handled — the caller sets `consumed`)
    // if a diagnostic fired, false if the shape is not a non-`this` owned-field
    // move (the caller falls through to its normal handling).
    //
    // Why reject rather than consume: the SOLE runtime null-after-move of the
    // field slot (LlvmBackend lowerUnary) and the field-restoration tracking
    // (`fieldMovedFrom_`, keyed by field name) are BOTH `this`-only. For a
    // non-`this` IDENTIFIER receiver the field is left populated at runtime and
    // never tracked, so after the move the sink adopts and disposes the value AND
    // `recv`'s own disposal (scope-end for a `using var`/owned local, the
    // constructed owner for a moved-out owned local, the true owner for a borrow)
    // disposes it again — a double-free / heap-use-after-free. This mirrors the
    // `this` arm's net effect through a different mechanism: `move this.f!` in
    // this same argument position is ALSO rejected — `recordFieldMove` marks the
    // field moved-from and the method-exit field-restoration check (§28.8.1)
    // rejects, because an argument-position move cannot restore the field before
    // the (throwable) call returns (§28.8.1 "WrongOrder"). The sound form is the
    // spec's "build then take then restore": `var t := move recv.f!; recv.f := …;
    // M(owned t)`, which routes through recordMovesIn, not this walk.
    //
    // For a TEMPORARY receiver (#2005) rejection is the ONLY sound outcome: the
    // §28.8.1 field-restoration rule cannot be satisfied (no stable place to
    // restore into — a second `getBox()` call would mint a fresh Box), AND the
    // temporary's own disposal path does not account for the field-move (codegen
    // emits no cleanup for the temporary after extracting its field), so the
    // temporary itself leaks — the 24-byte LSan hit on base (issue #2005). The
    // sound remedy is to bind the producer to a stable place first and use a
    // take-method (§28.6 / §23). // per §28.8.1, §28.8, §28.6
    bool rejectNonThisOwnedFieldMoveDischarge(const ast::MemberAccessExpr& ma,
                                              SourceLocation loc);

    // §28.3 / §28.11 (issue #860): mark a tracked local Owned because it
    // received ownership through an `out owned` argument. `origin` is the
    // AST node the dropped-without-disposal diagnostic points at. // per §28.3
    void markOwnedByName(std::string_view name, SourceLocation loc,
                         const ast::Node* origin);

    // §28.8 owned-sink check (issue #395): true when `argValue` — an
    // argument destined for an `owned` parameter, or the RHS of an
    // owning-collection `set(owned)` indexer assignment — denotes a *borrow*
    // rather than an owned value. Passing a borrow into an `owned` sink lets
    // the sink dispose a value it does not own (a double-dispose with the
    // value's real owner), so it must be rejected.
    //
    // The borrow forms flagged are: a tracked local currently in the Borrowed
    // state; a bare identifier resolving to a non-`owned` parameter (a borrowed
    // parameter, whose source is the caller's frame); and a *bare* read of an
    // `owned` field — `obj.f` / a bare `f` resolving to an owned field — which
    // §28.3 classifies as Borrowed (source: `obj`), the only owned-field read
    // that yields an owned value being `move obj.f` (§28.8.1). A leading `move`
    // is unwrapped first — `move b` of a borrow is no more an owned value than
    // the bare `b`, while `move obj.f` IS the legal owned producer (§28.3 /
    // §28.8.1) and so is NOT flagged: the owned-field branch only fires when no
    // enclosing `move` wrapped the read (issue #398). Every owned-value form
    // (`new T(...)`, a call returning `owned T`, a `move` of an owned local /
    // owned field / owned slot, a bare owned local) is NOT flagged, so legal
    // §28.8 transfers are never over-rejected. Anything the analysis cannot
    // classify (e.g. `this`, an unresolved symbol, a non-`owned` field) is
    // conservatively treated as NOT a borrow, so it is never falsely rejected.
    // // per §28.8, §28.3, §28.8.1
    bool ownedArgSourceIsBorrow(const ast::Expr& argValue);

    // fix #4042 / #4057 (§21.1 / §28.3 / §28.5 / §28.11, stdlib §5/§4.4/§9.6):
    // true iff `name` is a tracked local currently Borrowed whose LocalDecl
    // initializer is DIRECTLY one of the nine builtin owned-materializer
    // calls — the ONE fact `ownedArgSourceIsBorrow`'s #4042 carve-out and
    // `handleOwnedFieldAssignment`'s #4057 consume-fix both need, kept as a
    // single shared predicate so they cannot drift on the shape (the #6
    // dominant-defect-generator discipline). `ownedArgSourceIsBorrow` uses it
    // to answer "not a borrow" for the classification question;
    // `handleOwnedFieldAssignment` uses it to ALSO mark the source consumed
    // when it accepts such a local, exactly as it already does for a
    // genuinely-Owned source — closing #4057 (measured double-free: two
    // `owned` fields, or an owned field and an owning-array-slot store,
    // could each independently accept the SAME materializer-bound local
    // because nothing marked it moved-from after the first accept, since
    // `producerState`/`states_` deliberately still say Borrowed — see
    // `ownedArgSourceIsBorrow`'s own header comment for why that must stay
    // true). // per §21.1, §28.3, §28.5, §28.11, stdlib §5/§4.4/§9.6, #4042, #4057
    bool isMaterializerCarveOutBorrow(std::string_view name);

    // §28.3 (issue #398): true when `e` is a read of an `owned` field — either
    // an explicit member access `obj.f` whose member resolves to an owned
    // FieldSymbol, or a bare identifier resolving to an owned field of the
    // declaring class (implicit `this`). A bare such read is Borrowed per the
    // §28.3 producers table ("Field access `obj.f` where `f` is `owned` |
    // Borrowed (source: `obj`)"); only `move obj.f` reads it as an owned value
    // (§28.8.1). Used by `ownedArgSourceIsBorrow` to reject a bare owned-field
    // read flowing into an owned sink. // per §28.3, §28.8.1
    bool exprIsOwnedFieldRead(const ast::Expr& e) const;

    // §28.6 / §28.8.1 (issue #2011): does the RECEIVER of an owned-field read
    // `recv.f` (given an `e` that `exprIsOwnedFieldRead` already accepted) denote
    // a BORROW rather than an owned place? `move recv.f` reads an owned field as
    // an owned value ONLY when `recv` is an owned place — the class instance
    // itself (`this`, which owns its fields) or an owned local / using-var /
    // `owned` parameter this scope owns (§28.8.1). A BORROW receiver (a non-
    // `owned` parameter, a catch-bound exception §31.1, a Borrowed-state local, a
    // record-binding local, or another field read — field reads are borrows §28.3)
    // does NOT own the object, so it cannot transfer the object's owned fields
    // out: the field's real owner disposes the value and the owned sink would
    // dispose it a second time — a double-free (§28.6 "a borrow does not own the
    // object"; §28.8.1 owned-field move). Positive-rejection only (the discipline
    // of `ownedArgSourceIsBorrow`): returns true ONLY when the receiver is
    // provably a borrow — `this`, an owned place, or an unclassifiable receiver
    // (a temporary — a separate deferred case) returns false so a legal `move
    // this.f` / `move _f` / `move ownedBox.f!` is never over-rejected.
    // // per §28.6, §28.8.1
    bool ownedFieldMoveReceiverIsBorrow(const ast::Expr& fieldRead);

    // §28.6 / §5.11 (#1493): true when `e` is an `owned`-field read AND the
    // receiver is `this` — a bare identifier (implicit `this.f`) or an
    // explicit `this.f`, never another object's field. This is the exact
    // shape `checkBorrowReturnedThroughOwningType`'s owning-WRAPPER branch
    // admits as a borrow (mirroring the owning-collection branch's identical
    // receiver restriction, inlined there); factored out here so the caller-
    // side classifier below (`methodReturnsOnlyWrapperReceiverBorrow`) can
    // reuse the exact same test. // per §28.6, §28.3, §5.11
    bool exprIsReceiverOwnedFieldBorrow(const ast::Expr& e) const;

    // §28.6 / §5.11 (#1493): does every reachable `return` under `s` return
    // the wrapper-receiver-field-borrow shape above? Sets `sawReturn` when at
    // least one `return <expr>;` was visited, and clears `allBorrow` on the
    // first one that is `return;` (no value) or returns anything else. Does
    // NOT descend into nested closures — a lambda's `return` belongs to the
    // lambda, not the enclosing method. Statement coverage mirrors
    // `moveOnlyArrayLeakScanStmt`'s control-flow walk (§17 statement forms
    // that can carry a nested `return`). // per §28.6, §28.3, §5.11
    void scanReturnsForWrapperReceiverBorrow(const ast::Stmt* s,
                                              bool& sawReturn,
                                              bool& allBorrow) const;

    // §28.6 / §5.11 (#1493): true when EVERY return path of `md`'s body is the
    // wrapper-receiver-field-borrow shape (an expression body counts as its
    // single implicit return). `checkBorrowReturnedThroughOwningType`
    // guarantees this is the ONLY borrow shape it admits for a non-`owned`
    // owning-WRAPPER return type (Shared, Weak, ChannelSender, OwningTask,
    // IOwningEnumerable, CancellationTokenSource) — every other admitted
    // return there (`new`, `move`, an owned-returning call, a transferred
    // owned local/param) is a genuine owned producer the caller must still
    // discharge. So "every return matches" is a sound, purely structural test
    // for classifying a CALL to `md` as Borrowed rather than the wrapper's
    // default Owned (consumer-owns) classification in `producerState` below —
    // without it, a legitimate borrow-return (the shape #1493 fixed at the
    // definition site) was still tracked Owned at every call site, triggering
    // a spurious §28.3 auto-drop warning (memory-safe only by Dispose()'s
    // idempotency, §28.2 — not a double-free, but a wrong classification). A
    // body with no return in the walk is conservatively NOT classified
    // Borrowed (returns false). // per §28.6, §28.3, §5.11
    bool methodReturnsOnlyWrapperReceiverBorrow(const ast::MethodDecl& md) const;

    // §28.10 / §28.6 / §26.7 (#2137): true when `e` is a RECEIVER-SOURCED
    // borrow return shape — a bare `this` (§28.6 "Methods returning `this`";
    // in an extension body `this` IS the receiver-parameter borrow, §26.7), a
    // bare-identifier FIELD read (implicit `this.f`, or a static getter's bare
    // `static owned` field read — §28.3 v0.196 static-storage-sourced borrow),
    // or a `this`-rooted field-read chain (each hop a FieldSymbol). The
    // superset of exprIsReceiverOwnedFieldBorrow: NON-owned field reads are
    // borrows of their storage too (§28.3 producers), so they pin the same
    // way. // per §28.10, §28.6, §26.7, §28.3, #2137
    bool exprIsReceiverSourcedBorrowReturn(const ast::Expr& e) const;

    // #2137: the receiver-sourced twin of scanReturnsForWrapperReceiverBorrow —
    // same statement walk, `exprIsReceiverSourcedBorrowReturn` as the per-
    // return accept test. // per §28.10, §28.6, #2137
    void scanReturnsForReceiverSourcedBorrow(const ast::Stmt* s,
                                             bool& sawReturn,
                                             bool& allBorrow) const;

    // §28.10 / §28.6 / §26.7 (#2137): true when EVERY return path of `md`'s
    // body is a receiver-sourced borrow (bare `this`, or a receiver-rooted
    // field-read chain — exprIsReceiverSourcedBorrowReturn). The WIDER,
    // call-site-pin-only sibling of methodReturnsOnlyWrapperReceiverBorrow
    // (which stays the narrower test producerState's owning-type-head and
    // task-borrow classifications rely on): every accepted shape is a borrow
    // whose source is the call's receiver, exactly the §28.3 producers-row
    // classification for a non-`owned` instance/extension call. A body with no
    // scanned return is conservatively NOT recognized. // per §28.10, §28.6,
    // §26.7, #2137
    bool methodReturnsOnlyReceiverSourcedBorrows(const ast::MethodDecl& md)
        const;

    // §28.10 / §28.3 / §28.6 (#2137): true when a USER property declaration's
    // GETTER provably produces a receiver-/static-storage-sourced borrow:
    //   - expression-bodied (`=> _w`) — the body expr passes
    //     exprIsReceiverSourcedBorrowReturn;
    //   - block form — the `get` accessor's expression body passes the same
    //     test, or its `do … end` body's every return does (the scanner);
    //   - an initializer-LESS auto-property — the generated getter is a
    //     backing-field read, a borrow of storage per §28.3's producers row.
    //     An auto-property WITH an initializer is NOT recognized (who owns an
    //     initializer-minted class value is its own open defect — #2168 —
    //     so its read keeps today's adoption).
    // Abstract accessors (interface properties) and builtin properties have no
    // scannable body and are not recognized (adoption, as before). // per
    // §28.10, §28.3, §28.6, #2137
    bool propertyGetterIsRecognizedBorrow(const ast::PropertyDecl& pd) const;
    // §28.10 / §28.3 / §28.6 / #2137-residual: body-level predicate called
    // from propertyGetterIsRecognizedBorrow ONLY for the Task-family return
    // carve. A Task-family getter's body determines whether it MINTS a fresh
    // owned frame (a Task.Run(...) etc. — §28.3 owned-mint; the call's using
    // var adopts and disposes the task) or BORROWS an external task frame (a
    // receiver-sourced field read of a Task-typed field, a Drain alias, a
    // block whose every return is receiver-sourced). For the borrow-pin
    // decision we admit the borrow-source bodies only; mint-source bodies
    // keep adoption. // per §28.10, §28.3, §28.6, #2137
    bool propertyGetterBodyScan(const ast::PropertyDecl& pd) const;
    // §28.3 / #2137-residual: true when the property's declared return type
    // is a Task-family head (`Task` / `OwningTask`). Mirrors
    // callResultIsAsyncTask's narrowing for the AST type node form, applied
    // to the property's return type. // per §28.3, #2137
    bool propertyReturnTypeIsTaskFamily(const ast::PropertyDecl& pd) const;
    // #2175(b) / stdlib §4.5 v0.195: true when `e` is the BUILTIN `recv.Drain()`
    // (no user MethodSymbol) over a receiver whose chain terminates at
    // `this` (or a field of `this`). Used by propertyGetterIsRecognizedBorrow
    // to admit `property Q: IOwningEnumerable<R> => _items.Drain();` as a
    // borrow-pin: the view aliases the receiver's array (§4.5 v0.195), the
    // receiver is the getter's `this` (outlives the call), and the caller's
    // `using var q := h.Q;` correctly becomes a §28.10 borrow-pin. A user
    // `Drain` method, or a non-owning-collection receiver, is not the
    // alias and falls through. // per §28.10, §28.3, stdlib §4.5 v0.195, #2175
    bool getterBodyIsDrainAliasOverReceiverSourced(
        const ast::Expr& e) const;

    // §28.10 "using var initialized from a borrow" (#2128, extended #2137/
    // #2146): true when a `using var` initializer is one of the borrow shapes
    // the spec lists there beyond the bare-id / arm-meet / bind-atom /
    // bare-T-call shapes handled inline in handleLocalDecl — "a method call
    // without an `owned` return, an indexer access, or a field read." Such a
    // using var is a BORROW-PIN (no disposal obligation; its real owner
    // disposes it), so codegen must skip the scope-end dispose
    // (registerUsingDisposal honors usingVarSourceIsBorrow). Absent this, a
    // TYPED binding over one of these reaches classOfLocal (declared class →
    // unconditional dispose) and DOUBLE-FREES the borrow. Recognized
    // POSITIVELY (never off producerState's unreliable `Borrowed` verdict, which
    // also covers fresh-owned producers — collection literals, `RawBytes.From`,
    // iterator/subscription returns — that MUST keep adoption):
    //   - a FIELD read (obj.f / bare implicit-this f resolving to a FieldSymbol)
    //     — a lend (#2128);
    //   - a USER borrow-returning PROPERTY getter read — getter-body-scanned;
    //     §28.6 guarantees user getters are borrow-producing positions (#2137);
    //   - an INDEXER access (a bare IndexExpr) — a borrow out, source the
    //     collection (§28.11 element-slot table); `move coll[i]` / Pop are a
    //     UnaryExpr / call, not a bare IndexExpr, and keep adoption (#2128);
    //   - a concrete borrow-returning CALL — a directly-resolved user method
    //     whose declared return is not `owned` and whose EVERY return is a
    //     receiver-sourced borrow (`this`, or a receiver-rooted field read —
    //     methodReturnsOnlyReceiverSourcedBorrows) (#2128, widened #2137);
    //   - the builtin owning-collection Drain() ALIAS over a stable named
    //     receiver — stdlib §4.5 v0.195: the view IS the receiver; its owner
    //     keeps the one container obligation (#2146). // per §28.10, #2128,
    //     #2137, #2146
    bool usingVarInitIsRecognizedBorrow(const ast::Expr& init) const;

    // §28.8: report the owned-sink violation for `argValue` (at `loc`) when
    // `ownedArgSourceIsBorrow` holds. Returns true when it reported (the
    // caller then skips the bogus `markMovedByName` that would silently treat
    // the borrow as consumed). // per §28.8
    bool checkOwnedArgSource(const ast::Expr& argValue, SourceLocation loc);

    // §28.6 / §28.8 / §28.8.1 (issue #1995): reject `return move b` / `return
    // move b!` (and the `=>` expression-body form) of a BORROW returned through
    // an `owned` return type. The return position never ran the §28.8 owned-
    // source classifier — only the borrow-of-scope-local lifetime checks lived
    // there — so a `move`d borrow (a borrowed local/parameter, a catch-bound
    // exception §31.1, a Borrowed-state local, a non-`owned` field) was returned
    // as if owned: the caller disposes it while the borrow's real owner disposes
    // it too (a double-free). Reuses the exact ARGUMENT-sink predicate
    // (checkOwnedArgSource / ownedArgSourceIsBorrow, #1993). A no-op unless the
    // enclosing method's return type is `owned` (returnOwned_) and `e` is `move
    // <place>` — a plain/borrow return type is not the double-dispose hazard (the
    // caller does not dispose a returned borrow), and a non-`move` owned return (a
    // fresh producer, a bare-identifier discharge) is untouched. A `move`d owned-
    // FIELD read (`move this.f` / `move _f` / `move recv.f!`) is the legal owned-
    // field transfer (§28.8.1) and is excluded (its field-restoration obligation
    // is enforced separately). Returns true when it reported. // per §28.6, §28.8, §28.8.1
    bool checkOwnedReturnBorrowSource(const ast::Expr& e);

    // §28.3 *Borrow lifetime rules* / §28.6 *Methods returning `this`* (issue
    // #3993): the DIRECT-READ sibling of checkOwnedReturnBorrowSource. §28.3's
    // first borrow-lifetime bullet is unconditional — "A borrowed local may not
    // be returned from a method whose return type is `owned`" — but the only
    // callee-side enforcement of it is scoped to a `move E` operand
    // (checkOwnedReturnBorrowSource) or a bare OWNED-field read
    // (checkOwnedFieldReturn, §28.6 bullet 2); neither covers a bare non-move
    // read whose source is a plain BORROW — a borrowed parameter (`return b;`,
    // the spec's own `static method Pick(a: Res, b: Res): Res return a;`
    // example with `owned` added to the return type instead of left off), a bare
    // `this` (§28.6 *Methods returning `this`*: "may not be ... returned as
    // `owned` — it is the receiver under a different name"), a non-owned field
    // read, a catch-bound exception, or an indexer-get. handleReturn's bare-
    // identifier discharge fast-path (§28.3, Slice O1) unconditionally marks
    // such a return discharged with no regard to the declared return type, so
    // `static method Pick(a: Res, b: Res): owned Res return b;` compiled with
    // zero diagnostics and double-freed at runtime — `b`'s real owner (the
    // caller's `using var`) disposes it, and the caller-side `owned` contract
    // disposes the same object again. Reuses the exact §28.8 owned-source
    // classifier (`ownedArgSourceIsBorrow`) the sibling checks already use — the
    // single existing authority for "is this expression a borrow" — rather than
    // a second spelling. Excludes `move E` (checkOwnedReturnBorrowSource's job,
    // with tailored field-vs-general-borrow messaging) and a bare owned-field
    // read (checkOwnedFieldReturn's job, with the §28.6-bullet-2 "use `return
    // move <field>;`" remedy) so each borrow shape is diagnosed exactly once.
    // Fires from handleReturn (block bodies, BEFORE the bare-identifier fast
    // path swallows the return) and runExprBodyOwningReturnCheck (`=>` bodies).
    // // per §28.3 *Borrow lifetime rules*, §28.6 *Methods returning `this`*, §28.8
    void checkOwnedReturnDirectBorrow(const ast::Expr& e);

    // §28.8 / §28.8.1 / §28.6 (issues #2010, #2016): the owned-sink borrow-source
    // guard for a `move <place>` ARGUMENT to a ctor/call `owned` formal in the
    // LOCAL-DECL-initializer / assignment-RHS arg-move path (recordMovesIn /
    // recordMovesInArgs). This is the last owned-sink position that never ran the
    // §28.8 owned-source classifier: handleDischarge (the bare-statement discharge
    // walk, #1993/#1999) and checkOwnedReturnBorrowSource (the return position,
    // #1995/#2011) both guard it, but `var h := new Holder(move b)` / `var r :=
    // C.Sink(move recv.f!)` routed only through recordMovesIn, which marked the
    // source moved-from with no borrow check — so a `move` of a borrow flowed
    // silently into the owned sink and the value was disposed twice.
    //
    // Returns true — and reports (the §28.8 "borrow cannot be passed" diagnostic)
    // — when the moved place is NOT an owned value:
    //   * a BORROW source (`move b` / `move b!` of a borrowed local/parameter, a
    //     catch binding, or a Borrowed-state local — #2010), or
    //   * an owned field of a BORROW RECEIVER (`move recv.f` / `move recv.f!`
    //     where `recv` is a borrow — a borrower does not own the object and cannot
    //     move its owned fields out; the field's real owner disposes the value and
    //     the owned sink disposes it a second time, §28.6 — #2016).
    // Returns false — the caller records the move — for every legal owned source:
    // an owned local, an owned producer, or a `move` of an owned field whose
    // RECEIVER is an owned place (`move this.f` / `move _f` / `move ownedBox.f!`).
    // The owned-field-of-owned-receiver acceptance is checked directly via
    // `ownedFieldMoveReceiverIsBorrow` (peeling a single `!`) so BOTH the `!` and
    // non-`!` member-access spellings are classified — `ownedArgSourceIsBorrow`'s
    // owned-field branch is `move`-gated and reaches the receiver test only through
    // the `!` postfix form. // per §28.8, §28.8.1, §28.6
    bool checkOwnedArgMoveSource(const ast::UnaryExpr& moveExpr,
                                 SourceLocation loc);

    // §28.3 / §28.8 (issue #407): an owned-sink argument that is NOT a bare
    // identifier and NOT a bare owned-field read — those two forms have
    // dedicated handling at each call site that also records the move when the
    // source is a legal owned value. This catch-all covers the remaining class-
    // typed borrow forms a non-`move`d argument can take: an indexer-get
    // `coll[i]` (§28.3 / §28.11), a non-`owned` field read `obj.f` (§28.3), and
    // `this` (§28.3 / #403). None is ever a legal owned producer in bare form,
    // so there is nothing to mark moved-from; reports via `checkOwnedArgSource`
    // and returns true when it reported (the caller then treats the arg as
    // consumed). A `move`-wrapped argument is a legal owned producer and is NOT
    // reported (`ownedArgSourceIsBorrow` unwraps the `move` and returns false).
    // // per §28.3, §28.8, §28.11
    bool checkOwnedArgOtherBorrow(const ast::Expr& argValue);

    // §28.11 indexer-set (issue #395): true when `target` is `coll[i]` on an
    // owning collection whose `set(owned)` indexer requires an owned RHS. Per
    // §28.11 "Indexer set `coll[i] := value` requires `value` to be owned."
    // Recognizes a user indexer `PropertyDecl` whose set accessor is declared
    // `set(owned)`, and the built-in owning collections `OwningArray<T>` /
    // `OwningDictionary<K, V>` (stdlib §4.5 / §4.9), whose indexer setter takes
    // owned by definition. // per §28.11
    bool indexerSetterTakesOwned(const ast::Expr& target);

    // §22.3 "ref and out argument locations" (v0.184, #1682): the DATAFLOW
    // half of the ref/out operand-category rule — no location of OWNED
    // state may be passed by `ref`/`out`, because a ref/out write is an
    // assignment through an alias, invisible to the drop-on-overwrite
    // (§28.7) / consume-site-freeze (§28.3) machinery owned state relies
    // on. Checked for a bare `ref`/`out` argument (a no-op for every other
    // modifier) at the point it is encountered in the live ownership
    // dataflow, so `lookupState` reflects this exact program point:
    //   - an owned LOCAL — `lookupState(name)->state == Owned` (a `using
    //     var` local is skipped here: sema's checkRefOutOperandCategory
    //     already reports the more specific "using var" diagnostic for it,
    //     and it is ALSO Owned in this lattice, so skipping avoids a
    //     duplicate report for the same argument);
    //   - an `owned`/`out owned` PARAMETER — the declared modifier
    //     (ParameterSymbol::modifier), not dataflow, since a parameter's
    //     ownedness is a declared fact, unlike a plain local's;
    //   - an owned FIELD — exprIsOwnedFieldRead (existing helper, covers
    //     both the bare-identifier implicit-`this.f` and explicit
    //     `obj.f`/`this.f` shapes);
    //   - an owning-collection or owning-inline-array indexer slot —
    //     indexerSetterTakesOwned / targetIsOwningInlineArraySlot (existing
    //     helpers).
    // The declaration-based categories (structural shape, `using var`,
    // readonly-outside-context, mutable-static rule 2, property-setter
    // existence) are sema's job (ExprTC::checkRefOutOperandCategory) —
    // this function does not repeat them.
    void checkRefOutOwnedOperand(const ast::CallArgument& a);

    // §28.8 built-in owned-sink methods (issue #395): true when `callee` is a
    // member access `coll.M` whose receiver `coll` has a *built-in* owning-
    // collection type (which sema resolves to a Named TypeRef with a null
    // declaration, so the user-declaration path in recordMovesInArgs /
    // handleDischarge cannot see the `owned` formal) AND `M` is one of that
    // family's owned-taking methods at argument index `argPos` (stdlib §4.5 /
    // §4.7 / §4.9: `Add` / `Insert` / `AddRange` / `AddFirst` / `AddLast` /
    // `AddBefore` / `AddAfter` / `Push` / `Enqueue`). Gating on the built-in
    // receiver type means it never mis-fires on a user method of the same name
    // (those go through the precise declaration-based path). // per §28.8
    bool builtinOwnedSinkArg(const ast::Expr* callee, std::size_t argPos);

    // §28.3 / §28.11 (issue #860): true when `callee` is a member access
    // `recv.M` whose receiver `recv` has a *built-in* type carrying an
    // `out owned` parameter at argument index `argPos` — the caller-side
    // out-owned transfer that moves an element OUT of an owning collection
    // (or a Channel<T>/Weak<T> handle) into the caller's local. The owned-OUT
    // direction is the mirror of `builtinOwnedSinkArg` (which models owned-IN
    // sinks): there the argument local LOSES ownership, here it RECEIVES it.
    // Recognised: `OwningStack<T>.TryPop`, `OwningQueue<T>.TryDequeue`,
    // `OwningLinkedList<T>.TryPopFirst/TryPopLast`, `OwningSet<T>.TryTake`
    // (argPos 1), `OwningDictionary<K,V>.TryPopValue` (argPos 1),
    // `Channel<T>.TryReceive` (class T only — §6.9 value-T drops the
    // modifiers), and `Weak<T>.TryUpgrade` (stdlib §4.5/§4.7/§4.9/§6.9/§2.16).
    // The borrow forms `TryPop(out item)` / `TryPeek(out item)` carry no
    // `owned` and are NOT recognised. // per §28.3, §28.11
    bool builtinOutOwnedArg(const ast::Expr* callee, std::size_t argPos);

    // §28.3 / §28.11 (issue #860): after a call, mark each argument that is an
    // `out owned` transfer (a call-site `out owned` modifier on a method whose
    // formal is genuinely `out owned`, a user `out owned` parameter, or a
    // built-in out-owned method per `builtinOutOwnedArg`) and is a bare LOCAL
    // identifier as Owned — the local received ownership of the moved-out value
    // and now carries the §28.3 discharge obligation. Marked unconditionally
    // (conservative for the Try* false-return path, where the local is unchanged
    // / null); the canonical null-safe consumer `item?.Dispose()` discharges it.
    // // per §28.3, §28.11
    void markOutOwnedArgs(const std::vector<ast::CallArgument>& args,
                          const ast::Expr* callee);

    // §28.3 / §22.4 (issue #2429): the constructor-call overload. A `new
    // T(...)` call has no "callee" Expr in the CallExpr sense — its
    // parameter list is resolved via resolveCtorParams (recordMovesIn's
    // NewExpr branch already does this into `ctorParams`) rather than via
    // `ctx_.exprSymbol(*callee)` + MethodSymbol. This overload takes the
    // resolved parameter list directly and shares markOutOwnedArgsCore with
    // the callee-based overload above, so a constructor's `out owned`
    // recipient is marked Owned by the SAME logic a method call's is —
    // fixing #2429's root cause (recordMovesIn's NewExpr branch never
    // called any out-owned-marking equivalent, so a constructor's `out
    // owned var` recipient never entered the ownership dataflow's `states_`
    // and was invisible to the §28.3 affine auto-drop / must-use-warning
    // end-of-scope check — a silent leak, no diagnostic at all).
    // // per §28.3, §22.4
    void markOutOwnedArgs(const std::vector<ast::CallArgument>& args,
                          const CalleeFormals& params);

    // Shared implementation for both markOutOwnedArgs overloads above.
    // `callee` is threaded through only for builtinOutOwnedArg's dispatch
    // (which requires a MemberAccessExpr callee); the constructor overload
    // passes nullptr, so builtinOutOwnedArg(nullptr, pos) correctly returns
    // false for every arg — no builtin type is constructed via `new` with an
    // out-owned recipient. // per §28.3, §28.11, §22.4, issue #2429
    void markOutOwnedArgsCore(const std::vector<ast::CallArgument>& args,
                              const CalleeFormals& params,
                              const ast::Expr* callee);

    // §28.3 / §28.11 (#1466, #1407): scan an if/elif CONDITION expression for
    // PRE-DECLARED out-owned recipients (`if coll.TryX(out item) then …`) — a
    // bare `out item` argument at an `out owned` position (userOutOwned /
    // builtinOutOwnedArg) that resolves to an existing local — and insert each
    // recipient symbol into `out`. Consulted by processBlock to extend
    // ifCondOutOwnedSyms_ so foldPred demotes the recipient to PolyNull on the
    // FALSE edge (TryX returned false → the local was NOT written and is null),
    // exactly as it already does for the inline `out owned var` form. Without
    // this a discharge inside the then-branch alone (`item?.Dispose()`) meets a
    // spurious Owned on the false edge and the recipient is falsely reported
    // dropped. The inline forms are handled by collectOutOwnedVarSyms; this is
    // the pre-declared counterpart, which needs the callee param / builtin
    // dispatch (a member) to tell an owned-out `out` from a borrowing `out`.
    void collectCondOutOwnedRecipients(const ast::Expr& cond,
                                       std::unordered_set<sema::Symbol*>& out);

    // -- Async-readiness 3c: at-await-site Pattern A / B enforcement -----------
    //
    // §30.5 "Consumer-side rules". For `await <task>` the obligations are fixed
    // by the task's STATIC return type, which 3b's await typing makes readable:
    // the await UnaryExpr's operand carries the resolved `Task`/`Task<T>`/
    // `OwningTask<T>` TypeRef. These checks run as a single syntactic walk over
    // the async body (driven from `run()`), independent of the CFG dataflow, so
    // each await is examined once and no diagnostic is duplicated. They are
    // gated on `isAsync_`, so a synchronous body never reaches them. // per §30.5

    enum class AwaitTaskKind {
        NotTask,    // operand not a Task-family type (e.g. async-iterator await,
                    //   or an untyped operand) — no site rule applies here
        UnitTask,   // `await Task` — no result value, no ownership obligation
        PatternA,   // `await Task<T>` for CLASS T — borrow; needs owning location
        PatternB,   // `await OwningTask<T>` for class T — ownership transfers;
                    //   needs an owned-consuming context
        ValueResult,// `await Task<T>` for non-class T — no obligation crosses
    };

    // Classify the await by its operand's resolved task type (§30.3 / §30.5).
    AwaitTaskKind awaitTaskKind(const ast::UnaryExpr& await) const;

    // §30.5 Pattern A: true when `taskExpr` (the await's operand) occupies a
    // syntactically-visible OWNING location whose lifetime bounds the borrowed
    // result — a reference to a `using var`/`await using var`-bound local of
    // `Task<T>`, an `owned` parameter, an `owned` field of `this`, an
    // owning-collection indexer slot, or a borrowing-iteration variable — or (a
    // plain borrowed local/parameter of `Task<T>`, v0.193) a reference to any
    // admitted operand ABOVE via a borrowed ALIAS: `var t2 := t; await t2;`
    // where `t` is admitted (§30.5 rule (a), #1800). `depth` bounds the alias
    // walk against a pathological error-recovery cycle. The list mirrors §30.5's
    // admitted positions; anything else (a bare producer call, an arbitrary
    // expression) is NOT an owning location. // per §30.5
    bool awaitOperandIsOwningLocation(const ast::Expr& taskExpr, int depth = 0);

    // §30.5/§28.3 (v0.182, #1570): true when `e` is a syntactically fresh,
    // UNNAMED producer expression — a bare call (`Producer()`) or `new`
    // expression — as opposed to a reference to existing named storage (an
    // identifier, a member-access field read, or an indexer read). This is
    // the "unbound owned temporary" shape §28.3 defines: not bound to any
    // name, so its lifetime is the full expression that produces it. Used to
    // admit `Process(await Producer())` / `Producer().Result` WITHOUT
    // admitting a reference to a plain (non-`using`) owned local — `var t :=
    // Producer(); Process(await t);` — which is a NAMED value whose lifetime
    // is its declaring scope, not the statement that reads it, and stays
    // governed by `awaitOperandIsOwningLocation` alone. // per §28.3, §30.5
    static bool isUnboundProducerExpr(const ast::Expr& e);

    // Drive the at-await-site checks over the body. For each statement, decide
    // which awaits sit in an owned-consuming context (Pattern B) — passed via
    // `consumingAwaits` collected by `collectOwnedConsumingAwaits` — then walk
    // every await in the statement and diagnose: a Pattern A await whose
    // operand is not an owning location, and a Pattern B await not in an
    // owned-consuming context. Each expression walk also threads
    // `bindingValue` — true exactly when the walked expression's value would
    // flow directly into a `var`/`using var` binding or a store (a LocalDecl
    // initializer or an assignment RHS), propagated through pass-through
    // forms (`?:` branches, postfix `!`) and false everywhere the value is
    // merely consumed within the statement (a call argument, operator
    // operand, receiver, …). A Pattern A await whose operand is an unbound
    // producer expression (`isUnboundProducerExpr`) is admitted when
    // `bindingValue` is false — the v0.182 expression-confinement admission
    // (§30.5, #1570) — and rejected when true, since the borrow would then
    // escape the full expression that sources it (the classic `var x := await
    // Producer();` rejection). // per §30.5, §28.3
    // fix #3074 (§30.5 / §30.3 "`.Result` in lockstep", v0.249): the ONE
    // statement traversal both the await-site check and its `.Result` sibling
    // run on. `visit(e, bindingValue)` is called once per expression POSITION a
    // statement holds; nested statements are recursed into here, so neither
    // caller enumerates statement kinds itself.
    //
    // ⚠ THIS FUNCTION IS THE LIST. It exists because the two walks it replaces
    // each carried their own hand-maintained twelve-arm dispatch, and both were
    // missing the same three entries — `LocalConstDecl` (so `const cfg := await
    // Producer();` was silently accepted and dangled, #3074), `CaseStmt` (so no
    // await in a `case` arm was ever classified) and `IncrementStmt`. Two
    // independent routes finding two different holes is the signature of a list
    // that was accumulated rather than derived, so it is derived here, once,
    // from `ast::Node.hpp`'s statement hierarchy — every `: Stmt` subclass gets
    // an arm, including the two that hold no expression at all, so that adding a
    // statement kind without deciding its await behaviour is a visible omission
    // rather than a silent exemption. Keep the arms in the header's order.
    //
    // `bindingValue` is true at exactly the positions whose value flows directly
    // into a binding or a store and therefore SURVIVES the full expression — a
    // `var`/`using var` initializer, a `const` initializer, an assignment RHS, a
    // `return`/`yield return` value. It is false everywhere the value is merely
    // consumed within the statement (conditions, call arguments, `for` sources,
    // discards, assignment targets, `case` scrutinees and guards).
    using StmtExprSiteVisitor =
        std::function<void(const ast::Expr& e, bool bindingValue)>;
    void visitStmtExprSites(const ast::Stmt& s,
                            const StmtExprSiteVisitor& visit);

    // §28.8.1 "Exception paths are not exempt" (#3080): fill `stmtsInsideTry_`. A
    // purely syntactic pre-pass whose traversal mirrors collectLocalDepthsStmt's
    // — the ownership CFG flattens the control-flow forms, but here we only need
    // to know which statements a `try` lexically encloses. // per §28.8.1
    void collectStmtsInsideTry(
        const std::vector<std::unique_ptr<ast::Stmt>>& body);
    void collectStmtsInsideTryStmt(const ast::Stmt& s, bool insideTry,
                                   const ast::TryStmt* enclosingTry);

    // §28.8.1 (#3080): the section's throwable-expression inventory, as a SEARCH
    // rather than a predicate — returns the first sub-expression of `e` whose
    // evaluation can raise, or null when none can. It returns the NODE, not a
    // bool, because the diagnostic names a source location and the code that
    // names it must be the code that found it. // per §28.8.1
    const ast::Expr* findThrowable28_8_1(const ast::Expr* e) const;

    // §28.8.1 (R4b, #3241): the drop-on-overwrite clause of the non-throwable
    // assignments list — "a store into an `owned` field, a plain-`var` owned
    // local, or an owning-collection slot that may hold a live (non-null, not
    // moved-from) value is throwable". Returns the RHS node to report at when
    // the assignment is throwable by drop-on-overwrite, or nullptr when the
    // store is into a provably moved-from/null/borrow/value-typed location.
    // Distinguished from `findThrowable28_8_1` (which walks RHS sub-expressions
    // for the throwable inventory) by the LHS-side, ownership-lattice check.
    // Non-const because the local-state lookup reads the mutable `states_`
    // map (the lookup itself is non-mutating, but `lookupState` is not
    // const-qualified). // per §28.8.1, #3241 R4b
    const ast::Expr* assignmentIs28_8_1ThrowableByOverwrite(
        const ast::AssignmentStmt& as);

    // §28.8.1 (#3080): the exception-edge half of the field-restoration rule —
    // "a throwable expression between the move and the reassignment introduces an
    // exit path on which the reassignment has not run". Called from processBlock
    // at each statement's ENTRY state. // per §28.8.1
    void checkThrowableWhileFieldMovedFrom(const ast::Stmt& s);

    // §28.8.1 (#3241 R2): fire the deferred in-`try` throwable checks for
    // `ts`. A throwable in the try body whose fieldMovedFrom_ at its site
    // had the field moved-from is a violation iff at least one of the catch
    // arms in `catchArmExitStates` has the same field still moved-from at
    // ITS exit. The catch arm's exit state is the fieldMovedFrom_ snapshot
    // taken right after the arm body's last statement (see visitStmt for
    // TryStmt). The function erases the entries it processed so a later
    // enclosing try does not re-fire them. // per §28.8.1, #3241 R2
    void firePendingInTryChecks(
        const ast::TryStmt* ts,
        const std::vector<FieldMovedFrom>& catchArmExitStates);

    // §28.8.1 (#3241 R2): fire ALL pending in-`try` throwable checks, in
    // one pass. Used as a post-pass in run() after the dataflow has
    // converged; the per-`ts` firePendingInTryChecks above is a legacy
    // wrapper (no-op) for the original design that hooked the fire from
    // the unreachable `visitStmt` for TryStmt. This version uses a
    // SYNTACTIC check on the catch arm body (a top-level
    // `_field := …` assignment is "restoring") because the dataflow's
    // exit state for the catch arm's last block is not reliable for the
    // in-`try` case — pre-existing #3241 R1 residue. // per §28.8.1,
    // #3241 R2
    void fireAllPendingInTryChecks();

    void checkAwaitSites(const std::vector<std::unique_ptr<ast::Stmt>>& body);
    void checkAwaitSitesStmt(
        const ast::Stmt& s,
        const std::unordered_set<const ast::UnaryExpr*>& consumingAwaits);
    void checkAwaitSitesExpr(
        const ast::Expr& e,
        const std::unordered_set<const ast::UnaryExpr*>& consumingAwaits,
        bool bindingValue);

    // §30.5 owned-consuming contexts: collect into `out` every `await`
    // UnaryExpr that appears as the IMMEDIATE value of an owned-consuming
    // position — a LocalDecl `:= await` RHS (using/plain/await-using var), an
    // assignment-to-owned-location `target := await` RHS, an `owned` argument
    // `M(await)` / `coll.Add(await)`, an `owned`/`OwningTask` `return await`,
    // and an owned `yield return await`. A Pattern B await NOT in this set is
    // dropped without discharge and is diagnosed. // per §30.5
    void collectOwnedConsumingAwaits(
        const std::vector<std::unique_ptr<ast::Stmt>>& body,
        std::unordered_set<const ast::UnaryExpr*>& out);
    void collectOwnedConsumingAwaitsStmt(
        const ast::Stmt& s, std::unordered_set<const ast::UnaryExpr*>& out);
    // §30.5: record awaits passed as `owned` arguments (a call's `owned`
    // formal, or a built-in owning-collection owned-sink method) — those are
    // owned-consuming positions. Recurses into the expression. // per §30.5
    void collectOwnedConsumingInExpr(
        const ast::Expr& e, std::unordered_set<const ast::UnaryExpr*>& out);

    // The await UnaryExpr at the root of `e` when `e` is exactly `await X`
    // (the immediate-value form an owned-consuming position admits), else null.
    static const ast::UnaryExpr* awaitRoot(const ast::Expr& e);

    // -- #1530 (§30.3 / §30.5, v0.179): `Task<T>.Result` (Pattern A, class T)
    // owning-location check ------------------------------------------------
    //
    // Reading `.Result` on a completed `Task<T>` whose T is a class is
    // additionally subject to the SAME owning-source rule as `await Task<T>`
    // (§30.3): the read produces a borrow whose source is the task
    // expression's own location, so the receiver must occupy a syntactically-
    // visible owning location exactly as an awaited Pattern A operand must
    // (`awaitOperandIsOwningLocation`, reused verbatim — a `.Result` receiver
    // and an `await` operand are the same "task expression" shape). Unlike
    // `await`, there is no chained-slot admission for a bare producer call
    // (`return await Producer()`'s exception) — `.Result` is a synchronous
    // read, not part of an async state machine's return-await chain — but as
    // of v0.182 (§30.3/§28.3, #1570) a bare-producer receiver IS admitted when
    // its borrowed result is confined to the same full expression
    // (`SomeProducer().Result`, consumed immediately), exactly like the
    // `await` sibling rule; only an escaping borrow (bound to a `var`, stored,
    // returned) remains rejected. Runs UNCONDITIONALLY over every method body
    // (sync and async) — unlike the at-await-site checks above, which are
    // gated on `isAsync_` because `await` only appears in async bodies,
    // `.Result` is the synchronous escape hatch and is typically read from
    // ordinary (non-async) code. // per §30.3, §30.5, §28.3
    void checkResultSites(const std::vector<std::unique_ptr<ast::Stmt>>& body);
    void checkResultSitesStmt(const ast::Stmt& s);
    void checkResultSitesExpr(const ast::Expr& e, bool bindingValue);

    // §15.4 / §30.5 (#1616): Task.WhenAll/WhenAny are compiler intrinsics with
    // no MethodDecl parameter metadata, but their sole argument is the borrowing
    // target Array<Task>. Reject each fresh non-trivial-dispose Task producer in
    // an inline literal; otherwise its full-expression auto-disposal obligation
    // is lost in the borrowing array. Bound task borrows remain admitted.
    void checkBorrowingTaskArrayLiteral(const ast::CallExpr& call);

    // -- #1526 (§28.3 "Deferred-activation callees", v0.179) --------------------
    //
    // A call whose return type is a §29.3/§30.7 iterator/async-iterator
    // interface, a §29.11 lazy query, or a Task-family type (Task/Task<T>/
    // OwningTask<T>) returns a frame that carries a BORROW of its plain
    // (non-`owned`) reference-typed arguments until the frame itself
    // completes or is disposed (iterator/lazy query) or the task settles
    // (Task family). When such an argument is an UNBOUND OWNED TEMPORARY
    // (§28.3 "Unbound owned temporaries") — a bare `new`/call producing an
    // owned value, not bound to any name — the temporary is disposed at the
    // end of the FULL EXPRESSION that produces it, so the frame's borrow of
    // it dangles unless the frame is itself fully consumed within that same
    // full expression (awaited/blocked-on, for the Task family). Where the
    // frame instead escapes the expression — bound to a name (`var`/`using
    // var`), returned, stored, used as a `for`/`await for` source, or passed
    // to `Task.FireAndForget` — the program must be rejected.
    //
    // Implemented as a whole-body walk mirroring checkAwaitSites /
    // checkResultSites: `escaping` plays the role `bindingValue` plays
    // there, true at exactly the escaping positions above (LocalDecl
    // initializer, assignment RHS, return value, yield value, and — unlike
    // the await/.Result siblings — a ForStmt's `iterable`, since a `for`/
    // `await for` source is itself an escape here, §29.2) and propagated
    // through the same pass-through forms (postfix `!`, `?:` branches).
    // Every other recursion (a unary operand — including an `await`
    // operand, which always fully consumes its operand to settlement — a
    // binary operand, a member-access/index target, an ordinary call
    // argument) resets to false, exactly as the sibling checks reset
    // `bindingValue`: being borrowed by a further operation within the same
    // statement is the definition of "consumed within the full expression."
    // The one argument-position exception is `Task.FireAndForget`'s sole
    // argument, which is itself named as an escape by the spec. // per §28.3
    void checkDeferredActivationTempSites(
        const std::vector<std::unique_ptr<ast::Stmt>>& body);
    void checkDeferredActivationTempSitesStmt(const ast::Stmt& s);
    // `forSource` marks the ONE escaping position whose frame is bounded by a
    // loop rather than binding/returning/storing: a `for`/`await for` source
    // (§29.2's desugared `using var`). The #1526 unbound-temp check fires on it
    // (a temporary source dies at the full expression's end, before the loop
    // runs), but the #1818 capture-set check does NOT — a captured method-scope
    // owned source outlives the loop, so a direct for-source over a capturing
    // frame is sound. Propagated through the pass-through forms. // per §28.3,
    // §28.14, v0.196
    void checkDeferredActivationTempSitesExpr(const ast::Expr& e,
                                               bool escaping,
                                               bool forSource = false);

    // §28.3 / stdlib §5 (#1917): a lazy LINQ `Select(selector)` whose inline
    // lambda selector MINTS a fresh cleanup-bearing value per element — a
    // freshly-`new`'d owned class (`(x) => new Box(x)`) or a fresh string
    // (`(w) => w + "!"`, an interpolation) — launders that value out through the
    // projected sequence's receiver-sourced-borrow classification (stdlib §5
    // "Ownership at class-typed substitutions"): the pipeline takes no disposal
    // obligation on the projected elements (they are treated as borrows drawn
    // from the receiver), so every minted projection leaks and there is no
    // consumer that can reclaim it (`ToArray` owns only the CONTAINER, borrowing
    // elements; no lazy materializer takes ELEMENT ownership). This is the same
    // §28.3 laundering class as the `Aggregate` minted-seed rejection
    // (checkBareBorrowTempLaunder) and the Min/Max minting-selector case (#1915);
    // reject the launder at the selector. Called for every CallExpr the
    // deferred-activation walk visits (position-independent — a minting Select
    // leaks in for-source, binding, materialization, and terminal positions
    // alike). // per §28.3 / stdlib §5 (#1917)
    void checkLazySelectMintingSelector(const ast::CallExpr& ce);
    // §28.3 / stdlib §5 (#1915): a LINQ selector `Min<U>(selector)` /
    // `Max<U>(selector)` whose inline lambda selector MINTS a fresh
    // cleanup-bearing value per element — a freshly-`new`'d owned class
    // (`(o) => new Box(o)`) or a fresh string (`(s) => s + "!"`, an
    // interpolation). Stdlib §5 ("Ownership at class-typed substitutions",
    // v0.194) classifies the selector `Min<U>/Max<U>`'s result `U` as a
    // RECEIVER-SOURCED BORROW (the #1876 carve, re-enabled with the #1901 result
    // type fix), so the pointer-U aggregation path takes NO disposal obligation on
    // ANY projected value — correct for a receiver-sourced projection (identity /
    // field-borrow), but a minting selector's projections are freshly owned and
    // leak (winner AND losers: the selector runs on every element to find the
    // extremum, and nothing reclaims any of the minted values). This is the same
    // §28.3 laundering class as the `Aggregate` minted-seed rejection
    // (checkBareBorrowTempLaunder) and the lazy `Select` minting selector (#1917);
    // reject the launder at the selector. Called for every CallExpr the
    // deferred-activation walk visits (position-independent). // per §28.3 /
    // stdlib §5 (#1915)
    void checkMinMaxMintingSelector(const ast::CallExpr& ce);
    // The first return-path expression of `lam` (an inline lambda selector) that
    // MINTS a fresh cleanup-bearing value (lazySelectSelectorReturnMints), or
    // nullptr if none does. Scans the `=> E` body or the block body's `return`
    // statements, never crossing a nested lambda (its returns belong to it) —
    // mirrors the SB01-B return walk. Shared by the `Select` (#1917) and `Min`/
    // `Max` (#1915) minting-selector rejects. // per §28.3 / stdlib §5
    const ast::Expr* selectorLambdaMintingReturn(const ast::LambdaExpr& lam);
    // True when `retExpr` — a lazy `Select` selector's return value — mints a
    // fresh owned/heap value with no borrow source: an owned-class producer
    // (producerState == Owned), a fresh string (a `+` concatenation, an
    // interpolation, or (#1931) a call to a stdlib string-minting method on a
    // string receiver), or a conditional either of whose arms mints. A borrowed
    // projection (the element itself, a field / index read, a value-typed
    // projection) is NOT a mint and is admitted. // per §28.3 / stdlib §5
    // (#1917, #1931)
    bool lazySelectSelectorReturnMints(const ast::Expr& retExpr);
    // §28.3 / stdlib §9.6 (#1931): true when `ce` is a CALL to a STDLIB string-
    // minting method on a string receiver — the documented stdlib §9.6 (v0.197)
    // mint set (`ToUpper`/`ToLower`/`Substring`/`Trim`/`TrimStart`/`TrimEnd`/
    // `Replace`/`PadLeft`/`PadRight`/`Insert`/`Remove`). User overrides (which
    // may return a borrow) and non-string receivers are excluded. // per §28.3
    // / stdlib §9.6 (#1931)
    bool isStdlibStringMintingCall(const ast::CallExpr& ce);

    // True when `callResult`'s resolved type is one of the deferred-
    // activation frame shapes above: an iterator/async-iterator interface,
    // the lazy-query sentinel (mirrors DefiniteAssignment's
    // isLazyEnumerableExprType / codegen's isLazyEnumerableType — a Named
    // TypeRef with no declaration/astNode and at least one generic arg), or
    // Task/Task<T>/OwningTask<T>. // per §28.3, §29.3, §29.11, §30.1
    bool isDeferredActivationCallResult(const ast::Expr& callResult) const;

    // True when the enclosing method's DECLARED return type (`returnType_`) is
    // one of the deferred-activation frame shapes — an iterator/async-iterator
    // interface or a Task-family type. Read from the AST return-type node's head
    // name (as checkBorrowReturnedThroughOwningType reads it), so a frame
    // RETURNED under a deferred-activation type is recognized as the announced
    // frame residence rather than an escape (§28.14 *Call-bounded × deferred
    // activation*, v0.196). The lazy-query sentinel never appears as a written
    // return type, so only the named interfaces / Task are matched here.
    // // per §28.14, §28.3 v0.196
    bool returnTypeIsDeferredActivation() const;

    // #2343 (§28.14 *Call-bounded × deferred activation*, v0.196): per-lambda
    // sibling of returnTypeIsDeferredActivation, for the lambda's OWN `=>`
    // expression body. The frame-residence carve-out at an expr-body implicit
    // return needs the LAMBDA's return type (what ITS caller sees and
    // re-establishes against), not the enclosing method's returnType_. A lambda
    // with an explicit return-type ascription (lam->retType) reads that AST
    // node with the same head-name navigation as returnTypeIsDeferredActivation;
    // a lambda with an INFERRED return type has it equal to the exprBody's
    // resolved type, so isDeferredActivationCallResult(exprBody) alone is the
    // equivalent announcement (the inferred return type composes iff the body
    // is a frame). // per §28.14, v0.196, #2343
    bool lambdaReturnTypeIsDeferredActivation(
        const ast::LambdaExpr& lam, const ast::Expr& exprBody) const;

    // True when `arg` is a plain-argument "unbound owned temporary" per the
    // §28.3 Producers table: a bare `new T(...)` or call already classified
    // Owned by producerState, OR a call to a STATIC method (no receiver)
    // returning a disposable class/interface with no `owned` annotation —
    // the §28.3 "Factory methods and sourceless returns" rule, which treats
    // an unannotated static factory return as owned unconditionally (the
    // iterator-method carve-out there is a WARNING exemption only, not a
    // classification exemption — but producerState's own
    // staticFactoryReturnsFreshOwned helper deliberately excludes a
    // yield-only body, since it scans `return` statements for ITS purpose,
    // the local auto-drop obligation; that exclusion does not apply here).
    // // per §28.3
    bool isUnboundOwnedTempArg(const ast::Expr& arg);

    // True when argument `pos` of the deferred-activation call `ce` sits at a
    // PLAIN (non-`owned`) reference-typed parameter of a USER method/ctor — the
    // §28.3 "plain reference-typed arguments" the frame borrows. The borrow gate
    // that narrows the deferred-activation temp rule: an `owned`-parameter sink
    // (§29.8/§30.14 owned transfer) and any builtin callee with no resolvable
    // param list (Task.FromException/FromResult, Domain.Spawn, lazy queries —
    // which OWN or copy their args) return false, so an owned temp transferred
    // into a sink is not over-rejected. // per §28.3, §28.8, §29.8, §30.14
    bool deferredActivationArgIsPlainRefParam(const ast::CallExpr& ce,
                                              std::size_t pos) const;

    // #1818 (§28.3 "capture-set contribution", §28.14 frame residence, v0.196):
    // a carried source contributed to a deferred-activation frame by a plain
    // function-typed argument's captures. `ownedObligation` is true when the
    // captured source is a *live owned obligation of the enclosing frame* — an
    // `owned` parameter, or an owned local (a disposing `using var`, or a plain
    // owned local that auto-drops / becomes owned at scope end): a source the
    // enclosing frame itself disposes at scope exit, so a frame that captures it
    // and then escapes that scope would strand a borrow of a disposed object.
    // A plain (borrow) parameter or a borrow local is a carried source too but
    // NOT an owned obligation — it outlives the enclosing frame and the caller
    // re-establishes it (the transitive rows are later #1818 sub-tasks). // per
    // §28.3, §28.14, v0.196
    struct CapSource {
        const ast::IdentifierExpr* id;
        sema::Symbol* sym;
        bool ownedObligation;
    };

    // #1818 (§28.3 "capture-set contribution", v0.196): collect the carried
    // sources a plain function-typed ARGUMENT contributes to a deferred-
    // activation frame. Two provenance rows are handled: a LAMBDA LITERAL (T2 —
    // enumerate its captures via scopedCollectCaptures) and a METHOD REFERENCE
    // `obj.M` (T3 — its captured environment is the receiver, §27.6, reduced via
    // scopedCaptureWalkExpr; a static method reference reduces to a type root
    // that drops out). Both feed the SAME reduction, EXACTLY as
    // checkScopedAdmission does (`this`/`base` and receiver-mediated members
    // contribute nothing — a receiver is a caller-frame borrow that outlives
    // every local of this frame; a static / const / type / module ref
    // contributes nothing; a value-typed local lives in its heap capture cell —
    // the isBorrowableType gate; a lambda-internal parameter is not a capture of
    // the enclosing frame). A third row (T5, §28.3, v0.196) traces a plain
    // IDENTIFIER argument bound to a LOCAL to its provenance and contributes the
    // UNION of the traced forms' sources: a local bound to a lambda literal
    // (scopedLambdaLocalInit_) or a method reference (deferredMethodRefLocalInit_)
    // and never reassigned (scopedAssignedSyms_) recurses into that function
    // value's own row; a local carrying a call-bounded parameter
    // (callBoundedAlias_) contributes itself (a plain function param the caller
    // re-establishes — admitted). A reassigned or untraceable local contributes
    // nothing (conservative admit). An opaque function-typed parameter and a
    // plain-storage field fetch are later provenance rows and contribute nothing
    // here. // per §28.3, §27.6, §28.14, v0.196
    void deferredActivationCaptureSources(const ast::Expr& arg,
                                          std::vector<CapSource>& out);
    // True when the LocalSymbol names a *live owned obligation* — an owned local
    // the enclosing frame disposes at scope exit (a disposing `using var`, an
    // affine auto-drop local, or a null-then-reassigned owned local). The
    // owned-ness flags are populated by the dataflow before this post-pass runs
    // (Ownership.cpp). // per §28.3 v0.196
    bool localIsOwnedObligation(const sema::LocalSymbol* ls) const;

    // fix #3746 (§28.3 *Captured owned bindings*, v0.286): the ONE both-kinds
    // form of the question above — "is this symbol a binding whose owned value
    // THIS frame disposes?" §28.3 names the category once ("an owned local or
    // `owned` parameter of the enclosing body"); #1818's capture reduction had
    // already answered it for both, spelled inline at one site, and the
    // store-escape source resolution answered a narrower question at another.
    // This is that one spelling, so the two cannot drift. A symbol that is
    // neither a local nor a parameter (a field, a method) is not a binding of
    // this frame and answers false. // per §28.3, v0.286
    bool symbolIsOwnedObligation(const sema::Symbol* sym) const;

    // #1818 (§28.14 "Call-bounded × deferred activation", v0.196): true when
    // argument `pos` of the deferred-activation call `ce` sits at a PLAIN
    // (empty-modifier) parameter of a resolved USER method/ctor — the frame-
    // residence position whose callback captures the returned frame carries.
    // A `scoped` function parameter is storage-bounded and its capture admission
    // is handled by checkScopedAdmission (excluded here to avoid a double
    // diagnostic); an `owned`/`ref`/`out` parameter is a sink; a builtin callee
    // with no resolvable param list stays out of this FIRST-row check
    // (conservative — the stdlib lazy operators are a later #1818 sub-task).
    // // per §28.14 v0.196
    bool deferredActivationArgIsPlainFuncSlot(const ast::CallExpr& ce,
                                              std::size_t pos) const;

    // #1818 T9 (§28.14 `scoped`-field read-out bar, v0.196): true when `e` reads
    // a `scoped` FIELD as a value — a bare `_f`, `this._f`, or another object's
    // `other._f` — resolving to a `scoped`-modified FieldSymbol; names it in
    // `nameOut`. Passing such a read into a deferred-activation plain function-
    // typed position is the barred escaping read-out §28.14 names (the critique's
    // `Sneak` counterexample): the stored capture-bearing callback would ride the
    // returned frame beyond the bounding object's control and past severance's
    // reach. `this`'s OWN scoped field read out as a bare `_f`/`this._f` is
    // already rejected by the callee-side scoped scan (scopedCalleeWalkExpr,
    // kind 2); this covers the case that scan misses — another object's scoped
    // field — with a distinct diagnostic. // per §28.14 v0.196
    bool scopedFieldReadArg(const ast::Expr& e,
                            std::string_view* nameOut = nullptr) const;

    // #1818 T10 (§28.14 frame residence / §30.1, v0.196): true when the call
    // result is a Task-family (async) deferred-activation frame — `Task<T>` /
    // `OwningTask<T>`. The carried-source reject uses it to speak the §30.1
    // async settlement remedy (await the task in the source's scope) rather than
    // the iterator/lazy-query "materialize a collection" remedy. // per §28.14,
    // §30.1 v0.196
    bool callResultIsAsyncTask(const ast::CallExpr& ce) const;

    // -- Async-readiness 3c: SuspendLocal annotation + completion edges --------
    //
    // §30.5 / §30.9. Classify each owned local as suspend-local (live range
    // crosses an await) vs activation-local, recording the result on its
    // LocalOwnership.suspendLocal. Adds synthetic completion edges from each
    // await-bearing CFG block to the exit so the abandonment/cancellation
    // exits give a suspend-local the same frame-disposal treatment as the
    // §28.3 exception-path discharge. See the rationale block in Ownership.cpp.
    // // per §30.5, §30.9, §30.8
    void annotateSuspendLocals(const flow::CFG& cfg);
    void addCompletionEdges(flow::CFG& cfg);

    // Async-readiness 3c: the set of LocalDecls whose owned local is a
    // suspend-local candidate (declared at/before an await in CFG order; the
    // conservative over-approximation `annotateSuspendLocals` computes). Read
    // by handleLocalDecl to set LocalOwnership.suspendLocal when the local is
    // created. Empty for a synchronous body (annotateSuspendLocals is not
    // called) and for an async body with no await. // per §30.5
    std::unordered_set<const ast::Node*> suspendLocalDecls_;

    // SB01-C1 (§30.5 "A reference to a plain owned local of static type
    // `Task<T>`", v0.184 resolving #1533; the v0.190 SB-01 caller-side
    // classification that makes it reachable, §28.3 #1786): the set of plain
    // `var` owned-task LocalDecls that are BORROW-AWAITED in this body — an
    // `await t` (Pattern A) whose operand resolves, through the v0.193 (#1800)
    // alias chain, to a plain owned task local (minting initializer). §30.5
    // (line 7341) says such an awaited owned local "keeps its ordinary §28.3
    // discharge (auto-drop at scope end)" — exactly the disposition of an
    // ADMITTED `using var` task suspend-local (also disposed at scope end via
    // the codegen task path, normal+unwind edges) — so the async endState gate
    // routes it to auto-drop (with the must-use nudge to `using var`) rather
    // than the §28.3 suspend-local hard reject. The genuine D2 residual — a
    // NEVER-awaited plain owned task held live ACROSS a suspend point — is NOT
    // recorded here; SB01-D2 (#1868) instead admits it via the type-directed
    // declInitializerIsOwnedTaskMint gate in the async endState loop (its
    // fire-and-forget scope-end disposal covers the same reachable edges, so its
    // await-ness was never the real soundness criterion). Populated by
    // collectBorrowAwaitedOwnedTaskLocals before the dataflow; empty for a
    // synchronous body. // per §30.5, §28.3 (v0.190)
    std::unordered_set<const ast::LocalDecl*> borrowAwaitedOwnedTaskDecls_;
    void collectBorrowAwaitedOwnedTaskLocals(const flow::CFG& cfg);
    // SB01-D2 (§30.5 line 7341/7351, §28.12, v0.190, #1868): true when `ld`'s
    // initializer MINTS an owned task frame (a Task-family CALL classified Owned
    // by SB01-A whose static result type is `Task`/`Task<T>`/`OwningTask<T>`). A
    // suspend-local matching this is auto-dropped at scope end via the codegen
    // task path — which fires on every reachable terminal edge (normal, unwind,
    // and cancellation-as-fault) — rather than taking the §28.3 suspend-local
    // reject a plain owned CLASS local would. See the definition for the
    // full-edge-coverage argument. // per §30.5, §28.12, §28.3 (v0.190)
    bool declInitializerIsOwnedTaskMint(const ast::LocalDecl* ld);
    // The root plain-`var` owned-task LocalDecl an `await` operand borrows,
    // following the #1800 alias chain (`var t2 := t`) to the underlying binding;
    // null unless that binding's initializer MINTS an owned task
    // (producerState == Owned). // per §30.5, §28.3 (v0.190)
    const ast::LocalDecl* awaitedOwnedTaskLocalRoot(const ast::Expr& operand,
                                                    int depth);

    // Async-readiness 3c (§30.5 line ~6068): the set of `await` UnaryExprs that
    // appear directly as the value of a `return` statement. A Pattern A
    // `return await Producer()` is an explicitly-admitted owning position even
    // though `Producer()` is a bare call — the producer's task is owned by the
    // enclosing state machine and the borrow flows into the outer Pattern A
    // task's result slot. Populated by collectOwnedConsumingAwaitsStmt's
    // ReturnStmt branch (alongside the owned-consuming set) and consulted by
    // the Pattern A site check. // per §30.5
    std::unordered_set<const ast::UnaryExpr*> returnPositionAwaits_;

    // Async-readiness 3c (§30.5 line ~6110): the Pattern B await is
    // owned-consuming WITH RESPECT TO ITS OPERAND — awaiting an owned task
    // local consumes that local. When the await's operand is a bare owned
    // task local, mark it consumed (Disposed) so a straight-line SECOND await
    // of the same bound local is a read-after-consume (the statically-
    // detectable slice of the §30.4 single-consumer rule). Called from the
    // discharge/decl transfer functions on each await operand. // per §30.5
    void recordAwaitOperandConsume(const ast::Expr& awaitOperand,
                                   SourceLocation loc);

    // §30.5 / §30.4 (#834): true when `e`'s static type is `OwningTask<...>`
    // (the single-consumer owning task an `await` consumes); generic-safe
    // (name-head based), unlike awaitTaskKind's result-class gate.
    bool operandIsOwnedTaskType(const ast::Expr& e);

    // -- §28.14 (v0.175, #808): scoped callback parameters -------------------
    //
    // Storage-bounded capture. Two checker halves:
    //   CALL-SITE ADMISSION — a closure argument bound to a `scoped` formal is
    //   admitted iff the call's BOUNDING OBJECT resolves to a `using var` /
    //   `await using var` local L (constructor→constructed, owned-return-as-
    //   initializer→returned, else receiver) and every captured source is
    //   declared BEFORE L; the sources are then FROZEN (no move / dispose /
    //   reassign) until L's scope exits.
    //   CALLEE VERIFICATION — a `scoped` parameter may only be invoked, stored
    //   into a `scoped` field of `this`, or forwarded to a `scoped` parameter
    //   of a call on `this`; a `scoped` FIELD may only be invoked, overwritten
    //   from a `scoped` parameter, cleared to null, or null-compared.
    //
    // Both run as SYNTACTIC pre-passes at the start of run() (before the CFG
    // dataflow), so each diagnostic is emitted exactly once and the freeze map
    // is complete before the dataflow's enforcement hooks fire. // per §28.14

    // Declaration-order index of every local declaration node, assigned in
    // source order by scopedPreScan (which — unlike collectLocalDepths — also
    // walks lambda bodies). Keys: LocalDecl*, ForStmt* (iteration variable),
    // `out owned var` IdentifierExpr*. Parameters and `this` precede every
    // local (they have no entry; absence + ParameterSymbol/this = index -1).
    std::unordered_map<const ast::Node*, int> scopedDeclOrder_;
    int scopedDeclCounter_ = 0;
    // Initializer expression -> its `using var`/`await using var` LocalDecl,
    // for bounding forms 1 (constructor) and 2 (owned-return initializer).
    std::unordered_map<const ast::Expr*, const ast::LocalDecl*>
        scopedInitOfUsingLocal_;
    // §28.14 form 3 (v0.180, #1541): the RHS call expression of a `:=` whose
    // target is an `owned` field of `this` (`_sub := ticks.Subscribe(h)`). When
    // such a call's return is owned, its stored callback is bounded by `this`
    // (the owned field is disposed with `this`, severing the registration).
    std::unordered_set<const ast::Expr*> scopedInitOfOwnedFieldOfThis_;
    // The enclosing method's own parameter symbols — distinguishes a real
    // capture of a method parameter from a lambda-internal parameter.
    std::unordered_set<sema::Symbol*> methodParamSet_;
    // #2342 (§28.14): when non-null, `scopedCollectCaptures` records each
    // visited lambda's OWN parameter symbols into the pointed-to set. Set
    // transiently by `checkScopedAdmission` while collecting the captures of
    // the FORWARDED lambda (its own params + those of any lambda nested
    // within it are NOT captures — they are internal to the forwarded
    // closure; params of an enclosing lambda or the method ARE captures).
    // Null outside that one collection so the many other `scopedCollectCaptures`
    // call sites are unaffected. // per §28.14, #2342
    std::unordered_set<sema::Symbol*>* scopedLambdaInternalSink_ = nullptr;
    // §28.3 v0.196 (#1910): the enclosing method's ordered parameter symbol
    // list (matches md->params position-for-position). Used by
    // checkInstanceBorrowReturn to map a ParameterSymbol* back to its
    // positional index when the S10 carve-out's positional-source check
    // needs to know whether the parameter is at a SOURCE position in the
    // inherited declaration. Empty when no params were threaded (rare —
    // constructors/destructors).
    std::vector<sema::Symbol*> paramSyms_;
    // §28.14 freeze (v0.273): source symbol -> {bounding local's name,
    // admission call site, scope-exit offset}. A frozen source may not be
    // moved from, disposed, or reassigned in the INTERVAL
    // [admissionLoc.offset, scopeEndLoc.offset). The freeze ends at L's
    // scope exit (v0.273); L's scope-exit offset is the end of L's enclosing
    // block, computed at pre-scan time (scopedLocalScopeEnd_ below) and
    // stamped here. v0.273 corrects the v1 bound that kept the freeze for
    // the rest of the method — sound but over-rejecting mutation after L's
    // block closed (the `do … end` lift idiom Guide 1 §10 and stdlib §2.21
    // teach). // per §28.14 (v0.273)
    struct ScopedFreeze {
        std::string_view boundingName;
        SourceLocation admissionLoc;
        SourceLocation scopeEndLoc;
    };
    std::unordered_map<sema::Symbol*, ScopedFreeze> scopedFrozen_;
    // §28.14 v0.273: a `using var`/`await using var` local's scope-exit
    // offset is the END of its enclosing block — the offset just past the
    // `end` (or, at the outermost level, past the last body statement).
    // Populated by `scopedPreScan` (and the recursive `scopedPreScanStmt`)
    // using `scopedBlockEndStack_`; consulted at the freeze-emplace site in
    // `checkScopedAdmission`. Keys: the LocalDecl* of a using-var local.
    std::unordered_map<const ast::Node*, SourceLocation> scopedLocalScopeEnd_;
    // §28.14 v0.273: stack of enclosing-block `span.end` offsets maintained
    // during pre-scan. Initialized with the method body's effective end (the
    // last statement's `span.end`); pushed when entering a `do … end` block
    // (its `span.end` is the offset past the `end`); popped on exit. Other
    // block forms (if/while/for/try/case) currently share the enclosing
    // stack frame — `for`/`while` ALSO push their own `span.end` onto the
    // sibling `scopedLoopEndStack_` (below) for bound 2(b); the
    // freeze-interval upper bound for a `using var` is the MAX of the two
    // stacks' tops at declaration time. // per §28.14 (v0.273)
    std::vector<SourceLocation> scopedBlockEndStack_;
    // §28.14 v0.273 bound 2(b): stack of enclosing-LOOP `span.end` offsets.
    // When a `using var` is declared inside a loop body, its scope-exit is
    // pushed out to the innermost enclosing loop's end (the loop ends AFTER
    // the inner block, and a lift at the block's end would otherwise re-admit
    // the capture over a source the previous iteration consumed). Pushed at
    // while/for entry, popped at exit. Empty when no enclosing loop; in that
    // case the freeze interval uses `scopedBlockEndStack_.back()` alone.
    // // per §28.14 (v0.273, bound 2(b))
    std::vector<SourceLocation> scopedLoopEndStack_;

    // §28.14 verifiable-subset widening (#1420 triage): a local initialized
    // with a LAMBDA LITERAL and never reassigned is a verifiable function
    // value at a scoped call site — its captures are the literal's captures
    // (`var h := (v) => box.Add(v); … evt.Subscribe(h);`). Maps the
    // LocalSymbol to its initializer lambda; consulted only when the symbol
    // is absent from scopedAssignedSyms_.
    std::unordered_map<sema::Symbol*, const ast::LambdaExpr*>
        scopedLambdaLocalInit_;
    // #1818 (§28.3 traced-local provenance row, v0.196): a local initialized
    // with a METHOD REFERENCE `obj.M` and never reassigned traces to that
    // reference's captured receiver — the deferred-activation capture check
    // (deferredActivationCaptureSources' traced-local row) consults this
    // alongside scopedLambdaLocalInit_ to reduce `var f := scale.Apply;
    // return Doubled(nums, f);` to the owned receiver `scale`. Maps the
    // LocalSymbol to its initializer MemberAccessExpr; consulted only when the
    // symbol is absent from scopedAssignedSyms_. Recorded by scopedPreScanStmt
    // in the same LocalDecl pass as scopedLambdaLocalInit_. // per §28.3 v0.196
    std::unordered_map<sema::Symbol*, const ast::Expr*>
        deferredMethodRefLocalInit_;
    // Every symbol that is an assignment target anywhere in the body
    // (including through an `out`/`ref` call argument). A lambda-literal
    // local among them is NOT verifiable (the binding may have been swapped
    // before the call site — the pre-scan is flow-insensitive).
    std::unordered_set<sema::Symbol*> scopedAssignedSyms_;
    // Deferred §28.14 admission work items: the walk indexes declarations and
    // records the maps above; the admission checks run AFTER the full walk so
    // scopedLambdaLocalInit_ / scopedAssignedSyms_ are complete. (Declaration-
    // order comparisons are unchanged: a source declared after the bounding
    // local now carries a LARGER index instead of no index — rejected the
    // same way.)
    struct ScopedAdmission {
        const ast::Expr* callNode;
        const ast::Expr* callee;
        const ast::NewExpr* ne;
        const ast::Expr* arg;
        // §28.14 form 4 (v0.180, #1541): non-null for an `event += handler`
        // store — the event member-access expression whose chain head bounds
        // the callback. When set, the bound is resolved by
        // resolveChainHead(eventTarget) rather than the callee/using-var path.
        const ast::Expr* eventTarget = nullptr;
    };
    std::vector<ScopedAdmission> scopedAdmissionQueue_;

    // §32.5 (v0.186, #1719): extern calls whose argument list contains both an
    // `out` recipient and a callback are checked after scopedPreScan's whole-body
    // walk, when traceable lambda-local initializers and reassignment facts are
    // complete. The callback may run before native code initializes the `out`
    // recipient, so its capture set must not contain that same binding. Direct
    // lambdas, method references, and never-reassigned lambda/method-reference
    // locals have statically visible capture sets. // per §32.5
    std::vector<const ast::CallExpr*> externOutCaptureCallQueue_;
    void checkExternOutCallbackCapture(const ast::CallExpr& call);

    // §28.14 (v0.180) bounding-object resolution result. The object whose
    // teardown bounds a stored callback: a `using var`/`await using var` local
    // (forms 1/2/5, and form 4 with a local chain head), or `this` (forms 3/4 —
    // an `owned` field of `this`, or a `this`-rooted owned/event-backing member
    // chain). None = no usable bound (capture-free callbacks only).
    struct ScopedBound {
        enum class Kind { None, UsingLocal, This } kind = Kind::None;
        const ast::LocalDecl* local = nullptr;  // valid iff kind == UsingLocal
    };

    // Walk the body once: assign scopedDeclOrder_, record using-var
    // initializer anchors, then check every call with a `scoped` formal
    // (admission) and every use of a `scoped` param / field (callee rules).
    void scopedPreScan(const std::vector<std::unique_ptr<ast::Stmt>>& body);
    void scopedPreScanStmt(const ast::Stmt& s);
    void scopedPreScanExpr(const ast::Expr& e);

    // True when the call's formal at `argPos` is `scoped`. Resolves user
    // methods/ctors via sema symbols, builtin TaskScope.Spawn by receiver
    // type name, and function-VALUE callees via their Function TypeRef.
    //
    // fix #3785 (§22.5 / §28.14, v0.286): the METHOD arm reads the
    // §22.5-BOUND formal, not the source position — passing the call's
    // argument list lets it look the bound formal up by name.
    bool scopedFormalAt(const ast::Expr* callee, const ast::NewExpr* ne,
                        const std::vector<ast::CallArgument>* args,
                        std::size_t argPos);

    // §24.3 / stdlib §6.5, §6.1 (v0.175 signatures; enforced per #1420): true
    // when `callee` is a builtin subscribe-and-store call whose handler formal
    // at `argPos` is `scoped` — `<event>.Subscribe(handler)` (the target
    // resolves to an EventSymbol, or the receiver's static type is
    // EventSource / AsyncEventSource / IEvent / IAsyncEvent) or
    // `<token>.Register(callback)` (receiver type CancellationToken). Both
    // return `owned Subscription` (stdlib §2.11), so the §28.14 bounding
    // object resolves per form 2 (owned return initializing a `using var`) —
    // see scopedBoundingLocal. As of v0.180 (#1541) `EventSource<T>.Add` /
    // `AsyncEventSource<T>.Add` are `scoped` too (§24.3): a bare/qualified
    // `<event>.Add(handler)` (and the `+=` desugaring) admits per §28.14
    // bounding form 4 (the event's containing instance). `Remove` stays
    // call-bounded.
    bool builtinScopedCallbackFormal(const ast::Expr* callee,
                                     std::size_t argPos);
    // Name-head matchers for the builtin receiver types above (the same three
    // TypeRef fallbacks as typeRefIsTaskScope).
    bool typeRefIsEventSourceLike(const sema::TypeRef* rt) const;
    bool typeRefIsCancellationToken(const sema::TypeRef* rt) const;

    // §28.14 call-site admission for one argument bound to a scoped formal.
    // `callNode` is the CallExpr / NullCondCallExpr / NewExpr node; `eventTarget`
    // is non-null only for the `event += handler` store (form 4).
    void checkScopedAdmission(const ast::Expr& callNode,
                              const ast::Expr* callee, const ast::NewExpr* ne,
                              const ast::Expr& arg,
                              const ast::Expr* eventTarget = nullptr);

    // Resolve the §28.14 bounding object of a call: forms 1/2 (using-var
    // initializer), form 3 (owned-return → `owned` field of `this`), forms 4/5
    // (the receiver's member-chain head). ScopedBound::None when no bound is
    // usable (plain expression receiver, borrow-field hop, non-initializer
    // owned return, …). // per §28.14
    ScopedBound scopedBoundingLocal(const ast::Expr& callNode,
                                    const ast::Expr* callee,
                                    const ast::NewExpr* ne);

    // §28.14 (v0.180, #1439/#1545 "scoped construction"): true when `e`'s
    // resolved static type is a non-value-builtin class or interface — the
    // type category `checkScopedIsFunctionTyped` admits alongside function
    // types (TypeResolution_Names.cpp). Used to route a `scoped` call-site
    // argument to the single-source borrow-admission path below instead of
    // the closure-capture path above.
    bool scopedArgIsClassBorrow(const ast::Expr& e) const;

    // §28.14 (v0.180, #1439/#1545): true when `target` is `this.<field>` (or
    // the bare implicit-`this` field spelling) resolving to a FieldSymbol
    // that is NOT `owned` — the admitted storage slot for a class/interface-
    // typed `scoped` parameter (an ORDINARY non-owned back-reference field,
    // §28.5/§28.3 — no `scoped` field marker is used or required for this
    // shape, unlike the function-typed case's `scoped` field).
    bool exprIsReceiverPlainFieldOfThis(const ast::Expr& target) const;

    // §28.14 forms 4/5 (v0.180): resolve the chain head of a subscription
    // receiver (`<recv>.Add`/`+=`, or the receiver of a scoped call). Every hop
    // must be an `owned` field or a compiler-generated event backing member;
    // the head is `this` (forms 3/4) or a `using var` local (form 4 with a
    // local chain head / a form-5 receiver). A bare `this`/`base` *direct*
    // receiver is form 5 and yields no bound (only forms 3/4 reach `this`,
    // through a member hop). A borrow-field hop, an indexer, a getter, or a
    // plain-local head yields None.
    ScopedBound resolveChainHead(const ast::Expr* recv);
    ScopedBound chainHeadRec(const ast::Expr* e);

    // COMPLETE capture enumeration for a lambda argument (statements,
    // expressions, patterns, nested lambdas; `this` is an IdentifierExpr and
    // is collected like any identifier). Unlike collectLambdaIdentifiers this
    // walks every statement/expression form — a missed capture would be a
    // silently-admitted escape. `declared` accumulates the lambda's own
    // params/locals so uses of lambda-internal names are not captures.
    void scopedCollectCaptures(const ast::LambdaExpr& lam,
                               std::vector<const ast::IdentifierExpr*>& out);
    void scopedCaptureWalkStmt(const ast::Stmt& s,
                               std::vector<const ast::IdentifierExpr*>& out);
    void scopedCaptureWalkExpr(const ast::Expr& e,
                               std::vector<const ast::IdentifierExpr*>& out);

    // §28.14 freeze enforcement: report when `name` resolves to a frozen
    // source and the mutation site `loc` is at-or-after the admission site.
    // `action` names the rejected operation for the diagnostic.
    void checkScopedFreeze(std::string_view name, SourceLocation loc,
                           std::string_view action);

    // §28.14 callee verification pre-pass (see the header comment above).
    void scopedCalleeScan(const std::vector<std::unique_ptr<ast::Stmt>>& body);
    void scopedCalleeWalkStmt(const ast::Stmt& s);
    // `allowInvokeOf` lets a CallExpr's callee identifier through (invoke).
    void scopedCalleeWalkExpr(const ast::Expr& e);
    // Report + true when `e` is a bare use of a scoped param / scoped field
    // outside every admitted context.
    bool scopedRestrictedUse(const ast::Expr& e, std::string_view context);
    // True when `e` is an identifier/member-access denoting a `scoped`
    // parameter of THIS method (kind=1) or a `scoped` field (kind=2, bare or
    // this-qualified). Returns 0 otherwise.
    int scopedUseKind(const ast::Expr& e, std::string_view* nameOut = nullptr);

    // -- §28.14 v0.177 (#1419): the call-bounded default ----------------------
    //
    // A PLAIN (no-modifier) function-typed parameter is CALL-BOUNDED in this
    // callee: it may be invoked, bound to locals, and passed to another plain
    // function-typed parameter; it may NOT be stored in a field of any object,
    // stored into a collection reachable beyond the call (parameter-, field-,
    // or borrow-reachable — a purely local collection is a local binding),
    // returned (including a `yield return` or a write through an out/ref
    // parameter, both of which hand the value beyond the call), captured by a
    // closure that itself escapes by these same rules (transitively, followed
    // through local bindings), or forwarded into a `scoped` parameter position
    // (the callee cannot vouch for captures it cannot see). Compiler-verified
    // for user-authored callees — structural sink checks, no interprocedural
    // analysis; builtin callees state their retention in their signatures
    // (`scoped` where they store; §30.11 and the lazy query operators stay on
    // the stdlib-certified legacy path). Runs as a syntactic pre-pass beside
    // scopedCalleeScan, BEFORE scopedPreScan so the alias map is complete when
    // the §28.14 admission consults it for the forward-to-scoped shapes.
    // // per §28.14, §28.3, §22.3 (v0.177)

    // The enclosing method's plain function-typed parameters.
    std::unordered_set<sema::Symbol*> callBoundedParams_;
    // Locals that transitively CARRY a call-bounded parameter: bound from the
    // parameter directly, from a closure capturing one (or capturing another
    // carrier), from a conditional over carriers, or from a collection literal
    // containing one. Maps the carrier to the parameter it carries. Recorded
    // in source order; flow-insensitive (an alias is never un-recorded —
    // conservative, may over-reject a rebound name, never under-rejects).
    std::unordered_map<sema::Symbol*, sema::ParameterSymbol*> callBoundedAlias_;
    // Locals whose initializer is ROOTED at a parameter / field / `this` /
    // call result — a collection reached through one is reachable beyond the
    // call. (Only consulted for collection receivers.)
    std::unordered_set<sema::Symbol*> beyondCallRooted_;

    // The pre-pass driver + walkers (statement coverage mirrors
    // scopedCalleeWalkStmt).
    void callBoundedScan(const std::vector<std::unique_ptr<ast::Stmt>>& body);
    void callBoundedWalkStmt(const ast::Stmt& s);
    void callBoundedWalkExpr(const ast::Expr& e);
    // #1432 (§28.14 v0.177): a LAMBDA is a user-authored callee too — its OWN
    // plain function-typed parameters are call-bounded within its own body,
    // exactly as an enclosing method's/constructor's formals are (the seeding
    // in run() above). Called from callBoundedWalkExpr's LambdaExpr case
    // before descending into the body; inserts each eligible parameter symbol
    // (looked up via ctx_.lambdaParams, position-for-position with
    // lam.params) into callBoundedParams_. NOT popped after the body walk:
    // scopedPreScan's deferred checkScopedAdmission queue runs AFTER the
    // whole callBoundedScan walk finishes and must still see a lambda's own
    // parameter as call-bounded if it is forwarded to a `scoped` position
    // from inside that same lambda's body. Sound because each lambda
    // occurrence mints its own distinct ParameterSymbol* — no two lambdas
    // (sibling or nested) ever share one, so a resident entry cannot make an
    // unrelated identifier resolve to it. // per §28.14, v0.177, #1432
    void seedLambdaCallBoundedParams(const ast::LambdaExpr& lam);
    // The call-bounded parameter `e` denotes or transitively carries (bare
    // identifier, carrier local, capturing lambda, conditional, collection
    // literal, postfix-bang), or null.
    sema::ParameterSymbol* callBoundedSourceOf(const ast::Expr& e);
    // True when `recv` (a collection receiver / index-store target) is
    // reachable beyond the call: rooted at a parameter, field, `this`, a call
    // result, or a beyond-call-rooted local. Conservative for unresolved
    // roots.
    bool exprReachableBeyondCall(const ast::Expr& recv);
    // True when `callee` is a builtin-collection STORING mutator
    // (Add / Insert / Push / Enqueue / …) — the §28.14 collection-store sink.
    // User methods never match (their formals carry their own obligations).
    bool builtinCollectionStoreCallee(const ast::Expr* callee);
    // Report the §28.14 v0.177 sink violation, naming the parameter and the
    // spec-stated fix.
    void reportCallBounded(SourceLocation loc, std::string_view paramName,
                           std::string_view sink);
    // True when the call's formal at `argPos` resolves to a USER-authored
    // PLAIN function-typed parameter — a position v0.177 verifies callee-side
    // as call-bounded, so the CALLER may pass any capture-bearing closure
    // there (checkClosureEscapesOnReturn skips its legacy rejection for such
    // arguments). Builtin callees (the lazy query operators' storing
    // signatures), function-value callees, and unresolved callees stay on the
    // legacy caller-side rejection — the stdlib-certified path §28.14 v0.177
    // records. // per §28.14, §28.3 (v0.177)
    //
    // fix #3785 (§22.5 / §28.14, v0.286): the METHOD arm reads the
    // §22.5-BOUND formal, not the source position — passing the call's
    // argument list lets it look the bound formal up by name.
    bool verifiedCallBoundedFormalAt(const ast::Expr* callee,
                                     const ast::NewExpr* ne,
                                     const std::vector<ast::CallArgument>* args,
                                     std::size_t argPos);

    // -- §28.14 v0.179 (#1527): call-bounded × TASK-ABANDONMENT escape sink --
    //
    // A call-bounded callback's OWN signature may itself return a Task-family
    // value (`() -> Task`, `(int) -> Task<T>`, …). INVOKING it mints a task
    // that "inherits the call bound" (§28.14 v0.179, Appendix C.6): a
    // synchronous callee has no consuming position for the returned task (no
    // `await`), so the invocation itself is rejected — declare the parameter
    // `scoped` or make the callee `async`. An async callee must await the
    // minted task to settlement, on every normal-exit path, before it
    // returns — directly, or by accumulating it in a purely-local collection
    // fully discharged by an aggregation-helper await (`await
    // Task.WhenAll(pending)`); returning it, storing it, passing it to
    // `Task.FireAndForget`, or binding it and dropping it undisposed-to-
    // settlement is the same escape sink the other §28.14 sinks are.
    //
    // Runs as its own structural pre-pass AFTER callBoundedScan (needs the
    // completed callBoundedAlias_ map for the local-rebinding idiom `var g :=
    // f; …`). Deliberately does NOT descend into a nested LambdaExpr's own
    // body (mirrors checkTaskScopeConsumption's own scope): a lambda's
    // async-ness may differ from the enclosing callable's, and #1432 already
    // scoped the call-bounded machinery's lambda interactions narrowly —
    // extending task-settlement tracking across that boundary is left to a
    // follow-up issue if evidence demands it. Likewise recognizes the
    // invocation only at a fixed set of STATEMENT-level positions (a
    // LocalDecl initializer, an assignment RHS, a return/discard value, a
    // fan-out `.Add(...)` argument, a bare expression-statement, an `await`
    // operand) — a task-returning invocation reached only through some OTHER
    // nested expression position (an argument to an unrelated call, a
    // chained member access, …) is not specifically recognized; a deliberate,
    // recorded "provable subset" scope, the same posture v1 admission bound
    // #3 already states for this section. // per §28.14 v0.179, Appendix C.6
    void checkCallBoundedTaskSettlement(
        const std::vector<std::unique_ptr<ast::Stmt>>& body);
    // True + resolves the invoked call-bounded parameter's name when `ce`
    // invokes THIS callable's own call-bounded parameter (or a local alias,
    // §28.14 v0.177's carrier machinery) — the CALLEE position, not an
    // argument — AND `ce`'s result type is Task-family
    // (`callResultIsAsyncTask`) — the §28.14 v0.179 (2) invocation shape.
    bool callBoundedTaskInvocation(const ast::CallExpr& ce,
                                   std::string_view* paramNameOut);

    // -- stdlib §6.10 (v0.175, #808): TaskScope consumption rules ------------
    // TaskScope may only be consumed by `await using`; a plain `using var`, an
    // undisposed drop, or an explicit .Dispose() is a compile error naming the
    // rule; `await L.Join()` must appear on the normal path (checked
    // syntactically: error only when no such await exists AND the scope local
    // is never delegated as a call argument — the provable subset).
    bool exprTypeIsTaskScope(const ast::Expr& e) const;
    bool typeRefIsTaskScope(const sema::TypeRef* rt) const;
    void checkTaskScopeConsumption(
        const std::vector<std::unique_ptr<ast::Stmt>>& body);
    // Scan helpers: does `await <name>.Join()` / a bare-`name` argument occur?
    void taskScopeUsageScanStmt(const ast::Stmt& s, std::string_view name,
                                bool& sawAwaitJoin, bool& sawDelegation);
    void taskScopeUsageScanExpr(const ast::Expr& e, std::string_view name,
                                bool& sawAwaitJoin, bool& sawDelegation);
};

} // namespace bezel::ownership
