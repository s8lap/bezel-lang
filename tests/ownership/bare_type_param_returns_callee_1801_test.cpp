// Spec-derived tests for the §28.3 *Bare-type-parameter returns* keystone
// (v0.194, #1801 sub-task S6) — the CALLEE-side, per-instantiation-category
// admission (the §3.6 rule-2 pattern).
//
// §28.3 "Bare-type-parameter returns" — Callee side (v0.194): "The check is
// signature-keyed and decidable, and is enforced **per instantiation category**
// — the §3.6 rule 2 pattern ...: at value and runtime-managed substitutions of
// the returned parameter it is vacuous (returning a parameter copies the value);
// at class and interface substitutions the return-path admission is enforced ...
// With a **non-empty** potential source set, every return path must supply
// `null` or a borrow whose sources outlive the call: the receiver ..., a plain
// source parameter ..., or static storage .... A fresh owned value, or a borrow
// of an owned local, a `using var`, an `owned` parameter, or a temporary, is
// rejected at the return site — the value would die with the callee or escape
// unowned; a generic body that produces fresh values declares `owned T` and
// transfers."
//
// §3.6 rule 2 (per-instantiation evaluation): the return-path admission is
// enforced at class/interface substitutions and VACUOUS at value/runtime-managed
// ones; the `where T: value` exclusion (spec item 5) keeps a value-only returned
// parameter outside the rule entirely.
//
// #2064 (per §28.3 v0.194, §3.6 rule 2): the body-side admission is decidable
// at the declaration ONLY when ALL instantiations of the returned parameter are
// class/interface (a `where T: class` constraint at the parameter's own level).
// For a mixed-domain parameter (no class constraint), value substitutions are
// spec-vacuous, so the body is ADMITTED at the declaration; the per-call-site
// class-substitution check is the recorded residual (the AOT ownership pass
// does not re-check the body per instantiation today).
//
// The callee-side hole this closes: before S6 the ownership pass classified a
// bare-`T` non-`owned` return as unborrowable (isBorrowableType is false for an
// abstract type parameter), so checkStaticBorrowReturn was vacuous and a callee
// returning an `owned` parameter / a fresh owned value under a bare-`T` signature
// compiled clean — the caller-side classifies the result as a borrow of its
// arguments (S1/S5), so the returned value would dangle or leak.

#include <string>
#include <string_view>

#include "support/aot_support.hpp"        // runAot — AOT oracle for #2064
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;

