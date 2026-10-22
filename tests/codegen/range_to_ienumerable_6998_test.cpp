// Spec-derived regression test for issue #6998.
//
// §16.3 (v0.340): "`Range<T>` is a compiler-intrinsic record that implements
// `IEnumerable<T>`". §5.1 (v0.340): a type implicitly converts "to an interface
// it implements".
//
// THE DEFECT. A `Range<int>` (literal `1..4` or a local) passed to an
// `IEnumerable<int>` parameter was rejected, "not implicitly convertible
// (§5.1)": the builtin Range symbol carries no base list, so the user-type
// conformance walk found no `IEnumerable`. Admitting the conversion in sema is
// not enough: the lowered range is a by-value `%bz.range` struct with no
// vtable, so codegen has to mint an `IEnumerable<T>` wrapper at the three
// coercion sites (argument, local-decl init, RETURN) or the IR is malformed.
//
// REVIEW FIX-FORWARD (§16.3, §5.1, v0.340). The first revision keyed the
// codegen arms on the SPELLING `Range` (a user `class Range: IEnumerable<int>`
// crashed the compiler) and admitted the conversion at every §5.1 position,
// while codegen wrapped only three. A lambda returning a range crashed (SEGV in
// the lambda body / IR verify failure); a plain assignment overflowed the
// stack; an `Array<IEnumerable<int>>.Add` / collection-literal element SEGV'd;
// a field leaked. Now a lambda's return is wrapped like a method's, and every
// position without a disposal edge for the owning enumerator frame (assignment,
// field, collection element, record field, choice payload, default value, `??`,
// closure-call argument, `async` return) rejects the conversion naming #7029.
//
// Each row prints exact stdout. The RUN rows use runAotChecked (leak + ASan
// oracle); the 100x loop row would leak the drained array per iteration if the
// wrapper's disposal were not registered.
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

const char* kHelpers =
    "public class H "
    "  public static method Sum(xs: IEnumerable<int>): int "
    "    var n := 0; for var x in xs do n += x; end; return n; "
    "  end Sum; "
    "  public static method SumL(xs: IEnumerable<long>): long "
    "    var n: long := 0; for var x in xs do n += x; end; return n; "
    "  end SumL; "
    "  public static method Chars(xs: IEnumerable<char>): string "
    "    var s := \"\"; for var c in xs do s := s + $\"{c}\"; end; return s; "
    "  end Chars; "
    "  public static method Seq(xs: IEnumerable<int>): string "
    "    var s := \"\"; for var x in xs do s := s + $\"{x},\"; end; return s; "
    "  end Seq; "
    "  public static method Make(): IEnumerable<int> return 1..3; end Make; "
    "  public static method MakeLocal(): IEnumerable<int> "
    "    var r: Range<int> := 10..12; return r; "
    "  end MakeLocal; "
    "end H; ";

JitResult run(const char* mod, const std::string& body) {
    return runAotChecked(std::string("module ") + mod + "; " + kHelpers +
                         "public class M public static method Main(): int " +
                         body + " return 0; end Main; end M;");
}

}  // namespace

#define ROW(NAME, MOD, BODY, EXPECT)                                          \
    BEZEL_TEST(codegen_range_to_ienumerable_6998, NAME) {                     \
        JitResult r = run(MOD, BODY);                                         \
        BEZEL_EXPECT_TRUE(r.ran);                                             \
        BEZEL_EXPECT_EQ(r.exitCode, 0);                                       \
        BEZEL_EXPECT_EQ(r.stdoutText, std::string(EXPECT));                   \
    }

ROW(literal_range_argument_spec_16_3, "M6998a",
    "Console.WriteLine($\"{H.Sum(1..4)}\");", "10\n")
ROW(local_range_argument_spec_16_3, "M6998b",
    "var r: Range<int> := 1..4; Console.WriteLine($\"{H.Sum(r)}\");", "10\n")
