// Ownership tests for #8778 (§28.14, v0.359): a constructor CHAIN call —
// `base(f)` / `this(f, …)` — that forwards a call-bounded (plain) function-
// typed parameter into a `scoped` formal must be rejected exactly as the
// `new B(f)` spelling of the same forward is.
//
// §28.14 (v0.359), *The call-bounded default*: "A function-typed parameter
// declared without `scoped` is call-bounded … Within the callee the callback
// may be invoked, bound to locals, and passed on as an argument to another
// call-bounded parameter" — a `scoped` formal is not one, and the target's
// `scoped` field then stores the callback in the constructed object, which
// outlives the call.
//
// Before the fix, `scopedFormalAt` resolved a chain call's callee (the bare
// identifier `this`/`base`) to no symbol and answered "no `scoped` formal",
// so `checkScopedAdmission` never ran for a chain argument. Measured on the
// base under `--sanitize=address`: the issue's repro (`Wrap(f) => new D(f)`
// with `D(f) base(f)`), the direct `new D(lambda …)` spelling and the
// `this(f, 0)` twin are each a heap-use-after-free (the caller's capture cell
// is reclaimed after the call-bounded call, then read through the stored
// callback).
//
// Each row below is RED on the base (the program is accepted) and asserts the
// specific §28.14 diagnostic at the forwarded argument's location.

#include <cstdint>
#include <string>

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

// The diagnostic the `new B(f)` spelling already carries (checkScopedAdmission).
const char* const kForward =
    "parameter `f` is call-bounded and may not be forwarded into a `scoped` "
    "parameter position";

const bezel::Diagnostic* firstErrorWith(const OwnershipResult& r,
                                        const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return &d;
    return nullptr;
}

// A base class whose constructor stores its `scoped` callback (§28.14's own
// example shape), on lines 2-6 of every program below.
const char* const kScopedBase =
    "module T;\n"                                                    // 1
    "public class B\n"                                               // 2
    "  private scoped _f: () -> int;\n"                              // 3
    "  public constructor(scoped f: () -> int) _f := f; end;\n"      // 4
    "  public method Run(): int => _f();\n"                          // 5
    "end;\n";                                                        // 6

void expectAt(const OwnershipResult& r, const std::string& needle,
              std::uint32_t line, std::uint32_t column) {
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    const bezel::Diagnostic* d = firstErrorWith(r, needle);
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (!d) return;
    BEZEL_EXPECT_TRUE(d->message.find("§28.14") != std::string::npos);
    BEZEL_EXPECT_EQ(d->location.line, line);
    BEZEL_EXPECT_EQ(d->location.column, column);
}

}  // namespace

// The issue's own repro (#8778): `D(f) base(f)` into `B(scoped f)`, reached
// through a method that forwards its own call-bounded parameter to `new D(f)`
// (plain → plain, admitted) — the chain is the store.
BEZEL_TEST(ctor_chain_scoped_forward_8778, issue_repro_base_chain_rejected) {
    const std::string src =
        std::string(kScopedBase) +
        "public class D : B\n"                                       // 7
        "  public constructor(f: () -> int) base(f); end;\n"         // 8
        "end;\n"                                                     // 9
        "public static class Program\n"
        "  public static method Wrap(f: () -> int): owned D => new D(f);\n"
        "  public static method Make(n: int): owned D\n"
        "    var s := \"hi\" + n.ToString();\n"
        "    return Wrap(lambda(): int => s.Length);\n"
        "  end;\n"
        "  public static method Main(): int\n"
        "    using var d := Make(1);\n"
        "    Console.Write(d.Run());\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n";
    expectAt(parseAndCheckOwnership(src), kForward, 8, 41);
}

// The direct spelling the review found turned from a leak into a
// heap-use-after-free on staging (#6628): `new D(lambda …)` returned from the
// frame that owns the captured local, `D(f) base(f)`.
BEZEL_TEST(ctor_chain_scoped_forward_8778, base_chain_direct_new_rejected) {
    const std::string src =
        std::string(kScopedBase) +
        "public class D : B\n"                                       // 7
        "  public constructor(f: () -> int) base(f); end;\n"         // 8
        "end;\n"
        "public static class Program\n"
        "  public static method Make(n: int): owned D\n"
        "    var s := \"hi\" + n.ToString();\n"
        "    return new D(lambda(): int => s.Length);\n"
        "  end;\n"
        "  public static method Main(): int\n"
        "    using var d := Make(1);\n"
        "    Console.Write(d.Run());\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n";
    expectAt(parseAndCheckOwnership(src), kForward, 8, 41);
}

