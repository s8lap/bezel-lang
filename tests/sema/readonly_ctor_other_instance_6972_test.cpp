// Accept controls for #6972 — §22.8 (v0.340): a constructor writes its OWN
// instance's readonly inline-array elements through `this.A[i]`, the spelling
// the other-instance rejection must keep admitting. The reject rows are lit:
// tests/lit/sema/readonly_ctor_other_instance_6972__*.bzl.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

BEZEL_TEST(sema_readonly_ctor_other_instance_6972,
           class_own_inline_array_elements_accepted_spec_22_8) {
    auto r = runAot(R"(module M;
public class H
  public readonly A: [3]int;
  public constructor()
    this.A[0] := 1;
    this.A[1] := 2;
  end constructor;
  destructor end destructor;
end H;
public class App
  public static method Main(): int
    using var h := new H();
    Console.WriteLine(h.A[0] + h.A[1]);
    return 0;
  end Main;
  destructor end destructor;
end App;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "3\n");
}

BEZEL_TEST(sema_readonly_ctor_other_instance_6972,
           record_own_inline_array_elements_accepted_spec_22_8) {
    auto r = runAot(R"(module M;
public record P
  public readonly A: [2]int;
  public readonly B: int;
  public constructor(v: int)
    this.A[0] := v;
    this.A[1] := v + 1;
    this.B := v;
  end constructor;
end P;
public class App
  public static method Main(): int
    var p := new P(3);
    Console.WriteLine(p.A[0] + p.A[1] + p.B);
    return 0;
  end Main;
  destructor end destructor;
end App;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "10\n");
}