ROW(half_open_range_spec_16_3, "M6998c",
    "Console.WriteLine(H.Seq(1..<4));", "1,2,3,\n")
ROW(empty_range_spec_16_3, "M6998d",
    "Console.WriteLine($\"{H.Sum(5..<5)}\");", "0\n")
ROW(descending_range_spec_16_3, "M6998e",
    "Console.WriteLine(H.Seq(4..1 step -1));", "4,3,2,1,\n")
ROW(stepped_range_spec_16_3, "M6998f",
    "Console.WriteLine(H.Seq(0..10 step 5));", "0,5,10,\n")
ROW(long_range_spec_16_3, "M6998g",
    "var r: Range<long> := 1L..3L; Console.WriteLine($\"{H.SumL(r)}\");", "6\n")
ROW(char_range_spec_16_3, "M6998h",
    "Console.WriteLine(H.Chars('a'..'d'));", "abcd\n")
ROW(interface_local_spec_5_1, "M6998i",
    "var e: IEnumerable<int> := 5..7; var t := 0; "
    "for var x in e do t += x; end; Console.WriteLine($\"{t}\");", "18\n")
ROW(return_range_as_ienumerable_spec_5_1, "M6998j",
    "var t := 0; for var x in H.Make() do t += x; end; "
    "for var x in H.MakeLocal() do t += x; end; Console.WriteLine($\"{t}\");",
    "39\n")
ROW(linq_over_interface_value_spec_16_3, "M6998k",
    "var e: IEnumerable<int> := 1..10; "
    "Console.WriteLine($\"{e.Where(lambda(x) => x mod 2 = 0).Count()}\");",
    "5\n")
ROW(range_local_reused_across_calls_spec_5_1, "M6998l",
    "var r: Range<int> := 1..3; "
    "Console.WriteLine($\"{H.Sum(r)} {H.Sum(r)} {r.Start}\");", "6 6 1\n")
ROW(loop_100x_spec_16_3, "M6998n",
    "var t := 0; var i := 0; while i < 100 do t += H.Sum(1..3); i += 1; end; "
    "Console.WriteLine($\"{t}\");", "600\n")
ROW(interface_local_in_loop_spec_5_1, "M6998o",
    "var t := 0; for var i in 0..<100 do "
    "var e: IEnumerable<int> := 1..3; for var x in e do t += x; end; end; "
    "Console.WriteLine($\"{t}\");", "600\n")

