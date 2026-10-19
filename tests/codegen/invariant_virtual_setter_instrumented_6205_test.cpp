// Spec-derived runtime tests for §22.16's SETTER row on the virtual /
// interface-dispatch store arm (v0.338, issue #6205).
//
// §22.16 "The instrumentation set" (v0.338):
//
//   "- **Entry and normal exit** of every `public` **setter** — block-form,
//      auto-property, indexer, `set(owned)` alike."
//
// THE DEFECT. §22.16's setter row was emitted at exactly ONE store path —
// `lowerPropertySetValue`, which INLINES the accessor's body — and a
// `virtual`/interface property does not take that path: the assignment lowers
// through `lowerVirtualPropertySet`, which DISPATCHES to the accessor thunk
// (`ensurePropertyAccessorFn`) instead. That thunk emits the accessor's body
// and nothing else; no instrumentation point exists on the dispatching path at
// all. So the same declaration was instrumented or not purely as a function of
// the `virtual` keyword.
//
// MEASURED, `bezelc --checked-contracts`, on the pre-fix compiler
// (r30-c1 @ f468f8a9): a `class B` carrying `invariant(_x >= 0);` and a
// `public virtual property Q` whose block-form setter assigns `value`, with
// `b.Q := 0 - 5` inside a `try`/`catch ContractViolationException`, returned
// **42** where §22.16 requires **99**. Delete the word `virtual` from that one
// declaration — nothing else changes — and the same program raises (99), which
// is pinned by `tests/codegen/invariant_public_setter_instrumented_4614_test.cpp`.
//
// ORACLE DISCIPLINE. Every violating row has a SATISFIED twin built from the
// same declarations with a different argument, so a row cannot pass by raising
// unconditionally. The two boundaries are pinned SEPARATELY (`entry_` and
// `exit_` below): a fix that emitted only one of them leaves the other red.
// `runChecked` collapses "did not terminate normally" to -999, so no row passes
// by crashing, and every row runs on `runAotCheckedContracts`, whose
// `detectLeaks=true` (aot_support.hpp) arms LSan — a construction/store edge
// that leaked rather than disposed would turn a row red.
//
// CONTROLS. The over-check direction is the live risk for this fix, because
// §22.16 grants an exemption the exemption must survive: "A call whose receiver
// is `this` — explicit or implicit — from inside any instance member of the
// class or a subclass enters the member's unchecked face", and "a public method
// may call `this.Other()` while its own state is legitimately mid-update,
// where an algorithm may pass through broken intermediate states". The
// `this_receiver_*`, `base_receiver_*` and `release_mode_*` rows below are that
// direction, and each was 42 on the pre-fix compiler as well — a control that
// moved would be a defect in the fix, not a strengthening.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

namespace {

// §22.16 checked-contracts MODE — exactly `bezelc --checked-contracts`, and
// the only oracle that also arms LSan.
int runChecked(const std::string& program) {
    auto r = bezel::test::runAotCheckedContracts(program);
    return r.ran ? r.exitCode : -999;
}

// §22.16's unchecked (release) mode — the production default (#4386).
int runRelease(const std::string& program) {
    auto r = bezel::test::runAot(program, bezel::codegen::CodegenOptions{});
    return r.ran ? r.exitCode : -999;
}

// The issue's class: a `virtual` block-form property over a field the class
// invariant constrains. `clause` is placed on `B`; `_x`'s default is 0.
std::string virtualSetter(const std::string& clause, const std::string& store) {
    return "module M; "
           "class B "
           "  public _x: int := 0; " +
           clause +
           "  public virtual property Q: int "
           "    get => _x; "
           "    set => do _x := value; end; "
           "  end Q; "
           "end B; "
           "class Prog "
           "  public static method Main(): int "
           "    var r: int := 0; "
           "    using var b := new B(); "
           "    try "
           "      " + store + " "
           "      r := 42; "
           "    catch ContractViolationException => r := 99; "
           "    end; "
           "    return r; "
           "  end Main; "
           "end Prog;";
}

const char* kBad = "b.Q := 0 - 5;";  // drives `_x` below zero
const char* kOk = "b.Q := 5;";       // keeps the clause true

// `decls` + a `Prog` that stores through a `B`-typed reference `b` and
// returns 42 (the store completed) or 99 (ContractViolationException).
std::string storeProg(const std::string& decls, const std::string& store) {
    return decls +
           "class Prog "
           "  public static method Main(): int "
           "    var r: int := 0; "
           "    using var d := new D(); "
           "    var b: B := d; "
           "    try "
           "      " + store + " "
           "      r := 42; "
           "    catch ContractViolationException => r := 99; "
           "    end; "
           "    return r; "
           "  end Main; "
           "end Prog;";
}


}  // namespace