namespace {

bool errorSays(const OwnershipResult& r, std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

// The §28.3 v0.194 callee-side reject message (a stable substring).
constexpr std::string_view kBareReject =
    "bare-type-parameter return may not hand back a value the caller cannot own";

}  // namespace

// ---------------------------------------------------------------------------
// REJECT-1. A static `Sel<T>(a: T, owned b: T): T where T: class` returning the
// `owned` parameter `b` under a bare-`T` (non-`owned`) return. The potential
// source set is {a} (the plain parameter mentions T; the `owned` parameter is
// never a source), so it is NON-EMPTY. The `where T: class` constraint forces
// every substitution to be a class — the body-side admission is decidable at
// the declaration (#2064 per-instantiation-category gate). Returning `b` hands
// back a value the callee owns and disposes at frame end, while every caller
// re-establishes only a BORROW — a dangling borrow. Rejected at the return site
// (declare `owned T`).
// FAILS WITHOUT THE FIX: bare-`T` was unborrowable, so this compiled clean.
// ---------------------------------------------------------------------------

BEZEL_TEST(bare_type_param_returns_callee,
           static_returns_owned_param_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Util "
        "  public static method Sel<T>(a: T, owned b: T): T where T: class "
        "    return b; end Sel; "
        "end Util;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, kBareReject));
}

// The `=>` expression-body sibling of REJECT-1 — checked identically.
BEZEL_TEST(bare_type_param_returns_callee,
           static_expr_body_returns_owned_param_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Util "
        "  public static method Sel<T>(a: T, owned b: T): T where T: class => b; "
        "end Util;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, kBareReject));
}

// ---------------------------------------------------------------------------
// REJECT-2. A fresh owned value returned under a bare-`T` signature with a
// non-empty source set. `where T: class` fixes the class-instantiation category
// (where the rule bites — #2064's per-instantiation gate fires here); the
// return `new T()` is a fresh owned value the caller cannot own through a
// non-`owned` return. Rejected. FAILS WITHOUT THE FIX.
// ---------------------------------------------------------------------------

BEZEL_TEST(bare_type_param_returns_callee,
           static_returns_fresh_owned_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Util "
        "  public static method Fresh<T>(a: T): T where T: class, new() "
        "    return new T(); end Fresh; "
        "end Util;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, kBareReject));
}

// ---------------------------------------------------------------------------
// REJECT — enclosing-type parameter. The classification is inherited from the
// ENCLOSING TYPE's parameter, not only a method-level one: `class Box<T> where
// T: class ... static method Make(a: T, owned b: T): T` returns the class's own
// `T`. The enclosing type's `where T: class` makes the admission decidable at
// the declaration (#2064 per-instantiation gate). Same per-instantiation reject
// as the method-level case.
// ---------------------------------------------------------------------------

BEZEL_TEST(bare_type_param_returns_callee,
           enclosing_type_param_returns_owned_param_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Box<T> where T: class "
        "  public static method Make(a: T, owned b: T): T return b; end Make; "
        "end Box;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, kBareReject));
}

// ---------------------------------------------------------------------------
// ADMIT — the plain-parameter passthrough. `Sel<T>(a: T, b: T): T return a;`
// returns a plain (non-`owned`) parameter — a borrow whose source the caller
// re-establishes against its own argument. This is the legitimate generic
// passthrough the callee side must NOT reject (the §11.8 `Sum<T>`-style guard).
// ---------------------------------------------------------------------------

BEZEL_TEST(bare_type_param_returns_callee,
           static_returns_plain_param_accepted_spec_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Util "
        "  public static method Sel<T>(a: T, b: T): T return a; end Sel; "
        "end Util;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ADMIT — the instance-method receiver-sourced borrow. `class Box<T> ... method
// Get(): T return this._v;` returns a bare read reached through the receiver's
// field — a receiver-sourced borrow, admitted.
BEZEL_TEST(bare_type_param_returns_callee,
           instance_returns_receiver_field_accepted_spec_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Box<T> "
        "  private _v: T; "
        "  public constructor(v: T) this._v := v; end constructor; "
        "  destructor end destructor; "
        "  public method Get(): T return this._v; end Get; "
        "end Box;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ADMIT — value instantiation is vacuous (§3.6 rule 2). The SAME `Sel<T> return
// a;` declaration, instantiated at `T = int` (a value) and at a class, compiles:
// at a value substitution the return is a copy, no borrow attaches; the callee
// check bit nothing at the declaration (`return a` is a plain-parameter borrow).
BEZEL_TEST(bare_type_param_returns_callee,
           value_and_class_instantiation_accepted_spec_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "  public method Poke(): int return 1; end Poke; "
        "end C; "
        "class Util "
        "  public static method Sel<T>(a: T, b: T): T return a; end Sel; "
        "end Util; "
        "class Prog "
        "  public static method Main(): int "
        "    var x := Util.Sel(1, 2); "          // T = int (value): vacuous
        "    var a := new C(); "
        "    var b := new C(); "
        "    var m := Util.Sel(a, b); "          // T = C (class)
        "    var p := m.Poke(); "
        "    a.Dispose(); "
        "    b.Dispose(); "
        "    return x + p; "
        "  end Main; "
        "end Prog;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// ADMIT — the `where T: value` exclusion (spec item 5). The SAME fresh-owned
// body as REJECT-2 but with `where T: value` keeps the returned parameter
// outside the rule entirely: at every substitution T is a value type, so
// `new T()` is a value copy. The callee check must be vacuous here. (Contrast
// with REJECT-2's `where T: class`, which is rejected — same body, opposite
// instantiation category: the per-instantiation-category discipline in one
// pair.)
// ---------------------------------------------------------------------------

BEZEL_TEST(bare_type_param_returns_callee,
           where_t_value_exclusion_accepted_spec_28_3_3_6) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Util "
        "  public static method Fresh<T>(a: T): T where T: value, new() "
        "    return new T(); end Fresh; "
        "end Util;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// ADMIT — the `owned T` return is outside this rule (the ordinary ownership
// transfer, §28.8). `Take<T>(a: T, owned b: T): owned T return move b;` declares
// the transfer explicitly and is accepted — the recognizer excludes `owned T`
// returns (a §11.8 `Sum<T>`-style producer spells `owned T`).
// ---------------------------------------------------------------------------

BEZEL_TEST(bare_type_param_returns_callee,
           owned_return_transfer_accepted_spec_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Util "
        "  public static method Take<T>(a: T, owned b: T): owned T "
        "    where T: class return b; end Take; "
        "end Util;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// ADMIT — the EMPTY source set falls under the factory regime, not this check.
// `Create<T>(): T where T: new()` is a static method with no receiver and no
// plain parameter mentioning T — a structurally empty source set — so returning
// a fresh value is the sourceless-factory case (owned-with-warning caller-side),
// NOT the non-empty-set reject. The callee check is a no-op here.
// ---------------------------------------------------------------------------

BEZEL_TEST(bare_type_param_returns_callee,
           empty_source_set_factory_not_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Util "
        "  public static method Create<T>(): T where T: new() "
        "    return new T(); end Create; "
        "end Util;");
    // The non-empty-set bare-`T` reject must NOT fire (the factory regime
    // governs; the caller side treats the result as owned-with-warning).
    BEZEL_EXPECT_FALSE(errorSays(r, kBareReject));
}

// ---------------------------------------------------------------------------
// #2064 — per-instantiation-category admission (§28.3 v0.194, §3.6 rule 2).
//
// A mixed-domain generic helper (no `where T: class` constraint) whose body
// returns a fresh value via the constraint's `new()` member has a per-call-site
// verdict: at a value substitution the body is a value copy (vacuous per §28.3
// v0.194), and only at a class substitution would the borrow-vs-owned admission
// bite. The declaration-level body check is NOT decidable for the mixed-domain
// case — the per-call-site class-substitution enforcement is the recorded
// residual. The declaration must be ADMITTED so legitimate §11.8-style generic
// helpers (Sum, ZO, Nxt, P, Calc — the §2.19 producer members keyed through a
// constraint) are not over-rejected. WITHOUT THE #2064 FIX this body emitted
// kBareReject and forced these signatures to be spelled `owned T` to sidestep.
// ---------------------------------------------------------------------------

// Mixed-domain, non-empty source set, fresh owned value — ADMITTED at the
// declaration. The #2048-era generic helpers (e.g. `Sum<T>` over `INumber<T>`)
// have exactly this shape: a plain T source parameter plus a fresh value via
// the constraint's producer members.
BEZEL_TEST(bare_type_param_returns_callee,
           mixed_domain_fresh_owned_admitted_per_instantiation_2064) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Util "
        "  public static method Make<T>(a: T): T where T: new() "
        "    return new T(); end Make; "
        "end Util;");
    BEZEL_EXPECT_FALSE(errorSays(r, kBareReject));
}

// Mixed-domain, non-empty source set, owned-parameter borrow — ADMITTED at the
// declaration. The SAME signature as REJECT-1 but WITHOUT the `where T: class`
// constraint is admitted: at a value instantiation the `owned` modifier is
// erased per §28.1 (v0.196) and the body is a plain value copy. Contrast with
// REJECT-1, which uses `where T: class` to make the rejection decidable.
BEZEL_TEST(bare_type_param_returns_callee,
           mixed_domain_owned_param_admitted_per_instantiation_2064) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Util "
        "  public static method Sel<T>(a: T, owned b: T): T return b; end Sel; "
        "end Util;");
    BEZEL_EXPECT_FALSE(errorSays(r, kBareReject));
}

