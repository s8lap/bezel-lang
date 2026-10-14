// #6071 (§A.6 `LValue`, §7.5, v0.338): the null-conditional `?.` is not in the
// grammar of an assignment target (`LValue ::= Identifier | "this" | LValue "."
// Identifier | LValue "[" ArgList "]"`), so `a?.Count := 3` is a compile error
// on every chain shape and for every assignment form; reads and calls through
// `?.` are unaffected.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

bool hasErrorContaining(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

const char* kBox = R"(
module M;
public class Box
  public Count: int := 0;
  public Inner: Box? := null;
  public method Get(): int => 5;
end Box;
)";

}  // namespace

// §A.6 LValue / §7.5 (v0.338, #6071): the issue's repro, `a?.Count := 3` on an
// Array<int>? local. Pre-fix this compiled and dropped the store.
BEZEL_TEST(null_cond_assign_target_6071,
           array_nullcond_count_assignment_rejected_spec_a6) {
    auto r = runAot(R"(
module M;
class P public static method Main(): int
  var a: Array<int>? := null;
  a?.Count := 3;
  return 0;
end Main; end P;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "`?.` cannot appear in an assignment target"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "\xC2\xA7""A.6"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §A.6 LValue (v0.338, #6071): a user-class field through `?.`, assignment.
BEZEL_TEST(null_cond_assign_target_6071,
           user_field_nullcond_assignment_rejected_spec_a6) {
    auto r = runAot(std::string(kBox) + R"(
class P public static method Main(): int
  var b: Box? := null;
  b?.Count := 3;
  return 0;
end Main; end P;
)");
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "`?.` cannot appear in an assignment target"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §A.6 LValue (v0.338, #6071): a `?.` hop in the middle of a chain.
BEZEL_TEST(null_cond_assign_target_6071,
           nullcond_hop_inside_chain_assignment_rejected_spec_a6) {
    auto r = runAot(std::string(kBox) + R"(
class P public static method Main(): int
  var b: Box? := null;
  b?.Inner.Count := 3;
  return 0;
end Main; end P;
)");
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "`?.` cannot appear in an assignment target"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §A.6 LValue / §17.6 (v0.338, #6071): compound assignment and `++` through `?.`.
BEZEL_TEST(null_cond_assign_target_6071,
           nullcond_compound_and_increment_rejected_spec_a6_17_6) {
    auto r = runAot(std::string(kBox) + R"(
class P public static method Main(): int
  var b: Box? := null;
  b?.Count += 1;
  b?.Count++;
  return 0;
end Main; end P;
)");
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "`?.` cannot appear in an assignment target"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "of `++`"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// Accept control (§7.5, #6071): `?.` reads, `?.` calls and `?.` on a member
// named Count all still compile and run; a plain `.` assignment still compiles.
BEZEL_TEST(null_cond_assign_target_6071,
           nullcond_reads_and_calls_accepted_spec_7_5) {
    auto r = runAot(std::string(kBox) + R"(
class P public static method Main(): int
  var b: Box? := new Box();
  b.Count := 4;
  var n: int? := b?.Count;
  var g: int? := b?.Get();
  var inner: Box? := b?.Inner;
  Console.WriteLine(n!.ToString());
  Console.WriteLine(g!.ToString());
  return 0;
end Main; end P;
)");
    BEZEL_EXPECT_FALSE(hasErrorContaining(
        r, "`?.` cannot appear in an assignment target"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "4\n5\n");
}
