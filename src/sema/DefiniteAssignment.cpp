#include "sema/DefiniteAssignment.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "ast/Node.hpp"
#include "sema/EnumValues.hpp"  // enumHasZeroConstant — §7.9 / §17.11 enum row (#3072)
#include "sema/SemaContext.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeRef.hpp"

namespace bezel::sema {

namespace {

// ---------- §22.7 (v0.184) construction-time `this`-observation helpers ------
//
// The observation gate (§22.7 rule 1) is satisfied only where every field the
// declaring class must explicitly initialize is definitely assigned. That set
// is exactly §7.9's non-nullable *reference*-typed fields without an
// initializer (fields with an initializer count as assigned at body entry;
// value-typed fields have a defined default) — plus, per §7.9 v0.247, a
// zero-less enum field (the enum CARVE-OUT, see §17.11: an enum without a
// declared zero-valued constant has no well-defined default, so its storage
// is likewise §7.9-strict). Mirrors ownership's isNonNullableReferenceField
// (the O9 field-DA check) — same classification, so the observation gate and
// the field-DA obligation stay in lock-step. The R-2 deferred rows
// (records with non-nullable reference fields, inline arrays of non-nullable
// reference elements, owning inline arrays) are intentionally NOT folded in
// here — landing the enum row alone keeps the fix narrowly scoped (#3072).
bool isSevenNineStrictField(const ast::FieldDecl& f, SemaContext& ctx) {
    for (auto m : f.modifiers)
        if (m == "static") return false;
    if (f.initializer) return false;  // assigned at body entry
    if (!f.type) return false;
    auto* rt = ctx.resolvedType(*f.type);
    if (!rt || rt->nullable) return false;
    switch (rt->kind) {
        case TypeRef::Kind::Primitive:
            return rt->primitive == PrimitiveType::String ||
                   rt->primitive == PrimitiveType::Object;
        case TypeRef::Kind::Function:
            return true;
        case TypeRef::Kind::Named:
            if (!rt->declaration) return false;  // built-in / generic — skip
            // fix #3072 / §7.9 / §17.11: a zero-less enum field has no
            // well-defined default — `default(E)` is ill-formed for it
            // (and zero-fill leaves the slot holding an undeclared value, in
            // violation of §5.4's invariant). Treat it as §7.9-strict so the
            // constructor must assign it on every normal-exit path. An enum
            // whose declared-value set is incomplete (a non-constant
            // initializer, already diagnosed) is NOT rejected: enumHasZero
            // Constant returns nullopt and the conservative posture is to
            // accept — matching §17.11's `defaultIllFormed` arm.
            if (rt->declaration->kind == ast::TypeDeclKind::Enum) {
                std::optional<bool> hasZero =
                    enumHasZeroConstant(*rt->declaration);
                return hasZero && !*hasZero;
            }
            return rt->declaration->kind == ast::TypeDeclKind::Class ||
                   rt->declaration->kind == ast::TypeDeclKind::Interface;
        default:
            return false;  // Unit, InlineArray, Error → value/unknown
    }
}

// §22.7 (v0.184): a non-nullable-reference *auto-property* without an
// initializer is §7.9-strict backing storage too (§22.7: "auto-property
// assignments and reads … each is the backing field's assignment or read"), so
// it joins the observation gate's needing-set exactly like a field. Indexers,
// static, and initialized auto-properties are excluded (the last is assigned at
// entry). A computed (block/expression-bodied) property has no backing storage
// and is not counted. fix #3072: the enum CARVE-OUT (zero-less enum) is
// treated as §7.9-strict for the same reason as a field — auto-prop storage
// mints an undeclared value at construction when zero-filled.
//
// fix #4018 / §12.5 (found fixing #4012): the `abstract` shorthand
// (`abstract property Name: T;`) parses with this exact same
// `bodyKind == Auto` but has NO backing storage at all — a subclass supplies
// the real implementation. Before this fix `runConstructor`'s caller (below)
// added such a property's name to `fieldsNeedingInit_` for EVERY constructor
// of the ABSTRACT class that declared it, so an abstract base class with any
// explicit constructor false-rejected on "constructor does not initialize
// non-nullable field `Name`" even though there is no field `Name` to
// initialize — the very shape the closed sibling fix #4012 (Ownership.cpp's
// `isNonNullableReferenceAutoProp`) had already excluded on its own path.
// Uses the shared `isAbstractPropertyShorthand` (sema/Symbol.hpp) rather than
// a third copy of the modifier scan.
bool isSevenNineStrictAutoProp(const ast::PropertyDecl& p, SemaContext& ctx) {
    if (p.bodyKind != ast::PropertyBodyKind::Auto) return false;
    if (p.isIndexer || p.autoInitializer) return false;
    if (isAbstractPropertyShorthand(p)) return false;
    for (auto m : p.modifiers)
        if (m == "static") return false;
    if (!p.type) return false;
    auto* rt = ctx.resolvedType(*p.type);
    if (!rt || rt->nullable) return false;
    switch (rt->kind) {
        case TypeRef::Kind::Primitive:
            return rt->primitive == PrimitiveType::String ||
                   rt->primitive == PrimitiveType::Object;
        case TypeRef::Kind::Function:
            return true;
        case TypeRef::Kind::Named:
            if (!rt->declaration) return false;
            // fix #3072 / §7.9 / §17.11: zero-less enum auto-prop storage is
            // §7.9-strict (same reasoning as isSevenNineStrictField).
            if (rt->declaration->kind == ast::TypeDeclKind::Enum) {
                std::optional<bool> hasZero =
                    enumHasZeroConstant(*rt->declaration);
                return hasZero && !*hasZero;
            }
            return rt->declaration->kind == ast::TypeDeclKind::Class ||
                   rt->declaration->kind == ast::TypeDeclKind::Interface;
        default:
            return false;
    }
}

// fix #4015 / §7.9: the STATIC twin of isSevenNineStrictField above — §7.9's
// own text requires assignment "in every constructor (for instance fields) or
// in the static constructor (for static fields)" generically, and nothing in
// DefiniteAssignment.cpp or Ownership.cpp screened the static case at all: a
// non-nullable, non-defaultable `static readonly` field with no static
// constructor and no field initializer compiled clean and SIGSEGVed on first
// read, identically for a class and a record (issue #4015). A separate
// function rather than a `wantStatic` parameter on isSevenNineStrictField
// itself, deliberately: the type-kind switch below is IDENTICAL to
// isSevenNineStrictField's, so the two functions differ only in which
// `static` reading they require, but this file's static- and instance-field
// no-ctor screens are used by different callers (checkStaticNoCtorField
// Completion below vs. checkRecordNoCtorFieldCompletion/runConstructor/
// runRecordConstructor) with no shared call site that would benefit from a
// single parameterized function, and keeping them separate means neither
// reads its own boolean argument backwards at a call site.
bool isSevenNineStrictStaticField(const ast::FieldDecl& f, SemaContext& ctx) {
    bool isStatic = false;
    for (auto m : f.modifiers)
        if (m == "static") { isStatic = true; break; }
    if (!isStatic) return false;
    if (f.initializer) return false;  // assigned at body entry
    if (!f.type) return false;
    auto* rt = ctx.resolvedType(*f.type);
    if (!rt || rt->nullable) return false;
    switch (rt->kind) {
        case TypeRef::Kind::Primitive:
            return rt->primitive == PrimitiveType::String ||
                   rt->primitive == PrimitiveType::Object;
        case TypeRef::Kind::Function:
            return true;
        case TypeRef::Kind::Named:
            if (!rt->declaration) return false;  // built-in / generic — skip
            if (rt->declaration->kind == ast::TypeDeclKind::Enum) {
                std::optional<bool> hasZero =
                    enumHasZeroConstant(*rt->declaration);
                return hasZero && !*hasZero;
            }
            return rt->declaration->kind == ast::TypeDeclKind::Class ||
                   rt->declaration->kind == ast::TypeDeclKind::Interface;
        default:
            return false;  // Unit, InlineArray, Error → value/unknown
    }
}

// fix #4015 / §7.9: the STATIC auto-property twin, mirroring
// isSevenNineStrictStaticField exactly as isSevenNineStrictAutoProp mirrors
// isSevenNineStrictField. Deliberately its OWN function rather than a
// parameter on isSevenNineStrictAutoProp: per the R15 landmine note (#4018/
// #4020, still open), `abstract property Name: T;` (§12.5) parses with the
// SAME PropertyBodyKind::Auto a real auto-property gets, so any predicate
// walking auto-properties must exclude `abstract` explicitly or it
// misclassifies a signature-only member as backing storage needing
// assignment. MEASURED: `static abstract property Name: T;` is independently
// rejected ("static member `Name` may not be virtual, abstract, or override
// (§12.1)", TypeResolution_Checks.cpp:597/789/914) before this pass ever
// runs, so requiring `isStatic` already excludes every case this predicate
// could reach in a program that compiles this far. The explicit `abstract`
// exclusion below is kept anyway so this function reads as self-evidently
// correct against the landmine without relying on that separate, differently
// -scoped rejection remaining in place.
bool isSevenNineStrictStaticAutoProp(const ast::PropertyDecl& p,
                                      SemaContext& ctx) {
    if (p.bodyKind != ast::PropertyBodyKind::Auto) return false;
    if (p.isIndexer || p.autoInitializer) return false;
    bool isStatic = false, isAbstract = false;
    for (auto m : p.modifiers) {
        if (m == "static") isStatic = true;
        else if (m == "abstract") isAbstract = true;
    }
    if (!isStatic || isAbstract) return false;
    if (!p.type) return false;
    auto* rt = ctx.resolvedType(*p.type);
    if (!rt || rt->nullable) return false;
    switch (rt->kind) {
        case TypeRef::Kind::Primitive:
            return rt->primitive == PrimitiveType::String ||
                   rt->primitive == PrimitiveType::Object;
        case TypeRef::Kind::Function:
            return true;
        case TypeRef::Kind::Named:
            if (!rt->declaration) return false;
            if (rt->declaration->kind == ast::TypeDeclKind::Enum) {
                std::optional<bool> hasZero =
                    enumHasZeroConstant(*rt->declaration);
                return hasZero && !*hasZero;
            }
            return rt->declaration->kind == ast::TypeDeclKind::Class ||
                   rt->declaration->kind == ast::TypeDeclKind::Interface;
        default:
            return false;
    }
}

// True when `e` is the bare `this`/`base` keyword (stored as an IdentifierExpr
// with the keyword lexeme, per Parser_Exprs §22.3).
bool isNamedIdent(const ast::Expr& e, std::string_view n) {
    auto* id = dynamic_cast<const ast::IdentifierExpr*>(&e);
    return id && id->name == n;
}

// §22.7 rule 2: a `virtual`/`override`/`abstract` member (dispatching family).
bool methodIsVirtualFamily(const MethodSymbol& ms) {
    for (auto m : ms.modifiers)
        if (m == "virtual" || m == "override" || m == "abstract") return true;
    if (auto* md = dynamic_cast<const ast::MethodDecl*>(ms.decl))
        return md->bodyKind == ast::MethodBodyKind::Abstract;
    return false;
}
bool propIsVirtualFamily(const PropertySymbol& ps) {
    for (auto m : ps.modifiers)
        if (m == "virtual" || m == "override" || m == "abstract") return true;
    return false;
}

// §19.3/§19.6: an unguarded `_` (DiscardPattern) or unconstrained `var x`
// (BindingPattern with no type annotation, no `when` guard) matches any value —
// a trivial catch-all arm. Duplicated here rather than reused because
// ExprTypecheck.cpp's identically-named helper has internal (anonymous-
// namespace) linkage in that translation unit. Deliberately narrow — it
// recognizes only the two syntactic forms the spec calls out, not a full
// enum/bool coverage proof — so the CaseStmt join below stays conservative: a
// case lacking this explicit catch-all always carries a live no-match fall-
// through path (§19.6), never accidentally exhaustive. fix #1681
bool caseArmIsUnguardedCatchAll(const ast::CaseArm& arm) {
    if (arm.guard) return false;
    const ast::Pattern* p = arm.pattern.get();
    if (dynamic_cast<const ast::DiscardPattern*>(p)) return true;
    if (auto* bp = dynamic_cast<const ast::BindingPattern*>(p))
        return bp->type == nullptr;
    return false;
}

// fix #1753 / §17.x, §22.3, §22.7: join the per-branch definitely-assigned
// sets of an if/try/case, intersecting *only* the branches that do NOT provably
// diverge (throw/return). A branch flagged `terminated[i]` reaches no path to
// the join point, so — per "definitely assigned on every REACHING path" — it
// must contribute nothing to the join; including it wrongly shrinks the
// intersection and over-rejects (`if c then x:=1 else return end; _:=x`, or the
// field analog before a `this`-observation). A branch that falls through is
// still intersected, so a genuinely-unassigned read on some live path is still
// caught (no under-reject). If *every* branch diverges the join is unreachable;
// fall back to the whole-branch intersection (prior behavior) so no dead-code
// read that previously passed regresses — soundness is unaffected there because
// no reachable path observes the result. `entry` is used only for the
// degenerate no-branch case.
std::unordered_set<std::string_view> joinLiveBranches(
    const std::vector<std::unordered_set<std::string_view>>& outs,
    const std::vector<bool>& terminated,
    const std::unordered_set<std::string_view>& entry) {
    std::unordered_set<std::string_view> result;
    bool any = false;
    for (std::size_t i = 0; i < outs.size(); ++i) {
        if (i < terminated.size() && terminated[i]) continue;  // diverging arm
        if (!any) {
            result = outs[i];
            any = true;
        } else {
            std::unordered_set<std::string_view> tmp;
            for (auto n : result)
                if (outs[i].count(n)) tmp.insert(n);
            result = std::move(tmp);
        }
    }
    if (any) return result;
    if (outs.empty()) return entry;
    // All branches diverge → unreachable join; preserve prior full intersection.
    result = outs.front();
    for (std::size_t i = 1; i < outs.size(); ++i) {
        std::unordered_set<std::string_view> tmp;
        for (auto n : result)
            if (outs[i].count(n)) tmp.insert(n);
        result = std::move(tmp);
    }
    return result;
}

// fix #2187 / §22.8: join the per-branch *may-be-assigned* sets of an
// if/try/case for the readonly single-assignment tracker. Where the §7.9/§22.7
// definite-assignment tracker above uses INTERSECTION (a field is assigned only
// where assigned on every reaching path), single-assignment enforcement needs
// the dual: a readonly field is *possibly already assigned* after the construct
// if it was assigned on ANY live branch that reaches the join — a UNION over the
// non-terminated branches. A branch that provably diverges (throw/return)
// reaches no join, so its assignments cannot cause a later double-assign on a
// path that flows past the construct — excluding it (parallel to
// joinLiveBranches) is what keeps `if c then _f:=a; return; else end; _f:=b;`
// accepted (the only live path never assigned `_f`). Each branch out already
// starts from `entry` (the caller reset it), so the union is ⊇ entry. If every
// branch diverges the join is unreachable; return entry (value irrelevant, no
// reachable assignment follows).
std::unordered_set<std::string_view> joinUnionLive(
    const std::vector<std::unordered_set<std::string_view>>& outs,
    const std::vector<bool>& terminated,
    const std::unordered_set<std::string_view>& entry) {
    std::unordered_set<std::string_view> result;
    bool any = false;
    for (std::size_t i = 0; i < outs.size(); ++i) {
        if (i < terminated.size() && terminated[i]) continue;  // diverging arm
        for (auto n : outs[i]) result.insert(n);
        any = true;
    }
    if (!any) return entry;
    return result;
}

// §22.3 (fix #2595, #2631): does this parameter list declare a callee-side
// `out` parameter (plain `out` or `out owned`)? The `out var` spelling is a
// caller-side inline-declaration modifier, never a parameter mode — same
// exclusion MethodDA::seedOutParams makes. Forward-declared so the lambda-body
// walk in MethodDA::visitExpr's LambdaExpr arm (#2631) can gate on it without
// having to inline the predicate; the canonical definition is below the
// MethodDA class.
bool hasOutParam(const std::vector<ast::Param>& params);

// Per-method walker. The state is the set of locally-declared names
// that have been definitely assigned at the current program point.
// Names enter the set when:
//   * a LocalDecl with an initializer is processed,
//   * an AssignmentStmt sets a previously-declared local,
//   * an out-argument is passed to a call (§22.3 — definitely-assigned after),
//   * a `for` loop's iteration variable is introduced.
//
// Out-parameters of the current method are seeded into declared_ (not
// assigned_) so they start unassigned. They must be assigned on every
// exit path (§22.3 callee-side check — fix #925).
//
// Reads consult the set; an IdentifierExpr that names a local declared
// in the current scope set (or any nested scope along the walk) but
// *not* in the assigned set is reported.
class MethodDA {
public:
    explicit MethodDA(SemaContext& ctx) : ctx_(ctx) {}

