// Spec-derived regression test for #4711 — rule-corpus `s12_7_0004`
// (spec §12.7 / §6.6, v0.307).
//
// THE GOVERNING RULE. Spec §12.7 "Type tests: `is`, `is not`, `as`" (v0.307),
// verbatim: "What is *not* rejected is the **bare-binding** compound and
// `++`/`--` form: `o += 1` and `o++` are equivalent to `o := o + 1` (§6.6,
// §17.6), and that store targets the *declared* `Object`/interface location —
// it reads through the narrowing, boxes the result afresh under §5.1, and
// invalidates the narrowing, exactly as the spelled-out `o := o + 1` does".
// §6.6: "`x op= e` is equivalent to `x := x op e` with the target evaluated
// once"; T (the left-hand side's type) is the declared `Object`, and every R
// implicitly converts to it (§5.1 boxing), so the result is stored — boxed —
// directly.
//
// THE DEFECT. Sema's AssignmentStmt arm erased the target's §12.7 narrowing
// BEFORE visiting the right-hand side, so (a) the spelled-out `o := o + 1`
// read `o` at its declared `Object` ("operator `+` is not defined for type
// `Object`") and (b) the compound `o += 1` typed its arithmetic against
// `Object` ("compound assignment `+=` has no valid result: `Object + int`
// computes `int` ..."). A previous sema-only admission of (b) crashed codegen
// (IR verifier: "Call parameter type does not match function signature!"),
// because codegen's compound arm ran the arithmetic on the `Object` slot.
//
// THE FIX. The narrowing now stays in force for the RHS read and is dropped
// after it (the target itself is still typed at its declared type, so the
// store lands in the declared slot), and a bare-binding compound on a
// narrowed `Object`/interface-declared binding is rewritten to its exact §6.6
// desugaring `o := o op e`, so it is typed AND lowered as the spelled-out
// form (unbox the narrowed read, compute, box the result afresh).
//
// Every row below RUNS the program and asserts the printed VALUE, so a
// sema-only accept that then crashes at codegen fails here.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

