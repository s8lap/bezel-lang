// Regression tests for #6836: a bound method reference over an enum receiver.
//
// Spec (v0.340), §9.4 *The receiver value is fixed at the call*: "A **closure**
// formed in the body — a lambda that names `this`, ... or a bound method
// reference over it (§27.6) — captures the fixed receiver value **by copy**,
// taken at the closure's formation into its heap capture cell (§27.5), as a
// bound reference captures a value-shaped receiver". A record, enum or
// primitive receiver is such a value-kind receiver.
//
// An enum receiver is the bare scalar of the underlying type (CG30). The
// reference's thunk loaded the captured receiver from the env as a `ptr`, so
// `this.Idx` and `Color.Blue.Idx` failed the IR verifier ("Call parameter type
// does not match function signature", `ptr` where `i32` is wanted).
//
// per §9.4, §27.6, §10.3 (v0.340)

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
}  // namespace

// The issue's own repro: `this.Idx` returned as a `() -> int`, and the
// `Color.Blue.Idx` receiver-qualified form.
BEZEL_TEST(enum_receiver_method_ref_6836, this_and_qualified_ref_spec_9_4) {
    BEZEL_EXPECT_EQ(
        out("module M; "
            "public enum Color "
            "  Red, Green, Blue; "
            "  public method Idx(): int => int(this); "
            "  public method Ref(): () -> int "
            "    return this.Idx; "
            "  end Ref; "
            "end Color; "
            "public class Program "
            "  public static method Main() "
            "    var r := Color.Blue.Ref(); "
            "    Console.WriteLine(r()); "
            "    var q: () -> int := Color.Green.Idx; "
            "    Console.WriteLine(q()); "
            "  end Main; "
            "end Program;"),
        std::string("2\n1\n"));
}

// The value is fixed at formation (a copy), so the reference outlives the
// frame that formed it. Called 200x after a frame clobber.
BEZEL_TEST(enum_receiver_method_ref_6836, escaping_ref_reads_copy_spec_9_4) {
    BEZEL_EXPECT_EQ(
        out("module M; "
            "public enum Color "
            "  Red, Green, Blue; "
            "  public method Idx(): int => int(this); "
            "  public method Ref(): () -> int "
            "    return this.Idx; "
            "  end Ref; "
            "end Color; "
            "public class Program "
            "  private static method Make(): () -> int "
            "    var c := Color.Blue; "
            "    return c.Ref(); "
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
        std::string("8\n400\n"));
}

// Underlying types other than int, and a method with a parameter: the env
// field is the enum's own scalar, the thunk forwards the target's argument.
BEZEL_TEST(enum_receiver_method_ref_6836, other_underlying_types_spec_9_4) {
    BEZEL_EXPECT_EQ(
        out("module M; "
            "public enum Small: byte "
            "  A := 1, C := 100; "
            "  public method Raw(): int => int(this); "
            "  public method Add(n: int): int => int(this) + n; "
            "end Small; "
            "public enum Big: long "
            "  Lo := 5, Hi := 5000000000; "
            "  public method Get(): long => long(this); "
            "end Big; "
            "public class Program "
            "  public static method Main() "
            "    var r: () -> int := Small.C.Raw; "
            "    Console.WriteLine(r()); "
            "    var a: (int) -> int := Small.C.Add; "
            "    Console.WriteLine(a(5)); "
            "    var g: () -> long := Big.Hi.Get; "
            "    Console.WriteLine(g()); "
            "  end Main; "
            "end Program;"),
        std::string("100\n105\n5000000000\n"));
}

// A primitive extension receiver is a bare scalar too.
BEZEL_TEST(enum_receiver_method_ref_6836, primitive_extension_ref_spec_9_4) {
    BEZEL_EXPECT_EQ(
        out("module M; "
            "public extension int "
            "  public method Twice(): int => this * 2; "
            "end; "
            "public extension long "
            "  public method Plus1(): long => this + 1; "
            "end; "
            "public class Program "
            "  public static method Main() "
            "    var n: int := -7; "
            "    var f: () -> int := n.Twice; "
            "    Console.WriteLine(f()); "
            "    var m: long := 5000000000; "
            "    var g: () -> long := m.Plus1; "
            "    Console.WriteLine(g()); "
            "  end Main; "
            "end Program;"),
        std::string("-14\n5000000001\n"));
}