    // Run the walker. `params` lists the method's parameters. By-value
    // and `ref` params are definitely-assigned on entry (the caller
    // bound them). `out` params are NOT assigned on entry — the callee
    // must assign them on every exit path (§22.3); they are seeded into
    // `declared_` (so reads are tracked) but not into `assigned_`.
    void run(const std::vector<ast::Param>& params,
             const std::vector<std::unique_ptr<ast::Stmt>>& body) {
        seedOutParams(params);
        for (const auto& s : body) {
            if (s) visitStmt(*s);
        }
        // §22.3 fall-off check: if control can reach the end of the method
        // without a return/throw, every out-param must be assigned.
        if (!terminated_) checkOutParams(/*span=*/nullptr);
    }

    // §22.3 (v0.236, fix #2595): an EXPRESSION-bodied method `=> E;` has
    // exactly one normal-completion path, and the value it returns is `E` —
    // semantically the `return E;` the block form would spell. The walker was
    // never entered for this body kind (checkMember gated on
    // MethodBodyKind::Block), so an `out` parameter on an expression-bodied
    // method was never seeded and checkOutParams never ran: `method F(out n:
    // int): int => 5;` was ADMITTED, and the caller's recipient was left
    // holding unassigned storage — §22.3: "The callee must assign every `out`
    // parameter on every normal-completion (return) path … failing to do so is
    // a compile error."
    //
    // The check is a walk, NOT a blanket rejection of `out` on expression
    // bodies: an expression body CAN discharge the obligation by passing the
    // parameter on as an `out` argument (`=> Helper(out n)` — a §22.3
    // assignable storage location, since within the callee an `out` parameter
    // "denotes a callee-local slot"). That shape compiles and works today, so
    // rejecting `out` at the declaration would be a false reject on working
    // code. Walking `E` and then running the ordinary §22.3 check at the single
    // return point gives exact parity with the block form: `visitExpr` marks an
    // `out`-argument operand assigned and reports a read of a still-unassigned
    // out parameter, so `=> Helper(out n)` is accepted, `=> 5` and `=> n + 1`
    // are rejected with the same diagnostics their `return …;` counterparts
    // already produce.
    void runExpressionBody(const std::vector<ast::Param>& params,
                           const ast::Expr& body) {
        seedOutParams(params);
        sawUnanalyzedForm_ = false;
        visitExpr(body);
        // §17.12: "`throw` is an expression … Valid in any expression
        // position." §22.3 exempts it: "a path that exits by `throw` carries
        // no assignment obligation". A `=> throw E;` body therefore has NO
        // normal-completion path and owes nothing — exactly like the block
        // form's `throw E;`, whose ThrowStmt arm sets `terminated_` and is
        // skipped by the fall-off check at the end of `run`. Route the
        // expression form through the SAME `terminated_` gate rather than
        // adding a second exemption mechanism.
        //
        // Set here, on the BODY node, and deliberately NOT inside visitExpr's
        // ThrowExpr arm: a `throw` nested in a sub-expression does not
        // terminate the enclosing statement (`(if c then 1 else throw E)`
        // completes normally on the `then` path), so setting the flag there
        // would suppress legitimate fall-off §22.3 rejections on the BLOCK
        // path. Only a body that IS the throw diverges unconditionally.
        if (dynamic_cast<const ast::ThrowExpr*>(&body)) terminated_ = true;
        // §22.3: report only when the walk actually understood the whole body.
        // visitExpr has no arm for several composite forms (see
        // `sawUnanalyzedForm_`), and an `out` argument nested inside one is
        // invisible to it — reporting there would be a FALSE REJECT on code
        // that does discharge the obligation. Staying silent restores exactly
        // the pre-fix behavior for those shapes (they were never checked at
        // all) rather than inventing a wrong answer.
        if (!terminated_ && !sawUnanalyzedForm_)
            checkOutParams(&body.span.begin);
    }

    // No-param overload for destructors (which have no params).
    void run(const std::vector<std::unique_ptr<ast::Stmt>>& body) {
        for (const auto& s : body) {
            if (s) visitStmt(*s);
        }
        // No out-params to check.
    }

    // §22.7 (v0.184): walk a constructor body with construction-time
    // `this`-observation checking layered onto the ordinary local
    // definite-assignment walk. `klass` is the declaring class (for the
    // §7.9-strict field set, the sealed/abstract status, and rule 2). The walk
    // is the same flow-sensitive pass as `run`; the extra state is
    // `ctorMode_`/`assignedFields_`/`fieldsNeedingInit_` (see visitStmt/
    // visitExpr for where observations are gated).
    void runConstructor(const ast::ConstructorDecl& cd,
                        const ast::TypeDecl& klass) {
        ctorMode_ = true;
        roCtorMode_ = true;  // fix #2187: readonly single-assignment tracking
        bool sealed = false, abstract = false;
        for (auto m : klass.modifiers) {
            if (m == "sealed") sealed = true;
            if (m == "abstract") abstract = true;
        }
        classSealedNonAbstract_ = sealed && !abstract;
        // fix #2187 / §22.8: readonly INSTANCE fields that carry a field
        // initializer are assigned at step 2 — before a non-chaining ctor's body
        // runs — so a body assignment to one is a SECOND assignment. Collect
        // them here; they are seeded into `readonlyMaybeAssigned_` after the
        // chaining decision below (a `this(...)`-chaining ctor skips step 2, so
        // its initializers did not run in THIS ctor — see the seeding site).
        std::vector<std::string_view> readonlyInitialized;
        for (const auto& m : klass.members) {
            if (auto* f = dynamic_cast<const ast::FieldDecl*>(m.get())) {
                if (isSevenNineStrictField(*f, ctx_))
                    fieldsNeedingInit_.push_back(f->name);
                bool isStatic = false, isReadonly = false;
                for (auto mod : f->modifiers) {
                    if (mod == "static") isStatic = true;
                    else if (mod == "readonly") isReadonly = true;
                }
                if (isReadonly && !isStatic && f->initializer)
                    readonlyInitialized.push_back(f->name);
            } else if (auto* p =
                           dynamic_cast<const ast::PropertyDecl*>(m.get())) {
                if (isSevenNineStrictAutoProp(*p, ctx_))
                    fieldsNeedingInit_.push_back(p->name);
            }
        }
        // Seed out-params (constructors normally have none; kept for parity
        // with run() so an out-carrying ctor, if ever admitted, is checked).
        for (const auto& p : cd.params) {
            if (!p.name.empty() && p.modifier.rfind("out", 0) == 0 &&
                p.modifier.rfind("out var", 0) != 0) {
                outParams_.insert(p.name);
                declared_.insert(p.name);
            }
        }
        // §22.7 rule 1, chaining position: the `base(...)`/`this(...)` call is
        // the body's first statement (Node.hpp: CtorChain surfaces as an
        // ExpressionStmt of a CallExpr on `this`/`base`). Its arguments run
        // before any field of the declaring class is assigned, so any
        // observation of `this` there is an error. After a `this(...)` chain the
        // chained-to constructor has discharged every §7.9-strict field, so the
        // gate is satisfied on body entry; after `base(...)` (or the implicit
        // `base()`) the field initializers run but the needing-set is still
        // unassigned (the body must assign it).
        std::size_t startIdx = 0;
        bool isThisChain = false;
        if (const ast::CallExpr* chain = getChainingCall(cd, isThisChain)) {
            startIdx = 1;
            inChainingPosition_ = true;
            for (const auto& a : chain->args) visitCallArg(a);
            inChainingPosition_ = false;
            if (isThisChain) {
                for (auto n : fieldsNeedingInit_) assignedFields_.insert(n);
            }
        }
        // fix #2187 / §22.8: seed initializer-carrying readonly fields as
        // already-assigned for a base/implicit-base ctor (its field initializers
        // ran as step 2). A `this(...)`-chaining ctor SKIPS step 2 — its
        // initializers ran in the chained-to ctor — so seeding from initializers
        // here would be wrong; the chained-to ctor's own single-assignment is
        // checked when THAT ctor is walked. (Cross-ctor once-ness — a chaining
        // ctor re-assigning a field the chained-to ctor already set — is the
        // residual noted in the commit; this slice enforces within-ctor and
        // initializer+body once-ness, which closes the probe-confirmed exploit.)
        if (!isThisChain)
            for (auto n : readonlyInitialized) readonlyMaybeAssigned_.insert(n);
        for (std::size_t i = startIdx; i < cd.body.size(); ++i) {
            if (cd.body[i]) visitStmt(*cd.body[i]);
        }
        if (!terminated_) {
            checkOutParams(/*loc=*/nullptr);
            // §7.9 (Appendix C.1): the constructor's fall-off is a normal-exit
            // path — every non-nullable field must be definitely assigned here.
            // Reported at the constructor's own location (there is no falling-
            // off statement to point at).
            checkCtorFieldCompletion(&cd.span.begin);
        }
    }

    // fix #2187 / §22.8: walk a STATIC constructor body with readonly
    // single-assignment enforcement — the static analogue of runConstructor's
    // readonly tracking. §7.4 narrows `static readonly` fields too ("instance or
    // static"), so a double assignment in a static constructor is the same
    // soundness hole as the instance case. There is no instance under
    // construction, so none of §22.7's `this`-observation gating applies (this
    // walk stays in the ordinary local-DA regime, `ctorMode_` false); static
    // constructors take no parameters. `static readonly` fields carrying a
    // static field initializer run before the static-ctor body (§22.12), so they
    // are seeded as already-assigned, exactly like the instance initializer case.
    //
    // fix #4015 / §7.9: this walk previously tracked ONLY the readonly
    // single-assignment obligation (`roCtorMode_`), never the field-
    // completion obligation §7.9's own text states for a static constructor
    // ("assignment ... in the static constructor, for static fields") — so a
    // static readonly field a declared static constructor simply never got
    // around to assigning (every live path, not merely a double-assign) fell
    // off the end unset, and reading it back SIGSEGVed exactly as the
    // no-static-ctor-at-all case did (MEASURED: `static constructor() {
    // Other := 5; }` with `Other` unrelated to `static readonly Shared:
    // Widget;` compiled clean and crashed identically to no static
    // constructor at all). `staticCtorMode_` reuses the SAME two class-
    // agnostic mechanisms `recordCtorMode_` already reuses for records
    // (fix #4001's own comment on `runRecordConstructor` names them: marking
    // a store toward `assignedFields_`, and the fall-off completeness check)
    // — never `ctorMode_` itself, so none of the §22.7 this-observation
    // gating (which reads `ctorMode_` specifically, never `recordCtorMode_`
    // or this flag) is pulled in; a static constructor has no `this` to
    // observe. Callable for BOTH a class's and a record's static constructor
    // — nothing in this body is class- or record-specific — so checkMember's
    // ConstructorDecl dispatch below routes `cd->isStatic && inRecord`
    // through this same function, closing a second, separate gap that
    // existed alongside the field-completion one: a record's static
    // constructor previously matched NONE of checkMember's typed branches
    // and fell to the untyped `da.run` path, so it got no readonly single-
    // assignment tracking either (MEASURED: a record static constructor that
    // assigned the same `static readonly` field twice compiled with no
    // §22.8 diagnostic, run silently taking the second value — issue #4015's
    // repro sibling, closed by this same routing rather than filed
    // separately since it is the identical missing-dispatch-branch root
    // cause).
    void runStaticConstructor(const ast::ConstructorDecl& cd,
                              const ast::TypeDecl& klass) {
        roCtorMode_ = true;
        staticCtorMode_ = true;
        for (const auto& m : klass.members) {
            if (auto* f = dynamic_cast<const ast::FieldDecl*>(m.get())) {
                // fix #4015 / §7.9: a static field with no initializer is
                // the same §7.9-strict-storage shape isSevenNineStrictField
                // classifies for an instance field — isSevenNineStrictStatic
                // Field is its static-required twin (see its comment).
                if (isSevenNineStrictStaticField(*f, ctx_))
                    fieldsNeedingInit_.push_back(f->name);
                if (!f->initializer) continue;
                bool isStatic = false, isReadonly = false;
                for (auto mod : f->modifiers) {
                    if (mod == "static") isStatic = true;
                    else if (mod == "readonly") isReadonly = true;
                }
                if (isStatic && isReadonly)
                    readonlyMaybeAssigned_.insert(f->name);
            } else if (auto* p = dynamic_cast<const ast::PropertyDecl*>(
                           m.get())) {
                // fix #4015 / §7.9: the static auto-property analogue —
                // "auto-property assignments and reads ... each [are] the
                // backing field's assignment or read" (§22.7) applies to a
                // static auto-property's backing storage exactly as to an
                // instance one.
                if (isSevenNineStrictStaticAutoProp(*p, ctx_))
                    fieldsNeedingInit_.push_back(p->name);
            }
        }
        for (const auto& s : cd.body) {
            if (s) visitStmt(*s);
        }
        if (!terminated_) {
            // fix #4015 / §7.9 (Appendix C.1): the static constructor's
            // fall-off is a normal-exit path too — every §7.9-strict static
            // field/auto-property must be assigned by every live path
            // through it, mirroring runConstructor's/runRecordConstructor's
            // own call.
            checkCtorFieldCompletion(&cd.span.begin);
        }
    }

