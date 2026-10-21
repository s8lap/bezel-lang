// #6745 (§3.6 hashable row, §9.2, App. C, v0.340): a record that declares its
// own-typed `Equals` with `GetHashCode` -- and NO `=`/`<>` overload -- is
// hashable "on the strength of those members": a Set element / Dictionary key,
// compared and hashed by the user's pair. Pre-fix sema rejected it as not
// hashable (recordHasUserDeclaredEquality only saw the operators).
// `=`/`<>` on such a record stay §9.2's compile error (control row).
// Rows assert exact stdout; the collision rows use a pair that is coarser than
// structural equality (Amount mod 10) so a structural fallback gives a
// different answer.
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::failedAtCompileTimeWithRealLocation;

namespace {
bool hasErrorContaining(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

const char* kCoin = R"(
module M;
public record Coin(Amount: int)
  public method Equals(other: Coin): bool => (Amount mod 10) = (other.Amount mod 10);
  public method GetHashCode(): int => Amount mod 10;
end Coin;
)";
}  // namespace

// The issue's repro, extended with collisions: 1 and 11 are one element, 21
// is found, 2 is distinct.
BEZEL_TEST(record_equals_pair_hashable_6745, set_pair_only_collision_app_c) {
    auto r = runAot(std::string(kCoin) + R"(
public class Program
  public static method Main(): int
    var s := new Set<Coin>();
    s.Add(new Coin(1));
    s.Add(new Coin(11));
    s.Add(new Coin(2));
    Console.WriteLine($"set={s.Count}");
    Console.WriteLine($"has21={s.Contains(new Coin(21))}");
    Console.WriteLine($"has3={s.Contains(new Coin(3))}");
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("set=2\nhas21=True\nhas3=False\n"));
}

BEZEL_TEST(record_equals_pair_hashable_6745, dictionary_pair_only_collision_app_c) {
    auto r = runAot(std::string(kCoin) + R"(
public class Program
  public static method Main(): int
    var d := new Dictionary<Coin, string>();
    d.Add(new Coin(5), "a");
    d[new Coin(15)] := "b";
    d[new Coin(6)] := "c";
    Console.WriteLine($"d={d.Count} {d[new Coin(25)]} {d[new Coin(16)]}");
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("d=2 b c\n"));
}

// `override` spelling of the same pair (§9.2: the two spellings declare one
// member).
BEZEL_TEST(record_equals_pair_hashable_6745, set_override_spelling_spec_9_2) {
    auto r = runAot(R"(
module M;
public record Coin(Amount: int)
  public override method Equals(other: Coin): bool => (Amount mod 10) = (other.Amount mod 10);
  public override method GetHashCode(): int => Amount mod 10;
end Coin;
public class Program
  public static method Main(): int
    var s := new Set<Coin>();
    s.Add(new Coin(3));
    s.Add(new Coin(13));
    Console.WriteLine($"set={s.Count}");
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("set=1\n"));
}

// A body-only (non-positional) record.
BEZEL_TEST(record_equals_pair_hashable_6745, set_body_record_pair_only_app_c) {
    auto r = runAot(R"(
module M;
public record Tag
  public readonly property N: int;
  public constructor(n: int)  N := n;  end constructor;
  public method Equals(other: Tag): bool => (N mod 4) = (other.N mod 4);
  public method GetHashCode(): int => N mod 4;
end Tag;
public class Program
  public static method Main(): int
    var s := new Set<Tag>();
    s.Add(new Tag(1));
    s.Add(new Tag(5));
    s.Add(new Tag(2));
    Console.WriteLine($"set={s.Count}");
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("set=2\n"));
}

// 200x loop (leak oracle sees the collections path repeatedly).
BEZEL_TEST(record_equals_pair_hashable_6745, loop_set_dict_pair_only) {
    auto r = runAot(std::string(kCoin) + R"(
public class Program
  public static method Main(): int
    var total := 0;
    for var i in 0..199 do
      var s := new Set<Coin>();
      var d := new Dictionary<Coin, string>();
      s.Add(new Coin(i));
      s.Add(new Coin(i + 10));
      d[new Coin(i)] := $"v{i}";
      d[new Coin(i + 10)] := "w";
      total := total + s.Count + d.Count;
    end;
    Console.WriteLine($"total={total}");
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("total=400\n"));
}

// Control: `=` on a pair-only record stays §9.2's compile error.
BEZEL_TEST(record_equals_pair_hashable_6745, control_equals_operator_still_rejected_spec_9_2) {
    auto r = runAot(std::string(kCoin) + R"(
public class Program
  public static method Main(): int
    var a := new Coin(1);
    var b := new Coin(11);
    Console.WriteLine($"{a = b}");
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "`=` is not available on record `Coin`"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// Control: `Equals` alone (no GetHashCode) is still not hashable.
BEZEL_TEST(record_equals_pair_hashable_6745, control_equals_without_hash_not_hashable) {
    auto r = runAot(R"(
module M;
public record Coin(Amount: int)
  public method Equals(other: Coin): bool => Amount = other.Amount;
end Coin;
public class Program
  public static method Main(): int
    var s := new Set<Coin>();
    s.Add(new Coin(1));
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "`Set` requires a hashable element type"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}
