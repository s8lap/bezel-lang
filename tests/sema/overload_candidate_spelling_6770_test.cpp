// fix #6770 (§22.6, v0.340): "The compiler reports the error and cites the
// candidate overloads; it never silently selects one." A candidate is cited by
// its parameter types as spelled. The ambiguity diagnostic printed only the
// last segment of a named type, so two instantiations of one generic head
// (`F(IReadOnlyList<Animal>)` / `F(IReadOnlyList<Dog>)`) read as two identical
// lines, and `Pick(long)` / `Pick(long?)` read the same. Generic arguments and
// the `?` marker are now part of the citation.
//
// Probes: /var/tmp/bezel-scratch/r34-diag/p/t2_*.bzl.

#include <string>

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

std::string ambiguityMessage(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("ambiguous call to") != std::string::npos)
            return d.message;
    return "";
}

bool has(const std::string& s, const std::string& needle) {
    return s.find(needle) != std::string::npos;
}

}  // namespace

// §22.6: the issue's generic-argument repro. `Array<Dog>` converts to either
// instantiation, neither exact (§26.5, §12.9).
BEZEL_TEST(overload_candidate_spelling_6770,
           generic_arguments_are_cited_spec_22_6) {
    auto r = runAot(R"(
module M;
class Animal public constructor() end; end Animal;
class Dog: Animal public constructor() end; end Dog;
static class S
  public static method F(x: IReadOnlyList<Animal>): int => 1;
  public static method F(x: IReadOnlyList<Dog>): int => 2;
end S;
class P public static method Main(): int
  using var v0 := new Array<Dog>();
  Console.WriteLine(S.F(v0));
  return 0; end Main; end P;
)");
    const std::string m = ambiguityMessage(r);
    BEZEL_EXPECT_TRUE(has(m, "\n  F(IReadOnlyList<Animal>)"));
    BEZEL_EXPECT_TRUE(has(m, "\n  F(IReadOnlyList<Dog>)"));
    BEZEL_EXPECT_FALSE(has(m, "F(IReadOnlyList)"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §22.6: the issue's nullable repro. `3_000_000_000` converts to `long` and
// to `long?` and fits `int` in neither.
BEZEL_TEST(overload_candidate_spelling_6770,
           nullable_marker_is_cited_spec_22_6) {
    auto r = runAot(R"(
module M;
public class C
  public static method Pick(x: long): int => 1;
  public static method Pick(x: long?): int => 2;
  public static method Main(): int
    Console.WriteLine($"{C.Pick(3_000_000_000)}");
    return 0;
  end Main;
end C;
)");
    const std::string m = ambiguityMessage(r);
    BEZEL_EXPECT_TRUE(has(m, "\n  Pick(long)\n"));
    BEZEL_EXPECT_TRUE(has(m, "\n  Pick(long?)\n"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// The constructor twin renders through the same function.
BEZEL_TEST(overload_candidate_spelling_6770,
           constructor_candidates_cite_nullable_spec_22_6) {
    auto r = runAot(R"(
module M;
public class C
  public constructor(x: long) end constructor;
  public constructor(x: long?) end constructor;
  public static method Main(): int
    var c := new C(3_000_000_000);
    return 0;
  end Main;
end C;
)");
    const std::string m = ambiguityMessage(r);
    BEZEL_EXPECT_TRUE(has(m, "\n  new C(long)\n"));
    BEZEL_EXPECT_TRUE(has(m, "\n  new C(long?)\n"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// A later parameter is spelled too, and a plain type keeps its old spelling.
BEZEL_TEST(overload_candidate_spelling_6770,
           multi_parameter_candidates_cite_each_type_spec_22_6) {
    auto r = runAot(R"(
module M;
public class C
  public static method F(x: long, y: int): int => 1;
  public static method F(x: long?, y: int): int => 2;
  public static method Main(): int
    Console.WriteLine($"{C.F(3_000_000_000, 1)}");
    return 0;
  end Main;
end C;
)");
    const std::string m = ambiguityMessage(r);
    BEZEL_EXPECT_TRUE(has(m, "\n  F(long, int)\n"));
    BEZEL_EXPECT_TRUE(has(m, "\n  F(long?, int)\n"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// Control: an unambiguous call still compiles and selects as before.
BEZEL_TEST(overload_candidate_spelling_6770,
           unambiguous_overloads_unaffected_spec_22_6) {
    auto r = runAot(R"(
module M;
public class C
  public static method F(x: long): int => 1;
  public static method F(x: string): int => 2;
  public static method Main(): int
    Console.WriteLine($"{C.F(3)} {C.F("a")}");
    return 0;
  end Main;
end C;
)");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1 2\n"));
}