    // fix #2207 / §22.8: walk a record (value-type) instance-constructor body
    // with readonly single-assignment enforcement. §22.8 says "A field declared
    // in a class or record may include an initializer" and the readonly
    // single-assignment rule it states (§7.4/#2116 depend on it) applies to
    // records too. Class ctors get the check via runConstructor; static ctors
    // via runStaticConstructor. A record's authored ctor previously fell to
    // the plain `da.run` path with `roCtorMode_` false, so `_f := a; _f := b;`
    // inside a record ctor compiled — and §7.4/#2116 narrowing applies to
    // record readonly fields too, so the #2187 exploit (a stored-lambda
    // ambient fact derefing a value the ctor later nulled) reproduced on
    // records (probe-confirmed SEGV in `lam$0`). Enable readonly tracking
    // WITHOUT importing the class-specific §22.7 this-observation gating
    // (records have no base-first-construction footgun) — but, per fix #4001,
    // WITH the §7.9 field-completion obligation itself (`recordCtorMode_`):
    // §7.9's own text is generic ("every non-nullable field must be either
    // assigned … or given a field initializer") and §9.4 states outright that
    // a record constructor's stores are "governed by §22.8's once-only and
    // §7.9's definite-assignment machinery" — a record body field or
    // auto-property left unset by every constructor is exactly the same
    // unset-non-nullable-field state §7.9 forbids for a class, and reading it
    // back through a `new R(...)` value SIGSEGVs identically (issue #4001,
    // three measured repros: an explicit body field, a `readonly property`
    // auto-property, and an explicit user constructor that simply never
    // assigns the field — none diagnosed before this fix). INSTANCE readonly
    // fields carrying an initializer run as step 2 before a non-chaining
    // ctor's body (§22.8), so they are seeded as already-assigned exactly as
    // in runStaticConstructor; a `this(...)`-chaining ctor skips step 2 (its
    // initializers ran in the chained-to ctor), so the seeding is suppressed
    // in that case — mirroring runConstructor's choice, and (fix #4001) the
    // chained-to constructor's own §7.9 obligation already discharged every
    // needing-init field, so this constructor seeds them pre-assigned too,
    // exactly as runConstructor does for classes. Cross-ctor once-ness for
    // `readonly` (a chaining ctor re-assigning a field the chained-to ctor
    // already assigned) is the #2206 route B residual, out of scope here.
    void runRecordConstructor(const ast::ConstructorDecl& cd,
                              const ast::TypeDecl& rec) {
        roCtorMode_ = true;
        recordCtorMode_ = true;
        std::vector<std::string_view> readonlyInitialized;
        for (const auto& m : rec.members) {
            if (auto* f = dynamic_cast<const ast::FieldDecl*>(m.get())) {
                // fix #4001 / §7.9: a record body field is the same storage
                // shape isSevenNineStrictField already classifies for a
                // class field — the predicate reads only the field's own
                // modifiers/type/initializer, nothing class-specific.
                if (isSevenNineStrictField(*f, ctx_))
                    fieldsNeedingInit_.push_back(f->name);
                if (!f->initializer) continue;
                bool isStatic = false, isReadonly = false;
                for (auto mod : f->modifiers) {
                    if (mod == "static") isStatic = true;
                    else if (mod == "readonly") isReadonly = true;
                }
                if (!isStatic && isReadonly)
                    readonlyInitialized.push_back(f->name);
            } else if (auto* p = dynamic_cast<const ast::PropertyDecl*>(
                           m.get())) {
                // fix #4001 / §7.9 / §22.7: "auto-property assignments and
                // reads … each [are] the backing field's assignment or
                // read" — a record's auto-property backing storage is the
                // §7.9 obligation §7.9's "Default initialization" subsection
                // states for any non-defaultable storage, record or class.
                if (isSevenNineStrictAutoProp(*p, ctx_))
                    fieldsNeedingInit_.push_back(p->name);
            }
        }
        std::size_t startIdx = 0;
        bool isThisChain = false;
        if (getChainingCall(cd, isThisChain)) {
            startIdx = 1;
        }
        if (isThisChain) {
            // fix #4001, mirroring runConstructor: a `this(...)`-chained-to
            // record constructor has already discharged every §7.9-strict
            // field on its own normal-exit path (checked when THAT
            // constructor is walked), so this constructor's body starts with
            // them all assigned.
            for (auto n : fieldsNeedingInit_) assignedFields_.insert(n);
        } else {
            for (auto n : readonlyInitialized)
                readonlyMaybeAssigned_.insert(n);
        }
        for (std::size_t i = startIdx; i < cd.body.size(); ++i) {
            if (cd.body[i]) visitStmt(*cd.body[i]);
        }
        if (!terminated_) {
            // fix #4001 / §7.9 (Appendix C.1): the constructor's fall-off is
            // a normal-exit path too — mirrors runConstructor's own call.
            checkCtorFieldCompletion(&cd.span.begin);
        }
    }

private:
    SemaContext& ctx_;
    // declared_ records every local introduced in this method (by name).
    // assigned_ records those that are also known-assigned at the
    // current point. A read of a name in declared_ but not assigned_
    // is a diagnostic.
    std::unordered_set<std::string_view> declared_;
    std::unordered_set<std::string_view> assigned_;
    // out-parameter names that must be assigned on every exit path (§22.3).
    std::unordered_set<std::string_view> outParams_;
    // #1226 / §29.11: lazy LINQ bound vars that are definitely consumed at
    // the current program point (consumed = enumerated by a for-in or a
    // terminal op).  A second for-in over a definitely-consumed lazy var is
    // a compile error.  Tracking uses intersection semantics at branch joins
    // (parallel to assigned_): a var is "definitely consumed" only when it
    // is consumed on every path reaching the current point.
    std::unordered_set<std::string_view> consumedLazy_;
    // terminated_ = true when the current path ends unconditionally via
    // return/throw — used to suppress the fall-off out-param check on
    // unreachable tails (conservative: only set for top-level or all-
    // branch termination; partial-branch termination leaves it false so
    // the fall-off check runs on the live path).
    bool terminated_ = false;

    // §22.3 (fix #2595): set by visitExpr's fall-through when it meets an Expr
    // form it has no arm for and that is not a childless leaf — currently
    // `InterpolatedStringExpr`, `RangeExpr`, `CaseExpr`. An `out` argument
    // nested in one is invisible to the walk, so `runExpressionBody` declines
    // to run the §22.3 check rather than report an assignment it simply could
    // not see. Reset per expression-body walk; read ONLY by runExpressionBody
    // (the block-bodied path is unaffected).
    bool sawUnanalyzedForm_ = false;

    // §22.7 (v0.184) construction-time `this`-observation checking. Active only
    // while walking a constructor body (runConstructor). The observation gate
    // (rule 1) needs field-level definite assignment threaded flow-sensitively,
    // exactly parallel to `assigned_` for locals: `assignedFields_` is the set
    // of §7.9-strict fields definitely assigned on every path reaching the
    // current point; `fieldsNeedingInit_` is the (declaration-ordered) list of
    // §7.9-strict fields the class must explicitly initialize. A synchronous
    // observation of `this` is admitted only where fieldsNeedingInit_ ⊆
    // assignedFields_.
    bool ctorMode_ = false;
    // True while visiting the arguments of the `base(...)`/`this(...)` chaining
    // call, where no field is assigned yet — any observation of `this` there is
    // an error (§22.7 rule 1, chaining position).
    bool inChainingPosition_ = false;
    // The declaring class is `sealed` and not `abstract` — the sole exemption
    // to §22.7 rule 2 (virtual dispatch from a constructor body).
    bool classSealedNonAbstract_ = false;
    std::vector<std::string_view> fieldsNeedingInit_;
    std::unordered_set<std::string_view> assignedFields_;

    // fix #2187 / §22.8 (single-assignment of readonly fields). Active while
    // walking ANY class constructor body — instance (runConstructor) or static
    // (runStaticConstructor). §22.8: "A field declared `readonly` may be
    // assigned only in a field initializer or in a constructor body" — and,
    // referenced by §7.4/§21.1 as "§22.8's single-assignment rule" /
    // "member-level runtime single-assignment", a readonly field is assigned
    // *exactly once*. The compiler previously enforced only the PLACEMENT
    // (checkReadonlyAssignment in ExprTypecheck.cpp accepts any assignment while
    // `inConstructor_`), not the once-ness — so `_f := a; _f := b;` compiled,
    // breaking the §7.4 readonly-field narrowing (#2116) whose ambient-fact
    // ride-into-a-closure is sound EXACTLY because an observed non-null readonly
    // value is final. A double assignment made a stored lambda deref a value the
    // ctor later nulled (probe-confirmed SEGV in `lam$0`).
    //
    // `readonlyMaybeAssigned_` is the set of readonly fields possibly assigned
    // on some path reaching the current point — the DUAL of `assignedFields_`
    // (which is the all-paths definite set): union at joins (joinUnionLive),
    // seeded at body entry with readonly fields that carry a field initializer
    // (they run as step 2 before a non-chaining ctor's body, §22.8). An
    // assignment to a readonly field already in this set is a second assignment
    // → error. `inLoopDepth_ > 0` marks an assignment textually inside a loop
    // body: a loop could assign the field on more than one iteration, and the
    // construction window carves out §7.4 narrowing so the compiler cannot prove
    // a guard limits it to once — so any readonly assignment inside a loop is
    // rejected (the conservative, sound reading of single-assignment).
    bool roCtorMode_ = false;
    int inLoopDepth_ = 0;
    std::unordered_set<std::string_view> readonlyMaybeAssigned_;

    // fix #4001 / §7.9: record-constructor field-completion tracking. Active
    // only while walking a record's instance constructor body
    // (runRecordConstructor). Deliberately SEPARATE from `ctorMode_` rather
    // than reusing it: `ctorMode_` also drives the §22.7 this-observation
    // gate (gateObservation/gateChainPositionFieldRead/visitCtorCall/
    // visitCtorMemberAccess), which is genuinely class-specific machinery
    // guarding a base-first-construction/virtual-dispatch footgun records do
    // not have (checkMember's comment on the record ctor branch). This flag
    // reuses ONLY the two class-agnostic parts of that machinery — marking a
    // `this.f := v`/bare `f := v` store toward `assignedFields_`, and the
    // §7.9 fall-off completeness check (checkCtorFieldCompletion) — so a
    // record body field/auto-property gets the SAME "every non-nullable
    // field must be assigned by every constructor" guarantee §7.9 states
    // generically (confirmed at spec line ~1452: a record constructor's
    // stores are "governed by §22.8's once-only and §7.9's definite-
    // assignment machinery"), without importing any observation-gate
    // rejection a record never needed.
    bool recordCtorMode_ = false;

    // fix #4015 / §7.9: static-constructor field-completion tracking. Active
    // only while walking a static constructor body (runStaticConstructor),
    // for both a class's and a record's static constructor. Reuses the same
    // two class-agnostic mechanisms `recordCtorMode_` reuses (see its own
    // comment above) — marking toward `assignedFields_` and the
    // checkCtorFieldCompletion fall-off check — for the identical reason:
    // there is no `this` in a static constructor, so none of the §22.7
    // this-observation gating (`ctorMode_`-gated) applies, but §7.9's
    // generic field-completion obligation ("assignment ... in the static
    // constructor, for static fields") applies exactly as for the instance
    // case.
    bool staticCtorMode_ = false;

    // §22.4 (v0.181): short-circuit-sensitive definite-assignment state for a
    // boolean sub-expression. `afterTrue` / `afterFalse` are the sets of
    // definitely-assigned names on the paths where the expression evaluated to
    // `true` and to `false` respectively. They differ exactly where a
    // short-circuit operator skips a call: `A and Call(out var x)` assigns `x`
    // only on the true continuation; `A or Call(out var x)` only on the false
    // continuation.
    struct CondState {
        std::unordered_set<std::string_view> afterTrue;
        std::unordered_set<std::string_view> afterFalse;
    };

    // Set intersection helper (a local is definitely assigned only where it is
    // assigned on *every* incoming path).
    static std::unordered_set<std::string_view> intersectSets(
        const std::unordered_set<std::string_view>& a,
        const std::unordered_set<std::string_view>& b) {
        std::unordered_set<std::string_view> r;
        for (auto n : a)
            if (b.count(n)) r.insert(n);
        return r;
    }

    // §22.4 (v0.181): whether an inline `out var` recipient must be tracked
    // path-sensitively for definite assignment. Only a non-nullable
    // reference-typed inline out var is unsafe to read on a path where the
    // introducing call never ran; value-typed and nullable inline out vars are
    // default-initialized (§7.9) and readable everywhere they are in scope, so
    // they are left untracked. The decision is recorded on the LocalSymbol at
    // type-check time (from the DECLARED out-param type, before its nullability
    // was stripped), because `declType` alone can no longer distinguish a
    // declared `T?` reference out var from a `T` one.
    bool inlineOutVarNeedsDA(const ast::IdentifierExpr& id) const {
        auto* loc = dynamic_cast<const LocalSymbol*>(ctx_.exprSymbol(id));
        return loc && loc->outVarNeedsDefiniteAssignment;
    }

    // §22.4 (v0.181): evaluate `e` for its effect on `assigned_` with
    // short-circuit precision, returning the definitely-assigned sets on its
    // true and false continuations. Reads inside `e` are diagnosed in place; an
    // inline `out var` assignment inside a short-circuited operand lands only on
    // the branch where the introducing call actually ran. On return `assigned_`
    // is left at the intersection of the two branch sets — the state that holds
    // regardless of `e`'s value, correct for using `e` as a plain
    // sub-expression or statement.
    CondState evalCond(const ast::Expr& e) {
        if (auto* be = dynamic_cast<const ast::BinaryExpr*>(&e)) {
            if (be->op == "and") {
                CondState a = be->left ? evalCond(*be->left)
                                       : CondState{assigned_, assigned_};
                // The right operand is evaluated only when the left is true.
                assigned_ = a.afterTrue;
                CondState b = be->right ? evalCond(*be->right)
                                        : CondState{assigned_, assigned_};
                CondState r;
                r.afterTrue = b.afterTrue;  // whole is true ⇒ both ran and true
                r.afterFalse =
                    intersectSets(a.afterFalse, b.afterFalse);  // either false
                assigned_ = intersectSets(r.afterTrue, r.afterFalse);
                return r;
            }
            if (be->op == "or") {
                CondState a = be->left ? evalCond(*be->left)
                                       : CondState{assigned_, assigned_};
                // The right operand is evaluated only when the left is false.
                assigned_ = a.afterFalse;
                CondState b = be->right ? evalCond(*be->right)
                                        : CondState{assigned_, assigned_};
                CondState r;
                r.afterTrue =
                    intersectSets(a.afterTrue, b.afterTrue);  // either true
                r.afterFalse = b.afterFalse;  // whole is false ⇒ both ran, false
                assigned_ = intersectSets(r.afterTrue, r.afterFalse);
                return r;
            }
        }
        if (auto* ue = dynamic_cast<const ast::UnaryExpr*>(&e)) {
            if (ue->op == "not") {
                CondState a = ue->operand ? evalCond(*ue->operand)
                                          : CondState{assigned_, assigned_};
                // `not` swaps the two continuations; their intersection
                // (`assigned_`) is unchanged.
                return CondState{a.afterFalse, a.afterTrue};
            }
        }
        // Leaf / non-short-circuit expression: it evaluates unconditionally, so
        // its assignments and reads apply on both continuations.
        visitExpr(e);
        return CondState{assigned_, assigned_};
    }

    // §7.5 / §22.3 / §22.4 (v0.235, #2345): per-path definite-assignment
    // treatment for a null-conditional call `recv?.M(args)`, mirroring
    // evalCond's and/or short-circuit modeling. §7.5: "if any segment of a
    // chain is null, the remainder is not evaluated" — so when `recv` is
    // null, `M`'s argument list (and any `out` assignment it would produce)
    // never runs, exactly parallel to `and`'s unevaluated right operand when
    // the left is false. `afterFalse` is therefore the state where the
    // receiver was null and nothing beyond it evaluated; `afterTrue` is the
    // state where the receiver was non-null and the call's arguments ran.
    // The receiver expression `ncm.target` itself is evaluated
    // unconditionally (only the member access and the call's arguments are
    // the "remainder" §7.5 skips) — and if `ncm.target` is itself a further
    // null-conditional node (a chained `a?.b?.c(...)`), visitExpr recurses
    // into this same machinery for it, self-contained: it merges its own
    // split to the safe/conservative intersection before returning here, so
    // a nested `?.`-call's own out-var hazard is independently protected at
    // ITS OWN call site (see the chained-access self-pass test). The general
    // visitExpr entry point (below) merges the returned CondState to the
    // conservative intersection for ordinary (non-short-circuit-composed)
    // use — exactly as evalCond's and/or callers do.
    CondState evalNullCondCallArgs(const ast::CallExpr& ce,
                                   const ast::NullCondMemberExpr& ncm) {
        if (ncm.target) visitExpr(*ncm.target);
        // afterFalse: receiver null — the args (and any inline `out var`
        // declaration among them) are never evaluated.
        auto afterSkip = assigned_;
        // afterTrue: receiver non-null — the call's arguments run exactly as
        // an ordinary call's (existing visitCallArg semantics, unchanged).
        for (const auto& a : ce.args) visitCallArg(a);
        CondState r{/*afterTrue=*/assigned_, /*afterFalse=*/afterSkip};
        assigned_ = intersectSets(r.afterTrue, r.afterFalse);
        return r;
    }

    // True when `e` is a short-circuit boolean operator whose operands must be
    // evaluated path-sensitively (§22.4).
    static bool isShortCircuitExpr(const ast::Expr& e) {
        if (auto* be = dynamic_cast<const ast::BinaryExpr*>(&e))
            return be->op == "and" || be->op == "or";
        if (auto* ue = dynamic_cast<const ast::UnaryExpr*>(&e))
            return ue->op == "not";
        return false;
    }

    // §29.11 / #1226: true when `e`'s resolved type is the lazy LINQ
    // sentinel (Named TypeRef, no declaration, no astNode, no importedName,
    // at least one genericArg).  Must match isLazyEnumerableType() in codegen.
    bool isLazyEnumerableExprType(const ast::Expr& e) const {
        const TypeRef* ty = ctx_.exprType(e);
        return ty && ty->kind == TypeRef::Kind::Named && !ty->declaration &&
               !ty->astNode && !ty->genericArgs.empty() && ty->importedName.empty();
    }

    // §29.11 / #1353: mark a bound lazy local as consumed (or report a
    // compile error if it was already consumed). `context` briefly describes
    // the consumption point for the diagnostic ("enumeration", "LINQ chain",
    // "IEnumerable<T> argument"). per §29.11 v0.165
    void markLazyConsumed(const ast::IdentifierExpr& id,
                          const char* context) {
        if (consumedLazy_.count(id.name)) {
            ctx_.diagnostics().report(
                Severity::Error, id.span.begin,
                std::string("bound lazy query `") + std::string(id.name) +
                "` was already consumed — a lazy LINQ pipeline is "
                "single-consumption (§29.11); use `.ToArray()` to "
                "materialise it for multiple uses (consumed again via " +
                context + ")");
        } else {
            consumedLazy_.insert(id.name);
        }
    }