// ── ★ The issue's shape: the same store, `virtual` present ─────────────────

BEZEL_TEST(invariant_virtual_setter_instrumented_6205,
           virtual_block_setter_store_checks_clause_spec_22_16) {
    BEZEL_EXPECT_EQ(99, runChecked(virtualSetter("invariant(_x >= 0);", kBad)));
    BEZEL_EXPECT_EQ(42, runChecked(virtualSetter("invariant(_x >= 0);", kOk)));
}

// The entry and the exit are pinned SEPARATELY. ENTRY: the instance is broken
// through a path that is NOT an instrumentation point (an ordinary private
// field write, which §22.16 never guards), and the store leaves it TRUE again
// — only an entry check can see the violation. EXIT: the store is the only
// thing that breaks it.
BEZEL_TEST(invariant_virtual_setter_instrumented_6205,
           virtual_block_setter_entry_boundary_checked_spec_22_16) {
    BEZEL_EXPECT_EQ(
        99, runChecked("module M; "
                       "class B "
                       "  public _x: int := 0; "
                       "  invariant(_x >= 0); "
                       "  public virtual property Q: int "
                       "    get => _x; "
                       "    set => do _x := value; end; "
                       "  end Q; "
                       "  internal method BreakIt() _x := 0 - 9; end BreakIt; "
                       "end B; "
                       "class Prog "
                       "  public static method Main(): int "
                       "    var r: int := 0; "
                       "    using var b := new B(); "
                       "    try "
                       "      b.BreakIt(); b.Q := 1; "
                       "      r := 42; "
                       "    catch ContractViolationException => r := 99; "
                       "    end; "
                       "    return r; "
                       "  end Main; "
                       "end Prog;"));
    // …and the entry-only control: with the instance already valid, the same
    // store completes.
    BEZEL_EXPECT_EQ(
        42, runChecked("module M; "
                       "class B "
                       "  public _x: int := 0; "
                       "  invariant(_x >= 0); "
                       "  public virtual property Q: int "
                       "    get => _x; "
                       "    set => do _x := value; end; "
                       "  end Q; "
                       "  internal method BreakIt() _x := 0 - 9; end BreakIt; "
                       "end B; "
                       "class Prog "
                       "  public static method Main(): int "
                       "    var r: int := 0; "
                       "    using var b := new B(); "
                       "    try "
                       "      b.Q := 1; "
                       "      r := 42; "
                       "    catch ContractViolationException => r := 99; "
                       "    end; "
                       "    return r; "
                       "  end Main; "
                       "end Prog;"));
}

// ── ★ Dynamic dispatch ─────────────────────────────────────────────────────
// The store is written through a BASE-typed reference, so the accessor that
// runs is chosen by the vtable. §22.16 "Dispatch checks the dynamic class's
// invariant" (#4645) requires the DYNAMIC class's conjunction; the base's own
// clause is the base half of it.
BEZEL_TEST(invariant_virtual_setter_instrumented_6205,
           virtual_block_setter_through_base_ref_checks_base_clause_spec_22_16) {
    BEZEL_EXPECT_EQ(
        99, runChecked("module M; "
                       "class B "
                       "  public _x: int := 0; "
                       "  invariant(_x >= 0); "
                       "  public virtual property Q: int "
                       "    get => _x; "
                       "    set => do _x := value; end; "
                       "  end Q; "
                       "end B; "
                       "class D: B end D; "
                       "class Prog "
                       "  public static method Main(): int "
                       "    var r: int := 0; "
                       "    using var d := new D(); "
                       "    var b: B := d; "
                       "    try "
                       "      b.Q := 0 - 5; "
                       "      r := 42; "
                       "    catch ContractViolationException => r := 99; "
                       "    end; "
                       "    return r; "
                       "  end Main; "
                       "end Prog;"));
    BEZEL_EXPECT_EQ(
        42, runChecked("module M; "
                       "class B "
                       "  public _x: int := 0; "
                       "  invariant(_x >= 0); "
                       "  public virtual property Q: int "
                       "    get => _x; "
                       "    set => do _x := value; end; "
                       "  end Q; "
                       "end B; "
                       "class D: B end D; "
                       "class Prog "
                       "  public static method Main(): int "
                       "    var r: int := 0; "
                       "    using var d := new D(); "
                       "    var b: B := d; "
                       "    try "
                       "      b.Q := 5; "
                       "      r := 42; "
                       "    catch ContractViolationException => r := 99; "
                       "    end; "
                       "    return r; "
                       "  end Main; "
                       "end Prog;"));
}

