// Codegen / §28.9 — issue #7755: a captured, MUTATED local of a lambda inside
// a SPECIALIZED (`$spec`) generic body leaked its heap capture cell, once per
// call (4 B for an `int`). The non-generic twin is clean.
//
// Spec: §27.5 "Closures" (a closure captures by reference through a heap
// cell shared with the enclosing scope) and §28.9 "Runtime-managed types"
// (runtime-managed storage is reclaimed exactly once; nothing may leak), with
// the monomorphized body of §14.1 "Generic declarations". Spec revision
// v0.359.
//
// THE DEFECT. A captured local is boxed into a heap cell, and the cell is
// freed at the declaring scope's exit only when `currentFn_ ==
// cellFreeEligibleFn_` and the local is in `cellFreeEligible_` (the
// `collectCellFreeEligible` pre-scan). `emitFunctionBody` pins both;
// `buildSpecializedMethodFn` (the lazily built per-instantiation body) pinned
// the string, unwind and closure-release sets (#1483, #2807, #2854) but never
// the cell set, so every captured cell of a specialization leaked. #2854's
// header recorded the capture-cell gap as filed separately: this is it.
//
// Every row is LOOP-AMPLIFIED (100 calls): a one-call Main is not a reliable
// LSan oracle. `runAotChecked` runs the ASan/LSan leg. Values are written to
// stdout (exit codes are 8-bit truncated).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// (A) generic FUNCTION, int capture mutated by the lambda. 100 calls of
// LamArg<long>(i): the lambda adds 1,2,3 to the captured `s` => 6 per call.
const char* kGenericFnInt =
    "module M; "
    "class Program "
    "  public static method LamArg<T>(z: T, i: int): int "
    "    var s := i; "
    "    var f := lambda(k: int) => do s := s + k; end; "
    "    f(1); f(2); f(3); "
    "    return s; "
    "  end LamArg; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..100 do "
    "      total := total + Program.LamArg<long>(10000000000L, i); "
    "    end; "
    "    Console.WriteLine(total); "
    "    return 0; "
    "  end Main; "
    "end Program;";
// sum_{i=1..100} (i + 6) = 5050 + 600
const char* kGenericFnIntOut = "5650\n";

// (B) generic EXTENSION method, int capture mutated. The lambda does not use
// `this`; the captured, mutated local is `s`.
const char* kGenericExtInt =
    "module M; "
    "extension Array<T> "
    "  public method Sum3(i: int): int "
    "    var s := i; "
    "    var f := lambda(k: int) => do s := s + k; end; "
    "    f(1); f(2); f(3); "
    "    return s + this.Count; "
    "  end Sum3; "
    "end; "
    "class Program "
    "  public static method Main(): int "
    "    var e: Array<long> := [10000000000L, 20000000000L]; "
    "    var total := 0; "
    "    for var i in 1..100 do "
    "      total := total + e.Sum3(i); "
    "    end; "
    "    Console.WriteLine(total); "
    "    return 0; "
    "  end Main; "
    "end Program;";
// sum_{i=1..100} (i + 6 + 2) = 5050 + 800
const char* kGenericExtIntOut = "5850\n";

// (C) generic FUNCTION, string capture mutated (the cell holds a string
// reference; the string's own release is the #2807 machinery).
const char* kGenericFnString =
    "module M; "
    "class Program "
    "  public static method Cat<T>(z: T, i: int): int "
    "    var s := $\"a{i}\"; "
    "    var f := lambda(k: int) => do s := s + \"b\"; end; "
    "    f(1); f(2); "
    "    return s.Length; "
    "  end Cat; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..100 do "
    "      total := total + Program.Cat<long>(10000000000L, i); "
    "    end; "
    "    Console.WriteLine(total); "
    "    return 0; "
    "  end Main; "
    "end Program;";
// "a{i}bb": 1 + len(i) + 2 => sum = 300 + (9*1 + 90*2 + 3) = 300 + 192 = 492
const char* kGenericFnStringOut = "492\n";

// (D) generic EXTENSION method, string capture mutated.
const char* kGenericExtString =
    "module M; "
    "extension Array<T> "
    "  public method Cat(i: int): int "
    "    var s := $\"a{i}\"; "
    "    var f := lambda(k: int) => do s := s + \"b\"; end; "
    "    f(1); f(2); "
    "    return s.Length; "
    "  end Cat; "
    "end; "
    "class Program "
    "  public static method Main(): int "
    "    var e: Array<long> := [10000000000L, 20000000000L]; "
    "    var total := 0; "
    "    for var i in 1..100 do "
    "      total := total + e.Cat(i); "
    "    end; "
    "    Console.WriteLine(total); "
    "    return 0; "
    "  end Main; "
    "end Program;";
const char* kGenericExtStringOut = "492\n";

}  // namespace

BEZEL_TEST(codegen_generic_spec_lambda_captured_cell_release_7755,
           generic_function_int_capture_cell_released_spec_28_9) {
    JitResult r = runAotChecked(kGenericFnInt);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(kGenericFnIntOut));
}

BEZEL_TEST(codegen_generic_spec_lambda_captured_cell_release_7755,
           generic_extension_int_capture_cell_released_spec_28_9) {
    JitResult r = runAotChecked(kGenericExtInt);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(kGenericExtIntOut));
}

BEZEL_TEST(codegen_generic_spec_lambda_captured_cell_release_7755,
           generic_function_string_capture_cell_released_spec_28_9) {
    JitResult r = runAotChecked(kGenericFnString);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(kGenericFnStringOut));
}

BEZEL_TEST(codegen_generic_spec_lambda_captured_cell_release_7755,
           generic_extension_string_capture_cell_released_spec_28_9) {
    JitResult r = runAotChecked(kGenericExtString);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(kGenericExtStringOut));
}