    void visitStmt(const ast::Stmt& s) {
        if (auto* ld = dynamic_cast<const ast::LocalDecl*>(&s)) {
            if (ld->initializer) visitExpr(*ld->initializer);
            if (!ld->name.empty()) {
                declared_.insert(ld->name);
                if (ld->initializer) {
                    assigned_.insert(ld->name);
                } else if (ld->type) {
                    // §15.1: inline arrays are zero-initialized when declared
                    // without an initializer (the declaration is the init);
                    // treat them as definitely assigned.
                    if (auto* rt = ctx_.resolvedType(*ld->type)) {
                        if (rt->kind == sema::TypeRef::Kind::InlineArray)
                            assigned_.insert(ld->name);
                    }
                }
            }
            return;
        }
        if (auto* lc = dynamic_cast<const ast::LocalConstDecl*>(&s)) {
            if (lc->value) visitExpr(*lc->value);
            if (!lc->name.empty()) {
                declared_.insert(lc->name);
                assigned_.insert(lc->name);  // const requires initializer
            }
            return;
        }
        if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(&s)) {
            // RHS first — that's a read before the write.
            if (as->value) visitExpr(*as->value);
            // fix #2187 / §22.8: single-assignment of a readonly field. Runs for
            // EVERY assignment op (`:=`, `??=`, compound) — any second write
            // breaks the finality the §7.4 narrowing (#2116) relies on. Placed
            // before the §22.7 field-DA bookkeeping below so it fires regardless
            // of whether the target is §7.9-strict (a nullable readonly field —
            // the exploit's `_f: string?` — is not, yet still narrows).
            if (roCtorMode_ && as->target)
                checkReadonlyOnceness(*as->target, as->span.begin);
            // §22.7 rule 1 (v0.184): a `:=` store to a field or auto-property of
            // `this` marks that field definitely assigned toward the
            // observation gate, and is itself NOT an observation (do not recurse
            // into the `this` receiver). A computed-property `:=` target falls
            // through to visitExpr below, where its setter invocation is gated.
            // fix #4001 / §7.9: `recordCtorMode_` reuses this same bookkeeping
            // for a record constructor's own §7.9 obligation (see its
            // declaration) — a record has no observation gate to feed, only
            // the fall-off completeness check this marks toward. fix #4015:
            // `staticCtorMode_` reuses it identically for a static
            // constructor's static-field completion obligation.
            if ((ctorMode_ || recordCtorMode_ || staticCtorMode_) &&
                as->op == ":=" && as->target) {
                std::string_view fname;
                if (isThisFieldOrAutoPropTarget(*as->target, fname)) {
                    assignedFields_.insert(fname);
                    return;
                }
            }
            // Target as IdentifierExpr → local gets assigned. Other
            // targets (member/index) read their root path first.
            if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                    as->target.get())) {
                if (declared_.count(id->name)) {
                    assigned_.insert(id->name);
                }
                // Bare ident that's NOT a declared local — could be a
                // field-on-this or a typo. Sema slice 3d's silent fall-
                // through covers the first case; here we don't flag.
            } else if (as->target) {
                visitExpr(*as->target);
            }
            return;
        }
        if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(&s)) {
            if (es->expr) visitExpr(*es->expr);
            return;
        }
        if (auto* ds = dynamic_cast<const ast::DiscardStmt*>(&s)) {
            if (ds->value) visitExpr(*ds->value);
            return;
        }
        if (auto* rs = dynamic_cast<const ast::ReturnStmt*>(&s)) {
            if (rs->value) visitExpr(*rs->value);
            // §22.3: every out-param must be assigned at each return.
            checkOutParams(&rs->span.begin);
            // §7.9 (Appendix C.1): an explicit `return` from a constructor is a
            // normal-exit path too, so every non-nullable field must already be
            // assigned on THIS path (a later assignment does not help a path
            // that returned before it). No-ops outside a constructor.
            checkCtorFieldCompletion(&rs->span.begin);
            terminated_ = true;
            return;
        }
        if (auto* tr = dynamic_cast<const ast::ThrowStmt*>(&s)) {
            if (tr->value) visitExpr(*tr->value);
            // A throw is an exit path; per §22.3 out-params need not be
            // assigned before a throw (the callee did not return normally).
            terminated_ = true;
            return;
        }
        if (auto* ifs = dynamic_cast<const ast::IfStmt*>(&s)) {
            // Each branch starts from the entry-assigned set; at join
            // the intersection becomes the new assigned set.
            // consumedLazy_ uses the same intersection semantics (#1226/§29.11):
            // a var is "definitely consumed" only when consumed on every path.
            //
            // §22.4 (v0.181): the guard is evaluated short-circuit-sensitively.
            // The `then` branch is reached only when the guard is TRUE, so it
            // starts from the guard's after-true set (an inline `out var` in an
            // `and`-conjunct is assigned there); each `elif` guard, the `else`
            // body, and the no-else fall-through are reached only when the
            // preceding guards were FALSE, so they thread the guard's after-false
            // set — the path on which a short-circuited call never ran.
            bool entryTerminated = terminated_;
            terminated_ = false;
            auto entryAssigned = assigned_;  // fix #1753 join fallback
            CondState c0 = ifs->ifBranch.condition
                               ? evalCond(*ifs->ifBranch.condition)
                               : CondState{assigned_, assigned_};
            auto consumedLazyEntry = consumedLazy_;
            // §22.7 (v0.184): field-assignment state is threaded in parallel to
            // `assigned_`. A guard expression assigns no field (assignments are
            // statements), so every branch body starts from the same entry
            // field-state; the post-`if` state is the intersection of the branch
            // field-outs (a field is assigned after the `if` only where assigned
            // on every reaching path). `fieldEntry` is that per-branch start.
            auto fieldEntry = assignedFields_;
            // fix #2187 / §22.8: readonly may-be-assigned state threaded in
            // parallel, with UNION at the join (joinUnionLive) — the dual of the
            // field-DA intersection. Each branch starts from the same entry set;
            // after the `if`, a readonly field is possibly-assigned if it was on
            // any live branch. `roEntry` is that per-branch start.
            auto roEntry = readonlyMaybeAssigned_;
            // `falseState` threads the assigned set on the path where every
            // guard checked so far was false.
            auto falseState = std::move(c0.afterFalse);
            assigned_ = std::move(c0.afterTrue);
            for (const auto& bs : ifs->ifBranch.body) {
                if (bs) visitStmt(*bs);
            }
            std::vector<std::unordered_set<std::string_view>> branchOuts;
            branchOuts.push_back(std::move(assigned_));
            std::vector<std::unordered_set<std::string_view>> consumedLazyOuts;
            consumedLazyOuts.push_back(std::move(consumedLazy_));
            std::vector<std::unordered_set<std::string_view>> fieldOuts;
            fieldOuts.push_back(std::move(assignedFields_));
            std::vector<std::unordered_set<std::string_view>> roOuts;
            roOuts.push_back(std::move(readonlyMaybeAssigned_));
            std::vector<bool> branchTerminated;
            branchTerminated.push_back(terminated_);
            for (const auto& el : ifs->elifBranches) {
                assigned_ = falseState;
                consumedLazy_ = consumedLazyEntry;
                assignedFields_ = fieldEntry;
                readonlyMaybeAssigned_ = roEntry;
                terminated_ = false;
                CondState ci = el.condition
                                   ? evalCond(*el.condition)
                                   : CondState{assigned_, assigned_};
                falseState = std::move(ci.afterFalse);
                assigned_ = std::move(ci.afterTrue);
                for (const auto& bs : el.body) {
                    if (bs) visitStmt(*bs);
                }
                branchOuts.push_back(std::move(assigned_));
                consumedLazyOuts.push_back(std::move(consumedLazy_));
                fieldOuts.push_back(std::move(assignedFields_));
                roOuts.push_back(std::move(readonlyMaybeAssigned_));
                branchTerminated.push_back(terminated_);
            }
            if (ifs->hasElse) {
                assigned_ = falseState;
                consumedLazy_ = consumedLazyEntry;
                assignedFields_ = fieldEntry;
                readonlyMaybeAssigned_ = roEntry;
                terminated_ = false;
                for (const auto& bs : ifs->elseBody) {
                    if (bs) visitStmt(*bs);
                }
                branchOuts.push_back(std::move(assigned_));
                consumedLazyOuts.push_back(std::move(consumedLazy_));
                fieldOuts.push_back(std::move(assignedFields_));
                roOuts.push_back(std::move(readonlyMaybeAssigned_));
                branchTerminated.push_back(terminated_);
            } else {
                branchOuts.push_back(std::move(falseState)); // no-else: guard-false path
                consumedLazyOuts.push_back(consumedLazyEntry); // no-else: no new consumption
                fieldOuts.push_back(fieldEntry); // no-else: guard-false path assigns no field
                roOuts.push_back(roEntry); // no-else: guard-false path assigns no readonly field
                branchTerminated.push_back(false);     // no-else = path live
            }
            // Intersect assigned / field sets over the LIVE branches only —
            // a branch that provably diverges (throw/return) reaches no join
            // and must not shrink the intersection (fix #1753, §17.x/§22.7).
            assigned_ =
                joinLiveBranches(branchOuts, branchTerminated, entryAssigned);
            // Intersect field-assignment sets (§22.7), same live-branch join.
            assignedFields_ =
                joinLiveBranches(fieldOuts, branchTerminated, fieldEntry);
            // fix #2187 / §22.8: UNION the readonly may-be-assigned sets over the
            // live branches — the dual of the intersection above.
            readonlyMaybeAssigned_ =
                joinUnionLive(roOuts, branchTerminated, roEntry);
            // Intersect consumedLazy_ sets (#1226). Kept as a whole-branch
            // intersection: lazy single-consumption (#1226) is a distinct
            // obligation from definite-assignment and out of this fix's scope.
            auto consumedOut = consumedLazyOuts.front();
            for (std::size_t i = 1; i < consumedLazyOuts.size(); ++i) {
                std::unordered_set<std::string_view> tmp;
                for (auto n : consumedOut) {
                    if (consumedLazyOuts[i].count(n)) tmp.insert(n);
                }
                consumedOut = std::move(tmp);
            }
            consumedLazy_ = std::move(consumedOut);
            // Post-if path is terminated only if all branches terminated.
            bool allTerminated = true;
            for (bool t : branchTerminated) {
                if (!t) { allTerminated = false; break; }
            }
            terminated_ = entryTerminated || allTerminated;
            return;
        }
        if (auto* ws = dynamic_cast<const ast::WhileStmt*>(&s)) {
            // §22.4 (v0.181): the guard is evaluated short-circuit-sensitively
            // and at least once. The body runs only when the guard is TRUE, so
            // it starts from the guard's after-true set (an inline `out var` in
            // an `and`-conjunct is assigned there). A `while` exits with its
            // guard FALSE (or ran zero times), so the post-loop state is the
            // guard's after-false set. Body-internal assignments do not survive
            // (the loop may run zero times), so they are not propagated past.
            // consumedLazy_ (#1226) is likewise restored to its post-guard
            // state: a zero-trip loop consumes nothing inside the body.
            bool entryTerminated = terminated_;
            terminated_ = false;
            CondState c = ws->condition ? evalCond(*ws->condition)
                                        : CondState{assigned_, assigned_};
            auto consumedLazyEntry = consumedLazy_;
            assigned_ = std::move(c.afterTrue);
            // §22.7 (v0.184): a zero-trip loop assigns no field, so body field
            // assignments do not survive past the loop (an observation after the
            // loop cannot assume the body ran). Observations *inside* the body
            // still see the body's growing field-state (a single iteration).
            auto fieldEntry = assignedFields_;
            // fix #2187 / §22.8: any assignment to a readonly field inside a loop
            // body could recur on a later iteration — rejected by checkReadonly
            // Onceness while `inLoopDepth_ > 0`. The body's rejected assignments
            // never enter `readonlyMaybeAssigned_`, and a zero-trip loop assigns
            // nothing, so the set is restored to entry after the loop.
            auto roEntry = readonlyMaybeAssigned_;
            ++inLoopDepth_;
            for (const auto& bs : ws->body) {
                if (bs) visitStmt(*bs);
            }
            --inLoopDepth_;
            assigned_ = std::move(c.afterFalse);
            consumedLazy_ = std::move(consumedLazyEntry);
            assignedFields_ = std::move(fieldEntry);
            readonlyMaybeAssigned_ = std::move(roEntry);
            terminated_ = entryTerminated; // loop may not execute
            return;
        }
        if (auto* fs = dynamic_cast<const ast::ForStmt*>(&s)) {
            // #1226 / §29.11: check and mark lazy-enumerable iterable as
            // consumed.  A second for-in over the same bound lazy local is
            // a compile error (the runtime would raise InvalidOperationException
            // — the static check surfaces it earlier with a better message).
            if (fs->iterable) {
                if (const auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                        fs->iterable.get())) {
                    if (isLazyEnumerableExprType(*fs->iterable)) {
                        // §29.11 point 1: enumeration (for-in) consumes the
                        // bound lazy. Use markLazyConsumed for consistency.
                        markLazyConsumed(*id, "for-in enumeration");
                    }
                }
                visitExpr(*fs->iterable);
            }
            if (fs->withExpr) visitExpr(*fs->withExpr);
            auto entry = assigned_;
            // Save consumedLazy_ AFTER marking the iterable (so the iterable
            // mark persists after the loop) but BEFORE entering the body (so
            // body-internal lazy consumptions are rolled back conservatively —
            // a body that runs 0 times does not consume any inner lazy vars).
            auto consumedLazyEntry = consumedLazy_;
            // §22.7 (v0.184): like `while`, a zero-trip `for` body assigns no
            // field, so body field assignments do not survive past the loop.
            auto fieldEntry = assignedFields_;
            // fix #2187 / §22.8: as with `while`, a readonly assignment in a
            // `for` body is rejected (loop iteration ⇒ potential re-assignment)
            // and the may-be-assigned set is restored past the zero-trip loop.
            auto roEntry = readonlyMaybeAssigned_;
            bool entryTerminated = terminated_;
            terminated_ = false;
            if (!fs->varName.empty()) {
                declared_.insert(fs->varName);
                assigned_.insert(fs->varName);
            }
            ++inLoopDepth_;
            for (const auto& bs : fs->body) {
                if (bs) visitStmt(*bs);
            }
            --inLoopDepth_;
            assigned_ = std::move(entry);
            consumedLazy_ = std::move(consumedLazyEntry); // body-marks removed
            assignedFields_ = std::move(fieldEntry);
            readonlyMaybeAssigned_ = std::move(roEntry);
            terminated_ = entryTerminated; // loop may not execute
            return;
        }
        if (auto* dob = dynamic_cast<const ast::DoEndBlock*>(&s)) {
            // `do ... end` is a single straight-line block; terminated_
            // propagates naturally (a throw/return inside terminates the path).
            for (const auto& bs : dob->body) {
                if (bs) visitStmt(*bs);
            }
            return;
        }
        if (auto* ts = dynamic_cast<const ast::TryStmt*>(&s)) {
            auto entry = assigned_;
            auto consumedLazyEntry = consumedLazy_;
            // §22.7 (v0.184): the try-body may abort at any point (before a
            // field assignment completes), and each catch arm starts from the
            // entry field-state; a field is assigned after the `try` only where
            // assigned on every arm (the try-body-completed path and every
            // catch path). `fieldEntry` is that per-arm start.
            auto fieldEntry = assignedFields_;
            auto roEntry = readonlyMaybeAssigned_;
            bool entryTerminated = terminated_;
            terminated_ = false;
            for (const auto& bs : ts->body) {
                if (bs) visitStmt(*bs);
            }
            // Try-body may abort at any point → join with entry for
            // the post-try set; catch arms each start from entry.
            std::vector<std::unordered_set<std::string_view>> outs;
            outs.push_back(std::move(assigned_));
            std::vector<std::unordered_set<std::string_view>> consumedLazyOuts;
            consumedLazyOuts.push_back(std::move(consumedLazy_));
            std::vector<std::unordered_set<std::string_view>> fieldOuts;
            fieldOuts.push_back(std::move(assignedFields_));
            // fix #2187 / §22.8: the readonly may-be-assigned state after the try
            // BODY — a throw could occur after any try-body readonly assignment
            // completed, so each catch arm starts seeing those possible
            // assignments (over-approx: reset to `roTryBody`, NOT the pre-try
            // `roEntry`). This makes `try _f:=a; catch _f:=b; end;` a rejected
            // double-assign (one throw edge leaves `_f` already set), while two
            // mutually-exclusive catch arms each assigning `_f` stay accepted
            // (each resets to the same `roTryBody`, not accumulating).
            auto roTryBody = readonlyMaybeAssigned_;
            std::vector<std::unordered_set<std::string_view>> roOuts;
            roOuts.push_back(std::move(readonlyMaybeAssigned_));
            std::vector<bool> armTerminated;
            armTerminated.push_back(terminated_);
            for (const auto& arm : ts->catches) {
                assigned_ = entry;
                consumedLazy_ = consumedLazyEntry;
                assignedFields_ = fieldEntry;
                readonlyMaybeAssigned_ = roTryBody;
                terminated_ = false;
                if (arm.body) visitStmt(*arm.body);
                outs.push_back(std::move(assigned_));
                consumedLazyOuts.push_back(std::move(consumedLazy_));
                fieldOuts.push_back(std::move(assignedFields_));
                roOuts.push_back(std::move(readonlyMaybeAssigned_));
                armTerminated.push_back(terminated_);
            }
            // Intersect assigned / field sets over the LIVE arms only — the
            // try-body-completed path or a catch arm that provably diverges
            // (throw/return) reaches no post-try join, so it must not shrink
            // the intersection (fix #1753, §17.x/§22.7).
            assigned_ = joinLiveBranches(outs, armTerminated, entry);
            // Intersect field-assignment sets (§22.7), same live-arm join.
            assignedFields_ =
                joinLiveBranches(fieldOuts, armTerminated, fieldEntry);
            // fix #2187 / §22.8: UNION the readonly may-be-assigned sets over the
            // live arms (dual of the field intersection). The finally body below
            // runs in this joined state — a readonly assignment there over a
            // try/catch-assigned field is a rejected double-assign.
            readonlyMaybeAssigned_ =
                joinUnionLive(roOuts, armTerminated, roTryBody);
            // Intersect consumedLazy_ sets (#1226). Kept as a whole-arm
            // intersection: lazy single-consumption is a distinct obligation
            // from definite-assignment, out of this fix's scope.
            auto consumedPost = consumedLazyOuts.front();
            for (std::size_t i = 1; i < consumedLazyOuts.size(); ++i) {
                std::unordered_set<std::string_view> tmp;
                for (auto n : consumedPost) {
                    if (consumedLazyOuts[i].count(n)) tmp.insert(n);
                }
                consumedPost = std::move(tmp);
            }
            consumedLazy_ = std::move(consumedPost);
            // Post-try path is terminated only if all arms terminated.
            bool allTerminated = true;
            for (bool t : armTerminated) {
                if (!t) { allTerminated = false; break; }
            }
            terminated_ = entryTerminated || allTerminated;
            for (const auto& bs : ts->finallyBody) {
                if (bs) visitStmt(*bs);
            }
            return;
        }
        if (auto* cs = dynamic_cast<const ast::CaseStmt*>(&s)) {
            // §19 / §22.3 / §22.7 (fix #1681, rebased onto #1679's field-DA):
            // a statement-position `case` is a branching construct exactly
            // like `if`/`try` above — each arm is a mutually exclusive path
            // from the pre-case state, joined afterward by intersecting the
            // assigned sets (local AND field, §22.7) over the LIVE arms
            // (fix #1753). This construct was previously entirely unvisited
            // here (no dynamic_cast branch existed), so every statement inside
            // a case arm — `return`s, `out`-param assignments, and
            // `this.f := ...` field assignments — was invisible to
            // definite-assignment: an exhaustively-assigning case body was
            // wrongly REJECTED (the fall-off / observation check fired since
            // no arm's assignment was ever recorded), and a `return` in one
            // arm that never assigned an `out` param (or a field before a
            // `this`-observation) was wrongly ACCEPTED whenever an assignment
            // before/after the case masked the gap.
            //
            // §19.6: a statement-position case with no provable catch-all arm
            // has an implicit "no match" path that "falls through with no
            // action" — a live branch carrying the entry state unchanged,
            // joined in below exactly like `if`'s no-else branch.
            if (cs->scrutinee) visitExpr(*cs->scrutinee);
            bool entryTerminated = terminated_;
            auto entryAssigned = assigned_;
            auto entryConsumedLazy = consumedLazy_;
            auto fieldEntry = assignedFields_;
            // fix #2187 / §22.8: readonly may-be-assigned entry — case arms are
            // mutually exclusive (like if/else), so each resets to `roEntry` and
            // the post-case set is the UNION over live arms (joinUnionLive).
            auto roEntry = readonlyMaybeAssigned_;
            std::vector<std::unordered_set<std::string_view>> armOuts;
            std::vector<std::unordered_set<std::string_view>> armConsumedLazyOuts;
            std::vector<std::unordered_set<std::string_view>> armFieldOuts;
            std::vector<std::unordered_set<std::string_view>> armRoOuts;
            std::vector<bool> armTerminated;
            bool hasCatchAll = false;
            for (const auto& arm : cs->arms) {
                assigned_ = entryAssigned;
                consumedLazy_ = entryConsumedLazy;
                assignedFields_ = fieldEntry;
                readonlyMaybeAssigned_ = roEntry;
                terminated_ = false;
                // Pattern bindings (`var t`) are non-destructive fresh names
                // (§19 pattern binding), always definitely assigned within the
                // arm by construction, so they are intentionally not added to
                // declared_/assigned_ here (a read simply passes checkRead's
                // declared_ gate silently, same as a field or outer local;
                // §19.3).
                if (arm.guard) visitExpr(*arm.guard);
                if (caseArmIsUnguardedCatchAll(arm)) hasCatchAll = true;
                if (arm.bodyKind == ast::CaseArmBodyKind::Expression) {
                    if (arm.exprBody) visitExpr(*arm.exprBody);
                } else {
                    for (const auto& bs : arm.stmts) {
                        if (bs) visitStmt(*bs);
                    }
                }
                armOuts.push_back(std::move(assigned_));
                armConsumedLazyOuts.push_back(std::move(consumedLazy_));
                armFieldOuts.push_back(std::move(assignedFields_));
                armRoOuts.push_back(std::move(readonlyMaybeAssigned_));
                armTerminated.push_back(terminated_);
            }
            if (!hasCatchAll) {
                // §19.6 implicit no-match fall-through: a live path that
                // assigns nothing — carries the entry state unchanged.
                armOuts.push_back(entryAssigned);
                armConsumedLazyOuts.push_back(entryConsumedLazy);
                armFieldOuts.push_back(fieldEntry);
                armRoOuts.push_back(roEntry);
                armTerminated.push_back(false);
            }
            // Join over the LIVE arms only (fix #1753): a `return`/`throw` arm
            // reaches no post-case join, so it must not shrink the local- or
            // field-DA intersection. consumedLazy_ stays a whole-arm
            // intersection (lazy single-consumption is out of DA scope).
            assigned_ = joinLiveBranches(armOuts, armTerminated, entryAssigned);
            assignedFields_ =
                joinLiveBranches(armFieldOuts, armTerminated, fieldEntry);
            // fix #2187 / §22.8: UNION the readonly may-be-assigned sets over the
            // live arms (dual of the field intersection).
            readonlyMaybeAssigned_ =
                joinUnionLive(armRoOuts, armTerminated, roEntry);
            if (!armConsumedLazyOuts.empty()) {
                auto consumedOut = armConsumedLazyOuts.front();
                for (std::size_t i = 1; i < armConsumedLazyOuts.size(); ++i) {
                    std::unordered_set<std::string_view> tmp;
                    for (auto n : consumedOut) {
                        if (armConsumedLazyOuts[i].count(n)) tmp.insert(n);
                    }
                    consumedOut = std::move(tmp);
                }
                consumedLazy_ = std::move(consumedOut);
            } else {
                consumedLazy_ = std::move(entryConsumedLazy);
            }
            bool allTerminated = true;
            for (bool t : armTerminated) {
                if (!t) { allTerminated = false; break; }
            }
            terminated_ = entryTerminated || allTerminated;
            return;
        }
        if (auto* is = dynamic_cast<const ast::IncrementStmt*>(&s)) {
            if (is->target) visitExpr(*is->target);
            // fix #2187 / §22.8: `_f++`/`_f--` on a readonly field is a
            // read-modify-WRITE — a second write over any prior assignment
            // breaks single-assignment exactly as a `:=` would.
            if (roCtorMode_ && is->target)
                checkReadonlyOnceness(*is->target, is->span.begin);
            return;
        }
        if (auto* ys = dynamic_cast<const ast::YieldStmt*>(&s)) {
            if (ys->value) visitExpr(*ys->value);
            return;
        }
    }

    void visitExpr(const ast::Expr& e) {
        // §22.4 (v0.181): a short-circuit boolean operator evaluates its
        // operands path-sensitively. Route it through evalCond, which leaves
        // `assigned_` at the state that holds regardless of the operator's
        // value (the intersection of its true/false continuations) — so a
        // conditionally-assigned inline `out var` does not leak past the
        // operator as definitely assigned.
        if (isShortCircuitExpr(e)) {
            (void)evalCond(e);
            return;
        }
        if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(&e)) {
            // §22.7 rule 1 (v0.184): a bare `this` reached here is `this` used
            // as a value — an argument, a stored/returned reference, an
            // `is`/`is not`/`as` operand — i.e. a synchronous observation.
            // Receiver-`this` uses (`this.f`, `this.M(...)`) are intercepted by
            // the parent MemberAccess/Call handlers and never recurse into this
            // leaf. `base` alone is not a value (only `base.member`), so only
            // `this` is gated. per §22.7
            if (ctorMode_ && id->name == "this") {
                gateObservation(id->span.begin, "reference to `this`");
                return;
            }
            // fix #3583 (§22.8 step 0 / §22.10), widened by fix #3829
            // (§12.8 / §22.10): the bare spelling of a field, auto-property,
            // or primary-constructor parameter in the chaining position.
            gateChainPositionFieldRead(*id, id->span.begin);
            checkRead(*id);
            return;
        }
        // Recurse into children. We share this with the ownership
        // analyzer's shape but don't need its discharge logic.
        if (auto* be = dynamic_cast<const ast::BinaryExpr*>(&e)) {
            if (be->left) visitExpr(*be->left);
            if (be->right) visitExpr(*be->right);
            return;
        }
        if (auto* ue = dynamic_cast<const ast::UnaryExpr*>(&e)) {
            if (ue->operand) visitExpr(*ue->operand);
            return;
        }
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
            // §22.7 (v0.184): `this.member`/`base.member` — a computed-property
            // read is an observation; a field/auto-property read is not.
            // Handled (and the `this`/`base` receiver not recursed into) here.
            if (visitCtorMemberAccess(*ma)) return;
            if (ma->target) visitExpr(*ma->target);
            return;
        }
        if (auto* nc = dynamic_cast<const ast::NullCondMemberExpr*>(&e)) {
            if (nc->target) visitExpr(*nc->target);
            return;
        }
        if (auto* ce = dynamic_cast<const ast::CallExpr*>(&e)) {
            // §22.7 rule 1/2 (v0.184): a receiver-`this` call (`this.M(...)`,
            // `base.M(...)`, or an implicit-`this` bare `M(...)`) is a
            // construction-time observation; handle it (and its args) here.
            if (visitCtorCall(*ce)) return;
            // §29.11 point 2 (#1353): applying a further deferred LINQ
            // operator to a bound lazy local consumes that local.  Detect
            // by: call result type is lazy-enumerable AND the callee is a
            // MemberAccessExpr whose receiver is a lazy-enumerable identifier.
            // Example: `var q2 := q.Select(f)` — q is consumed here.
            // per §29.11 v0.165
            if (isLazyEnumerableExprType(e)) {
                if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(
                        ce->callee.get())) {
                    if (ma->target) {
                        if (const auto* rid =
                                dynamic_cast<const ast::IdentifierExpr*>(
                                    ma->target.get())) {
                            if (isLazyEnumerableExprType(*ma->target)) {
                                // Check DA assignment (read) first, then
                                // mark the bound lazy as consumed.
                                checkRead(*rid);
                                markLazyConsumed(*rid, "LINQ chain operator");
                                // Visit the call's own args (lambda/predicates)
                                // — but not the callee identifier again.
                                for (const auto& a : ce->args) {
                                    visitCallArg(a);
                                }
                                return;
                            }
                        }
                    }
                }
            }
            // §7.5 / §22.4 (v0.235, #2345): `recv?.M(args)` — the callee is a
            // NullCondMemberExpr. Give this the same per-path short-circuit
            // treatment evalCond already gives and/or (see
            // evalNullCondCallArgs): the args — and any `out` assignment
            // they produce — run only on the receiver-non-null continuation.
            // Previously this fell through to the generic handling below,
            // which visited the callee (a no-op read of the receiver chain)
            // and then walked every argument via visitCallArg UNCONDITIONALLY
            // — marking an inline `out var` recipient "assigned" on the
            // straight-line path regardless of whether the call was actually
            // reachable only conditionally (the gap #2345 tracks).
            if (auto* ncm = dynamic_cast<const ast::NullCondMemberExpr*>(
                    ce->callee.get())) {
                (void)evalNullCondCallArgs(*ce, *ncm);
                return;
            }
            if (ce->callee) visitExpr(*ce->callee);
            for (const auto& a : ce->args) {
                visitCallArg(a);
            }
            return;
        }
        if (auto* nc = dynamic_cast<const ast::NullCondCallExpr*>(&e)) {
            if (nc->callee) visitExpr(*nc->callee);
            for (const auto& a : nc->args) {
                visitCallArg(a);
            }
            return;
        }
        if (auto* ix = dynamic_cast<const ast::IndexExpr*>(&e)) {
            if (ix->target) visitExpr(*ix->target);
            for (const auto& a : ix->indices) {
                if (a.value) visitExpr(*a.value);
            }
            return;
        }
        if (auto* bg = dynamic_cast<const ast::PostfixBangExpr*>(&e)) {
            if (bg->operand) visitExpr(*bg->operand);
            return;
        }
        if (auto* ne = dynamic_cast<const ast::NewExpr*>(&e)) {
            // §22.4 / §7.9 (v0.235, #2409): route each constructor argument
            // through visitCallArg, exactly as the CallExpr case below does
            // for a method call's arguments — an inline `out var`/`out owned
            // var` recipient of a constructor call is a pure WRITE (marks
            // `assigned_`, and — when `inlineOutVarNeedsDA` says the
            // recipient is a non-nullable reference type — `declared_`, so a
            // later read on a path where the `new` call did not run is
            // diagnosed), not a plain read of a not-yet-declared name. Before
            // this fix every constructor argument (including an `out var`
            // recipient's own declaring occurrence) fell through to a bare
            // `visitExpr`, whose IdentifierExpr leaf case is `checkRead` —
            // never `assigned_.insert`/`declared_.insert` — so a ctor
            // out-var's name was NEVER registered with this pass at all: a
            // later read on a short-circuit-skipped path (`flag and new
            // T(..., out var n).ok`) silently passed DA (n not in
            // `declared_`, so `checkRead` is a no-op) instead of being
            // rejected per §22.4's "a read of a non-nullable-reference-typed
            // out variable [on a path where the call may not have run] is a
            // compile error" — confirmed live as a SEGFAULT (garbage-pointer
            // comparison), not a diagnostic, before this fix. This mirrors
            // #2409's sema-registration fix (ExprTypecheck_Exprs.cpp's
            // NewExpr `out var` handling, which now sets
            // `outVarNeedsDefiniteAssignment`) — that fix alone left this gap
            // live, because `inlineOutVarNeedsDA` is only ever consulted from
            // inside `visitCallArg`, which the NewExpr case never called.
            for (const auto& a : ne->args) {
                visitCallArg(a);
            }
            return;
        }
        if (auto* cd = dynamic_cast<const ast::ConditionalExpr*>(&e)) {
            if (cd->condition) visitExpr(*cd->condition);
            if (cd->thenValue) visitExpr(*cd->thenValue);
            if (cd->elseValue) visitExpr(*cd->elseValue);
            return;
        }
        // §17.15 (v0.216, #2098): the binding atom `(var x := E; body)` —
        // clauses evaluate first (each initializer is a read position), the
        // body last. A clause binding is definitely assigned by its own
        // clause (A.7 BindClause mandates the initializer), so no
        // declared-unassigned state ever arises for it; the walk exists so
        // reads of ENCLOSING unassigned locals inside the atom are still
        // diagnosed.
        if (auto* bx = dynamic_cast<const ast::BindExpr*>(&e)) {
            for (const auto& cl : bx->clauses)
                if (cl && cl->initializer) visitExpr(*cl->initializer);
            if (bx->body) visitExpr(*bx->body);
            return;
        }
        if (auto* te = dynamic_cast<const ast::TypeTestExpr*>(&e)) {
            if (te->operand) visitExpr(*te->operand);
            return;
        }
        if (auto* th = dynamic_cast<const ast::ThrowExpr*>(&e)) {
            if (th->operand) visitExpr(*th->operand);
            return;
        }
        if (auto* we = dynamic_cast<const ast::WithExpr*>(&e)) {
            if (we->source) visitExpr(*we->source);
            for (const auto& ia : we->assigns) {
                if (ia.value) visitExpr(*ia.value);
            }
            return;
        }
        if (auto* lam = dynamic_cast<const ast::LambdaExpr*>(&e)) {
            // §22.3 (fix #2631): a lambda body is a CALLEE — it can declare
            // an `out` parameter (`(out int) -> ()` is a legal function type,
            // and `tests/codegen/ref_out_params_test.cpp:115-124` exercises the
            // shape), and §22.3 imposes the same callee-side assignment
            // obligation on it as on a method: every `out` parameter must be
            // assigned on every normal-completion (return) path, throw
            // exempt. Before this fix the walker skipped the node entirely
            // with the comment "we don't analyze it recursively here; that's
            // an exercise for a later slice (capture model and closure-DA are
            // non-trivial)". That comment is still right about *full*
            // closure-DA, but the `out`-parameter obligation is callee-local
            // — an `out` parameter of a lambda is not a captured variable, so
            // the check needs no capture analysis. Walking the lambda body
            // with a fresh MethodDA discharges the §22.3 obligation; the
            // wider closure-DA work is left as a follow-up.
            //
            // Narrow scope, matching #2595's narrow expression-body fix: a
            // lambda with NO `out` parameter is untouched — entering the
            // walk unconditionally would newly subject every lambda in the
            // language to the walker's other (§29.11 lazy-consumption,
            // local-DA read) bookkeeping for no §22.3 benefit.
            //
            // Both body forms matter: block (`isBlockBody` → `stmts`,
            // `do … end`) and expression (`!isBlockBody` → `exprBody`,
            // `=> E;`). A fresh MethodDA per lambda expression so each
            // lambda's `out`-parameter set and body are analyzed
            // independently — capturing semantics (the wider closure-DA
            // problem) are deliberately out of scope here.
            if (hasOutParam(lam->params)) {
                MethodDA da(ctx_);
                if (lam->isBlockBody) {
                    da.run(lam->params, lam->stmts);
                } else if (lam->exprBody) {
                    da.runExpressionBody(lam->params, *lam->exprBody);
                }
            }
            return;
        }
        if (auto* cl = dynamic_cast<const ast::CollectionLiteralExpr*>(&e)) {
            for (const auto& k : cl->keys) {
                if (k) visitExpr(*k);
            }
            for (const auto& v : cl->elements) {
                if (v) visitExpr(*v);
            }
            return;
        }
        // Everything above either returned or is a genuinely childless leaf.
        // Anything ELSE reaching here is a composite form this walker has no
        // arm for and therefore cannot see into. As of v0.236 that is
        // `InterpolatedStringExpr` (its holes), `RangeExpr` (lo/hi/step), and
        // `CaseExpr` (scrutinee, guards, arm bodies) — each can contain a call,
        // hence an `out` argument that DISCHARGES the §22.3 obligation.
        //
        // Record it so `runExpressionBody` can decline to report rather than
        // emit a false reject (fix #2595 follow-up). Deliberately a leaf
        // WHITELIST rather than a blind-form blacklist: a newly added Expr node
        // then defaults to "unanalyzed" — a missed diagnosis, the safe
        // direction — instead of being silently mis-reported as unassigned.
        // Read only by `runExpressionBody`; the block-bodied path never
        // consults it, so this adds no behavior change there.
        if (!isChildlessLeaf(e)) sawUnanalyzedForm_ = true;
    }

    // True for the Expr forms that genuinely have no `Expr` child, so their
    // absence from visitExpr's chain loses nothing. `TypeOfExpr`/`NameOfExpr`/
    // `DefaultExpr` hold a `Type`/`QualifiedName`, not an expression;
    // `MarkedTransferExpr` holds only the transferred name (`owned x`).
    static bool isChildlessLeaf(const ast::Expr& e) {
        return dynamic_cast<const ast::IntLiteralExpr*>(&e) ||
               dynamic_cast<const ast::FloatLiteralExpr*>(&e) ||
               dynamic_cast<const ast::CharLiteralExpr*>(&e) ||
               dynamic_cast<const ast::StringLiteralExpr*>(&e) ||
               dynamic_cast<const ast::VerbatimStringExpr*>(&e) ||
               dynamic_cast<const ast::BoolLiteralExpr*>(&e) ||
               dynamic_cast<const ast::NullLiteralExpr*>(&e) ||
               dynamic_cast<const ast::TypeOfExpr*>(&e) ||
               dynamic_cast<const ast::NameOfExpr*>(&e) ||
               dynamic_cast<const ast::DefaultExpr*>(&e) ||
               dynamic_cast<const ast::UnitExpr*>(&e) ||
               dynamic_cast<const ast::MarkedTransferExpr*>(&e);
    }

    // §22.3 (#747 R43): an `out` argument is an OUTPUT location, not a read —
    // "the variable need not be initialized before the call", and the callee
    // assigns it on every exit path, so a bare local passed as `out x` is
    // DEFINITELY ASSIGNED after the call. Mark it assigned and do not treat it
    // as a read. (Previously every call argument was visited as a read, so a
    // pre-declared `var x: int; M(out x)` drew a false "read of unassigned"
    // both at the argument position and at every post-call read. The inline
    // `out var x` form was unaffected only because its name is not tracked in
    // declared_.) A `ref` argument, by contrast, IS a read (the variable must
    // be assigned before the call) and is visited normally. An `out` target
    // that is a member/index location (`out obj.f`, `out arr[i]`) still reads
    // its sub-expressions (the receiver and index), so only a bare identifier
    // target is treated as a pure write.
    void visitCallArg(const ast::CallArgument& a) {
        if (!a.value) return;
        // fix #2206 / §22.8: a `ref`/`out` argument whose operand resolves to a
        // readonly field is an assignment to that field — the callee writes
        // through the alias (`out` definitely, `ref` possibly; soundness assumes
        // it might). The once-check (#2187's checkReadonlyOnceness) previously
        // fired only on AssignmentStmt/IncrementStmt, so a readonly field
        // assigned by `:=` could be written a SECOND time through an `out`/`ref`
        // argument without diagnosis — the same finality break the §7.4/#2116
        // narrowing anchor is sound only against (an observed non-null readonly
        // value must be final). Feed every `ref`/`out` operand through the
        // once-check: a first, non-loop write is admitted as the legitimate
        // single assignment (placement is separately gated by
        // checkRefOutFieldOperand while `inConstructor_`); a SECOND one, or any
        // such write textually inside a loop, is rejected. The check no-ops on
        // non-field operands (a local/param is not a FieldSymbol) and outside
        // the readonly ctor mode, so inline `out var x`, plain `out local`, and
        // all non-ctor call sites are unaffected.
        if (roCtorMode_ &&
            (a.modifier.rfind("out", 0) == 0 || a.modifier == "ref"))
            checkReadonlyOnceness(*a.value, a.value->span.begin);
        if (a.modifier.rfind("out", 0) == 0) {
            if (auto* id =
                    dynamic_cast<const ast::IdentifierExpr*>(a.value.get())) {
                // §22.4 (v0.181): an inline `out var x` declares a fresh local
                // that is definitely assigned *only on paths where this call
                // actually evaluated*. Marking it in `assigned_` here records
                // the assignment on the current (call-ran) path; the
                // short-circuit machinery (evalCond) drops it again on any
                // branch where the call was skipped. To make a later read on
                // such a skipped path an error, the name must also be tracked in
                // `declared_` — but only when it is a non-nullable reference
                // type (§7.9); value-typed and nullable inline out vars are
                // default-initialized and readable on every path, so they stay
                // untracked (as before). The owning inline forms (`out owned
                // var` / `out using var`) carry a per-path disposal obligation
                // handled by the ownership pass and are not DA-tracked here.
                // A plain `out x` / `out owned x` targets a pre-declared local
                // (already in `declared_`) and is just marked assigned
                // (§22.3 / #747).
                if (a.modifier == "out var" && inlineOutVarNeedsDA(*id))
                    declared_.insert(id->name);
                assigned_.insert(id->name);
                return;  // pure write — not a read
            }
        }
        // §29.11 point 3 (#1353): passing a bound lazy query as an argument
        // (not ref/out) consumes it — the single-enumeration right transfers
        // to the callee.  In well-typed Bezel the parameter type must be
        // IEnumerable<T> (or a supertype), so any non-ref/non-out lazy arg
        // is a Point 3 consumption. per §29.11 v0.165
        if (a.modifier != "ref") {  // not a ref arg (ref is still a read)
            if (const auto* id =
                    dynamic_cast<const ast::IdentifierExpr*>(a.value.get())) {
                if (isLazyEnumerableExprType(*a.value)) {
                    checkRead(*id);  // DA assignment check
                    markLazyConsumed(*id, "IEnumerable<T> argument");
                    return;  // consumed — treated as a read+consume
                }
            }
        }
        visitExpr(*a.value);
    }

    // §22.3: seed the callee's `out` parameters as declared-but-unassigned.
    // By-value and `ref` params are definitely-assigned on entry (the caller
    // bound them) and are not tracked here. Shared by every body-form entry
    // point (`run`, `runExpressionBody`) so they cannot drift apart.
    void seedOutParams(const std::vector<ast::Param>& params) {
        for (const auto& p : params) {
            if (!p.name.empty() && p.modifier.rfind("out", 0) == 0 &&
                p.modifier.rfind("out var", 0) != 0) {
                // "out" or "out owned" — callee must assign. "out var"
                // is a caller-side inline-decl modifier, not a callee
                // param mode, so it's excluded here (no method body uses
                // "out var" as a param modifier; the check is belt-and-
                // suspenders).
                outParams_.insert(p.name);
                declared_.insert(p.name);
                // Intentionally NOT inserted into assigned_ — starts unassigned.
            }
        }
    }

    // §22.3: check that every out-parameter is in assigned_ at the current
    // program point. Reports an error for each unassigned one. `loc` is the
    // source location to attach the diagnostic (null → no location).
    void checkOutParams(const SourceLocation* loc) {
        for (auto name : outParams_) {
            if (!assigned_.count(name)) {
                SourceLocation sl;
                if (loc) sl = *loc;
                ctx_.diagnostics().report(
                    Severity::Error, sl,
                    std::string("out parameter `") + std::string(name) +
                    "` is not definitely assigned on every exit path "
                    "(§22.3 — the callee must assign every `out` parameter "
                    "before returning)");
            }
        }
    }

    // §7.9 (Appendix C.1) / §22.7 (v0.184): at a constructor's normal exit — a
    // fall-off or an explicit `return` — every non-nullable field the class must
    // initialize (`fieldsNeedingInit_`) must be definitely assigned on THIS path
    // (`assignedFields_`, threaded as the all-paths join over branches, loops,
    // and case arms). A field missing here is exactly §7.9's forbidden state: an
    // object escaping construction with a non-nullable field unset — a null
    // dereference (SIGSEGV) on first read. The all-paths join makes this sound
    // where the ownership pass's earlier *any-path* collector (Slice O9) was not:
    // a field assigned on only some branch/arm/loop paths is now caught, while a
    // field assigned on every LIVE branch (including one whose sibling diverges,
    // #1755) is accepted. This is the §7.9 normal-exit completeness gate; the
    // ownership pass retains only the no-explicit-constructor screen (a class
    // with such a field and no constructor at all — a shape this pass never
    // visits). No-ops outside a constructor (`fieldsNeedingInit_` is empty). A
    // `this(...)` chain seeds every field as assigned (runConstructor), so a
    // delegating constructor reports nothing here. per §7.9, Appendix C.1
    //
    // fix #4001: `recordCtorMode_` reaches this gate too — a record
    // constructor's own §7.9 obligation for a body field/auto-property
    // (runRecordConstructor populates `fieldsNeedingInit_`/`assignedFields_`
    // identically, minus the class-only observation gate). fix #4015:
    // `staticCtorMode_` reaches it the same way, for a static constructor's
    // static-field obligation. No-ops outside all three modes
    // (`fieldsNeedingInit_` is empty then regardless).
    void checkCtorFieldCompletion(const SourceLocation* loc) {
        if (!ctorMode_ && !recordCtorMode_ && !staticCtorMode_) return;
        for (auto n : fieldsNeedingInit_) {
            if (!assignedFields_.count(n)) {
                SourceLocation sl;
                if (loc) sl = *loc;
                ctx_.diagnostics().report(
                    Severity::Error, sl,
                    std::string("constructor does not initialize non-nullable "
                                "field `") +
                        std::string(n) +
                        "` (§7.9 — every constructor's normal-exit path must "
                        "assign it, or chain to `this(…)`)");
            }
        }
    }

    // fix #2187 / §22.8: enforce single-assignment of a `readonly` field within
    // a constructor body. Called at every AssignmentStmt (any op: `:=`, `??=`,
    // compound) while `roCtorMode_`. Resolving the target to its FieldSymbol
    // covers every receiver shape uniformly — `this._f`, `TypeName._s`, and a
    // bare `_f` — and reads the declared `readonly` modifier directly. A field
    // that is already in `readonlyMaybeAssigned_` (from an earlier body
    // assignment on a reaching path, or from a field initializer seeded at body
    // entry) is being assigned a SECOND time; an assignment textually inside a
    // loop body could recur on a later iteration. Both break §22.8's
    // single-assignment rule (§7.4/§21.1) and are rejected. A first, non-loop
    // assignment records the field and is admitted (the legitimate single
    // assignment, including the conditional-single-across-branches shape, which
    // the per-branch reset + union join keeps accepted). Placement (assignment
    // outside any constructor) is a separate check — checkReadonlyAssignment in
    // ExprTypecheck.cpp; this one only adds the once-ness §7.4's soundness needs.
    void checkReadonlyOnceness(const ast::Expr& target,
                              const SourceLocation& loc) {
        if (!roCtorMode_) return;
        auto* fs = dynamic_cast<FieldSymbol*>(ctx_.exprSymbol(target));
        if (!fs) return;
        bool isReadonly = false;
        for (auto mod : fs->modifiers)
            if (mod == "readonly") { isReadonly = true; break; }
        if (!isReadonly) return;
        std::string_view name = fs->name;
        if (inLoopDepth_ > 0) {
            ctx_.diagnostics().report(
                Severity::Error, loc,
                std::string("cannot assign to readonly field `") +
                std::string(name) +
                "` inside a loop — a readonly field is assigned exactly once, "
                "and a loop body could assign it on more than one iteration "
                "(§22.8 single-assignment)");
            return;
        }
        if (readonlyMaybeAssigned_.count(name)) {
            ctx_.diagnostics().report(
                Severity::Error, loc,
                std::string("cannot assign to readonly field `") +
                std::string(name) +
                "` more than once — a readonly field is assigned exactly once "
                "across its field initializer and constructor body "
                "(§22.8 single-assignment)");
            return;
        }
        readonlyMaybeAssigned_.insert(name);
    }

    void checkRead(const ast::IdentifierExpr& id) {
        if (id.name == "this" || id.name == "base") return;
        // Only flag locals declared in this method that haven't been
        // assigned. Fields, params, and outer scopes pass silently.
        if (declared_.count(id.name) && !assigned_.count(id.name)) {
            ctx_.diagnostics().report(
                Severity::Error, id.span.begin,
                std::string("read of unassigned local `") +
                std::string(id.name) +
                "` (definite-assignment rule — every read must follow "
                "an assignment on every reaching path)");
        }
    }

    // ---- §22.7 (v0.184) construction-time observation helpers (ctor mode) ----

    // The `base(...)`/`this(...)` chaining call, if this constructor has one —
    // the body's first ExpressionStmt of a CallExpr on `this`/`base`
    // (Node.hpp). Null otherwise (implicit `base()`). `isThis` is set true
    // for `this(...)`, false for `base(...)` (written only when a chain call
    // is returned) — fix #3624: forwards to the ONE §12.8 chain-call
    // recognizer (`ast::chainCallOfStmt`, Node.hpp) so callers no longer
    // re-derive the this/base discrimination with a second `dynamic_cast` on
    // the callee.
    static const ast::CallExpr* getChainingCall(const ast::ConstructorDecl& cd,
                                                bool& isThis) {
        if (cd.body.empty() || !cd.body.front()) return nullptr;
        return ast::chainCallOfStmt(*cd.body.front(), isThis);
    }

    // §22.7 rule 1: report a synchronous `this`-observation that is not admitted
    // at the current program point — in the chaining position (no field yet
    // assigned) it is always an error; otherwise it is an error iff some
    // §7.9-strict field is not definitely assigned on every reaching path.
    // `what` names the observation form for the diagnostic. Returns nothing;
    // one diagnostic per unadmitted observation.
    void gateObservation(const SourceLocation& loc, const std::string& what) {
        if (!ctorMode_) return;
        if (inChainingPosition_) {
            ctx_.diagnostics().report(
                Severity::Error, loc,
                "cannot observe `this` (" + what +
                ") in the chaining position — `base(...)`/`this(...)` arguments "
                "run before any field of the declaring class is assigned "
                "(§22.7, v0.184)");
            return;
        }
        for (auto n : fieldsNeedingInit_) {
            if (!assignedFields_.count(n)) {
                ctx_.diagnostics().report(
                    Severity::Error, loc,
                    "cannot observe `this` (" + what +
                    ") before non-nullable field `" + std::string(n) +
                    "` is definitely assigned on every path — the observation "
                    "could read an unset field, breaking §7.9's guarantee that "
                    "a fully-constructed object has every non-nullable field "
                    "set (§22.7, v0.184)");
                return;
            }
        }
    }

    // fix #3583 (§22.8 step 0 / §22.10 / §22.7 rule 1, v0.286), WIDENED by
    // fix #3829 (§12.8 / §22.10, v0.291): a read of a field, auto-property, or
    // §22.10 PRIMARY-CONSTRUCTOR PARAMETER of `this` in the chaining position.
    //
    // §22.7 rule 1's gate above deliberately excludes a field read — "a field
    // or auto-property read is backing storage … its own §7.9/§28.8.1 DA rules
    // apply" — and a primary-constructor parameter resolves to a (compiler-
    // generated) FieldSymbol, so it fell into that exclusion and nothing
    // checked it. But its §7.9 rule is not the one that applies here: §22.8
    // puts the capture of those parameters at **step 0 of the chained-to
    // primary**, and says in as many words that a chaining constructor does not
    // run it:
    //
    //   "A constructor that chains via `this(...)` **skips steps 0 and 2** …
    //    (Step 0 is vacuous for such a constructor anyway: … primary-parameter
    //    capture happens inside the chained-to primary.)"
    //
    // Step 1 (the chain call, arguments included) is evaluated BEFORE that
    // step 0, so on every ordering the slot read here is unwritten. §22.10's
    // scope list agrees from the other side: the parameters are in scope
    // "Inside field initializers", "Inside the inheritance clause's base call"
    // and "Inside the body of any method, property, constructor, or destructor"
    // — and §12.8 puts the chaining position outside the body ("it is
    // syntactically distinct from the body's statements").
    //
    // This was memory-unsafe rather than merely undefined: with §22.8 step 0
    // ALSO running at the `new` site under a blind argument-i/field-i pairing,
    // the slot held whatever the convenience constructor's i-th argument was,
    // and `class B(n: int, s: string)` + `constructor(v: int, w: int)
    // this(v, s);` + `new B(5, 9)` loaded the integer 9 as a string pointer and
    // SIGSEGVed (issue 3562). Removing that store — which #3583 does, because
    // §22.8 forbids it — would have turned the crash into a quiet read of the
    // calloc zero. It is rejected instead, which is the outcome issues 3562 and
    // 3585 both recommend. (issue 3585 asks the spec to state the rule
    // explicitly; this implements the reading its own Recommendation gives, and
    // is recorded there.)
    //
    // #3583 landed only the primary-constructor-parameter half (`fs->posParam`
    // required below). §12.8 states the GENERAL rule of which that is one
    // instance — "it may **not** read a field or auto-property of the object
    // under construction — nor, in a class with a primary constructor, a
    // primary-constructor parameter" (§12.8, v0.291) — and §22.10 gives the
    // same fact from the field side: "as is reading any field or auto-property
    // of the not-yet-constructed `this` from that position, for the same
    // reason: a `this(...)`/`base(...)` argument runs before any field of the
    // declaring class is assigned" (§22.10, v0.291). A primary-constructor
    // parameter, a plain field, and an auto-property's backing storage are all
    // the SAME underlying fact — a FieldSymbol (or auto-property storage) with
    // no value yet in the chaining position — so #3829 widens this ONE
    // predicate to cover all three rather than adding a second one (issue
    // 3829, measured: `constructor(v: int) this(v, _f); end constructor;`
    // compiled cleanly before this fix, reading `_f`'s not-yet-initialized
    // storage).
    void gateChainPositionFieldRead(const ast::Expr& e,
                                    const SourceLocation& loc) {
        if (!ctorMode_ || !inChainingPosition_) return;
        if (auto* fs = dynamic_cast<FieldSymbol*>(ctx_.exprSymbol(e))) {
            if (!fs->parent || fs->parent->kind != ast::TypeDeclKind::Class)
                return;
            // §12.8: "the only reads a `base(...)`/`this(...)` argument may
            // make are of the constructor's own parameters, constants, and
            // statics" — a static field has no per-instance storage and does
            // not depend on `this` being constructed, so it is admitted. A
            // primary-constructor parameter is never static (§22.10 admits no
            // `static` modifier on a primary-constructor parameter), so this
            // check cannot exempt the `fs->posParam` arm below.
            for (auto mod : fs->modifiers)
                if (mod == "static") return;
            if (fs->posParam) {
                ctx_.diagnostics().report(
                    Severity::Error, loc,
                    std::string("cannot read primary-constructor parameter `") +
                    std::string(fs->name) +
                    "` in the chaining position — the primary-constructor "
                    "parameters are captured as step 0 of the chained-to "
                    "primary constructor, which runs after the `this(...)` "
                    "arguments are evaluated, so the parameter has no value "
                    "here (§22.8 step 0, §22.10)");
            } else {
                ctx_.diagnostics().report(
                    Severity::Error, loc,
                    std::string("cannot read field `") + std::string(fs->name) +
                    "` in the chaining position — a `this(...)`/`base(...)` "
                    "argument runs before any field of the declaring class is "
                    "assigned, so the field has no value here (§12.8, §22.10)");
            }
            return;
        }
        if (auto* ps = dynamic_cast<PropertySymbol*>(ctx_.exprSymbol(e))) {
            if (ps->isIndexer || !isAutoPropertyStorageSymbol(ps, ctx_)) return;
            if (!ps->parent || ps->parent->kind != ast::TypeDeclKind::Class)
                return;
            // §12.8 admits "the constructor's own parameters, constants, and
            // statics". A static auto-property is already excluded ABOVE, by
            // `isAutoPropertyStorageSymbol`: both the local collector
            // (SymbolCollection.cpp's hasAstAutoPropertyDecl) and the export
            // path (ModuleMetadataBuilder.cpp) refuse to classify a `static`
            // auto-property as auto-property storage, so control never reaches
            // here for one. The loop below is therefore REDUNDANT TODAY and is
            // kept only as a defence in depth: if that classification is ever
            // widened to admit statics, this gate must not start rejecting
            // them. `static_auto_property_control_accepted` pins the observable
            // behaviour so the guarantee survives either mechanism.
            for (auto mod : ps->modifiers)
                if (mod == "static") return;
            ctx_.diagnostics().report(
                Severity::Error, loc,
                std::string("cannot read auto-property `") +
                std::string(ps->name) +
                "` in the chaining position — a `this(...)`/`base(...)` "
                "argument runs before any field of the declaring class is "
                "assigned, so the auto-property's backing storage has no "
                "value here (§12.8, §22.10)");
        }
    }

    // §22.7 rule 2: a dispatching call on `this` to a `virtual`/`override`/
    // `abstract` member from a non-sealed (or abstract) constructor body is a
    // compile error — during construction the runtime type may be a derived
    // class whose field initializers have not run. `base.M()` is non-dispatching
    // and exempt (caller passes viaBase=true). Returns true iff it reported (so
    // the caller skips the rule-1 gate for the same site).
    bool checkVirtualDispatch(Symbol* calleeSym, bool viaBase,
                              const SourceLocation& loc,
                              std::string_view mname) {
        if (viaBase || classSealedNonAbstract_) return false;
        bool virt = false;
        if (auto* ms = dynamic_cast<MethodSymbol*>(calleeSym))
            virt = methodIsVirtualFamily(*ms);
        else if (auto* ps = dynamic_cast<PropertySymbol*>(calleeSym))
            virt = propIsVirtualFamily(*ps);
        if (!virt) return false;
        ctx_.diagnostics().report(
            Severity::Error, loc,
            "cannot call `virtual`/`override`/`abstract` member `" +
            std::string(mname) +
            "` on `this` from a constructor body of a non-sealed class — "
            "dispatch could reach a derived override over not-yet-initialized "
            "fields (§22.7, v0.184); make the class `sealed` (and not "
            "`abstract`), or call through `base.`");
        return true;
    }

    // True when `e` targets a field or auto-property of `this` (`this.f` or a
    // bare `f`) — a direct store that is NOT an observation and that counts
    // toward the §22.7 rule-1 gate (§22.7: direct field and auto-property
    // assignments keep their definite-assignment rules and the assignment
    // counts toward the gate). A computed (block/expression-bodied) property
    // target is excluded here — its setter is user code, an observation gated
    // elsewhere.
    bool isThisFieldOrAutoPropTarget(const ast::Expr& e, std::string_view& out) {
        Symbol* sym = nullptr;
        std::string_view name;
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
            if (!ma->target || !isNamedIdent(*ma->target, "this")) return false;
            sym = ctx_.exprSymbol(*ma);
            name = ma->member;
        } else if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(&e)) {
            sym = ctx_.exprSymbol(*id);
            name = id->name;
        } else {
            return false;
        }
        if (dynamic_cast<FieldSymbol*>(sym)) {
            out = name;
            return true;
        }
        // fix #3152 (§22.7 rule 1 / §2.8.2 clause 1): the STORAGE predicate.
        // `this.X` in a constructor body resolves to an IMPORTED PropertySymbol
        // whenever X is inherited from a base class in another module, and the
        // AST-only predicate answered `false` for every one of those — so the
        // store fell through to `visitExpr`, which reads it as a computed-
        // property setter invocation and gates it as an observation. MEASURED:
        // `this.W := 5;` on an imported base's auto-property was rejected under
        // §22.7 while its single-module twin ran and returned 7.
        if (auto* ps = dynamic_cast<PropertySymbol*>(sym)) {
            if (ps->isIndexer) return false;
            if (isAutoPropertyStorageSymbol(ps, ctx_)) {
                out = name;
                return true;
            }
            // fix #4015 / §7.9: `isAutoPropertyStorageSymbol` answers `false`
            // for EVERY static property by its own documented contract
            // (`hasAstAutoPropertyDecl`, SymbolCollection.cpp: "for auto mod :
            // ps->modifiers if (mod == "static") return false;") — it
            // classifies INSTANCE per-object backing-storage layout, which a
            // static property's storage (one process-global slot, not part of
            // any object's layout) never participates in. Reusing it verbatim
            // here made a static constructor's `Shared := v;` on its OWN
            // type's static auto-property invisible to this marking — a
            // MEASURED false reject (the assignment compiled and ran fine,
            // yet checkCtorFieldCompletion still reported it unassigned,
            // c16/c17-shaped repros caught by self-review before this fix
            // shipped). Scoped to `staticCtorMode_` only, so instance/record
            // ctor marking (which must keep using the INSTANCE-storage
            // question above) is untouched.
            if (staticCtorMode_) {
                auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
                if (pd && !pd->isIndexer &&
                    pd->bodyKind == ast::PropertyBodyKind::Auto) {
                    bool isStatic = false, isAbstract = false;
                    for (auto mod : ps->modifiers) {
                        if (mod == "static") isStatic = true;
                        else if (mod == "abstract") isAbstract = true;
                    }
                    if (isStatic && !isAbstract) {
                        out = name;
                        return true;
                    }
                }
            }
        }
        return false;
    }

    // §22.7 rule 1: a `this`-receiver call in a constructor body is an
    // observation. Detects the two receiver-`this` call shapes (`this.M(...)`/
    // `base.M(...)` and the implicit-`this` bare `M(...)`), applies rule 2
    // (virtual dispatch) then, unless rule 2 fired, the rule-1 gate, and visits
    // the arguments for their own observations and local DA. Returns true iff
    // the call was a receiver-`this` call it handled (so the caller returns).
    bool visitCtorCall(const ast::CallExpr& ce) {
        if (!ctorMode_ || !ce.callee) return false;
        bool viaThis = false, viaBase = false;
        std::string_view mname;
        if (auto* ma =
                dynamic_cast<const ast::MemberAccessExpr*>(ce.callee.get())) {
            if (ma->target && isNamedIdent(*ma->target, "this")) {
                viaThis = true;
                mname = ma->member;
            } else if (ma->target && isNamedIdent(*ma->target, "base")) {
                viaBase = true;
                mname = ma->member;
            }
        } else if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                       ce.callee.get())) {
            // A bare call to a non-static instance method is an implicit-`this`
            // call (an observation). `this(...)`/`base(...)` chaining calls are
            // handled in runConstructor and never reach here as body statements.
            if (id->name != "this" && id->name != "base") {
                if (auto* ms =
                        dynamic_cast<MethodSymbol*>(ctx_.exprSymbol(*id))) {
                    bool isStatic = false;
                    for (auto m : ms->modifiers)
                        if (m == "static") isStatic = true;
                    if (!isStatic) {
                        viaThis = true;
                        mname = id->name;
                    }
                }
            }
        }
        if (!viaThis && !viaBase) return false;
        Symbol* calleeSym = ctx_.exprSymbol(*ce.callee);
        // §22.7 rule 1 (v0.184, #1752): when the callee resolves to a field or
        // an auto-property, `this.X(...)`/`base.X(...)` is *not* an instance-
        // call observation — it is a field/auto-prop READ (already excluded
        // from the gate; see visitCtorMemberAccess) followed by an ordinary
        // invocation of the function VALUE that read produced. Invoking a
        // function value has no bearing on `this` — only a call whose
        // *receiver* is `this`/`base` (a method or computed-property accessor
        // invocation) is the observation rule 1 describes. Do not recurse into
        // the callee (its receiver is `this`/`base`, already accounted for as
        // a non-observing read); just visit the call arguments.
        // fix #3152: the STORAGE predicate — an IMPORTED function-typed
        // auto-property invoked as `this.F(...)` is the same field-read-then-
        // invoke shape as a local one, and the AST-only predicate called it a
        // method-on-`this` observation.
        if (dynamic_cast<FieldSymbol*>(calleeSym) ||
            isAutoPropertyStorageSymbol(calleeSym, ctx_)) {
            for (const auto& a : ce.args) visitCallArg(a);
            return true;
        }
        bool reported =
            checkVirtualDispatch(calleeSym, viaBase, ce.span.begin, mname);
        if (!reported)
            gateObservation(ce.span.begin,
                            "instance call `" + std::string(mname) + "`");
        // Visit arguments for their own observations / DA / lazy tracking —
        // but not the callee (its `this`/`base` receiver is this observation,
        // already accounted for).
        for (const auto& a : ce.args) visitCallArg(a);
        return true;
    }

    // §22.7 rule 1: a read of a member of `this`/`base` in a constructor body.
    // A field or auto-property read is backing storage — NOT an observation
    // (its own §7.9/§28.8.1 DA rules apply). A computed (block/expression-
    // bodied) property read is an accessor invocation — an observation (rule 1,
    // and rule 2 if virtual). Returns true iff `e` was a `this`/`base` member
    // access it handled (so the caller does not recurse into the receiver and
    // wrongly flag it as a bare-`this` observation).
    bool visitCtorMemberAccess(const ast::MemberAccessExpr& ma) {
        if (!ctorMode_ || !ma.target) return false;
        bool viaThis = isNamedIdent(*ma.target, "this");
        bool viaBase = isNamedIdent(*ma.target, "base");
        if (!viaThis && !viaBase) return false;
        if (auto* ps = dynamic_cast<PropertySymbol*>(ctx_.exprSymbol(ma))) {
            // fix #3152 (§22.7 rule 1 / §2.8.2 clause 1): the STORAGE
            // predicate. MEASURED — `var t := base.W;` on an imported base's
            // auto-property, read before a local non-nullable field is
            // assigned, was rejected as "cannot observe `this` (property `W`)"
            // while its character-identical single-module twin compiled and
            // ran. An auto-property read is backing storage, not an accessor
            // invocation, wherever it was declared.
            if (!ps->isIndexer && !isAutoPropertyStorageSymbol(ps, ctx_)) {
                bool reported = checkVirtualDispatch(ps, viaBase, ma.span.begin,
                                                     ma.member);
                if (!reported)
                    gateObservation(ma.span.begin,
                                    "property `" + std::string(ma.member) + "`");
            }
        }
        // fix #3583 (§22.8 step 0 / §22.10), widened by fix #3829 (§12.8 /
        // §22.10): the `this.p` spelling of a field, auto-property, or
        // primary-constructor parameter in the chaining position. §22.10's
        // capture rule makes a primary-constructor parameter a compiler-
        // generated field, so both spellings reach the same unwritten slot.
        gateChainPositionFieldRead(ma, ma.span.begin);
        // Field / auto-property / method-group / unresolved: not an
        // observation; do not recurse into the `this`/`base` receiver.
        return true;
    }
};

