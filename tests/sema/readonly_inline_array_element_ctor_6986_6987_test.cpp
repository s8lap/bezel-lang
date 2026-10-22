// #6986 / #6987 — §22.8 (v0.340): "a constructor of the declaring type may
// **not** write another value's `readonly` storage held in a parameter,
// local, or field" (#6986: ref/out of another instance's readonly
// inline-array element), and the constructor write window "does not include
// a nested function body" (#6987: a lambda in a constructor writing the own
// readonly inline-array element). Accept controls pin what must keep running.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

BEZEL_TEST(sema_readonly_inline_array_element_ctor_6986_6987,
           ref_other_instance_element_rejected_6986_spec_22_8) {
    auto r = runAot(R"(module M;
public class U
  public static method Bump(ref v: int) v := v + 1; end Bump;
  public static method Set(out v: int) v := 1; end Set;
  destructor end destructor;
end U;
public class H
  public readonly A: [2]int;
  public owned Next: H?;
  public constructor()
    this.Next := new H(); U.Bump(ref this.Next!.A[0]);
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
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "argument may not be an element of readonly field `A`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§22.8"));
}

BEZEL_TEST(sema_readonly_inline_array_element_ctor_6986_6987,
           out_other_instance_element_rejected_6986_spec_22_8) {
    auto r = runAot(R"(module M;
public class U
  public static method Bump(ref v: int) v := v + 1; end Bump;
  public static method Set(out v: int) v := 1; end Set;
  destructor end destructor;
end U;
public class H
  public readonly A: [2]int;
  public owned Next: H?;
  public constructor()
    this.Next := new H(); U.Set(out this.Next!.A[0]);
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
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "argument may not be an element of readonly field `A`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§22.8"));
}

