// fix #4097 (§7.9 "Default initialization", §9.3 "Combined form", v0.338): a
// combined-form record with a different-arity secondary constructor and a
// body field/auto-property of NULLABLE type with NO initializer. §7.9: a
// nullable type is defaultable (default `null`), so the field "satisfies the
// must-initialize rule vacuously and is implicitly initialized to that
// default"; §9.3: the auto-generated primary stays available beside a
// different-arity constructor and "a defaultable one may be left to its
// default". `new R(9)` is therefore legal; before the fix
// `Emitter::buildRecordValue`'s `bodyStorageAdmitsFastPath` declined it and
// codegen reported "no constructor of `R` accepts them". Values go on stdout.
#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
std::string out(const std::string& p) {
    auto r = runAotChecked(p);
    return r.ran ? r.stdoutText : std::string("<did not run>");
}
}  // namespace

BEZEL_TEST(codegen_combined_record_nullable_body_field_4097, nullable_value_field_issue_repro) {
    BEZEL_EXPECT_EQ(out(R"(module M;
record ComboNulN(x: int)
  public extra: int?;
  public constructor(a: int, b: int) this.x := a + b; end constructor;
end ComboNulN;
class C
  public static method Main(): int
    var r := new ComboNulN(9);
    Console.WriteLine((r.x + (r.extra ?? -1)).ToString());
    return 0;
  end Main;
end C;
)"), std::string("8\n"));
}

// Reference-nullable, a nullable record, a nullable auto-property, and one
// field WITH an initializer beside them; looped so a stray slot would show.
BEZEL_TEST(codegen_combined_record_nullable_body_field_4097, mixed_nullable_members_in_loop) {
    BEZEL_EXPECT_EQ(out(R"(module M;
record Pt(a: int, b: int) end Pt;
record Combo(x: int)
  public s: string?;
  public p: Pt?;
  public property Q: long?;
  public k: int := 5;
  public constructor(a: int, b: int) this.x := a + b; end constructor;
end Combo;
class C
  public static method Main(): int
    var total := 0;
    for var i in 0..149 do
      var r := new Combo(i);
      var n := 0;
      if r.s = null then n := n + 1; end;
      if r.p = null then n := n + 2; end;
      if r.Q = null then n := n + 4; end;
      total := total + n + r.k + r.x;
    end;
    Console.WriteLine(total.ToString());
    return 0;
  end Main;
end C;
)"), std::string("12975\n"));
}

// Control: a NON-defaultable body field without an initializer still cannot
// ride the fast path (§9.3: only a field initializer satisfies it).
BEZEL_TEST(codegen_combined_record_nullable_body_field_4097, non_nullable_uninitialised_still_rejected_control) {
    auto r = runAot(R"(module M;
record R(x: string)
  public extra: string;
  public constructor(a: int, b: int) this.x := "q"; end constructor;
end R;
class C
  public static method Main(): int
    var r := new R("a");
    return 0;
  end Main;
end C;
)");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}