// §22.3 (fix #2595): does this parameter list declare a callee-side `out`
// parameter (plain `out` or `out owned`)? The `out var` spelling is a
// caller-side inline-declaration modifier, never a parameter mode — same
// exclusion MethodDA::seedOutParams makes.
bool hasOutParam(const std::vector<ast::Param>& params) {
    for (const auto& p : params) {
        if (!p.name.empty() && p.modifier.rfind("out", 0) == 0 &&
            p.modifier.rfind("out var", 0) != 0)
            return true;
    }
    return false;
}

// fix #4011 / §7.9: forward-declared so checkMember's NestedTypeMember arm
// (below) can run the same no-ctor screen checkTopDecl runs for a top-level
// record — defined after checkRecordNoCtorFieldCompletion, further down this
// file.
void checkNoCtorScreenForType(const ast::TypeDecl& td, SemaContext& ctx);

// `enclosing` is the type whose members are being walked (null for extension
// members and top-level free code). §22.7 construction-time `this`-observation
// checking runs only for a CLASS's non-static constructors: records have no
// base-first-construction/virtual-dispatch footgun for it to guard against.
// Fix #4001 narrows that to observation-gating specifically — a record's
// constructor DOES carry its own §7.9 field-completion obligation (below,
// `runRecordConstructor`/`recordCtorMode_`), the same as the O9 field-DA
// obligation Ownership.cpp enforces for a class with no explicit constructor
// at all. Interfaces/enums/choices still get the ordinary local-DA walk
// unchanged — neither §22.7 nor §7.9's field obligation applies to them.
void checkMember(const ast::Member& m, SemaContext& ctx,
                 const ast::TypeDecl* enclosing) {
    if (auto* md = dynamic_cast<const ast::MethodDecl*>(&m)) {
        if (md->bodyKind == ast::MethodBodyKind::Block) {
            MethodDA da(ctx);
            da.run(md->params, md->stmts);
        } else if (md->bodyKind == ast::MethodBodyKind::Expression &&
                   md->exprBody && hasOutParam(md->params)) {
            // §22.3 (v0.236, fix #2595): an expression-bodied method was never
            // walked, so its `out` parameters escaped the callee-side
            // definite-assignment obligation entirely. Walk the body as the
            // single return path it is (MethodDA::runExpressionBody).
            //
            // The walk is entered ONLY for a method that actually declares an
            // `out` parameter. That is deliberate scope control, not an
            // oversight: an expression body declares no locals, so with no out
            // parameter seeded the walker's `declared_` set is empty and the
            // §22.3 check is the only diagnosis it could produce — while
            // entering the walk unconditionally would newly subject every
            // expression-bodied method in the language to the walker's other
            // (§29.11 lazy-consumption) bookkeeping for no §22.3 benefit. The
            // residual local-DA gap for expression bodies, property accessors,
            // and lambda bodies is pre-existing and untouched here.
            MethodDA da(ctx);
            da.runExpressionBody(md->params, *md->exprBody);
        }
    } else if (auto* cd = dynamic_cast<const ast::ConstructorDecl*>(&m)) {
        MethodDA da(ctx);
        bool inClass =
            enclosing && enclosing->kind == ast::TypeDeclKind::Class;
        bool inRecord =
            enclosing && enclosing->kind == ast::TypeDeclKind::Record;
        if (!cd->isStatic && inClass) {
            da.runConstructor(*cd, *enclosing);  // §22.7 + §22.8 checking
        } else if (cd->isStatic && inClass) {
            // fix #2187 / §22.8: static-ctor readonly single-assignment.
            da.runStaticConstructor(*cd, *enclosing);
        } else if (!cd->isStatic && inRecord) {
            // fix #2207 / §22.8: record-ctor readonly single-assignment. §22.8
            // and the §7.4/#2116 narrowing it underwrites apply to records too,
            // so the #2187 exploit reproduces on records (pre-existing SEGV).
            // Enable roCtorMode_ without the class-specific §22.7
            // observation-gate (records have no base-first-construction /
            // virtual-dispatch footgun for gateObservation/checkVirtualDispatch
            // to guard against, §22.7 rules 1-2 — genuinely class-specific).
            //
            // CORRECTED by fix #4001: an earlier revision of this comment
            // additionally claimed "§7.9 applies to class instances, not
            // record values — records are value types with no per-instance
            // non-nullable-field set to check", and skipped the §7.9
            // fall-off completeness check on that premise. That premise does
            // not hold: §7.9's own text states the must-initialize rule
            // generically over "every non-nullable field", and §9.4 confirms
            // records are inside it by name — a record constructor's stores
            // are "governed by §22.8's once-only and §7.9's definite-
            // assignment machinery" (spec line ~1452). A record's non-
            // nullable body field or auto-property left unassigned by every
            // constructor is exactly §7.9's forbidden state, and `new R(...)`
            // reading it back SIGSEGVs identically to the class case (three
            // measured repros, issue #4001). `runRecordConstructor` now
            // tracks the SAME `fieldsNeedingInit_`/`assignedFields_` §7.9
            // obligation `runConstructor` tracks for classes and runs the
            // same `checkCtorFieldCompletion` fall-off check
            // (`recordCtorMode_`) — everything EXCEPT the observation gate,
            // which stays class-only as this comment always correctly said.
            // A record with NO explicit constructor at all is not visited
            // here (there is no ConstructorDecl member to walk); that shape
            // is screened separately at the type level, mirroring
            // Ownership.cpp's `checkOwnedFieldDA` no-ctor screen for classes
            // (see `checkRecordNoCtorFieldCompletion` below).
            da.runRecordConstructor(*cd, *enclosing);
        } else if (cd->isStatic && inRecord) {
            // fix #4015 / §7.9: a record's static constructor previously
            // matched NONE of the branches above and fell to the plain
            // `da.run` path below — this comment used to say "Static ctors
            // on records (if admitted) are out of scope here", but they ARE
            // admitted (`static constructor()` on a record parses and
            // compiles, MEASURED) and that fall-through meant a record
            // static constructor got NEITHER readonly single-assignment
            // tracking NOR §7.9 field-completion tracking — a `static
            // readonly` field double-assigned in a record's static
            // constructor compiled with no §22.8 diagnostic, and one left
            // unassigned on some/every path SIGSEGVed on first read, exactly
            // as the class case did before fix #2187/this fix. `runStatic
            // Constructor` has nothing class-specific in it (unlike
            // `runConstructor`'s §22.7 this-observation gating), so it is
            // reused verbatim for a record's static constructor — same
            // routing precedent as `runRecordConstructor` reusing
            // `runConstructor`'s class-agnostic bookkeeping.
            da.runStaticConstructor(*cd, *enclosing);
        } else {
            // Interfaces, enums, choices, and any remaining type — readonly
            // single-assignment (§22.8) does not apply. No class this-
            // observation gating either. Plain local-DA walk.
            da.run(cd->params, cd->body);
        }
    } else if (auto* dd = dynamic_cast<const ast::DestructorDecl*>(&m)) {
        MethodDA da(ctx);
        da.run(dd->body);  // destructors have no params
    } else if (auto* od = dynamic_cast<const ast::OperatorDecl*>(&m)) {
        if (od->bodyKind == ast::MethodBodyKind::Block) {
            MethodDA da(ctx);
            da.run(od->params, od->stmts);
        }
    } else if (auto* nt = dynamic_cast<const ast::NestedTypeMember*>(&m)) {
        if (nt->decl) {
            // fix #4011 / §7.9: a nested type's OWN no-ctor obligation was
            // never checked — this arm recursed into nt->decl->members
            // (below) but never re-ran the type-level no-ctor screen for
            // nt->decl itself, so a nested record with a non-nullable field/
            // auto-property and no constructor compiled clean and SIGSEGVed
            // on first read, even though the identical top-level shape is
            // rejected by checkTopDecl. Run the same screen here, keyed on
            // nt->decl->kind exactly as checkTopDecl does.
            checkNoCtorScreenForType(*nt->decl, ctx);
            for (const auto& innerM : nt->decl->members) {
                if (innerM) checkMember(*innerM, ctx, nt->decl.get());
            }
        }
    }
}

