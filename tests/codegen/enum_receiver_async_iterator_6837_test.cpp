// Regression tests for #6837: an async or iterator member declared on an enum
// (or over a primitive extension receiver).
//
// Spec (v0.340), §9.4 *The receiver value is fixed at the call*: "A member
// whose activation outlives the call — an `async` member (§30.1) or an
// iterator member (§29.3) — holds the fixed receiver value in its frame, as
// the frame holds a plain value argument (§29.4)". A record, enum or primitive
// receiver is such a value-kind receiver.
//
// An enum receiver is the bare scalar of the underlying type (CG30). The frame
// typed its `this` field `ptr`, restored it as a `ptr` and registered it as a
// §30.1 borrow, so every such member failed the IR verifier ("sext ptr to i32",
// "Call parameter type does not match function signature" at
// bz_register_borrow(i32)).
//
// per §9.4, §30.1 (v0.340)

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

BEZEL_TEST(enum_receiver_async_iterator_6837, async_member_spec_9_4) {
    BEZEL_EXPECT_EQ(
        out("module M; "
            "public enum Color "
            "  Red, Green, Blue; "
            "  public async method As(): Task<int> "
            "    return int(this) * 3; "
            "  end As; "
            "end Color; "
            "public class Program "
            "  public static async method Main(): Task "
            "    Console.WriteLine(await Color.Blue.As()); "
            "    Console.WriteLine(await Color.Green.As()); "
            "  end Main; "
            "end Program;"),
        std::string("6\n3\n"));
}

BEZEL_TEST(enum_receiver_async_iterator_6837, iterator_member_spec_9_4) {
    BEZEL_EXPECT_EQ(
        out("module M; "
            "public enum Color "
            "  Red, Green, Blue; "
            "  public method IterL(): IEnumerable<int> "
            "    yield return int(this) * 2; "
            "    yield return int(this) * 2 + 1; "
            "  end IterL; "
            "end Color; "
            "public class Program "
            "  public static async method Main(): Task "
            "    for var x in Color.Blue.IterL() do "
            "      Console.WriteLine(x); "
            "    end; "
            "  end Main; "
            "end Program;"),
        std::string("4\n5\n"));
}

// The receiver is held across a suspension: the member awaits, then reads
// `this` after resumption (the frame field, not the entry's register). Run
// 200x so a bad restore would show.
BEZEL_TEST(enum_receiver_async_iterator_6837, receiver_held_across_await_spec_9_4) {
    BEZEL_EXPECT_EQ(
        out("module M; "
            "public enum Color "
            "  Red, Green, Blue; "
            "  public async method After(): Task<int> "
            "    var k := await Task.FromResult(10); "
            "    return k + int(this); "
            "  end After; "
            "end Color; "
            "public class Program "
            "  public static async method Main(): Task "
            "    var s := 0; "
            "    for var i in 0..199 do "
            "      s := s + await Color.Blue.After(); "
            "    end; "
            "    Console.WriteLine(s); "
            "  end Main; "
            "end Program;"),
        std::string("2400\n"));
}

// Underlying types other than int: the frame field is the enum's own scalar.
BEZEL_TEST(enum_receiver_async_iterator_6837, other_underlying_types_spec_9_4) {
    BEZEL_EXPECT_EQ(
        out("module M; "
            "public enum Big: long "
            "  Lo := 5, Hi := 5000000000; "
            "  public async method As(): Task<long> "
            "    return long(this); "
            "  end As; "
            "  public method It(): IEnumerable<long> "
            "    yield return long(this); "
            "  end It; "
            "end Big; "
            "public class Program "
            "  public static async method Main(): Task "
            "    Console.WriteLine(await Big.Hi.As()); "
            "    for var x in Big.Hi.It() do Console.WriteLine(x); end; "
            "  end Main; "
            "end Program;"),
        std::string("5000000000\n5000000000\n"));
}

// A primitive extension receiver is a bare scalar too.
BEZEL_TEST(enum_receiver_async_iterator_6837, primitive_extension_spec_9_4) {
    BEZEL_EXPECT_EQ(
        out("module M; "
            "public extension int "
            "  public async method As(): Task<int> "
            "    return this * 2; "
            "  end As; "
            "  public method It(): IEnumerable<int> "
            "    yield return this + 1; "
            "  end It; "
            "end; "
            "public extension bool "
            "  public async method Flip(): Task<bool> "
            "    return not this; "
            "  end Flip; "
            "end; "
            "public class Program "
            "  public static async method Main(): Task "
            "    var n: int := -7; "
            "    Console.WriteLine(await n.As()); "
            "    for var x in n.It() do Console.WriteLine(x); end; "
            "    var b: bool := true; "
            "    Console.WriteLine(await b.Flip()); "
            "  end Main; "
            "end Program;"),
        std::string("-14\n-6\nFalse\n"));
}
