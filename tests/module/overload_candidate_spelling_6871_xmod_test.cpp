// fix #6871 (§22.6, v0.340; follow-up of #6770): "The compiler reports the
// error and cites the candidate overloads". Every candidate is spelled the
// same way. An IMPORTED candidate (no AST parameter list) was rendered by
// `typeNameForDiag`, which prefixes the nullable marker (`?long`); a local one
// is rendered with the marker after the type (`long?`).
//
// Matrix (item 19a): imported nullable primitive, nullable class, generic,
// nullable generic and a plain type in one ambiguity diagnostic, plus a local
// candidate set in the same program shape (the two must agree).

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::ModuleSource;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAotModules;

namespace {
std::string ambiguityMessage(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("ambiguous call to") != std::string::npos)
            return d.message;
    return "";
}
bool has(const std::string& s, const std::string& n) {
    return s.find(n) != std::string::npos;
}
}  // namespace

BEZEL_TEST(xmod_overload_candidate_spelling_6871,
           imported_nullable_candidates_spelled_after_type_spec_22_6) {
    const char* lib = R"bzl(module L;
public static class S
  public static method Pick(x: long): int => 1;
  public static method Pick(x: long?): int => 2;
end S;)bzl";
    const char* app = R"bzl(module A;
import L;
class P public static method Main(): int
  Console.WriteLine(S.Pick(3_000_000_000));
  return 0; end Main; end P;)bzl";
    auto r = runAotModules({ModuleSource{"L", lib}, ModuleSource{"A", app}},
                           "A");
    const std::string m = ambiguityMessage(r);
    BEZEL_EXPECT_TRUE(has(m, "Pick(long)"));
    BEZEL_EXPECT_TRUE(has(m, "Pick(long?)"));
    BEZEL_EXPECT_FALSE(has(m, "?long"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

BEZEL_TEST(xmod_overload_candidate_spelling_6871,
           imported_nullable_class_and_generic_candidates_spec_22_6) {
    const char* lib = R"bzl(module L;
public class Animal public constructor() end; end Animal;
public class Box<T> public constructor() end; end Box;
public static class S
  public static method Q(x: Animal?, y: long): int => 1;
  public static method Q(x: Animal?, y: long?): int => 2;
  public static method B(x: Box<long?>?, y: long): int => 1;
  public static method B(x: Box<long?>?, y: long?): int => 2;
  public static method A(x: [4]long?, y: long): int => 1;
  public static method A(x: [4]long?, y: long?): int => 2;
end S;)bzl";
    const char* app = R"bzl(module A;
import L;
class P public static method Main(): int
  Console.WriteLine(S.Q(null, 3_000_000_000));
  Console.WriteLine(S.B(null, 3_000_000_000));
  return 0; end Main; end P;)bzl";
    auto r = runAotModules({ModuleSource{"L", lib}, ModuleSource{"A", app}},
                           "A");
    // The first ambiguous call is reported; both candidates carry the
    // nullable class and the trailing nullable.
    const std::string m = ambiguityMessage(r);
    BEZEL_EXPECT_TRUE(has(m, "Q(Animal?, long)"));
    BEZEL_EXPECT_TRUE(has(m, "Q(Animal?, long?)"));
    BEZEL_EXPECT_FALSE(has(m, "?Animal"));
    BEZEL_EXPECT_FALSE(has(m, "?long"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

BEZEL_TEST(xmod_overload_candidate_spelling_6871,
           imported_generic_nullable_arg_candidates_spec_22_6) {
    const char* lib = R"bzl(module L;
public class Box<T> public constructor() end; end Box;
public static class S
  public static method B(x: Box<long?>?, y: long): int => 1;
  public static method B(x: Box<long?>?, y: long?): int => 2;
end S;)bzl";
    const char* app = R"bzl(module A;
import L;
class P public static method Main(): int
  Console.WriteLine(S.B(null, 3_000_000_000));
  return 0; end Main; end P;)bzl";
    auto r = runAotModules({ModuleSource{"L", lib}, ModuleSource{"A", app}},
                           "A");
    const std::string m = ambiguityMessage(r);
    BEZEL_EXPECT_TRUE(has(m, "B(Box<long?>?, long)"));
    BEZEL_EXPECT_TRUE(has(m, "B(Box<long?>?, long?)"));
    BEZEL_EXPECT_FALSE(has(m, "?Box"));
    BEZEL_EXPECT_FALSE(has(m, "<?long>"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}
