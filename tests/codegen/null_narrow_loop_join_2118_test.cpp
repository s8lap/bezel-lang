// Codegen AOT — §7.4 loop-body narrowing join (issue #2118).
//
// §7.4 "How narrowing is established / invalidated" (v0.220): narrowing is
// flow-sensitive. A loop body may execute ZERO times, so a narrowing
// established INSIDE a `while`/`for` body must not leak past the loop (that
// unsound-acceptance edge is pinned in the lit reject track
// tests/lit/sema/sema_null_narrow__loop_body_narrowing_does_not_leak_2118.bzl).
// The complementary ACCEPT direction — which the fix must NOT break — is:
//
//   (1) a narrowing established BEFORE the loop that the body never invalidates
//       still holds after the loop (present on both the zero-iteration and the
//       >=1-iteration path), and
//   (2) a narrowing established inside the body is usable for the remainder of
//       that same body iteration.
//
// These programs must COMPILE (sema accepts the narrowed access) and produce
// the spec-stated value at runtime. They pass both before and after the fix and
// guard against the join over-restricting (wrongly dropping a surviving fact).

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// §7.4 (#2118) accept (1), `while`: `s` is narrowed by the outer `if s <> null`
// guard; the `while` body only mutates `total`/`i`, never `s`, so the narrowing
// survives the loop and `s.Length` reads without a further guard. With s="abc"
// (Length 3) and n=3 the body runs 3 times (total 3): 3 + 3 == 6.
BEZEL_TEST(codegen_loop_narrow_join_2118, pre_loop_narrowing_survives_while_spec_7_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method PreLoopSurvivesWhile(s: string?, n: int): int "
        "    var total: int := 0; "
        "    if s <> null then "
        "      var i: int := 0; "
        "      while i < n do total := total + 1; i := i + 1; end; "
        "      return s.Length + total; "
        "    end; "
        "    return -1; "
        "  end PreLoopSurvivesWhile; "
        "  public static method Main(): int "
        "    return C.PreLoopSurvivesWhile(\"abc\", 3); "
        "  end Main; end C;"), 6);
}

// §7.4 (#2118) accept (1), `for`: same, over an inclusive range `0..3` (4
// iterations). `s` narrowed before the loop, body touches only `total`, so the
// narrowing survives. s="abcd" (Length 4), total 4: 4 + 4 == 8.
BEZEL_TEST(codegen_loop_narrow_join_2118, pre_loop_narrowing_survives_for_spec_7_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method PreLoopSurvivesFor(s: string?): int "
        "    var total: int := 0; "
        "    if s <> null then "
        "      for var i in 0..3 do total := total + 1; end; "
        "      return s.Length + total; "
        "    end; "
        "    return -1; "
        "  end PreLoopSurvivesFor; "
        "  public static method Main(): int "
        "    return C.PreLoopSurvivesFor(\"abcd\"); "
        "  end Main; end C;"), 8);
}

// §7.4 (#2118) accept (2), `for` in-body use: a narrowing established inside the
// body (`s := "xy"`) is usable for the rest of that iteration — the after-loop
// join does not affect in-body flow. `0..2` inclusive runs 3 times; each adds
// s.Length (2): 3 * 2 == 6. (The `while` in-body counterpart is pinned in
// tests/codegen/null_narrow_coalesce_assign_2104_test.cpp.)
BEZEL_TEST(codegen_loop_narrow_join_2118, for_body_narrowing_usable_within_body_spec_7_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method InFor(): int "
        "    var s: string? := null; "
        "    var total: int := 0; "
        "    for var i in 0..2 do "
        "      s := \"xy\"; "
        "      total := total + s.Length; "
        "    end; "
        "    return total; "
        "  end InFor; "
        "  public static method Main(): int "
        "    return C.InFor(); "
        "  end Main; end C;"), 6);
}
