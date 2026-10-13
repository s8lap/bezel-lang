// Spec-derived regression for issue #5808: undischarged `owned` parameters of a
// synchronous body are disposed in reverse declaration order (§28.3, v0.338;
// ruling recorded at the v0.334 changelog entry, #5747 item 1).
//
// The issue's probe: `M(owned a: Res, owned b: Res)` whose body discharges
// neither parameter, each `Res` printing its name in its destructor. The
// expected observable is `b` then `a`, on the normal edge and when the body
// throws (the exception-path discharge of §28.3 runs the same disposal on the
// unwinding edge).
//
// Both rows run under runAotChecked: the ASan/LSan oracle also guards the
// generated disposal code, and the stdout channel captures the destructor
// order directly.

#include "support/aot_support.hpp"  // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

const char* kPrelude5808 = R"BZL(module Probe;

public class Res
  public name: string;
  public constructor(n: string) name := n; end constructor;
  destructor
    Console.WriteLine(name);
  end destructor;
end Res;
)BZL";

} // namespace

// §28.3 (v0.338): normal exit edge — end of the method disposes `b` first, then `a`.
BEZEL_TEST(owned_param_disposal_order_5808,
           sync_method_owned_params_disposed_reverse_declaration_order_normal_edge_spec_28_3) {
    const std::string src = std::string(kPrelude5808) + R"BZL(
public class Holder
  public static method M(owned a: Res, owned b: Res): int
    return 0;
  end M;
end Holder;

public class Program
  public static method Main(): int
    Holder.M(new Res("a"), new Res("b"));
    return 0;
  end Main;
end Program;
)BZL";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("b\na\n"));
}

// §28.3 (v0.338): exception edge — the same reverse order when the body throws
// (the exception-path discharge, edge parity with the normal edge).
BEZEL_TEST(owned_param_disposal_order_5808,
           sync_method_owned_params_disposed_reverse_declaration_order_throw_edge_spec_28_3) {
    const std::string src = std::string(kPrelude5808) + R"BZL(
public class Holder
  public static method M(owned a: Res, owned b: Res): int
    throw new Exception("boom");
  end M;
end Holder;

public class Program
  public static method Main(): int
    try
      Holder.M(new Res("a"), new Res("b"));
    catch
      var ex: Exception => Console.WriteLine("caught"),
    end try;
    return 0;
  end Main;
end Program;
)BZL";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("b\na\ncaught\n"));
}