// The DYNAMIC target is an OVERRIDE whose body differs from the base's, and
// the base's clause is still what the store is measured against. Without a
// clause anywhere in the chain nothing is evaluated (the class never sits on
// an instrumented boundary) — that is the second half of this row.
BEZEL_TEST(invariant_virtual_setter_instrumented_6205,
           virtual_block_setter_dispatch_reaches_override_spec_22_16) {
    const std::string overridden =
        "module M; "
        "class B "
        "  public _x: int := 0; "
        "  invariant(_x >= 0); "
        "  public virtual property Q: int "
        "    get => _x; "
        "    set => do _x := value; end; "
        "  end Q; "
        "end B; "
        "class D: B "
        "  public override property Q: int "
        "    get => _x; "
        "    set => do _x := value + 0; end; "
        "  end Q; "
        "end D; ";
    const std::string tail =
        "class Prog "
        "  public static method Main(): int "
        "    var r: int := 0; "
        "    using var d := new D(); "
        "    var b: B := d; "
        "    try "
        "      b.Q := 0 - 5; "
        "      r := 42; "
        "    catch ContractViolationException => r := 99; "
        "    end; "
        "    return r; "
        "  end Main; "
        "end Prog;";
    BEZEL_EXPECT_EQ(99, runChecked(overridden + tail));
    // The same dynamic dispatch with a satisfying value completes.
    BEZEL_EXPECT_EQ(42, runChecked(overridden +
                                   "class Prog "
                                   "  public static method Main(): int "
                                   "    var r: int := 0; "
                                   "    using var d := new D(); "
                                   "    var b: B := d; "
                                   "    try "
                                   "      b.Q := 5; "
                                   "      r := 42; "
                                   "    catch ContractViolationException "
                                   "      => r := 99; "
                                   "    end; "
                                   "    return r; "
                                   "  end Main; "
                                   "end Prog;"));
}

// The SUBCLASS's own clause, evaluated at a store whose accessors the base
// declares. This is the `virtual property` cell of the #4645 matrix, which
// that fix held out precisely because of #6205.
BEZEL_TEST(invariant_virtual_setter_instrumented_6205,
           virtual_block_setter_checks_derived_clause_spec_22_16) {
    const std::string decls =
        "module M; "
        "class B "
        "  public _x: int := 0; "
        "  public virtual property Q: int "
        "    get => _x; "
        "    set => do _x := value; end; "
        "  end Q; "
        "end B; "
        "class D: B "
        "  invariant(_x >= 0); "
        "end D; ";
    const std::string bad = storeProg(decls, "b.Q := 0 - 5;");
    const std::string good = storeProg(decls, "b.Q := 5;");
    BEZEL_EXPECT_EQ(99, runChecked(bad));
    BEZEL_EXPECT_EQ(42, runChecked(good));
}

// ── Controls: what the fix must NOT change ─────────────────────────────────
// §22.16 "The visibility boundary and the `this`-reentrancy exemption": a
// store whose receiver is `this`, written from inside an instance member of
// the class, is NOT an instrumented boundary — the member is the
// implementation's interior, "where an algorithm may pass through broken
// intermediate states". Each program below drives its own instance negative
// and restores it, and was 42 on the pre-fix compiler too.

BEZEL_TEST(invariant_virtual_setter_instrumented_6205,
           this_receiver_store_stays_unchecked_spec_22_16) {
    for (const char* store : {"this.Q := 0 - 5;", "Q := 0 - 5;"}) {
        BEZEL_EXPECT_EQ(
            42, runChecked("module M; "
                           "class B "
                           "  public _x: int := 0; "
                           "  invariant(_x >= 0); "
                           "  public virtual property Q: int "
                           "    get => _x; "
                           "    set => do _x := value; end; "
                           "  end Q; "
                           "  public method Work(): int "
                           "    " + std::string(store) +
                           " "
                           "    this.Q := 7; "
                           "    return _x; "
                           "  end Work; "
                           "end B; "
                           "class Prog "
                           "  public static method Main(): int "
                           "    var r: int := 0; "
                           "    using var b := new B(); "
                           "    try "
                           "      b.Work(); "
                           "      r := 42; "
                           "    catch ContractViolationException "
                           "      => r := 99; "
                           "    end; "
                           "    return r; "
                           "  end Main; "
                           "end Prog;"));
    }
}

