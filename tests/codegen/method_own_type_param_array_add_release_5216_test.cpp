// Regression tests for issue #5216 (§28.9 / §28.14 / §28.11, v0.313).
//
// The #5116 call-bounded arm in lowerCallDispatch (LlvmBackend_Calls2.cpp)
// released a lambda LITERAL bound to a bare METHOD-OWN type-parameter formal
// (`AddTo<U>(l: Array<U>, x: U)` at `U := () -> int`) at the call's end,
// arguing that "a method-own-`U` argument can never be stored into … a
// collection reachable beyond the call". It can: `l.Add(x)` stores it into
// the CALLER's `Array<U>`, sema admits that for every `U` (§28.14's sink
// obligations are keyed on a function-TYPED formal, which a bare `U` is
// not), and the borrowing insert takes no reference of its own for a
// function element. The literal's only +1 was gone while the array still
// pointed at the cell — heap-use-after-free at the next invocation.
//
// The fix arms that release only when the callee body provably uses the
// formal by direct invocation alone (calleeBodyUsesFormalOnlyByInvocation,
// fail-closed; for a bare `U` formal, which §27.7 forbids invoking, that
// means "never references it"). Every other shape keeps the cell alive.
//
// Oracles. The storing rows use runAotCheckedLenient: ASan (the UAF /
// double-free detector) stays ON, only LeakSanitizer is off, because the
// stored cell is then LEAKED — MEASURED to be exactly what the non-generic
// `Array<() -> int>.Add(lambda () => 42)` path does today (§28.11's
// runtime-managed element row is implemented for `string` only; a separate
// gap, not this issue's). Each such row asserts the program's VALUE on
// stdout (the invocations through the stored cells), so "the cell survived
// intact" is checked, not just "nothing crashed". The control row (callee
// never references `x`) is under runAotChecked: the release must still fire
// there and the program must be leak-free. Every loop runs 200× so a
// per-call fault cannot hide in a one-shot.
//
// Without the fix, `lambda_literal_stored_via_array_u_add_no_uaf` (the
// issue's literal repro) dies under ASan with heap-use-after-free.
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {
// 200 stored cells, each answering 42, invoked once after the loop.
constexpr const char* kSum = "8400\n";
}  // namespace