BEZEL_TEST(sema_readonly_inline_array_element_ctor_6986_6987,
           lambda_assign_own_element_rejected_6987_spec_22_8) {
    auto r = runAot(R"(module M;
public class U
  public static method Bump(ref v: int) v := v + 1; end Bump;
  public static method Set(out v: int) v := 1; end Set;
  destructor end destructor;
end U;
public class H
  public readonly A: [2]int;
  public owned Next: H?;
  public constructor()
    var f := lambda() => do this.A[0] := 1; end; f();
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
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "cannot assign to readonly field `A` outside the declaring class"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§22.8"));
}

BEZEL_TEST(sema_readonly_inline_array_element_ctor_6986_6987,
           lambda_bare_assign_own_element_rejected_6987_spec_22_8) {
    auto r = runAot(R"(module M;
public class U
  public static method Bump(ref v: int) v := v + 1; end Bump;
  public static method Set(out v: int) v := 1; end Set;
  destructor end destructor;
end U;
public class H
  public readonly A: [2]int;
  public owned Next: H?;
  public constructor()
    var f := lambda() => do A[0] := 1; end; f();
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
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "cannot assign to readonly field `A` outside the declaring class"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§22.8"));
}

BEZEL_TEST(sema_readonly_inline_array_element_ctor_6986_6987,
           lambda_ref_own_element_rejected_6987_spec_22_8) {
    auto r = runAot(R"(module M;
public class U
  public static method Bump(ref v: int) v := v + 1; end Bump;
  public static method Set(out v: int) v := 1; end Set;
  destructor end destructor;
end U;
public class H
  public readonly A: [2]int;
  public owned Next: H?;
  public constructor()
    var f := lambda() => do U.Bump(ref this.A[0]); end; f();
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
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "argument may not be an element of readonly field `A`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§22.8"));
}

BEZEL_TEST(sema_readonly_inline_array_element_ctor_6986_6987,
           lambda_increment_own_element_rejected_6987_spec_22_8) {
    auto r = runAot(R"(module M;
public class U
  public static method Bump(ref v: int) v := v + 1; end Bump;
  public static method Set(out v: int) v := 1; end Set;
  destructor end destructor;
end U;
public class H
  public readonly A: [2]int;
  public owned Next: H?;
  public constructor()
    var f := lambda() => do this.A[0]++; end; f();
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
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "cannot increment/decrement readonly field `A` outside the declaring class"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§22.8"));
}

BEZEL_TEST(sema_readonly_inline_array_element_ctor_6986_6987,
           param_ref_element_rejected_6986_spec_22_8) {
    auto r = runAot(R"(module M;
public class U
  public static method Bump(ref v: int) v := v + 1; end Bump;
  public static method Set(out v: int) v := 1; end Set;
  destructor end destructor;
end U;
public class H
  public readonly A: [2]int;
  public owned Next: H?;
  public constructor() end constructor;
  public constructor(o: H)
    U.Bump(ref o.A[0]);
  end constructor;
  destructor end destructor;
end H;
public class App
  public static method Main(): int
    using var h0 := new H(); using var h := new H(h0);
    Console.WriteLine(h.A[0] + h.A[1]);
    return 0;
  end Main;
  destructor end destructor;
end App;
)");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "argument may not be an element of readonly field `A`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§22.8"));
}

BEZEL_TEST(sema_readonly_inline_array_element_ctor_6986_6987,
           param_out_element_rejected_6986_spec_22_8) {
    auto r = runAot(R"(module M;
public class U
  public static method Bump(ref v: int) v := v + 1; end Bump;
  public static method Set(out v: int) v := 1; end Set;
  destructor end destructor;
end U;
public class H
  public readonly A: [2]int;
  public owned Next: H?;
  public constructor() end constructor;
  public constructor(o: H)
    U.Set(out o.A[1]);
  end constructor;
  destructor end destructor;
end H;
public class App
  public static method Main(): int
    using var h0 := new H(); using var h := new H(h0);
    Console.WriteLine(h.A[0] + h.A[1]);
    return 0;
  end Main;
  destructor end destructor;
end App;
)");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "argument may not be an element of readonly field `A`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§22.8"));
}

BEZEL_TEST(sema_readonly_inline_array_element_ctor_6986_6987,
           own_element_assign_accepted_spec_22_8) {
    auto r = runAot(R"(module M;
public class U
  public static method Bump(ref v: int) v := v + 1; end Bump;
  public static method Set(out v: int) v := 1; end Set;
  destructor end destructor;
end U;
public class H
  public readonly A: [2]int;
  public owned Next: H?;
  public constructor()
    this.A[0] := 1; this.A[1] := 2;
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

BEZEL_TEST(sema_readonly_inline_array_element_ctor_6986_6987,
           own_element_ref_out_accepted_spec_22_8) {
    auto r = runAot(R"(module M;
public class U
  public static method Bump(ref v: int) v := v + 1; end Bump;
  public static method Set(out v: int) v := 1; end Set;
  destructor end destructor;
end U;
public class H
  public readonly A: [2]int;
  public owned Next: H?;
  public constructor()
    U.Bump(ref this.A[0]); U.Set(out this.A[1]); U.Bump(ref A[1]);
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

BEZEL_TEST(sema_readonly_inline_array_element_ctor_6986_6987,
           local_inline_array_accepted_spec_22_8) {
    auto r = runAot(R"(module M;
public class U
  public static method Bump(ref v: int) v := v + 1; end Bump;
  public static method Set(out v: int) v := 1; end Set;
  destructor end destructor;
end U;
public class H
  public readonly A: [2]int;
  public owned Next: H?;
  public constructor()
    var l: [2]int; l[0] := 1; U.Bump(ref l[0]); U.Set(out l[1]); this.A[0] := l[0]; this.A[1] := l[1];
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

BEZEL_TEST(sema_readonly_inline_array_element_ctor_6986_6987,
           lambda_local_inline_array_accepted_spec_22_8) {
    auto r = runAot(R"(module M;
public class U
  public static method Bump(ref v: int) v := v + 1; end Bump;
  public static method Set(out v: int) v := 1; end Set;
  destructor end destructor;
end U;
public class H
  public readonly A: [2]int;
  public owned Next: H?;
  public constructor()
    var f := lambda() => do var l: [2]int; l[0] := 1; U.Bump(ref l[0]); end; f(); this.A[0] := 3;
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
