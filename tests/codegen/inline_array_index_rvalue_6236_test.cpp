// Codegen — indexing an RVALUE inline array (§15.1, v0.338, #6236).
//
// THE DEFECT. The inline-array index read in `lowerExpr`
// (LlvmBackend_Exprs.cpp) resolved its base with `lvalueAddr`, so it required
// the base to be a PLACE. An inline array that is an rvalue — a method-call
// result (`Mk()[1]`), a `DictionaryEntry`'s `Key` (`e.Key[i]`) — has no place,
// so `lvalueAddr` returned nullptr and the whole read returned nullptr.
//
// `lowerExpr`'s caller then reported #3079 ("the argument of this
// `Console.WriteLine` failed to lower to a value, so the whole statement emits
// nothing — not even the line terminator") for a statement-position read, so the
// LINE DISAPPEARED; or the #1386 placeholder (`lowerExpr returned null for
// interpolated-string expression part`) for the same expression inside an
// interpolated string, rendering `0` — a silently WRONG value at exit 0, the
// §21.9/§2.1 class of defect the project treats as the worst shape a codegen
// bug can take.
//
// §15.1 (v0.338) makes an inline array a VALUE type and `a[i]` its element
// read; nothing restricts the indexed operand to a named place. An rvalue base
// needs only the value: lower it, spill it, GEP into the spill.
//
// MEASURED on this fix's parent (2210462e), the issue's own repro printing
// `3` then `10` where the spec requires `8`, `3`, `19` — the `8` line missing
// entirely, and `Mk()[2]` rendering as `0` beside a correct `1`.
//
// SCOPE, deliberately narrow, and pinned by a row below: the fix is in the READ
// path only. It is NOT in `lvalueAddr`, which serves assignment targets as well,
// so `Mk()[1] := 9` stays a compile error (§28.3 — a call result is not a
// binding; sema rejects it at grammar A.6's lvalue rule).

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

std::string runStdout(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}

}  // namespace

// §15.1: `Mk()` returns `[7,8,9]`; `Mk()[1]` reads slot 1 → 8. Pre-fix the
// statement emitted NOTHING, so the whole line was missing from the output.
BEZEL_TEST(codegen_inline_array_index_rvalue_6236,
           call_result_index_statement_position_spec_15_1) {
    BEZEL_EXPECT_EQ(runStdout(
        "module M; class C "
        "public static method Mk(): [3]int "
        "  var a: [3]int := [7, 8, 9]; return a; end Mk; "
        "public static method Main(): int "
        "  Console.WriteLine(Mk()[1]); "
        "  return 0; "
        "end Main; end C;"), "8\n");
}

// §15.1: every slot of an rvalue inline array reads, not just the second.
BEZEL_TEST(codegen_inline_array_index_rvalue_6236,
           call_result_every_slot_spec_15_1) {
    BEZEL_EXPECT_EQ(runStdout(
        "module M; class C "
        "public static method Mk(): [3]int "
        "  var a: [3]int := [7, 8, 9]; return a; end Mk; "
        "public static method Main(): int "
        "  Console.WriteLine($\"{Mk()[0]} {Mk()[1]} {Mk()[2]}\"); "
        "  return 0; "
        "end Main; end C;"), "7 8 9\n");
}

// §21.9 / §2.1: the #1386 placeholder shape. Pre-fix `Mk()[2]` inside an
// interpolated string rendered `0` and the program exited 0 — a silently wrong
// VALUE rather than a dropped statement, so this row is the sharper oracle of
// the two. It also proves the fix does not merely restore the statement drop.
BEZEL_TEST(codegen_inline_array_index_rvalue_6236,
           call_result_index_interpolated_no_placeholder_spec_21_9) {
    BEZEL_EXPECT_EQ(runStdout(
        "module M; class C "
        "public static method Mk(): [3]int "
        "  var a: [3]int := [7, 8, 9]; return a; end Mk; "
        "public static method Main(): int "
        "  Console.WriteLine($\"{Mk()[0]}{Mk()[2]}\"); "
        "  return 0; "
        "end Main; end C;"), "79\n");
}

// §15.1: the issue's own field-path contrast — a FIELD path (`r.A[2]`) already
// worked, because a field is a place. Pinned so the fix is not mistaken for the
// thing that made field paths work.
BEZEL_TEST(codegen_inline_array_index_rvalue_6236,
           field_path_index_unchanged_spec_15_1) {
    BEZEL_EXPECT_EQ(runStdout(
        "module M; record R(A: [3]int, N: int); class C "
        "public static method Main(): int "
        "  var a: [3]int := [1, 2, 3]; "
        "  var r := new R(a, 5); "
        "  Console.WriteLine(r.A[2]); "
        "  return 0; "
        "end Main; end C;"), "3\n");
}

