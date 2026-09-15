// Spec-derived accept-side control tests for issue #4206 — `ast::InvariantDecl`'s
// own doc comment (src/ast/Node.hpp) claimed a `static invariant` clause is
// "rejected at sema as out-of-fragment", but no sema check anywhere reads
// `InvariantDecl::isStatic` to reject it, and MEASURED on this tree a
// `static invariant(true);` inside an ORDINARY (non-static) class compiles at
// exit 0 with zero diagnostics — the doc comment's claim does not hold.
//
// §22.16 "Class invariants" (v0.298) "Out of scope in this revision" list
// states, verbatim: "`static invariant` parses from day one and is useful
// when the prover fragment lands." It does NOT say a `static` invariant is
// rejected meanwhile — the natural reading (and the one the codegen side
// already implements, `Emitter::emitInvariantCheck` in
// `src/codegen/LlvmBackend_Setup.cpp`: `if (inv->expr && !inv->isStatic)`,
// unconditionally excluding `isStatic` invariants from instrumentation) is
// INERT-until-the-prover-lands, not rejected. §22.16's separate "static-class
// and static-state invariants" bullet is about the ENCLOSING BODY KIND being
// a static class (already rejected — fix #4186,
// tests/sema/record_static_class_invariant_member_4186_test.cpp) — a
// different axis from the `"static"?` QUALIFIER on a clause inside an
// ORDINARY class, which is this file's subject.
//
// Per CLAUDE.md's "the spec is the source of truth" / "decline-with-reasons"
// discipline: the spec does not authorize rejecting this, so no rejection is
// implemented here. This file is the regression test pinning the CURRENT,
// spec-consistent behavior (accept + inert) against a future lane
// "fixing" this from the doc comment alone (as #4186's own commit almost
// did — it quoted this same doc comment as supporting authority, prompting
// review to check the claim, which is how #4206 was filed). See
// src/ast/Node.hpp's `InvariantDecl` comment (corrected by this same commit)
// for the full citation trail.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;

namespace {

int runFull(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// =====================================================================
// ACCEPT — a `static invariant` inside an ordinary class: the issue's own
// repro (adapted to prove genuine compile+link+run via a distinguishing
// return value, not just "exit 0" — LANE_RULES §3, "exit 0" alone is not
// an oracle).
// =====================================================================
BEZEL_TEST(static_invariant_ordinary_class_4206,
           static_invariant_true_ordinary_class_accepted_spec_22_16) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class Account4206 "
        "  private _balance: decimal := 0m; "
        "  static invariant(true); "
        "  public constructor() end constructor; "
        "end Account4206; "
        "class C4206 public static method Main(): int "
        "  var a := new Account4206(); "
        "  return 42; "
        "end Main; end C4206;"),
        42);
}

// ACCEPT — a `static invariant(false);` (an UNCONDITIONALLY-false predicate)
// alone in an ordinary class: if the doc comment's stale claim were real
// (rejected at sema), this would never reach codegen at all; if it were
// silently accepted but still INSTRUMENTED like a real invariant, this would
// raise ContractViolationException at construction exit and the catch arm
// would set the discriminator to 99. Neither happens — it is genuinely
// INERT, per §22.16's "useful when the prover fragment lands" framing and
// `emitInvariantCheck`'s `!inv->isStatic` exclusion.
BEZEL_TEST(static_invariant_ordinary_class_4206,
           static_invariant_false_is_inert_no_fault_spec_22_16) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class Account4206b "
        "  private _balance: decimal := 0m; "
        "  static invariant(false); "
        "  public constructor() end constructor; "
        "  public method Bump(): int "
        "    _balance := _balance + 1m; "
        "    return 7; "
        "  end Bump; "
        "end Account4206b; "
        "class C4206b public static method Main(): int "
        "  var r: int := 0; "
        "  try "
        "    var a := new Account4206b(); "
        "    r := a.Bump(); "
        "  catch ContractViolationException => r := 99; "
        "  end; "
        "  return r; "
        "end Main; end C4206b;"),
        7);
}

// ACCEPT (cross-check) — a `static invariant(false);` SIDE BY SIDE with a
// real (non-static) invariant that the constructor deliberately violates:
// the static sibling must not suppress instrumentation of the real one.
// `emitInvariantCheck` builds `chain` by filtering EACH member with
// `!inv->isStatic`, not by an all-or-nothing per-class gate, so the real
// invariant's own violation must still fault — proving the two are
// independent, not that "any static invariant present" turns off checking
// for the whole class.
BEZEL_TEST(static_invariant_ordinary_class_4206,
           static_invariant_sibling_real_invariant_still_enforced_spec_22_16) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class Account4206c "
        "  public x: int := 0; "
        "  invariant(x > 0); "
        "  static invariant(false); "
        "  public constructor(v: int) x := v; end constructor; "
        "  public static method Main(): int "
        "    var r: int := 0; "
        "    try "
        "      using var a := new Account4206c(-1); "
        "    catch ContractViolationException => r := 99; "
        "    end; "
        "    return r; "
        "  end Main; "
        "end Account4206c;"),
        99);
}
