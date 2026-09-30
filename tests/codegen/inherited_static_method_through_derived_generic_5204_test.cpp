// Regression tests for issue #5204 — §22.12 (v0.313): "a static member declared
// on a base class is a member of every class derived from it ... may be named
// through a derived type ... one cell per declaring instantiation, whatever
// type name reaches it." The static-METHOD sibling of #5152.
//
// THE DEFECT (silent wrong code). `D<string>.Bump()` (`class D<T>: B<T>`,
// `Bump` a static of `B<T>` that touches `B<T>._c`) ran the method at an
// erased `T`, so it mutated a different cell than `B<string>._c`: the repro
// printed `1 2 0`, expected `1 2 2`. The static-call receiver-substitution
// block in LlvmBackend_Calls2.cpp only installed the substitution when the
// receiver's declaration WAS the method's declaring class; a derived receiver
// now walks the base chain via computeAncestorGenericSubst.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

void expectBoth(const char* src, const std::string& expected) {
    JitResult plain = runAot(src);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 0);
    BEZEL_EXPECT_EQ(plain.stdoutText, expected);
    JitResult checked = runAotChecked(src);
    BEZEL_EXPECT_TRUE(checked.ran);
    BEZEL_EXPECT_EQ(checked.exitCode, 0);
    BEZEL_EXPECT_EQ(checked.stdoutText, expected);
}

}  // namespace

// The issue's repro, verbatim. Pre-fix: "1\n2\n0\n".
BEZEL_TEST(inherited_static_method_through_derived_generic_5204, issue_5204_repro) {
    expectBoth(R"BZL(
module M;
public class B<T>
  public static _c: int := 0;
  public constructor() end constructor;
  public static method Bump(): int
    B<T>._c := B<T>._c + 1;
    return B<T>._c;
  end Bump;
end B;
public class D<T>: B<T>
  public constructor() base(); end constructor;
end D;
public class Program
  public static method Main(): int
    Console.WriteLine(D<string>.Bump());
    Console.WriteLine(D<string>.Bump());
    Console.WriteLine(B<string>._c);
    return 0;
  end Main;
end Program;
)BZL",
               "1\n2\n2\n");
}

// Two-level chain D2<T>: D<T>: B<T>, a non-generic derived E: B<int>, and the
// instantiations stay distinct (B<string> vs B<int>).
BEZEL_TEST(inherited_static_method_through_derived_generic_5204, two_level_chain_and_nongeneric_derived) {
    expectBoth(R"BZL(
module M;
public class B<T>
  public static _c: int := 0;
  public constructor() end constructor;
  public static method Bump(): int
    B<T>._c := B<T>._c + 1;
    return B<T>._c;
  end Bump;
end B;
public class D<T>: B<T>
  public constructor() base(); end constructor;
end D;
public class D2<T>: D<T>
  public constructor() base(); end constructor;
end D2;
public class E: B<int>
  public constructor() base(); end constructor;
end E;
public class Program
  public static method Main(): int
    Console.WriteLine(D<string>.Bump());
    Console.WriteLine(D2<string>.Bump());
    Console.WriteLine(B<string>._c);
    Console.WriteLine(E.Bump());
    Console.WriteLine(B<int>.Bump());
    Console.WriteLine(B<int>._c);
    Console.WriteLine(D<int>.Bump());
    Console.WriteLine(B<string>._c);
    return 0;
  end Main;
end Program;
)BZL",
               "1\n2\n2\n1\n2\n2\n3\n2\n");
}
