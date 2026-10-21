// Regression tests for #6838: inside a member of an unsigned-backed enum,
// `int(this)` of a value above the signed midpoint.
//
// Spec (v0.340), §5.4 *Enum casts*: "For an enum `E` with underlying type `U`
// (§10.3) and any integer type `I` (§4.2), the cast `I(e)` converts `e` to `U`
// and then applies §5.2's integer conversion `I(u)`: a widening always
// succeeds". So `int(e)` of a `byte`-backed `e` holding 200 is the widening
// `int(byte 200)` = 200, never -56. Outside the enum `int(Small.C)` already
// printed 200.
//
// Sema records no type for `this` inside an enum member (CG30), so codegen
// read the receiver's signedness as "signed" and sign-extended 200 -> -56.
// The same gap made `this < other` / `this > other` compare signed.
//
// per §5.2, §5.4, §10.3 (v0.340)

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

// The issue's repro.
BEZEL_TEST(enum_receiver_unsigned_this_6838, byte_enum_int_this_spec_5_4) {
    BEZEL_EXPECT_EQ(
        out("module M; "
            "public enum Small: byte "
            "  A := 1, C := 200; "
            "  public method Raw(): int => int(this); "
            "  public method Raw2(): int "
            "    return int(this); "
            "  end Raw2; "
            "  public method Wide(): long => long(this); "
            "end Small; "
            "public class Program "
            "  public static method Main() "
            "    Console.WriteLine(Small.C.Raw()); "
            "    Console.WriteLine(Small.C.Raw2()); "
            "    Console.WriteLine(Small.C.Wide()); "
            "    Console.WriteLine(Small.A.Raw()); "
            "  end Main; "
            "end Program;"),
        std::string("200\n200\n200\n1\n"));
}

// Relational operators on `this` use the underlying type's signedness.
BEZEL_TEST(enum_receiver_unsigned_this_6838, relational_on_this_spec_10_3) {
    BEZEL_EXPECT_EQ(
        out("module M; "
            "public enum Small: byte "
            "  A := 1, C := 200; "
            "  public method Above(o: Small): bool => this > o; "
            "end Small; "
            "public class Program "
            "  public static method Main() "
            "    Console.WriteLine(Small.C.Above(Small.A)); "
            "    Console.WriteLine(Small.A.Above(Small.C)); "
            "  end Main; "
            "end Program;"),
        std::string("True\nFalse\n"));
}

// Async and iterator members of the same enum, and an unsigned wider enum
// (ushort, ulong) — each emitted in its own Resume/MoveNext function.
BEZEL_TEST(enum_receiver_unsigned_this_6838, async_and_iterator_spec_5_4) {
    BEZEL_EXPECT_EQ(
        out("module M; "
            "public enum Small: byte "
            "  A := 1, C := 200; "
            "  public async method As(): Task<int> "
            "    return int(this); "
            "  end As; "
            "  public method It(): IEnumerable<int> "
            "    yield return int(this); "
            "  end It; "
            "end Small; "
            "public enum Wide: ushort "
            "  P := 3, Q := 60000; "
            "  public method Raw(): int => int(this); "
            "end Wide; "
            "public enum Huge: ulong "
            "  U := 18000000000000000000, W := 1; "
            "  public async method As(): Task<ulong> "
            "    return ulong(this); "
            "  end As; "
            "end Huge; "
            "public class Program "
            "  public static async method Main(): Task "
            "    Console.WriteLine(await Small.C.As()); "
            "    for var x in Small.C.It() do Console.WriteLine(x); end; "
            "    Console.WriteLine(Wide.Q.Raw()); "
            "    Console.WriteLine(await Huge.U.As()); "
            "  end Main; "
            "end Program;"),
        std::string("200\n200\n60000\n18000000000000000000\n"));
}

// The lambda path (fixed by #6768) stays correct: 200 looped.
BEZEL_TEST(enum_receiver_unsigned_this_6838, lambda_path_looped_spec_5_4) {
    BEZEL_EXPECT_EQ(
        out("module M; "
            "public enum Small: byte "
            "  A := 1, C := 200; "
            "  public method Sum(): int "
            "    var f := lambda(): int => int(this); "
            "    var s := 0; "
            "    for var i in 0..199 do s := s + f(); end; "
            "    return s; "
            "  end Sum; "
            "end Small; "
            "public class Program "
            "  public static method Main() "
            "    Console.WriteLine(Small.C.Sum()); "
            "  end Main; "
            "end Program;"),
        std::string("40000\n"));
}
