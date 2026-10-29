// Codegen/ownership -- `for var x in <expr>!` where `<expr>` is an `owned
// IEnumerable<int>?` field or a nullable local (issue #7526, spec v0.350).
//
// Emitter::classifyBuiltinForInSource keyed its array-vs-vtable protocol choice
// on the iterable's AST shape (CallExpr / NewExpr / IdentifierExpr /
// MemberAccessExpr / IndexExpr). A postfix `!` wrapper (PostfixBangExpr) matched
// none, so `h.F!` fell to the raw bz_array protocol, read the iterator frame as
// an array, and looped ZERO times (neither the body nor the generator's
// `finally` ran). `!` only asserts non-null and yields the operand's value, so
// the source is classified by looking through it.
//
// Spec: sec 29.2 (a `for` over a named/owned source BORROWS it; the loop does not
// assume its disposal obligation), sec 29.7 (the loop's own enumerator is
// disposed on every exit, running the iterator's `finally`), sec 28.5 (the owned
// field is disposed once by the holder's cascade), sec 16.5 (`!` null assertion).
// Spec revision v0.350.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

int run(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

// The issue's repro shape, looped 50x: body runs 2x per pass (sum 3), the
// generator's finally runs once per pass, and the field frame is disposed once
// by the holder's cascade (ASan-clean).
BEZEL_TEST(codegen_forin_bang_source_7526, owned_nullable_field_bang_loops_50x) {
    std::string src =
        "module M;\n"
        "public class H\n"
        "  public owned F: IEnumerable<int>? := null;\n"
        "  public static fins: int := 0;\n"
        "  public constructor() end constructor;\n"
        "  public method GenS(): IEnumerable<int>\n"
        "    try yield return 1; yield return 2;\n"
        "    finally H.fins := H.fins + 1; end;\n"
        "  end GenS;\n"
        "end H;\n"
        "public class C\n"
        "  public static method Main(): int\n"
        "    var sum := 0; var i := 0;\n"
        "    while i < 50 do\n"
        "      using var h := new H();\n"
        "      h.F := h.GenS();\n"
        "      for var x in h.F! do sum := sum + x; end;\n"
        "      i := i + 1;\n"
        "    end;\n"
        "    if sum = 150 and H.fins = 50 then return 7; end;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
    BEZEL_EXPECT_EQ(run(src), 7);
}

// Same shape through a nullable LOCAL: `e!`.
BEZEL_TEST(codegen_forin_bang_source_7526, nullable_local_bang) {
    std::string src =
        "module M;\n"
        "public class H\n"
        "  public constructor() end constructor;\n"
        "  public method GenS(): IEnumerable<int>\n"
        "    yield return 1; yield return 2;\n"
        "  end GenS;\n"
        "end H;\n"
        "public class C\n"
        "  public static method Main(): int\n"
        "    var sum := 0; var i := 0;\n"
        "    while i < 50 do\n"
        "      using var h := new H();\n"
        "      var e: IEnumerable<int>? := h.GenS();\n"
        "      for var x in e! do sum := sum + x; end;\n"
        "      i := i + 1;\n"
        "    end;\n"
        "    if sum = 150 then return 7; end;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
    BEZEL_EXPECT_EQ(run(src), 7);
}

}  // namespace
