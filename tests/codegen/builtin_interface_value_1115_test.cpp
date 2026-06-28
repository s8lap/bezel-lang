// Spec-derived tests for #1115 (path-2): the auto-imported builtin generic
// interfaces IComparable<T> (stdlib §2.8) and IEquatable<T> (§2.9) are usable as
// ordinary interface-typed VALUES — fields, parameters, returns, locals, heap
// references — dispatching through the itable like a user-declared interface.
//
// Before path-2 these were declaration-less (registered for name resolution but
// with no synthesized InterfaceDecl/vtable), so used as a value type they
// IR-failed ("Call parameter type does not match"). The fix synthesizes a real
// Interface TypeSymbol for each (carrying the CompareTo/Equals contract member),
// so every codegen layer — which keys on declaration->kind==Interface — picks
// them up with no new codegen. Records implementing IComparable (#1141) and the
// `where T: IComparable<T>` constraint form keep working unchanged.
//
// runJit returns the FULL int from Main() (not 8-bit wrapped).

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}
const char* CMP =
    "module M; "
    "class C: IComparable<int> "
    "  private _v: int; "
    "  public constructor(v: int) _v := v; end constructor; "
    "  public method CompareTo(other: int): int => (2 if _v > other else 0); "
    "end C; ";
const char* EQ =
    "module M; "
    "class C: IEquatable<int> "
    "  private _v: int; "
    "  public constructor(v: int) _v := v; end constructor; "
    "  public method Equals(other: int): bool => _v = other; "
    "end C; ";
}  // namespace

// #1115: IComparable<int> as a PARAMETER type — dispatch through the interface.
BEZEL_TEST(builtin_interface_value_1115, icomparable_as_param_dispatches) {
    BEZEL_EXPECT_EQ(run(
        std::string(CMP) +
        "class App "
        "  private static method Use(x: IComparable<int>): int => x.CompareTo(3); "
        "  public static method Main(): int "
        "    using var c := new C(5); return Use(c); "          // 5>3 → 2
        "  end Main; end App;"), 2);
}

// #1115: IComparable<int> as a LOCAL variable type — dispatch.
BEZEL_TEST(builtin_interface_value_1115, icomparable_as_local_dispatches) {
    BEZEL_EXPECT_EQ(run(
        std::string(CMP) +
        "class App public static method Main(): int "
        "  using var c := new C(5); var x: IComparable<int> := c; "
        "  return x.CompareTo(3); "                             // 2
        "end Main; end App;"), 2);
}

// #1115: IEquatable<int> as a value + Equals dispatch.
BEZEL_TEST(builtin_interface_value_1115, iequatable_as_value_dispatches) {
    BEZEL_EXPECT_EQ(run(
        std::string(EQ) +
        "class App public static method Main(): int "
        "  using var c := new C(5); var e: IEquatable<int> := c; "
        "  return (7 if e.Equals(5) else 0) + (1 if e.Equals(9) else 0); "  // 7
        "end Main; end App;"), 7);
}

// #1115: IComparable<int> as a RETURN type.
BEZEL_TEST(builtin_interface_value_1115, icomparable_as_return_type) {
    BEZEL_EXPECT_EQ(run(
        std::string(CMP) +
        "class App "
        "  private static method Make(): IComparable<int> => new C(9); "
        "  public static method Main(): int return Make().CompareTo(3); end Main; "  // 2
        "end App;"), 2);
}

// #1115: IComparable<int> as a FIELD type, read then dispatched.
BEZEL_TEST(builtin_interface_value_1115, icomparable_as_field_type) {
    BEZEL_EXPECT_EQ(run(
        std::string(CMP) +
        "class Box "
        "  public owned cmp: IComparable<int>; "
        "  public constructor() cmp := new C(9); end constructor; "
        "  public static method Main(): int "
        "    using var b := new Box(); return b.cmp.CompareTo(3); "             // 2
        "  end Main; end Box;"), 2);
}

// #1115 keep-green: the `where T: IComparable<T>` constraint form is unchanged.
BEZEL_TEST(builtin_interface_value_1115, where_icomparable_constraint_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System; "
        "record Money(cents: int): IComparable<Money> "
        "  public method CompareTo(other: Money): int => "
        "    (1 if cents > other.cents else (-1 if cents < other.cents else 0)); "
        "end Money; "
        "class App public static method Main(): int "
        "  var a := new Money(150); var b := new Money(99); "
        "  return Math.Min<Money>(a, b).cents; "                // 99 (#1141 path)
        "end Main; end App;"), 99);
}