// The `this(f, 0)` twin: a convenience constructor chaining its call-bounded
// parameter into a sibling's `scoped` formal.
BEZEL_TEST(ctor_chain_scoped_forward_8778, this_chain_rejected) {
    const std::string src =
        "module T;\n"                                                // 1
        "public class D\n"                                           // 2
        "  private scoped _f: () -> int;\n"                          // 3
        "  public constructor(scoped f: () -> int, n: int) _f := f; end;\n"
        "  public constructor(f: () -> int) this(f, 0); end;\n"      // 5
        "  public method Run(): int => _f();\n"
        "end;\n"
        "public static class Program\n"
        "  public static method Make(n: int): owned D\n"
        "    var s := \"hi\" + n.ToString();\n"
        "    return new D(lambda(): int => s.Length);\n"
        "  end;\n"
        "  public static method Main(): int\n"
        "    using var d := Make(1);\n"
        "    Console.Write(d.Run());\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n";
    expectAt(parseAndCheckOwnership(src), kForward, 5, 41);
}

// The scoped formal is the chain target's SECOND parameter: the check reads
// the bound formal, not argument position 0.
BEZEL_TEST(ctor_chain_scoped_forward_8778, base_chain_second_position_rejected) {
    const std::string src =
        "module T;\n"                                                // 1
        "public class B\n"                                           // 2
        "  private scoped _f: () -> int;\n"                          // 3
        "  public n: int;\n"                                         // 4
        "  public constructor(n: int, scoped f: () -> int) _f := f; this.n := n; end;\n"
        "  public method Run(): int => _f() + n;\n"                  // 6
        "end;\n"                                                     // 7
        "public class D : B\n"                                       // 8
        "  public constructor(f: () -> int) base(1, f); end;\n"      // 9
        "end;\n"
        "public static class Program\n"
        "  public static method Main(): int\n"
        "    var s := \"hi\";\n"
        "    using var k := new D(lambda(): int => s.Length);\n"
        "    Console.Write(k.Run());\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n";
    expectAt(parseAndCheckOwnership(src), kForward, 9, 44);
}

// A NAMED chain argument binds through §22.5 to the `scoped` formal `f`.
BEZEL_TEST(ctor_chain_scoped_forward_8778, base_chain_named_argument_rejected) {
    const std::string src =
        std::string(kScopedBase) +
        "public class D : B\n"                                       // 7
        "  public constructor(g: () -> int) base(f := g); end;\n"    // 8
        "end;\n"
        "public static class Program\n"
        "  public static method Main(): int\n"
        "    var s := \"hi\";\n"
        "    using var k := new D(lambda(): int => s.Length);\n"
        "    Console.Write(k.Run());\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n";
    expectAt(parseAndCheckOwnership(src),
             "parameter `g` is call-bounded and may not be forwarded into a "
             "`scoped` parameter position",
             8, 46);
}

// §28.14 "captured by a closure that itself escapes the callee (judged by
// these same rules, applied transitively)": wrapping the call-bounded
// parameter in a lambda does not launder it into the `scoped` chain formal.
BEZEL_TEST(ctor_chain_scoped_forward_8778,
           base_chain_lambda_capturing_call_bounded_rejected) {
    const std::string src =
        std::string(kScopedBase) +
        "public class D : B\n"                                       // 7
        "  public constructor(f: () -> int) base(lambda(): int => f()); end;\n"
        "end;\n"
        "public static class Program\n"
        "  public static method Main(): int\n"
        "    var s := \"hi\";\n"
        "    using var k := new D(lambda(): int => s.Length);\n"
        "    Console.Write(k.Run());\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n";
    expectAt(parseAndCheckOwnership(src),
             "closure bound to a `scoped` parameter captures `f`, a "
             "call-bounded function-typed parameter",
             8, 58);
}

// Form 1 for a chain call: the constructed object is `this`, so a closure
// capturing a borrow PARAMETER of the constructor is a captured source that
// is not `this` — the stored callback outlives the constructor's frame.
BEZEL_TEST(ctor_chain_scoped_forward_8778,
           base_chain_lambda_capturing_borrow_param_rejected) {
    const std::string src =
        "module T;\n"                                                // 1
        "public class Box\n"                                         // 2
        "  public v: int;\n"                                         // 3
        "  public constructor(x: int) v := x; end;\n"                // 4
        "end;\n"                                                     // 5
        "public class B\n"                                           // 6
        "  private scoped _f: () -> int;\n"                          // 7
        "  public constructor(scoped f: () -> int) _f := f; end;\n"  // 8
        "  public method Run(): int => _f();\n"                      // 9
        "end;\n"                                                     // 10
        "public class D : B\n"                                       // 11
        "  public constructor(b: Box) base(lambda(): int => b.v); end;\n"
        "end;\n"
        "public static class Program\n"
        "  public static method Main(): int\n"
        "    using var bx := new Box(4);\n"
        "    using var k := new D(bx);\n"
        "    Console.Write(k.Run());\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n";
    expectAt(parseAndCheckOwnership(src), "captured source `b` is not `this`",
             12, 52);
}