BEZEL_TEST(codegen_range_to_ienumerable_6998,
           mismatched_element_type_still_rejected_spec_5_1) {
    JitResult r = runAot(
        std::string("module M6998p; ") + kHelpers +
        "public class M public static method Main(): int "
        "Console.WriteLine($\"{H.SumL(1..4)}\"); return 0; "
        "end Main; end M;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "not implicitly convertible"));
}

BEZEL_TEST(codegen_range_to_ienumerable_6998,
           range_int_to_ienumerable_string_rejected_spec_5_1) {
    JitResult r = runAot(
        "module M6998q; "
        "public class M public static method Main(): int "
        "var e: IEnumerable<string> := 1..4; return 0; "
        "end Main; end M;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "not implicitly convertible"));
}

// The control: an `Array<int>` argument to the same parameter shape. It passes
// on the pre-fix compiler too (it is the issue's `rng_arr` control), so it
// carries its own minimal program, free of the range-returning helpers.
BEZEL_TEST(codegen_range_to_ienumerable_6998, array_control_spec_5_1) {
    JitResult r = runAotChecked(
        "module M6998m; "
        "public class H public static method Sum(xs: IEnumerable<int>): int "
        "var n := 0; for var x in xs do n += x; end; return n; "
        "end Sum; end H; "
        "public class M public static method Main(): int "
        "var a := new Array<int>(0); a.Add(4); a.Add(5); "
        "Console.WriteLine($\"{H.Sum(a)}\"); a.Dispose(); return 0; "
        "end Main; end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("9\n"));
}

// ---- review fix-forward rows (§16.3, §5.1, v0.340) ----

// A program with its own Main (no kHelpers), run under the leak + ASan oracle.
#define PROG_ROW(NAME, SRC, EXPECT)                                          \
    BEZEL_TEST(codegen_range_to_ienumerable_6998, NAME) {                     \
        JitResult r = runAotChecked(SRC);                                     \
        BEZEL_EXPECT_TRUE(r.ran);                                             \
        BEZEL_EXPECT_EQ(r.exitCode, 0);                                       \
        BEZEL_EXPECT_EQ(r.stdoutText, std::string(EXPECT));                   \
    }

// A lambda's expression body returning a range, to a `() -> IEnumerable<int>`
// parameter. Pre-fix: SEGV inside the lambda (the raw range struct returned
// where a frame pointer was expected). 100x: the frame is disposed per call.
PROG_ROW(lambda_expr_body_argument_spec_16_3,
    "module M6998r; "
    "public class M "
    "public static method Run(f: () -> IEnumerable<int>): int "
    "var s := 0; for var x in f() do s += x; end; return s; end Run; "
    "public static method Main(): int "
    "var t := 0; for var i in 0..<100 do t += Run(lambda() => 1..3); end; "
    "Console.WriteLine($\"{t}\"); return 0; end Main; end M;",
    "600\n")

// The return type ascribed on the lambda itself. Pre-fix: IR verify failure.
PROG_ROW(lambda_ascribed_return_spec_16_3,
    "module M6998s; "
    "public class M public static method Main(): int "
    "var f := lambda(): IEnumerable<int> => 1..3; var s := 0; "
    "for var i in 0..<100 do for var x in f() do s += x; end; end; "
    "Console.WriteLine($\"{s}\"); return 0; end Main; end M;",
    "600\n")

// A block-bodied lambda's `return`. Pre-fix: SEGV.
PROG_ROW(lambda_block_return_spec_16_3,
    "module M6998t; "
    "public class M public static method Main(): int "
    "var f: () -> IEnumerable<int> := lambda() => do return 1..3; end; "
    "var s := 0; "
    "for var i in 0..<100 do for var x in f() do s += x; end; end; "
    "Console.WriteLine($\"{s}\"); return 0; end Main; end M;",
    "600\n")

// An expression-bodied method and a property getter return the range.
// Pre-fix: the property getter failed IR verification.
PROG_ROW(expression_bodied_method_and_property_spec_16_3,
    "module M6998u; "
    "public class P public property E: IEnumerable<int> => 4..5; end P; "
    "public class M "
    "public static method Get(): IEnumerable<int> => 1..3; "
    "public static method Main(): int "
    "var p := new P(); var s := 0; "
    "for var i in 0..<100 do "
    "for var x in Get() do s += x; end; for var x in p.E do s += x; end; "
    "end; Console.WriteLine($\"{s}\"); return 0; end Main; end M;",
    "1500\n")

// A USER class named `Range` is not the builtin (§2.8; #6973's
// `isBuiltinRangeSym`). The first revision matched codegen's arms on the
// spelling and crashed the compiler here (rc 139). The callee does not iterate:
// iterating a user IEnumerable class through an interface head is #3522.
PROG_ROW(user_class_named_range_is_not_builtin_spec_16_3,
    "module M6998v; "
    "public class Range: IEnumerable<int> "
    "private owned _a: Array<int>; "
    "public constructor() _a := new Array<int>(4); _a.Add(7); end constructor; "
    "public method GetEnumerator(): IEnumerator<int> "
    "return _a.GetEnumerator(); end GetEnumerator; end Range; "
    "public class M "
    "public static method Touch(xs: IEnumerable<int>): int return 5; end Touch; "
    "public static method Main(): int "
    "using var r := new Range(); var e: IEnumerable<int> := r; "
    "Console.WriteLine($\"{Touch(r)} {Touch(e)}\"); return 0; end Main; end M;",
    "5 5\n")

// Positions with no disposal edge for the owning enumerator frame are
// rejected, naming the deferral (#7029), instead of miscompiled.
#define REJECT_ROW(NAME, SRC)                                                 \
    BEZEL_TEST(codegen_range_to_ienumerable_6998, NAME) {                     \
        JitResult r = runAot(SRC);                                            \
        BEZEL_EXPECT_FALSE(r.ran);                                            \
        BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(             \
            r, "converts a `Range<T>` to `IEnumerable<T>`"));                 \
        BEZEL_EXPECT_TRUE(                                                    \
            bezel::test::diagnosticMessageContains(r, "issue #7029"));        \
    }

// Pre-fix: AddressSanitizer stack-buffer-overflow (the 32-byte range stored
// into the pointer-typed local).
REJECT_ROW(assignment_rejected_spec_16_3,
    "module M6998w; "
    "public class M public static method Main(): int "
    "var a := new Array<int>(1); a.Add(1); "
    "var e: IEnumerable<int> := a; e := 1..3; "
    "var s := 0; for var x in e do s += x; end; "
    "Console.WriteLine($\"{s}\"); return 0; end Main; end M;")

// Pre-fix: the field initializer and the field assignment were accepted and
// leaked (3 allocations, about 144 B, per store).
REJECT_ROW(field_initializer_rejected_spec_16_3,
    "module M6998x; "
    "public class F public f: IEnumerable<int> := 1..3; end F; "
    "public class M public static method Main(): int "
    "var h := new F(); var s := 0; for var x in h.f do s += x; end; "
    "Console.WriteLine($\"{s}\"); return 0; end Main; end M;")

REJECT_ROW(field_assignment_rejected_spec_16_3,
    "module M6998y; "
    "public class F public f: IEnumerable<int>; "
    "public constructor() f := 4..6; end constructor; end F; "
    "public class M public static method Main(): int "
    "var h := new F(); var s := 0; for var x in h.f do s += x; end; "
    "Console.WriteLine($\"{s}\"); return 0; end Main; end M;")

// Pre-fix: SEGV.
REJECT_ROW(array_add_element_rejected_spec_16_3,
    "module M6998z; "
    "public class M public static method Main(): int "
    "var a := new Array<IEnumerable<int>>(); a.Add(1..3); "
    "var s := 0; for var e in a do for var x in e do s += x; end; end; "
    "Console.WriteLine($\"{s}\"); return 0; end Main; end M;")

// Pre-fix: SEGV.
REJECT_ROW(collection_literal_element_rejected_spec_16_3,
    "module M6998aa; "
    "public class M public static method Main(): int "
    "var a: Array<IEnumerable<int>> := [1..3, 5..6]; "
    "var s := 0; for var e in a do for var x in e do s += x; end; end; "
    "Console.WriteLine($\"{s}\"); return 0; end Main; end M;")

// A record's positional field is a store, not a call argument. Pre-fix: ASan
// stack-buffer-overflow.
REJECT_ROW(record_positional_field_rejected_spec_16_3,
    "module M6998ab; "
    "public record R(E: IEnumerable<int>); "
    "public class M public static method Main(): int "
    "var r := new R(1..3); var s := 0; for var x in r.E do s += x; end; "
    "Console.WriteLine($\"{s}\"); return 0; end Main; end M;")

// An `async` method's result lands in the task frame. Pre-fix: ASan
// stack-buffer-overflow in the resume function.
REJECT_ROW(async_return_rejected_spec_16_3,
    "module M6998ac; "
    "public class M "
    "public static async method Get(): Task<IEnumerable<int>> "
    "return 1..3; end Get; "
    "public static async method Main(): Task<int> "
    "var e := await Get(); var s := 0; for var x in e do s += x; end; "
    "Console.WriteLine($\"{s}\"); return 0; end Main; end M;")
