// Regression tests for #6768: `this` in a lambda formed in an enum member body.
//
// Spec (v0.340), §9.4 *The receiver value is fixed at the call*:
//   "A member call on a copyable value-kind receiver — a record, enum or
//    primitive receiver, ... runs over the value its receiver expression
//    denoted when that expression was evaluated."
//   "A closure formed in the body — a lambda that names `this`, ... —
//    captures the fixed receiver value by copy, taken at the closure's
//    formation into its heap capture cell (§27.5)"
//   "nothing where the type holds no borrows (§3.6), so such a closure is
//    capture-free and goes wherever a capture-free function value goes —
//    returned, stored, passed"
//
// An enum receiver is the bare scalar. Before the fix the lambda's `this`
// capture typed its env field `ptr` and stored/restored the scalar through it,
// so any use of `this` in the lambda failed the IR verifier ("sext ptr to
// i32", "icmp eq ptr, i32"). The capture now stores the scalar by value.
//
// per §9.4, §27.5, §10.4 (v0.340)

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
std::string out(const std::string& program) {
    auto r = runAotChecked(program);
    return (r.ran && r.exitCode == 0) ? r.stdoutText
                                      : std::string("<did-not-run>");
}
const char* kColor =
    "module M; "
    "public enum Color "
    "  Red, Green, Blue; "
    "  public method IsRed(): int "
    "    var f := lambda(): bool => this = Color.Red; "
    "    if f() then return 1; end; "
    "    return 0; "
    "  end IsRed; "
    "  public method Idx(): int "
    "    var f := lambda(): int => int(this); "
    "    return f(); "
    "  end Idx; "
    "  public method Name(): string "
    "    var f := lambda(): string => "
    "      (case this of Color.Red => \"r\", Color.Green => \"g\", "
    "       Color.Blue => \"b\"); "
    "    return f(); "
    "  end Name; "
    "  public method Maker(): () -> int "
    "    return lambda(): int => int(this) * 10; "
    "  end Maker; "
    "  public method Nested(): int "
    "    var f := lambda(): int => do "
    "      var g := lambda(): int => int(this) + 100; "
    "      return g(); "
    "    end; "
    "    return f(); "
    "  end Nested; "
    "end Color; ";
}  // namespace

BEZEL_TEST(enum_receiver_this_in_lambda_6768,
           this_equality_and_conversion_spec_9_4) {
    BEZEL_EXPECT_EQ(
        out(std::string(kColor) +
            "public class Program "
            "  public static method Main() "
            "    Console.WriteLine(Color.Red.IsRed()); "
            "    Console.WriteLine(Color.Blue.IsRed()); "
            "    Console.WriteLine(Color.Blue.Idx()); "
            "  end Main; "
            "end Program;"),
        std::string("1\n0\n2\n"));
}

BEZEL_TEST(enum_receiver_this_in_lambda_6768, case_this_in_lambda_spec_9_4) {
    BEZEL_EXPECT_EQ(
        out(std::string(kColor) +
            "public class Program "
            "  public static method Main() "
            "    Console.WriteLine(Color.Red.Name()); "
            "    Console.WriteLine(Color.Green.Name()); "
            "    Console.WriteLine(Color.Blue.Name()); "
            "  end Main; "
            "end Program;"),
        std::string("r\ng\nb\n"));
}

BEZEL_TEST(enum_receiver_this_in_lambda_6768, nested_lambda_spec_9_4) {
    BEZEL_EXPECT_EQ(
        out(std::string(kColor) +
            "public class Program "
            "  public static method Main() "
            "    Console.WriteLine(Color.Blue.Nested()); "
            "  end Main; "
            "end Program;"),
        std::string("102\n"));
}

// The closure escapes the member; the enum is copyable, so §9.4 admits it and
// it reads the receiver value fixed at formation. Called 200x.
BEZEL_TEST(enum_receiver_this_in_lambda_6768,
           escaping_closure_reads_copy_spec_9_4) {
    BEZEL_EXPECT_EQ(
        out(std::string(kColor) +
            "public class Program "
            "  private static method Make(): () -> int "
            "    var c := Color.Blue; "
            "    return c.Maker(); "
            "  end Make; "
            "  private static method Clobber(a: int, b: int, c: int): int "
            "    var p := a * 3 + b; "
            "    return p + c; "
            "  end Clobber; "
            "  public static method Main() "
            "    var f := Make(); "
            "    Console.WriteLine(Clobber(1, 2, 3)); "
            "    var s := 0; "
            "    for var i in 0..199 do s := s + f(); end; "
            "    Console.WriteLine(s); "
            "  end Main; "
            "end Program;"),
        std::string("8\n4000\n"));
}

// Explicit underlying type with explicit values (flags-shaped): the scalar is
// a byte / long, not an i32.
BEZEL_TEST(enum_receiver_this_in_lambda_6768,
           explicit_underlying_values_spec_9_4) {
    BEZEL_EXPECT_EQ(
        out("module M; "
            "public enum Flag: byte "
            "  A := 1, B := 2, C := 4; "
            "  public method Mk(): () -> int "
            "    return lambda(): int => int(this) + 1000; "
            "  end Mk; "
            "  public method Has(): int "
            "    var f := lambda(): bool => this = Flag.C; "
            "    if f() then return 1; end; "
            "    return 0; "
            "  end Has; "
            "end Flag; "
            "public enum Big: long "
            "  Lo := 5, Hi := 5000000000; "
            "  public method Get(): () -> long "
            "    return lambda(): long => long(this); "
            "  end Get; "
            "end Big; "
            "public class Program "
            "  public static method Main() "
            "    Console.WriteLine(Flag.C.Mk()()); "
            "    Console.WriteLine(Flag.C.Has()); "
            "    Console.WriteLine(Flag.A.Has()); "
            "    Console.WriteLine(Big.Hi.Get()()); "
            "  end Main; "
            "end Program;"),
        std::string("1004\n1\n0\n5000000000\n"));
}
