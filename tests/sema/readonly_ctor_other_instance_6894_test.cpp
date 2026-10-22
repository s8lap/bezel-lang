// Accept control for #6894 — §22.8 / §23.1 (v0.340): a constructor writes its
// OWN instance's readonly field and auto-property through `this.X` (and binds
// them to `ref`), the spelling the this-rooted rule must keep admitting. The
// reject rows are lit: tests/lit/sema/readonly_ctor_other_instance_6894__*.bzl.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

// §22.8 / §23.1: `this.X := 5`, `this.P := 7`, `ref this.P` in the declaring ctor.
BEZEL_TEST(sema_readonly_ctor_other_instance_6894,
           own_this_member_writes_still_accepted) {
    auto r = runAot(R"(module M;
public class Util
  public static method Bump(ref v: int) v := v + 1; end Bump;
  destructor end destructor;
end Util;
public class Holder
  public readonly X: int;
  public readonly property P: int := 0;
  public constructor()
    this.X := 5;
    this.P := 7;
    Util.Bump(ref this.P);
  end constructor;
  destructor end destructor;
end Holder;
public class App
  public static method Main(): int
    using var h := new Holder();
    return h.X + h.P;
  end Main;
  destructor end destructor;
end App;
)");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 13);
}