// fix #4001 / §7.9: the record analogue of Ownership.cpp's `checkOwnedFieldDA`
// no-ctor screen. `runRecordConstructor` (above) closes §7.9's obligation for
// every DECLARED record constructor, but a record with NO explicit instance
// constructor at all has no ConstructorDecl member for checkMember to visit —
// the same shape Ownership.cpp's own comment names for classes ("a class with
// such a field and no constructor at all — a shape this pass never visits").
// A record's auto-generated positional constructor binds ONLY the declared
// POSITIONAL fields (§9.1) — positional fields never appear in `rec.members`
// (they ride `TypeDecl::positionalFields` instead, ast/Node.hpp), so this loop
// naturally sees only §9.2 body storage; a body field or auto-property the
// synthesized constructor does not bind is left at its zero-filled slot,
// which is exactly the unset-non-nullable-field state §7.9 forbids (repro 1/2
// of issue #4001: a combined-form record's explicit body field, and a body
// record's `readonly property` auto-property, both with no user constructor
// at all). Scoped to TOP-LEVEL record declarations only, mirroring
// `checkOwnedFieldDA`'s own scope — a nested record (or class) with no
// constructor is a pre-existing, shared gap in both passes (neither is called
// from the NestedTypeMember branch of `checkMember`), filed separately as
// issue #4011 rather than folded in here.
void checkRecordNoCtorFieldCompletion(const ast::TypeDecl& rec,
                                      SemaContext& ctx) {
    bool hasInstanceCtor = false;
    for (const auto& m : rec.members) {
        if (auto* cd = dynamic_cast<const ast::ConstructorDecl*>(m.get())) {
            if (!cd->isStatic) { hasInstanceCtor = true; break; }
        }
    }
    // A record WITH an explicit instance constructor is fully covered by
    // runRecordConstructor's own per-constructor fall-off check (every
    // declared constructor is visited via checkMember); nothing to add here.
    if (hasInstanceCtor) return;
    for (const auto& m : rec.members) {
        std::string_view name;
        SourceLocation loc;
        if (auto* f = dynamic_cast<const ast::FieldDecl*>(m.get())) {
            if (!isSevenNineStrictField(*f, ctx)) continue;
            name = f->name;
            loc = f->span.begin;
        } else if (auto* p = dynamic_cast<const ast::PropertyDecl*>(m.get())) {
            if (!isSevenNineStrictAutoProp(*p, ctx)) continue;
            name = p->name;
            loc = p->span.begin;
        } else {
            continue;
        }
        ctx.diagnostics().report(
            Severity::Error, loc,
            std::string("non-nullable field `") + std::string(name) +
                "` needs an initializer or a constructor that assigns it — "
                "this record declares no constructor, so its auto-generated "
                "positional constructor (which binds only the record's "
                "positional fields, §9.1) leaves it unset (§7.9)");
    }
}