// Issue #5216's literal repro (p1L_literal.bzl), looped 200×: the literal is
// stored into the caller-supplied `Array<U>` and invoked after every call
// has returned.
BEZEL_TEST(codegen_5216, lambda_literal_stored_via_array_u_add_no_uaf) {
    auto r = runAotCheckedLenient(
        "module Conf; "
        "class H "
        "  public static method AddTo<U>(l: Array<U>, x: U): int "
        "    l.Add(x); "
        "    return 1; "
        "  end AddTo; "
        "end H; "
        "class Program "
        "  public static method Main(): int "
        "    var keep := new Array<() -> int>(); "
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      total := total + H.AddTo<() -> int>(keep, lambda () => 42); "
        "      i := i + 1; "
        "    end; "
        "    var s := 0; "
        "    for var g in keep do "
        "      s := s + g(); "
        "    end; "
        "    Console.WriteLine($\"{s}\"); "
        "    return total; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 200);
    BEZEL_EXPECT_EQ(r.stdoutText, kSum);
}

// Control: the callee never references `x`, so the call-end release must
// still fire and the program must be leak-free (runAotChecked, LSan on).
BEZEL_TEST(codegen_5216,
           lambda_literal_unreferenced_formal_released_at_call_end) {
    auto r = runAotChecked(
        "module Conf; "
        "class H "
        "  public static method Keep<U>(x: U): int "
        "    return 1; "
        "  end Keep; "
        "end H; "
        "class Program "
        "  public static method Main(): int "
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      total := total + H.Keep<() -> int>(lambda () => 42); "
        "      i := i + 1; "
        "    end; "
        "    return total; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 200);
}

// Store into a caller-supplied `Dictionary<int, U>`.
BEZEL_TEST(codegen_5216, lambda_literal_stored_via_dictionary_u_add_no_uaf) {
    auto r = runAotCheckedLenient(
        "module Conf; "
        "class H "
        "  public static method Put<U>(d: Dictionary<int, U>, k: int, x: U): int "
        "    d.Add(k, x); "
        "    return 1; "
        "  end Put; "
        "end H; "
        "class Program "
        "  public static method Main(): int "
        "    var keep := new Dictionary<int, () -> int>(); "
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      total := total + H.Put<() -> int>(keep, i, lambda () => 42); "
        "      i := i + 1; "
        "    end; "
        "    var s := 0; "
        "    for var e in keep do "
        "      s := s + e.Value(); "
        "    end; "
        "    Console.WriteLine($\"{s}\"); "
        "    return total; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 200);
    BEZEL_EXPECT_EQ(r.stdoutText, kSum);
}

// Store into a `T`-typed field of a generic class instantiated at `U`,
// returned to the caller as an owned holder.
BEZEL_TEST(codegen_5216,
           lambda_literal_stored_into_generic_class_u_field_no_uaf) {
    auto r = runAotCheckedLenient(
        "module Conf; "
        "class Cell<T> "
        "  public V: T; "
        "  public constructor(v: T) "
        "    V := v; "
        "  end constructor; "
        "end Cell; "
        "class H "
        "  public static method Wrap<U>(x: U): owned Cell<U> "
        "    return new Cell<U>(x); "
        "  end Wrap; "
        "end H; "
        "class Program "
        "  public static method Main(): int "
        "    using var keep := new OwningArray<Cell<() -> int>>(); "
        "    var i := 0; "
        "    while i < 200 do "
        "      keep.Add(H.Wrap<() -> int>(lambda () => 42)); "
        "      i := i + 1; "
        "    end; "
        "    var s := 0; "
        "    for var c in keep do "
        "      s := s + c.V(); "
        "    end; "
        "    Console.WriteLine($\"{s}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, kSum);
}

// Captured by a wrapper lambda inside the callee, which is then stored into
// a caller-supplied `Array<() -> U>`.
BEZEL_TEST(codegen_5216, lambda_literal_captured_by_callee_wrapper_no_uaf) {
    auto r = runAotCheckedLenient(
        "module Conf; "
        "class H "
        "  public static method Cap<U>(l: Array<() -> U>, x: U): int "
        "    var w: () -> U := lambda () => x; "
        "    l.Add(w); "
        "    return 1; "
        "  end Cap; "
        "end H; "
        "class Program "
        "  public static method Main(): int "
        "    var keep := new Array<() -> () -> int>(); "
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      total := total + H.Cap<() -> int>(keep, lambda () => 42); "
        "      i := i + 1; "
        "    end; "
        "    var s := 0; "
        "    for var w in keep do "
        "      var g := w(); "
        "      s := s + g(); "
        "    end; "
        "    Console.WriteLine($\"{s}\"); "
        "    return total; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 200);
    BEZEL_EXPECT_EQ(r.stdoutText, kSum);
}

// Forwarded to a second generic callee that does the storing: the outer
// callee references `x` in argument position, which the walk refuses.
BEZEL_TEST(codegen_5216,
           lambda_literal_forwarded_to_second_generic_callee_no_uaf) {
    auto r = runAotCheckedLenient(
        "module Conf; "
        "class H "
        "  public static method Inner<V>(l: Array<V>, y: V): int "
        "    l.Add(y); "
        "    return 1; "
        "  end Inner; "
        "  public static method Outer<U>(l: Array<U>, x: U): int "
        "    return H.Inner<U>(l, x); "
        "  end Outer; "
        "end H; "
        "class Program "
        "  public static method Main(): int "
        "    var keep := new Array<() -> int>(); "
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      total := total + H.Outer<() -> int>(keep, lambda () => 42); "
        "      i := i + 1; "
        "    end; "
        "    var s := 0; "
        "    for var g in keep do "
        "      s := s + g(); "
        "    end; "
        "    Console.WriteLine($\"{s}\"); "
        "    return total; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 200);
    BEZEL_EXPECT_EQ(r.stdoutText, kSum);
}

// Returned as `U` and stored by the caller.
BEZEL_TEST(codegen_5216, lambda_literal_returned_as_u_then_stored_no_uaf) {
    auto r = runAotCheckedLenient(
        "module Conf; "
        "class H "
        "  public static method Id<U>(x: U): U "
        "    return x; "
        "  end Id; "
        "end H; "
        "class Program "
        "  public static method Main(): int "
        "    var keep := new Array<() -> int>(); "
        "    var i := 0; "
        "    while i < 200 do "
        "      keep.Add(H.Id<() -> int>(lambda () => 42)); "
        "      i := i + 1; "
        "    end; "
        "    var s := 0; "
        "    for var g in keep do "
        "      s := s + g(); "
        "    end; "
        "    Console.WriteLine($\"{s}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, kSum);
}

// A function value that is NOT a closure literal: a static method reference
// (§27.6) stored the same way.
BEZEL_TEST(codegen_5216,
           static_method_reference_stored_via_array_u_add_no_uaf) {
    auto r = runAotCheckedLenient(
        "module Conf; "
        "class H "
        "  public static method AddTo<U>(l: Array<U>, x: U): int "
        "    l.Add(x); "
        "    return 1; "
        "  end AddTo; "
        "  public static method FortyTwo(): int "
        "    return 42; "
        "  end FortyTwo; "
        "end H; "
        "class Program "
        "  public static method Main(): int "
        "    var keep := new Array<() -> int>(); "
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      total := total + H.AddTo<() -> int>(keep, H.FortyTwo); "
        "      i := i + 1; "
        "    end; "
        "    var s := 0; "
        "    for var g in keep do "
        "      s := s + g(); "
        "    end; "
        "    Console.WriteLine($\"{s}\"); "
        "    return total; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 200);
    BEZEL_EXPECT_EQ(r.stdoutText, kSum);
}

// A `virtual` callee whose base body never references `x` but whose runtime
// override stores it: the walk must refuse the base body (the release
// decision is made against the static callee), so nothing is released.
BEZEL_TEST(codegen_5216, lambda_literal_virtual_callee_not_released_no_uaf) {
    auto r = runAotCheckedLenient(
        "module Conf; "
        "class Base "
        "  public virtual method Keep<U>(x: U): int "
        "    return 1; "
        "  end Keep; "
        "end Base; "
        "class Der: Base "
        "  public F: Object? := null; "
        "  public override method Keep<U>(x: U): int "
        "    F := x; "
        "    return 2; "
        "  end Keep; "
        "end Der; "
        "class Program "
        "  public static method Main(): int "
        "    var total := 0; "
        "    var i := 0; "
        "    var b: Base := new Der(); "
        "    while i < 200 do "
        "      total := total + b.Keep<() -> int>(lambda () => 7); "
        "      i := i + 1; "
        "    end; "
        "    Console.WriteLine($\"{total}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "400\n");
}
