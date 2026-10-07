// Record `.Equals(other)` must NOT dispose either operand's string fields
// (§8.4 / §9.1, v0.315).
//
// §8.4: "`a = b` invokes `a.Equals(b)`" — the method spelling and the operator
// spelling are ONE comparison and must agree on every observable, including
// what the operands are afterwards. §9.1 gives a record value semantics:
// "assignment copies the value". A copy of a record STRUCT copies its field
// POINTERS; it does not retain the string objects they address. The strings
// belong to whoever built the operands — never to the comparison.
//
// Fix #4832's release block discharged both operands' string fields at the
// `.Equals` call, on the premise that the callee's struct copies were
// "independent copies" owning their own strings. They do not: the copies alias
// the caller's strings, so the release freed heap objects the caller still
// owned, and the next statement read freed memory. Silent wrong code —
// `a.Equals(b)` returned True and only the following read was wrong.
//
// MEASURED (a24-wt-5272, r28-receq, bezelc --sanitize=address), pre-fix,
// `record Name(value: string)` with `a := new Name("hi")`,
// `b := new Name("h" + "i")` (the concatenation is what makes the operand a
// heap object rather than a global):
//   pa  hash a, hash b (no Equals)  -> 3856, 3856        clean
//   pb  a.Equals(b), hash a, hash b -> True, 3856, UAF   freed by the Equals
//   pe  a.Equals(b), print b.Text   -> True, SEGV @ 0x8  b's field zeroed
//   ph  a = b,       print b.value  -> True, "hi"         `=` was always clean
//   pi  record Num(n: int), .Equals -> True, 5            no heap payload
//   pk  var c := b;  print b.value  -> "hi", "hi", 3856   copy semantics (§9.1)
//
// TWO ORACLE FACTS this suite is built around, both measured here:
//
//  1. Under the PLAIN AOT oracle the operand-survival rows are VACUOUS. A
//     freed chunk usually still holds its old bytes, so a value assertion on
//     the post-call read still succeeds and the row passes on the pre-fix
//     tree. ASan is the only oracle that sees this defect.
//
//  2. The defect needs a HEAP operand. With a string LITERAL operand the
//     string is a global, #4832's release is inert, and the row passes on both
//     trees — measured: the literal-operand loop row below is green pre-fix
//     and post-fix, which is exactly why it is labelled a CONTROL and not
//     offered as a pin. Every pinning row therefore uses a concatenation.
//
// Row-by-row fail-without (pre-fix tree, `git checkout <base> -- <src file>`):
//   equals_leaves_argument_string_intact_spec_8_4   FAIL (UAF)
//   equals_leaves_receiver_string_intact_spec_8_4   FAIL (UAF)
//   hash_agrees_after_equals_call_spec_8_3          FAIL (0, expected 1)
//   equals_method_and_operator_agree_spec_8_4       FAIL
//   equals_loop_300_no_leak_literal_control_spec_28_2  PASS  <- control

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
int runChecked(const std::string& program) {
    auto r = runAotChecked(program);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §8.4 / §9.1: the ARGUMENT survives the comparison — its string field still
// holds the value it was constructed with. Pre-fix the operand's string was
// freed at the `.Equals` call, so this read touches freed memory (ASan:
// heap-use-after-free). The concatenation makes the operand a heap object;
// see oracle fact 2 above for why that is load-bearing here.
BEZEL_TEST(record_equals_disposes_operands_4832,
           equals_leaves_argument_string_intact_spec_8_4) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; record Name(value: string); "
        "class C public static method Main(): int "
        "  var a := new Name(\"hi\"); "
        "  var b := new Name(\"h\" + \"i\"); "
        "  var r := a.Equals(b); "
        "  if b.value = \"h\" + \"i\" then return 0; else return 1; end; "
        "end Main; end C;"), 0);
}

// §8.4 / §9.1: the RECEIVER survives too — the same retired block released it.
// A receiver that reads back intact is the row that pins the release of `recv`
// specifically; the argument row alone would still pass if only `other` were
// released. Heap receiver, literal argument: the mirror image of the row above.
BEZEL_TEST(record_equals_disposes_operands_4832,
           equals_leaves_receiver_string_intact_spec_8_4) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; record Name(value: string); "
        "class C public static method Main(): int "
        "  var a := new Name(\"x\" + \"y\"); "
        "  var b := new Name(\"bye\"); "
        "  var r := a.Equals(b); "
        "  if a.value = \"x\" + \"y\" then return 0; else return 1; end; "
        "end Main; end C;"), 0);
}