// Mixed-domain at the enclosing-type level — ADMITTED at the declaration. Same
// shape as the enclosing-type REJECT but WITHOUT the enclosing type's
// `where T: class` constraint, so the per-instantiation gate (#2064) does not
// fire at the declaration.
BEZEL_TEST(bare_type_param_returns_callee,
           enclosing_type_param_mixed_domain_admitted_per_instantiation_2064) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Box<T> "
        "  public static method Make(a: T, owned b: T): T return b; end Make; "
        "end Box;");
    BEZEL_EXPECT_FALSE(errorSays(r, kBareReject));
}

// ---------------------------------------------------------------------------
// #2064 contrast pair — `where T: notnull` is NOT a class constraint (it admits
// both class and non-null value substitutions), so the per-instantiation gate
// does not fire at the declaration; the body is admitted. The class-only reject
// remains decidable only under `where T: class`.
// ---------------------------------------------------------------------------

BEZEL_TEST(bare_type_param_returns_callee,
           notnull_constraint_does_not_force_class_category_2064) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Util "
        "  public static method Sel<T>(a: T, owned b: T): T where T: notnull "
        "    return b; end Sel; "
        "end Util;");
    BEZEL_EXPECT_FALSE(errorSays(r, kBareReject));
}

// ---------------------------------------------------------------------------
// #2064 — AOT-level end-to-end run (§28.3 v0.194, §3.6 rule 2). The same
// mixed-domain generic helper from the sema-level ADMIT test above, called at
// a VALUE-type (record Pt) substitution: the program compiles (the body is no
// longer over-rejected at the declaration) AND runs to clean exit — at a value
// substitution the return is a plain value copy, vacuous per §28.3 v0.194.
// FAILS WITHOUT THE #2064 FIX: the over-strict declaration-level check emitted
// kBareReject at sema and the AOT compile never linked. (The caller-side
// classification is unchanged: at a value substitution the call produces a
// value, not a borrow — no leak.)
// ---------------------------------------------------------------------------

BEZEL_TEST(bare_type_param_returns_callee,
           mixed_domain_helper_runs_at_value_substitution_spec_28_3) {
    std::string src =
        "module M; "
        "record Pt "
        "  public x: int := 0; "
        "  public y: int := 0; "
        "  public constructor() end constructor; "
        "  public constructor(nx: int, ny: int) x := nx; y := ny; end constructor; "
        "end Pt; "
        "class Util "
        "  public static method Make<T>(proto: T): T where T: new() "
        "    return new T(); end Make; "             // mixed-domain fresh-value body
        "end Util; "
        "class Prog "
        "  public static method Main(): int "
        "    var p := Util.Make(new Pt(3, 4)); "      // T = Pt (value): body is a copy
        "    return 7 + p.x - 3; "                    // = 7 + 0 - 3 = 4 (new Pt() = 0,0)
        "  end Main; "
        "end Prog;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 4);   // deterministic exit code (new Pt() = default 0,0)
}
