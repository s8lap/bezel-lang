// Spec-derived regression test for issue #3701 — §9.7 "Records and disposal"
// (v0.287) "in an instance field of a class" leg, the LAMBDA-BODY axis.
//
// §9.7's instance-field storage bullet (verbatim, v0.287):
//
//   "A borrow-bearing record value may not be stored in an **instance field of
//    a class** … A class instance's lifetime is not bounded by any borrow
//    source's scope, and a record-carried borrow — unlike a **direct**
//    class-typed borrow field, whose every use is a member access through the
//    reference and therefore hits the disposed-flag check — may be consumed
//    through access paths with **no per-access disposed-flag check** (a span's
//    buffer window is the canonical case). The checked-mode trust regime is
//    therefore unavailable and the store is **rejected statically**."
//
// The rule is a TYPE rule ("a borrow-bearing record may not be stored in an
// instance field of a class, unconditionally"): it consults no borrow SOURCE,
// only the RHS's carrier kind and the target's declaring class. fix #3565
// (v0.286) widened `checkBorrowStoreEscape`'s lambda-body channel for §31.1
// and §28.9 and deliberately GATED OUT this leg — "the ONE leg that consults
// no borrow SOURCE" — because widening it from the dataflow's lambda-body
// driver would have been a DIFFERENT rule newly reaching a position it had
// never reached before, on that fix's coat-tails. #3701 is the widen.
//
// Measured pre-fix (r12-cycle-1 = 48fe111):
//   * method-level twin (control row 2): rejected, §9.7 diagnostic, exit 1
//   * lambda-body twin (issue repro, row 1): silent accept, exit 0
// Post-fix the §9.7 type rule fires in both positions and rejects both.
//
// // per §9.7 / §28.3, v0.287 (issue #3701)

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;

namespace {

// §9.7 instance-field rejection diagnostic text — the assignment-site
// wording (Dest::Field) rather than the field-initializer wording. The two
// are intentionally distinct in the source so a row that pins the
// assignment-site rule does not match an unrelated initializer rejection.
constexpr const char* kRecordAssign =
    "a borrow-bearing record may not be stored in an instance field of a class";
constexpr const char* kSpanAssign =
    "a span may not be stored in an instance field of a class";

// Shared class prelude. `Holder` is a borrow-bearing record over an
// ArraySpan<int> backing store (§9.7 transitivity — stdlib §4.15). `Box`
// holds an instance field of that type — the §9.7 rule is scoped to an
// INSTANCE FIELD of a CLASS, exactly what `Holder` in `Box.r` is. `Box`
// declares a constructor to admit the non-`owned` field per §22.13.
constexpr const char* kPrelude =
    "module M; "
    "record Holder(S: ArraySpan<int>); "
    "class Box "
    "  public r: Holder; "
    "end Box; "
    "class C public static method Main(): int return 0; end Main; end C; ";

}  // namespace