// §8.3 / §8.4: both operands still hash by content after the comparison, so the
// synthesized GetHashCode agrees with the synthesized Equals — the §8.3
// hash-equality invariant, which a freed field broke by hashing whatever the
// allocator left behind. Plain oracle on purpose: this is the shape the
// originally-red `codegen_record_str_eq.body_record_string_auto_property_spec_9_2`
// row exercises on a BODY record, restated on a positional one, and it is the
// row whose answer a reader can check without a sanitizer.
BEZEL_TEST(record_equals_disposes_operands_4832,
           hash_agrees_after_equals_call_spec_8_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Person(name: string, age: int); "
        "class C public static method Main(): int "
        "  var a := new Person(\"Ann\", 30); "
        "  var b := new Person(\"An\" + \"n\", 30); "
        "  var r := a.Equals(b); "
        "  return (if a.GetHashCode() = b.GetHashCode() then 1 else 0); "
        "end Main; end C;"), 1);
}

// §8.4: the two spellings agree — `a.Equals(b)` and `a = b` return the same
// answer for the same operands, true for equal records and false for unequal
// ones. Asserting the DISAGREEMENT cases too is deliberate: a comparison that
// answered `false` to everything would satisfy the equal-record rows and pass
// vacuously.
BEZEL_TEST(record_equals_disposes_operands_4832,
           equals_method_and_operator_agree_spec_8_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Name(value: string); "
        "class C public static method Main(): int "
        "  var a := new Name(\"hi\"); "
        "  var b := new Name(\"h\" + \"i\"); "
        "  var c := new Name(\"bye\"); "
        "  return (if a.Equals(b) and a = b "
        "             and (a.Equals(c) = false) and (a = c) = false then 1 "
        "           else 0); "
        "end Main; end C;"), 1);
}

// CONTROL — NOT a pin, and measured as such. This row passes on the pre-fix
// tree AND on the fixed tree (literal operands are globals; see oracle fact 2).
// It earns its place by guarding the OTHER direction: retiring #4832's release
// must not introduce a leak, and this is the loop-amplified leak shape that
// #4832's own test file uses (300×, per the project's one-shot-repro rule).
// It is labelled a control so nobody mistakes it for fail-without evidence.
BEZEL_TEST(record_equals_disposes_operands_4832,
           equals_loop_300_no_leak_literal_control_spec_28_2) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; record Name(value: string); "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 300 do "
        "    var a := new Name(\"hi\"); "
        "    var b := new Name(\"bye\"); "
        "    var r := a.Equals(b); "
        "    i := i + 1; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// ---------------------------------------------------------------------------
// RESIDUAL LEAK — a SEPARATE defect, deliberately NOT pinned here.
//
// With this fix in place, `.Equals` called in a LOOP on a record whose operand
// string is a CONCATENATION leaks that string once per iteration: 299
// allocations / 10465 bytes over 300 iterations (MEASURED, a24-wt-5272,
// bezelc --sanitize=address). It is NOT introduced by this fix and NOT fixed by
// it — pre-fix the same program died on the use-after-free before LSan could
// report, so the leak was masked by the worse defect.
//
// Measured discriminator (fixed tree):
//   single `.Equals`, heap ARGUMENT           -> clean
//   single `.Equals`, heap RECEIVER            -> clean
//   `.Equals` in a loop, LITERAL operands      -> clean
//   `.Equals` in a loop, one heap operand      -> leaks ~1 object/iteration
//   `.Equals` in a loop, both operands heap    -> leaks ~2 objects/iteration
// so it needs the loop AND a heap-allocated operand — which is why the pinning
// rows above are single-call and why no pinning row uses the loop.
//
// Tracked separately (see handback). It is a leak, not a wrong answer, and
// trading the use-after-free for it is #3480's own conservative direction —
// "a bounded leak is the conservative direction; a UAF is not".
// ---------------------------------------------------------------------------