// §15.1 / §22.10: a `DictionaryEntry`'s `Key` is an rvalue inline array — the
// second shape the issue names, and the one that becomes reachable once the
// dictionary key itself is a value. Keys are bound to locals first, because a
// collection literal written directly as an inline-array dictionary key is a
// SEPARATE known limitation (#6237), not this fix's subject.
//
// The row emits the entry's own Value beside the three key slots, so the
// expectation is self-identifying: whichever order the dictionary enumerates in,
// the line pairing "x" with 7,8,9 and "y" with 1,2,3 is what §15.1.2's
// element-wise read requires. (Both orders were observed in practice; asserting
// a fixed line order would pin an implementation detail rather than the rule.)
BEZEL_TEST(codegen_inline_array_index_rvalue_6236,
           dictionary_entry_key_index_spec_15_1) {
    BEZEL_EXPECT_EQ(runStdout(
        "module M; class C "
        "public static method Main(): int "
        "  var k1: [3]int := [7, 8, 9]; "
        "  var k2: [3]int := [1, 2, 3]; "
        "  var d: Dictionary<[3]int, string> := new Dictionary<[3]int, string>(); "
        "  d.Add(k1, \"x\"); d.Add(k2, \"y\"); "
        "  for var e in d do "
        "    Console.WriteLine($\"{e.Value} {e.Key[0]},{e.Key[1]},{e.Key[2]}\"); "
        "  end for; "
        "  return 0; "
        "end Main; end C;"), "x 7,8,9\ny 1,2,3\n");
}

// §28.3 / grammar A.6: the fix is in the READ path only. `Mk()[1] := 99` must
// STAY a compile error — a call result is not a binding, so it is not an
// assignment target. A fix that put the rvalue arm into `lvalueAddr` would make
// this compile, so this row is the scope guard.
//
// `runAot` reports `ran == false` for a program the compiler REJECTED, so the
// oracle is `!ran` — an exit-code comparison would be vacuous here (runAot
// fills `diagnostics` only on a reject, and `exitCode` is not the verdict).
BEZEL_TEST(codegen_inline_array_index_rvalue_6236,
           rvalue_index_is_not_an_assignment_target_spec_28_3) {
    auto r = runAot(
        "module M; class C "
        "public static method Mk(): [3]int "
        "  var a: [3]int := [7, 8, 9]; return a; end Mk; "
        "public static method Main(): int "
        "  Mk()[1] := 99; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(!r.ran);
}

// §15.1 / §31.2: the bounds check still raises on an rvalue base. Pre-fix the
// check was unreachable on this shape (the read dropped before reaching it),
// so an out-of-range rvalue index silently produced a wrong answer; now it is
// IndexOutOfRangeException as for a place base.
BEZEL_TEST(codegen_inline_array_index_rvalue_6236,
           rvalue_index_out_of_range_throws_spec_31_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "public static method Mk(): [3]int "
        "  var a: [3]int := [7, 8, 9]; return a; end Mk; "
        "public static method Main(): int "
        "  try "
        "    Console.WriteLine(Mk()[9]); "
        "    return 1; "
        "  catch "
        "    var e: IndexOutOfRangeException => return 42; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 42);
}

// §15.1: a rvalue base whose index is itself an expression — the GEP index is
// the lowered index value, not the constant, so a computed index reads the
// right slot.
BEZEL_TEST(codegen_inline_array_index_rvalue_6236,
           rvalue_index_computed_index_spec_15_1) {
    BEZEL_EXPECT_EQ(runStdout(
        "module M; class C "
        "public static method Mk(): [3]int "
        "  var a: [3]int := [7, 8, 9]; return a; end Mk; "
        "public static method Main(): int "
        "  var i: int := 1; "
        "  var j: int := 2; "
        "  Console.WriteLine($\"{Mk()[i]} {Mk()[j]}\"); "
        "  return 0; "
        "end Main; end C;"), "8 9\n");
}

// §15.1 / §27.5: an rvalue base inside a lambda body — the spill is an ENTRY
// alloca, so it dominates every use in the lambda's own frame. A per-block
// alloca would not have dominated the bounds-check throw block.
BEZEL_TEST(codegen_inline_array_index_rvalue_6236,
           rvalue_index_inside_lambda_spec_27_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "public static method Mk(): [3]int "
        "  var a: [3]int := [7, 8, 9]; return a; end Mk; "
        "public static method Main(): int "
        "  var f: (int) -> int := lambda(i) => Mk()[i]; "
        "  return f(2); "
        "end Main; end C;"), 9);
}

// NOT a row, recorded so a reader does not go looking for it: §15.1 (v0.338)
// forbids an inline array as the element of another (`[N][M]T` is "deferred to
// a future revision"), so there is no nested-rvalue-index shape to pin. The
// compiler rejects `[N][M]T` at both the type and the literal, which is
// §15.1-conformant and is a pre-existing behavior this fix does not touch.
