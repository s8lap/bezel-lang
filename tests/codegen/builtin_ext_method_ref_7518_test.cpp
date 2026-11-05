// Regression tests for #7518 — a bound method reference to a USER EXTENSION
// member over a BUILT-IN receiver miscompiles.
//
// Language spec §27.6 (v0.355, docs/Bezel_Language_Spec.md:6382-6432),
// *Method references*, verbatim:
//
//   "**Receiver-qualified** — `v.Scaled` over `extension Vec`, the member
//    resolved on the value exactly as the call `v.Scaled()` is (§26.5) — the
//    reference is a bound reference: the receiver is captured at formation as
//    the instance-method sentence above states (by reference for a class- or
//    interface-typed receiver, by copy for a value-shaped one — a record, an
//    enum, a copyable choice, a non-owning inline array, a primitive, the
//    unit value, a `string` or a function value — and rejected over a move-only
//    one), and the resulting function has the method's own parameters only
//    (`var scaled: () -> int := v.Scaled;`)."
//
// Language spec §26.5 rule 2 (v0.355, docs/Bezel_Language_Spec.md:6115),
// verbatim:
//
//   "If the receiver's type declares no instance member of that name, the
//    compiler searches the current module and all imported modules … for
//    extension blocks on `x`'s type, base types, or implemented interfaces …
//    The outcome is the same for every member access on a value, not only a
//    call — a property or field read or write, and a method reference, which
//    resolves its member exactly as the call does (§27.6) …"
//
// THE DEFECT (MEASURED on this fix's parent, r30-c1 = 443b6701). The issue's
// own repro — a `string` receiver:
//
//   module M; import System;
//   extension string public method Twice(): int => 42; end;
//   class C public static method Main(): int
//     var a: string := "abc"; var f: () -> int := a.Twice;
//     Console.WriteLine(f()); return 0;
//   end Main; end C;
//
// compiled with the internal warning
//
//   lowerLocalDecl's initializer expression failed to lower to a value for
//   this local's slot (degraded to a zero-initialized value) — #2434
//
// and then SEGV'd (rc 139). The `string` receiver's built-in member arm in
// codegen (LlvmBackend_Exprs.cpp's CG27a) returned a bare `nullptr` for every
// member name that is not a property getter — and a §27.6 method reference's
// symbol is a MethodSymbol, never a getter — so the closure cell was never
// built.
//
// THE FIX — src/codegen/LlvmBackend_Exprs.cpp: the CG27a arm no longer claims
// a member access SEMA bound to a MethodSymbol (a §27.6 method reference);
// such an access falls through to the method-reference lowering (CG60), which
// builds the closure cell that captures the receiver.
//
// ORACLE: runAotChecked (the strict AOT oracle) on every row, with the
// program's EXACT stdout asserted. The heap-receiver row loops 50× so a leak
// or an early free of the captured receiver is visible.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

void expectClean(const JitResult& r, const char* out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

}  // namespace

// ---- the issue's own repro: a `string` receiver ---------------------------

BEZEL_TEST(builtin_ext_method_ref_7518,
           string_receiver_bound_reference_calls_the_extension_spec_27_6) {
    expectClean(
        runAotChecked(
            "module M; import System;\n"
            "extension string public method Twice(): int => 42; end;\n"
            "class C public static method Main(): int\n"
            "  var a: string := \"abc\";\n"
            "  var f: () -> int := a.Twice;\n"
            "  Console.WriteLine(f());\n"
            "  return 0;\n"
            "end Main; end C;\n"),
        "42\n");
}

// Control: the CALL spelling of the same member is unaffected (it was already
// admitted and already answered the extension's value).
BEZEL_TEST(builtin_ext_method_ref_7518,
           string_receiver_call_form_still_answers_spec_27_6) {
    expectClean(
        runAotChecked(
            "module M; import System;\n"
            "extension string public method Twice(): int => 42; end;\n"
            "class C public static method Main(): int\n"
            "  var a: string := \"abc\";\n"
            "  Console.WriteLine(a.Twice());\n"
            "  return 0;\n"
            "end Main; end C;\n"),
        "42\n");
}

// ---- the other built-in receiver kinds ------------------------------------

// A builtin COLLECTION head (stdlib §4.4 `Array<T>`), a name the head does
// NOT declare — §26.5 rule 2 sends it to the extension blocks.
BEZEL_TEST(builtin_ext_method_ref_7518,
           array_receiver_bound_reference_calls_the_extension_spec_26_5) {
    expectClean(
        runAotChecked(
            "module M; import System;\n"
            "extension Array<int> public method Total(): int => 42; end;\n"
            "class C public static method Main(): int\n"
            "  var a: Array<int> := [1, 2, 3];\n"
            "  var f: () -> int := a.Total;\n"
            "  Console.WriteLine(f());\n"
            "  return 0;\n"
            "end Main; end C;\n"),
        "42\n");
}

// The `Length` name the builtin heads and the stdlib disagree about
// (stdlib §4.4 declares `Count`, not `Length`).
BEZEL_TEST(builtin_ext_method_ref_7518,
           array_length_extension_reference_calls_the_extension_spec_27_6) {
    expectClean(
        runAotChecked(
            "module M; import System;\n"
            "extension Array<int> public method Length(): int => 42; end;\n"
            "class C public static method Main(): int\n"
            "  var a: Array<int> := [1, 2, 3];\n"
            "  var f: () -> int := a.Length;\n"
            "  Console.WriteLine(f());\n"
            "  return 0;\n"
            "end Main; end C;\n"),
        "42\n");
}

