// fix #4836 (§11.1 / §11.2, v0.307): a class implementing a user-defined
// generic interface at two instantiations with overloads was rejected.
// `D: IM<int>, IM<string>` with `Get(int)` and `Get(string)` overloads was
// rejected because the witness-selection walk picked the first same-arity
// overload unconditionally. §11.2 requires matching parameter types under the
// obligation's substitution: `IM<int>.Get(T)` requires the `int` overload,
// `IM<string>.Get(T)` requires the `string` overload.
//
// THE DEFECT:
// - Sema: `checkAbstractImplementations` picked `Get(int)` as the witness for
//   BOTH obligations (first same-arity, no parameter-type check), then the
//   parameter-type check fired and rejected because `Get(int)` doesn't match
//   `IM<string>.Get(T)` under {T→string}.
// - Codegen: `findInterfaceImpl` (pre-#5115) picked the first same-arity
//   overload unconditionally at codegen time, so `IM<string>` dispatch reached
//   `Get(int)` at runtime and read the string pointer as an int (garbage).
//
// THE FIX:
// - Sema (#5097): `paramsMatchObligation` searches every same-arity candidate
//   for one whose parameter types actually match the current obligation under
//   its substitution, falling back to the first-found only when no candidate
//   matches.
// - Codegen (#5115): `findInterfaceImplAt` does the same parameter-type match
//   at codegen time when building per-instantiation itables.
//
// THIS TEST: verifies the observable runtime behavior — that each interface
// reference calls its matching overload. The sema-only test in
// `tests/sema/interface_witness_param_type_5097_test.cpp` only proves
// compilation succeeds; this test proves the overload dispatch is correct.

#include "support/aot_support.hpp"   // runAot / runAotOut — AOT test harness
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// Returns true if ran successfully, leaving the exit code in `code`.
bool runChecked(const std::string& program, int& code) {
    auto r = runAot(program);
    if (!r.ran) return false;
    code = r.exitCode;
    return true;
}

}  // namespace

// ACCEPT: a user-defined generic interface `IM<T>` at two instantiations
// (`IM<int>`, `IM<string>`) with two correctly-typed overloads
// (`Get(int): int` and `Get(string): int`). Verifies the observable runtime
// behavior: `IM<string>` dispatch calls the string overload (returns
// "hello".Length + 100 = 105), `IM<int>` dispatch calls the int overload
// (returns 41 + 1 = 42).
BEZEL_TEST(user_interface_two_instantiation_overloads_4836,
     user_defined_generic_interface_two_overloads_accepted_and_correctly_dispatched_spec_11_2) {
    const std::string src =
        "module M; "
        "public interface IM<T> "
        "  method Get(x: T): int; "
        "end IM; "
        "public class D: IM<int>, IM<string> "
        "  public constructor() end constructor; "
        "  public method Get(x: int): int => x + 1; "
        "  public method Get(x: string): int => x.Length + 100; "
        "end D; "
        "class Pgm "
        "  public static method Main(): int "
        "    using var d := new D(); "
        "    var m: IM<string> := d; "
        "    var n: IM<int> := d; "
        "    Console.WriteLine(m.Get(\"hello\")); "
        "    Console.WriteLine(n.Get(41)); "
        "    return 0; "
        "  end Main; "
        "end Pgm;";
    // The stdout pin: "hello".Length + 100 = 5 + 100 = 105, 41 + 1 = 42.
    // runAotOut strips exactly one trailing newline (for platform-independence),
    // so the expected string is "105\n42" (not "105\n42\n").
    // Without the fix: codegen dispatches IM<string> to Get(int), so the first
    // line prints garbage (the string pointer interpreted as int), the second
    // prints 42. The stdout would NOT be "105\n42".
    int code = -1;
    BEZEL_EXPECT_TRUE(runChecked(src, code));
    BEZEL_EXPECT_EQ(runAotOut(src), "105\n42");
    // Exit code is 0 (the explicit return 0).
    BEZEL_EXPECT_EQ(code, 0);
}