// fix #4015 / §7.9: the STATIC no-ctor screen — the static-field analogue of
// checkRecordNoCtorFieldCompletion above and Ownership.cpp's checkOwnedField
// DA, and the piece issue #4015 reports missing altogether: no code anywhere
// screened "declares a non-nullable static field/auto-property, and declares
// no static constructor to assign it" for EITHER a class or a record. Unlike
// the instance-side split (Ownership.cpp for classes, this file for
// records — kept apart by §22.7's class-only this-observation gating), the
// static-completion obligation carries no such class/record asymmetry: a
// static constructor has no receiver to gate, so ONE screen covers both type
// kinds, run from checkNoCtorScreenForType below for both (that dispatcher
// already reaches both kinds via checkTopDecl and, per fix #4011, the
// NestedTypeMember arm, so a nested class's or record's static field gets
// the same screen for free). A type WITH a declared static constructor is
// fully covered by runStaticConstructor's own per-constructor fall-off check
// (every declared static constructor is visited via checkMember); nothing to
// add here for that case — mirroring the instance-side screens' own
// has-a-constructor early return.
void checkStaticNoCtorFieldCompletion(const ast::TypeDecl& td,
                                      SemaContext& ctx) {
    if (td.kind != ast::TypeDeclKind::Class &&
        td.kind != ast::TypeDeclKind::Record)
        return;
    bool hasStaticCtor = false;
    for (const auto& m : td.members) {
        if (auto* cd = dynamic_cast<const ast::ConstructorDecl*>(m.get())) {
            if (cd->isStatic) { hasStaticCtor = true; break; }
        }
    }
    if (hasStaticCtor) return;
    for (const auto& m : td.members) {
        std::string_view name;
        SourceLocation loc;
        if (auto* f = dynamic_cast<const ast::FieldDecl*>(m.get())) {
            if (!isSevenNineStrictStaticField(*f, ctx)) continue;
            name = f->name;
            loc = f->span.begin;
        } else if (auto* p = dynamic_cast<const ast::PropertyDecl*>(m.get())) {
            if (!isSevenNineStrictStaticAutoProp(*p, ctx)) continue;
            name = p->name;
            loc = p->span.begin;
        } else {
            continue;
        }
        ctx.diagnostics().report(
            Severity::Error, loc,
            std::string("non-nullable static field `") + std::string(name) +
                "` needs an initializer or a static constructor that "
                "assigns it (§7.9)");
    }
}