// The borrowing LIST INTERFACE head (stdlib §4.1 `IList<T>`).
BEZEL_TEST(builtin_ext_method_ref_7518,
           ilist_receiver_bound_reference_calls_the_extension_spec_26_5) {
    expectClean(
        runAotChecked(
            "module M; import System;\n"
            "extension IList<int> public method Total(): int => 42; end;\n"
            "class C public static method Main(): int\n"
            "  var a: Array<int> := [1, 2, 3];\n"
            "  var l: IList<int> := a;\n"
            "  var f: () -> int := l.Total;\n"
            "  Console.WriteLine(f());\n"
            "  return 0;\n"
            "end Main; end C;\n"),
        "42\n");
}

// A PRIMITIVE receiver (value-shaped: captured by copy, §27.6).
BEZEL_TEST(builtin_ext_method_ref_7518,
           int_receiver_bound_reference_calls_the_extension_spec_27_6) {
    expectClean(
        runAotChecked(
            "module M; import System;\n"
            "extension int public method Twice(): int => 42; end;\n"
            "class C public static method Main(): int\n"
            "  var n: int := 7;\n"
            "  var f: () -> int := n.Twice;\n"
            "  Console.WriteLine(f());\n"
            "  return 0;\n"
            "end Main; end C;\n"),
        "42\n");
}

// A builtin CLASS receiver (stdlib §3.1 `StringBuilder`).
BEZEL_TEST(builtin_ext_method_ref_7518,
           string_builder_receiver_bound_reference_calls_the_extension) {
    expectClean(
        runAotChecked(
            "module M; import System;\n"
            "extension StringBuilder public method Size(): int => 42; end;\n"
            "class C public static method Main(): int\n"
            "  var sb: StringBuilder := new StringBuilder();\n"
            "  var f: () -> int := sb.Size;\n"
            "  Console.WriteLine(f());\n"
            "  return 0;\n"
            "end Main; end C;\n"),
        "42\n");
}

// A method WITH A PARAMETER: "the resulting function has the method's own
// parameters only" (§27.6).
BEZEL_TEST(builtin_ext_method_ref_7518,
           bound_reference_with_a_parameter_calls_the_extension_spec_27_6) {
    expectClean(
        runAotChecked(
            "module M; import System;\n"
            "extension string public method Plus(n: int): int => 40 + n; end;\n"
            "class C public static method Main(): int\n"
            "  var a: string := \"abc\";\n"
            "  var f: (int) -> int := a.Plus;\n"
            "  Console.WriteLine(f(2));\n"
            "  return 0;\n"
            "end Main; end C;\n"),
        "42\n");
}

// A GENERIC extension block (stdlib §26.4): the block's parameters are bound
// by the receiver at the formation (§27.6 / §26.4).
BEZEL_TEST(builtin_ext_method_ref_7518,
           generic_extension_block_reference_calls_the_extension_spec_26_4) {
    expectClean(
        runAotChecked(
            "module M; import System;\n"
            "extension Array<T> public method Head(): T => this[0]; end;\n"
            "class C public static method Main(): int\n"
            "  var a: Array<int> := [7, 2, 3];\n"
            "  var f: () -> int := a.Head;\n"
            "  Console.WriteLine(f());\n"
            "  return 0;\n"
            "end Main; end C;\n"),
        "7\n");
}

// The reference STORED IN A FIELD and called later — the closure outlives the
// statement that formed it.
BEZEL_TEST(builtin_ext_method_ref_7518,
           bound_reference_stored_in_a_field_is_called_later_spec_27_6) {
    expectClean(
        runAotChecked(
            "module M; import System;\n"
            "extension string public method Twice(): int => 42; end;\n"
            "class Holder\n"
            "  public field: () -> int;\n"
            "  public constructor()\n"
            "    field := \"abc\".Twice;\n"
            "  end constructor;\n"
            "  public method Call(): int\n"
            "    return field();\n"
            "  end Call;\n"
            "end Holder;\n"
            "class C public static method Main(): int\n"
            "  var h := new Holder();\n"
            "  Console.WriteLine(h.Call());\n"
            "  return 0;\n"
            "end Main; end C;\n"),
        "42\n");
}

// A HEAP-ALLOCATED receiver (a +1 string producer) captured by the closure:
// the closure must neither leak it nor over-release it. The loop runs 50× so
// a missing or a doubled release is visible under the sanitizer.
BEZEL_TEST(builtin_ext_method_ref_7518,
           heap_string_receiver_bound_reference_loops_cleanly_spec_28_9) {
    expectClean(
        runAotChecked(
            "module M; import System;\n"
            "extension string public method Len(): int => this.Length; end;\n"
            "class Make\n"
            "  public static method Str(n: int): string\n"
            "    return \"s\" + n.ToString();\n"
            "  end Str;\n"
            "end Make;\n"
            "class C public static method Main(): int\n"
            "  var t: int := 0;\n"
            "  for var i in 0..49 do\n"
            "    var f: () -> int := Make.Str(i).Len;\n"
            "    t := t + f();\n"
            "  end;\n"
            "  Console.WriteLine(t);\n"
            "  return 0;\n"
            "end Main; end C;\n"),
        "140\n");
}