// §12.3: `base.P := v` bypasses virtual dispatch, and the SAME exemption
// names it. Checked from a DERIVED member, mid-update.
BEZEL_TEST(invariant_virtual_setter_instrumented_6205,
           base_receiver_store_stays_unchecked_spec_22_16) {
    BEZEL_EXPECT_EQ(
        42, runChecked("module M; "
                       "class B "
                       "  public _x: int := 0; "
                       "  invariant(_x >= 0); "
                       "  public virtual property Q: int "
                       "    get => _x; "
                       "    set => do _x := value; end; "
                       "  end Q; "
                       "end B; "
                       "class D: B "
                       "  public override property Q: int "
                       "    get => _x; "
                       "    set => do base.Q := value; end; "
                       "  end Q; "
                       "  public method Work(): int "
                       "    base.Q := 0 - 5; "
                       "    base.Q := 7; "
                       "    return _x; "
                       "  end Work; "
                       "end D; "
                       "class Prog "
                       "  public static method Main(): int "
                       "    var r: int := 0; "
                       "    using var d := new D(); "
                       "    try "
                       "      d.Work(); "
                       "      r := 42; "
                       "    catch ContractViolationException => r := 99; "
                       "    end; "
                       "    return r; "
                       "  end Main; "
                       "end Prog;"));
}

// §22.16 "in unchecked (release) mode — the default — unproven `ensures` and
// `invariant` clauses are not evaluated". 45 = `_x` (-5) + 50, so the store
// really happened and nothing raised.
BEZEL_TEST(invariant_virtual_setter_instrumented_6205,
           release_mode_evaluates_no_clause_spec_22_16) {
    BEZEL_EXPECT_EQ(
        45, runRelease("module M; "
                       "class B "
                       "  public _x: int := 0; "
                       "  invariant(_x >= 0); "
                       "  public virtual property Q: int "
                       "    get => _x; "
                       "    set => do _x := value; end; "
                       "  end Q; "
                       "end B; "
                       "class Prog "
                       "  public static method Main(): int "
                       "    using var b := new B(); "
                       "    b.Q := 0 - 5; "
                       "    return b.Q + 50; "
                       "  end Main; "
                       "end Prog;"));
}

// A class whose chain declares no clause anywhere never sits on an
// instrumented boundary, so the same store is inert. 45 = `_x` (-5) + 50, so
// the store really happened and nothing raised.
BEZEL_TEST(invariant_virtual_setter_instrumented_6205,
           virtual_setter_on_clause_less_class_is_not_checked_spec_22_16) {
    BEZEL_EXPECT_EQ(
        45, runChecked("module M; "
                       "class B "
                       "  public _x: int := 0; "
                       "  public virtual property Q: int "
                       "    get => _x; "
                       "    set => do _x := value; end; "
                       "  end Q; "
                       "end B; "
                       "class Prog "
                       "  public static method Main(): int "
                       "    using var b := new B(); "
                       "    b.Q := 0 - 5; "
                       "    return b.Q + 50; "
                       "  end Main; "
                       "end Prog;"));
}

// §13.6 accessor visibility: it is the SETTER's visibility that decides the
// row, not the property's. `public property P: int private set;` is a public
// property with a NON-public setter, which §22.16's never-instrumented list
// excludes. `Poke` is `internal`, so its own `this.P := v` is exempt twice
// over (non-public member AND a `this` receiver) — 42 on both sides of the fix.
BEZEL_TEST(invariant_virtual_setter_instrumented_6205,
           private_set_accessor_is_not_a_public_setter_spec_22_16) {
    BEZEL_EXPECT_EQ(
        42, runChecked("module M; "
                       "class B "
                       "  public _x: int := 0; "
                       "  invariant(_x >= 0); "
                       "  public property P: int private set; "
                       "  internal method Poke(v: int): int "
                       "    this.P := v; "
                       "    return _x; "
                       "  end Poke; "
                       "end B; "
                       "class Prog "
                       "  public static method Main(): int "
                       "    var r: int := 0; "
                       "    using var b := new B(); "
                       "    try "
                       "      b.Poke(0 - 5); "
                       "      r := 42; "
                       "    catch ContractViolationException => r := 99; "
                       "    end; "
                       "    return r; "
                       "  end Main; "
                       "end Prog;"));
}