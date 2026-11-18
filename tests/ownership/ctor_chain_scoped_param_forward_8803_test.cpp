// Ownership tests for #8803 (§28.14, v0.359): a constructor that forwards its
// OWN `scoped` parameter through a constructor chain call — `base(f)` /
// `this(f, …)` — into the chain target's `scoped` formal is the admitted
// same-bounding-object forward.
//
// §28.14 (v0.359), callee side: "The callee may store the callback, but only
// in `scoped` fields of the bounding object itself (below), or forward it to
// another `scoped` parameter whose bounding object is the same object"; and for
// a class- or interface-typed `scoped` parameter (*scoped construction*): the
// callee "may forward it only to another `scoped` parameter with the same
// bounding object". A chain call's target constructs the object under
// construction, `this` — the same object.
//
// Before the fix `scopedCalleeWalkExpr` recognised a same-object forward only
// for `this.M(...)` or a bare instance-method name, never for a chain call, and
// rejected every chain spelling with "`f` is a `scoped` parameter — it may be
// forwarded only to another `scoped` parameter of a call on the same bounding
// object (`this`)". That is also the remedy #8778's call-bounded diagnostic
// names ("declare `f` itself `scoped` to store it"), so after #8778 no spelling
// could forward a callback through a chain into a `scoped` base formal.
//
// Each row is RED on the base (the program is rejected).

#include <string>

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

// Every error message, for a readable failure.
std::string errorsOf(const OwnershipResult& r) {
    std::string out;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error) out += d.message + "\n";
    return out;
}

}  // namespace

// `D(scoped f) base(f)` into `B(scoped f)`, constructed under a `using var`
// bound whose captured local is declared before it (§28.14's admitted caller).
BEZEL_TEST(ctor_chain_scoped_param_forward_8803, base_chain_forward_admitted) {
    auto r = parseAndCheckOwnership(
        "module T;\n"
        "public class B\n"
        "  private scoped _f: () -> int;\n"
        "  public constructor(scoped f: () -> int) _f := f; end;\n"
        "  public method Run(): int => _f();\n"
        "end;\n"
        "public class D : B\n"
        "  public constructor(scoped f: () -> int) base(f); end;\n"
        "end;\n"
        "public static class Program\n"
        "  public static method Main(): int\n"
        "    var s := \"hi\";\n"
        "    using var k := new D(lambda(): int => s.Length);\n"
        "    Console.Write(k.Run());\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n");
    BEZEL_EXPECT_EQ(errorsOf(r), std::string());
}

// The `this(f, 0)` twin into a sibling's `scoped` formal.
BEZEL_TEST(ctor_chain_scoped_param_forward_8803, this_chain_forward_admitted) {
    auto r = parseAndCheckOwnership(
        "module T;\n"
        "public class D\n"
        "  private scoped _f: () -> int;\n"
        "  public constructor(scoped f: () -> int, n: int) _f := f; end;\n"
        "  public constructor(scoped f: () -> int) this(f, 0); end;\n"
        "  public method Run(): int => _f();\n"
        "end;\n"
        "public static class Program\n"
        "  public static method Main(): int\n"
        "    var s := \"hi\";\n"
        "    using var k := new D(lambda(): int => s.Length);\n"
        "    Console.Write(k.Run());\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n");
    BEZEL_EXPECT_EQ(errorsOf(r), std::string());
}

// *Scoped construction*: a class-typed `scoped` borrow parameter forwarded by
// `base(s)` into the base's `scoped` borrow formal.
BEZEL_TEST(ctor_chain_scoped_param_forward_8803,
           base_chain_scoped_borrow_forward_admitted) {
    auto r = parseAndCheckOwnership(
        "module T;\n"
        "public class Box\n"
        "  public v: int;\n"
        "  public constructor(x: int) v := x; end;\n"
        "end;\n"
        "public class B\n"
        "  private src: Box;\n"
        "  public constructor(scoped s: Box) src := s; end;\n"
        "  public method Get(): int => src.v;\n"
        "end;\n"
        "public class D : B\n"
        "  public constructor(scoped s: Box) base(s); end;\n"
        "end;\n"
        "public static class Program\n"
        "  public static method Main(): int\n"
        "    using var bx := new Box(4);\n"
        "    using var k := new D(bx);\n"
        "    Console.Write(k.Get());\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n");
    BEZEL_EXPECT_EQ(errorsOf(r), std::string());
}