bool rejectsWith(const std::string& src, const std::string& needle) {
    auto r = runAot(src);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

// rule-corpus `s12_7_0004`, verbatim body, extended with an observation of
// the stored value: `o += 1` through an `is int` narrowing stores a fresh
// boxed `6` into the declared `Object` slot.
BEZEL_TEST(sema_narrowed_object_compound_assign_4711,
           s12_7_0004_int_compound_runs_and_stores_boxed_result) {
    auto r = runAot(
        "module Conf; "
        "public class Program "
        "  public static method Main() "
        "    var n: int := 5; "
        "    var o: Object := n; "
        "    if o is int then "
        "      o += 1; "
        "    end if; "
        "    if o is int then "
        "      var k: int := o; "
        "      Console.WriteLine(k); "
        "    end if; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("6\n"));
}

// The spelled-out twin §12.7 names: `o := o + 1` reads `o` THROUGH the
// narrowing (the invalidation applies after the statement's RHS is read).
BEZEL_TEST(sema_narrowed_object_compound_assign_4711,
           spelled_out_assignment_reads_through_narrowing) {
    auto r = runAot(
        "module Conf; "
        "public class Program "
        "  public static method Main() "
        "    var o: Object := 5; "
        "    if o is int then "
        "      o := o + 1; "
        "    end if; "
        "    if o is int then "
        "      var k: int := o; "
        "      Console.WriteLine(k); "
        "    end if; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("6\n"));
}

// A `string` narrowing (a reference-type narrowing at an `Object` location):
// `o += "cd"` is `o := o + "cd"` — §4.6 concatenation of the narrowed read,
// stored into the declared `Object` slot.
BEZEL_TEST(sema_narrowed_object_compound_assign_4711,
           string_compound_concatenates_through_narrowing) {
    auto r = runAot(
        "module Conf; "
        "public class Program "
        "  public static method Main() "
        "    var o: Object := \"ab\"; "
        "    if o is string then "
        "      o += \"cd\"; "
        "    end if; "
        "    if o is string then "
        "      var s: string := o; "
        "      Console.WriteLine(s); "
        "    end if; "
        "    var p: Object := \"x\"; "
        "    if p is string then "
        "      p := p + \"y\"; "
        "    end if; "
        "    Console.WriteLine(p.ToString()); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("abcd\nxy\n"));
}

// The result type is R of the desugared `o op e`, boxed afresh — NOT the
// narrowed type: §6.6's narrow-integer write-back applies only when T is an
// integer or `char`, and T here is the declared `Object`. So `o += 2.5` on an
// `int` narrowing boxes a `double` 7.5 (§6.2 promotion), and `q += 10` on a
// `byte` narrowing boxes the `int` 260 (§6.2: the addition runs in `int`) —
// exactly what `o := o + 2.5` / `q := q + 10` store.
BEZEL_TEST(sema_narrowed_object_compound_assign_4711,
           result_is_the_desugared_binary_type_boxed_afresh) {
    auto r = runAot(
        "module Conf; "
        "public class Program "
        "  public static method Main() "
        "    var o: Object := 5; "
        "    if o is int then "
        "      o += 2.5; "
        "    end if; "
        "    if o is double then "
        "      var d: double := o; "
        "      Console.WriteLine(d); "
        "    end if; "
        "    var b: byte := 250; "
        "    var q: Object := b; "
        "    if q is byte then "
        "      q += 10; "
        "    end if; "
        "    if q is byte then Console.WriteLine(\"byte\"); end if; "
        "    if q is int then "
        "      var k: int := q; "
        "      Console.WriteLine(k); "
        "    end if; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7.5\n260\n"));
}

// Each loop iteration re-narrows with a fresh `is` test, so every `*=` reads
// the CURRENT box: 7 → 14 → 28 → 56. Also covers a narrowed PARAMETER and an
// `Object?` declared carrier.
BEZEL_TEST(sema_narrowed_object_compound_assign_4711,
           loop_parameter_and_nullable_object_carriers) {
    auto r = runAot(
        "module Conf; "
        "public class Program "
        "  public static method F(o: Object): int "
        "    if o is int then "
        "      o += 10; "
        "    end if; "
        "    if o is int then "
        "      var k: int := o; "
        "      return k; "
        "    end if; "
        "    return -1; "
        "  end F; "
        "  public static method Main() "
        "    var t: Object := 7; "
        "    for var i in 0..<3 do "
        "      if t is int then "
        "        t *= 2; "
        "      end if; "
        "    end; "
        "    if t is int then "
        "      var k: int := t; "
        "      Console.WriteLine(k); "
        "    end if; "
        "    Console.WriteLine(F(3)); "
        "    var u: Object? := 5; "
        "    if u is int then "
        "      u -= 1; "
        "    end if; "
        "    if u is int then "
        "      var k2: int := u; "
        "      Console.WriteLine(k2); "
        "    end if; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("56\n13\n4\n"));
}

// Where NO narrowing is in force the compound form is still the §6.6 compile
// error: an un-narrowed `Object` target, and a target whose narrowing an
// earlier reassignment in the same block invalidated (§12.7: "Reassignment
// within the controlled block invalidates the narrowing" — `m <<= 2` is such
// a reassignment itself, and so is `m := 1`).
BEZEL_TEST(sema_narrowed_object_compound_assign_4711,
           compound_without_narrowing_still_rejected) {
    const std::string needle =
        "compound assignment `+=` has no valid result: `Object + int`";
    BEZEL_EXPECT_TRUE(rejectsWith(
        "module Conf; "
        "public class Program "
        "  public static method Main() "
        "    var o: Object := 5; "
        "    o += 1; "
        "  end Main; "
        "end Program;",
        needle));
    BEZEL_EXPECT_TRUE(rejectsWith(
        "module Conf; "
        "public class Program "
        "  public static method Main() "
        "    var m: Object := 6; "
        "    if m is int then "
        "      m := 1; "
        "      m += 1; "
        "    end if; "
        "  end Main; "
        "end Program;",
        needle));
    BEZEL_EXPECT_TRUE(rejectsWith(
        "module Conf; "
        "public class Program "
        "  public static method Main() "
        "    var m: Object := 6; "
        "    if m is int then "
        "      m <<= 2; "
        "      m += 1; "
        "    end if; "
        "  end Main; "
        "end Program;",
        needle));
}

// Box accounting: every `o += 1` boxes afresh and the displaced box is
// released (§8.6 / §28.9). Loop-amplified (300 iterations) under the ASan/
// LSan oracle so a per-iteration leak or a double release is caught.
BEZEL_TEST(sema_narrowed_object_compound_assign_4711,
           compound_reboxing_loop_is_asan_clean) {
    JitResult r = runAotChecked(
        "module Conf; "
        "public class Program "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var i in 0..<300 do "
        "      var o: Object := i; "
        "      if o is int then "
        "        o += 1; "
        "      end if; "
        "      if o is int then "
        "        var k: int := o; "
        "        acc := acc + k; "
        "      end if; "
        "    end; "
        "    if acc = 45150 then return 42; end if; "
        "    return 7; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}
