// Regression tests for issue #5129 (§27.5 / §28.3 / §28.9 / §28.14,
// v0.313) — narrowed.
//
// A producer-initialized closure local passed as the argument for a bare
// METHOD-OWN type-parameter formal (`H.Keep<() -> int>(f)` with
// `Keep<U>(x: U)`) was classified ESCAPING by ClosureEscapeWalker
// (LlvmBackend_Setup.cpp) — an unknown callee could retain it — and so was
// never released: 48 B leaked per call. The fix treats such an argument as
// a non-consuming read ONLY when the callee is a locally compiled,
// non-async, non-virtual method whose body provably uses the formal by
// direct invocation alone (calleeBodyUsesFormalOnlyByInvocation, fail-
// closed; a bare `U` formal cannot be invoked at all per §27.7, so in
// practice: never referenced). The local's +1 then stays in its own slot
// and is released once at scope end.
//
// RESIDUAL (why this is `narrow #5129`, not `fix`): a callee that stores
// the formal — into an `Object?` field (the pinned
// codegen_5127_escape.method_own_type_param_object_field_escape_no_uaf
// row, which therefore STAYS on runAotCheckedLenient), a caller-supplied
// `Array<U>`, a forwarded generic call — keeps today's leak. Releasing
// there was the R24 attempt (13a18e4d) and it is a heap-use-after-free
// (issue #5216): the storing paths take no reference of their own for a
// function element, so the local's +1 is the only one keeping the stored
// cell alive. The storing rows below pin ASan-clean + correct value under
// runAotCheckedLenient (ASan on, LSan off), the released rows pin
// leak-free under runAotChecked. Loops run 200×.
//
// Without the fix, `named_closure_local_unreferenced_formal_released_at_
// scope_end` fails under LeakSanitizer (9600 B in 200 allocations).
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {
constexpr const char* kKeep =
    "class H "
    "  public static method Keep<U>(x: U): int "
    "    return 1; "
    "  end Keep; "
    "end H; ";
}  // namespace

// The issue's named-local shape against a callee that never references the
// formal: released at scope end, leak-free.
BEZEL_TEST(codegen_5129,
           named_closure_local_unreferenced_formal_released_at_scope_end) {
    auto r = runAotChecked(std::string("module Conf; ") + kKeep +
        "class Program "
        "  public static method Main(): int "
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      var f: () -> int := lambda () => 42; "
        "      total := total + H.Keep<() -> int>(f) + f() - 42; "
        "      i := i + 1; "
        "    end; "
        "    return total; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 200);
}

// The local is ALSO returned from the enclosing method: the walk's return
// arm still wins (escape), nothing is over-released, and the returned
// closure is invoked by the caller. Leak-free.
BEZEL_TEST(codegen_5129,
           named_local_returned_after_unreferenced_callee_no_over_release) {
    auto r = runAotChecked(std::string("module Conf; ") + kKeep +
        "class Program "
        "  public static method Make(): () -> int "
        "    var f: () -> int := lambda () => 1; "
        "    var k := H.Keep<() -> int>(f); "
        "    return f; "
        "  end Make; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      var g := Program.Make(); "
        "      total := total + g(); "
        "      i := i + 1; "
        "    end; "
        "    return total; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 200);
}

// A PARAMETER (a slot that owns no +1) passed to the same callee: must not
// be released by anyone but the literal's own owner. Leak-free.
BEZEL_TEST(codegen_5129, param_arg_to_unreferenced_callee_no_over_release) {
    auto r = runAotChecked(std::string("module Conf; ") + kKeep +
        "class Program "
        "  public static method Mid(g: () -> int): int "
        "    return H.Keep<() -> int>(g) + g() - 1; "
        "  end Mid; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      total := total + Program.Mid(lambda () => 1); "
        "      i := i + 1; "
        "    end; "
        "    return total; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 200);
}

// An alias of the local is what gets passed: the alias arm keeps its
// pre-existing escape verdict (this row leaks the same 48 B/iteration with
// and without the fix — MEASURED), and no slot is over-released.
BEZEL_TEST(codegen_5129,
           named_local_alias_to_unreferenced_callee_no_over_release) {
    auto r = runAotCheckedLenient(std::string("module Conf; ") + kKeep +
        "class Program "
        "  public static method Main(): int "
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      var f: () -> int := lambda () => 1; "
        "      var a := f; "
        "      total := total + H.Keep<() -> int>(a) + a() + f() - 2; "
        "      i := i + 1; "
        "    end; "
        "    return total; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 200);
}

// RESIDUAL, issue #5216's named-local twin (p1_list.bzl): the callee stores
// the local into a caller-supplied `Array<U>`; the local must NOT be
// released (the R24 attempt did, and this row then dies with
// heap-use-after-free). The stored cells are invoked after the loop.
BEZEL_TEST(codegen_5129, named_closure_local_stored_via_array_u_add_no_uaf) {
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
        "      var f: () -> int := lambda () => 42; "
        "      total := total + H.AddTo<() -> int>(keep, f); "
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
    BEZEL_EXPECT_EQ(r.stdoutText, "8400\n");
}

// RESIDUAL: the callee stores the local into a caller-supplied holder's
// `Object?` field. Not released; ASan-clean.
BEZEL_TEST(codegen_5129, named_closure_local_object_field_store_no_uaf) {
    auto r = runAotCheckedLenient(
        "module Conf; "
        "class Holder "
        "  public F: Object? := null; "
        "end Holder; "
        "class H "
        "  public static method Put<U>(h: Holder, x: U): int "
        "    h.F := x; "
        "    return 1; "
        "  end Put; "
        "end H; "
        "class Program "
        "  public static method Main(): int "
        "    var total := 0; "
        "    var h := new Holder(); "
        "    var i := 0; "
        "    while i < 200 do "
        "      var f: () -> int := lambda () => 42; "
        "      total := total + H.Put<() -> int>(h, f); "
        "      i := i + 1; "
        "    end; "
        "    return total; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 200);
}

// A `virtual` callee whose base body never references `x` but whose runtime
// override stores it: the walk refuses the base body, so neither the named
// local nor the literal is released.
BEZEL_TEST(codegen_5129,
           named_closure_local_virtual_callee_not_released_no_uaf) {
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
        "      var f: () -> int := lambda () => 42; "
        "      total := total + b.Keep<() -> int>(f); "
        "      total := total + b.Keep<() -> int>(lambda () => 7); "
        "      i := i + 1; "
        "    end; "
        "    Console.WriteLine($\"{total}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "800\n");
}