// fix #4011 / §7.9: shared dispatch so checkTopDecl (top-level types) and
// checkMember's NestedTypeMember arm (nested types) run the IDENTICAL no-ctor
// screen — before this helper, the screen was invoked only from checkTopDecl,
// so a nested record with a non-nullable field/auto-property and no
// constructor bypassed it entirely. Classes get their own analogous
// INSTANCE-side screen from the ownership pass (checkOwnedFieldDA,
// Ownership.cpp) via its own mirrored dispatch (checkNoCtorScreenForType
// there) — that pass and this one are separate translation units invoked
// from separate driver entry points (checkOwnership / checkDefiniteAssignment
// ), so neither can call into the other's TopDecl walk. fix #4015: the STATIC
// screen (checkStaticNoCtorFieldCompletion) is added here, for BOTH type
// kinds, rather than split by kind like the instance-side screens are — see
// that function's own comment for why no class/record split is needed for
// statics.
void checkNoCtorScreenForType(const ast::TypeDecl& td, SemaContext& ctx) {
    if (td.kind == ast::TypeDeclKind::Record) {
        checkRecordNoCtorFieldCompletion(td, ctx);
    }
    checkStaticNoCtorFieldCompletion(td, ctx);
}

void checkTopDecl(const ast::TopDecl& d, SemaContext& ctx) {
    if (auto* td = dynamic_cast<const ast::TypeDecl*>(&d)) {
        checkNoCtorScreenForType(*td, ctx);
        for (const auto& m : td->members) {
            if (m) checkMember(*m, ctx, td);
        }
    } else if (auto* ext = dynamic_cast<const ast::ExtensionDecl*>(&d)) {
        for (const auto& m : ext->members) {
            if (m) checkMember(*m, ctx, /*enclosing=*/nullptr);
        }
    }
}

} // namespace

bool checkDefiniteAssignment(const ast::SourceFile& src, SemaContext& ctx) {
    for (const auto& d : src.topDecls) {
        if (d) checkTopDecl(*d, ctx);
    }
    return !ctx.diagnostics().hasErrors();
}

} // namespace bezel::sema