// =============================================================================
// ROW 1 — REJECT. The issue's repro verbatim: a borrow-bearing record stored
// in an instance field of a class, positioned inside a LAMBDA BODY. Pre-fix:
// silent accept (rc=0, no §9.7 diagnostic). Post-fix: §9.7 rejection.
// =============================================================================
BEZEL_TEST(instance_field_borrow_lambda_3701,
           lambda_body_instance_field_record_rejected_9_7) {
    auto r = parseAndCheckOwnership(
        std::string(kPrelude) +
        "class D "
        "  public method Store(buf: Array<int>): int "
        "    var span := buf.AsSpan(); "
        "    var h := new Holder(span); "
        "    var mk := lambda(): int => do "
        "      this.r := h; "
        "      return 0; "
        "    end; "
        "    return mk(); "
        "  end Store; "
        "end D;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kRecordAssign));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "(§9.7"));
}

// The SPAN face of the same rule inside a lambda body. §9.7 names a span's
// buffer window as the CANONICAL case of this bullet; `isBorrowBearingRecordOrSpan`
// is the predicate the assignment site uses, and widening it to LambdaBody
// scope reaches the span shape too. The diagnostic carries the span-specific
// wording. Pre-fix: silent accept. Post-fix: §9.7 span rejection.
BEZEL_TEST(instance_field_borrow_lambda_3701,
           lambda_body_instance_field_span_rejected_9_7) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Box "
        "  public s: ArraySpan<int>; "
        "end Box; "
        "class C "
        "  public static method Store(buf: Array<int>): int "
        "    var span := buf.AsSpan(); "
        "    var mk := lambda(): int => do "
        "      var b := new Box(); "
        "      b.s := span; "
        "      return 0; "
        "    end; "
        "    return mk(); "
        "  end Store; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kSpanAssign));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "(§9.7"));
}

// =============================================================================
// ROW 2 — REJECT (CONTROL). The METHOD-level twin of row 1 — byte-identical
// RHS, byte-identical target, only the lambda boundary removed. This is the
// "still rejected" row: the pre-existing method-scope rejection must continue
// to fire after the lambda-body widening. The diagnostic text is the SAME
// §9.7 wording the assignment site already emits — there is one rule, two
// positions.
// =============================================================================
BEZEL_TEST(instance_field_borrow_lambda_3701,
           method_body_instance_field_record_rejected_9_7) {
    auto r = parseAndCheckOwnership(
        std::string(kPrelude) +
        "class D "
        "  public method Store(buf: Array<int>): int "
        "    var span := buf.AsSpan(); "
        "    var h := new Holder(span); "
        "    this.r := h; "
        "    return 0; "
        "  end Store; "
        "end D;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kRecordAssign));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "(§9.7"));
}

// =============================================================================
// ROW 3 — ACCEPT. The §9.7 rule is a TYPE rule, not a lifetime rule: it
// fires only when the RHS is a borrow-bearing record / span / choice. A plain
// value-typed assignment to an instance field is NOT a §9.7 violation —
// `new R()` with R a non-trivial-dispose user class goes through §28.5
// (owned-field path) or §28.6 (fresh-producer-into-non-owned-field path) but
// not §9.7. This row is the absence-of-borrow-bearing control: the RHS is a
// bare `int`, the target is a fresh instance field; the program must compile
// clean.
// =============================================================================
BEZEL_TEST(instance_field_borrow_lambda_3701,
           lambda_body_instance_field_int_accepted_9_7) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Box public n: int; end Box; "
        "class C "
        "  public static method Main(): int "
        "    var b := new Box(); "
        "    var mk := lambda(): int => do "
        "      b.n := 42; "
        "      return 0; "
        "    end; "
        "    var rc := mk(); "
        "    return b.n; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =============================================================================
// ROW 4 — ACCEPT. The §9.7 rule's destination bullet is "an instance field
// of a class". A store to a LOCAL is not §9.7's destination — it is §28.3's
// (or, for closure-capture shapes, §28.9's). This row stores a borrow-bearing
// record into a lambda-body LOCAL — which §9.7 does not forbid, and which
// the §28.3 lifetime rule accepts when the local is a same-scope capture.
// Pre-fix the lambda-body store was reachable through no path; post-fix the
// §9.7 widening consults `storeTargetIsClassField`, which returns false for
// the local target, so the rule declines and the program compiles clean.
// =============================================================================
BEZEL_TEST(instance_field_borrow_lambda_3701,
           lambda_body_borrow_to_local_accepted_9_7) {
    auto r = parseAndCheckOwnership(
        std::string(kPrelude) +
        "class D "
        "  public method Store(buf: Array<int>): int "
        "    var span := buf.AsSpan(); "
        "    var h := new Holder(span); "
        "    var mk := lambda(): int => do "
        "      var local := h; "
        "      return local.S.Length; "
        "    end; "
        "    return mk(); "
        "  end Store; "
        "end D;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =============================================================================
// ROW 5 — REJECT (NESTED). The §9.7 widening is reached on every assignment
// a lambda body walks to; the `forEachNestedStmt` walker the lambda-body
// driver uses (#3656's statement grammar) descends into the same body
// positions the method-level walker does. This row packs the §9.7-violating
// assignment inside a nested control-flow expression (`if true then … end`)
// to verify the walker reaches the assignment through the conditional arm.
// Pre-fix: silent accept. Post-fix: §9.7 rejection.
// =============================================================================
BEZEL_TEST(instance_field_borrow_lambda_3701,
           lambda_body_nested_if_instance_field_record_rejected_9_7) {
    auto r = parseAndCheckOwnership(
        std::string(kPrelude) +
        "class D "
        "  public method Store(buf: Array<int>): int "
        "    var span := buf.AsSpan(); "
        "    var h := new Holder(span); "
        "    var mk := lambda(): int => do "
        "      if true then "
        "        this.r := h; "
        "      end; "
        "      return 0; "
        "    end; "
        "    return mk(); "
        "  end Store; "
        "end D;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kRecordAssign));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "(§9.7"));
}
