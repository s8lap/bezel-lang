// Regression tests for issue #5215 — §22.12 (v0.313): "a static member declared
// on a base class is a member of every class derived from it ... one cell per
// declaring instantiation, whatever type name reaches it." An unqualified
// `Bump()` inside a class is an ordinary member lookup (§22.6) that resolves to
// the same inherited static.
//
// THE DEFECT (silent wrong code). #5204 fixed the member-access form
// `D<string>.Bump()`; the RECEIVERLESS call to an inherited static of a generic
// base (from a non-generic `E: B<int>` static method, or from a method of
// `D<T>: B<T>` running as `D<string>`) still ran the static at an erased `T`,
// so it wrote a cell the qualified reads `B<int>._c` / `B<string>._c` never
// observed. The static-call substitution in LlvmBackend_Calls2.cpp
// (lowerCallDispatch) only handled a MemberAccessExpr callee; a bare
// IdentifierExpr callee now walks the ENCLOSING class's base chain with
// computeAncestorGenericSubst.

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

// The issue's repro, verbatim. Pre-fix (with #5204):
// "Foo 10|Bs 10|Inner 11|Bs 11|Inner2 1|Bi 0|InstInner 1|Bs 1|Bi 0".
BEZEL_TEST(receiverless_inherited_static_generic_base_5215, issue_5215_repro) {
    expectBoth(R"BZL(
module M;
public class B<T>
  public static _c: int := 0;
  public constructor() end constructor;
  public static method Bump(): int
    B<T>._c := B<T>._c + 1;
    return B<T>._c;
  end Bump;
  public static method Foo<U>(): int
    B<T>._c := B<T>._c + 10;
    return B<T>._c;
  end Foo;
end B;
public class D<T>: B<T>
  public constructor() base(); end constructor;
  public static method Inner(): int
    return Bump();
  end Inner;
  public method InstInner(): int
    return Bump();
  end InstInner;
end D;
public class E: B<int>
  public constructor() base(); end constructor;
  public static method Inner2(): int
    return Bump();
  end Inner2;
end E;
public class Program
  public static method Main(): int
    Console.WriteLine("Foo " + D<string>.Foo<int>().ToString());
    Console.WriteLine("Bs " + B<string>._c.ToString());
    Console.WriteLine("Inner " + D<string>.Inner().ToString());
    Console.WriteLine("Bs " + B<string>._c.ToString());
    Console.WriteLine("Inner2 " + E.Inner2().ToString());
    Console.WriteLine("Bi " + B<int>._c.ToString());
    var d := new D<string>();
    Console.WriteLine("InstInner " + d.InstInner().ToString());
    Console.WriteLine("Bs " + B<string>._c.ToString());
    Console.WriteLine("Bi " + B<int>._c.ToString());
    return 0;
  end Main;
end Program;
)BZL",
               "Foo 10\nBs 10\nInner 11\nBs 11\nInner2 1\nBi 1\n"
               "InstInner 12\nBs 12\nBi 1\n");
}

// Derived-of-derived chain with DIFFERENT type-parameter names at every level
// (D2<X>: D<X>: B<T>-instantiated), plus a non-generic `F: D<string>` two hops
// from the declaring class, at two distinct instantiations.
BEZEL_TEST(receiverless_inherited_static_generic_base_5215, two_level_chain_and_distinct_instantiations) {
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
public class D<U>: B<U>
  public constructor() base(); end constructor;
end D;
public class D2<X>: D<X>
  public constructor() base(); end constructor;
  public static method S(): int
    return Bump();
  end S;
  public method I(): int
    return Bump();
  end I;
end D2;
public class F: D<string>
  public constructor() base(); end constructor;
  public static method S(): int
    return Bump();
  end S;
end F;
public class Program
  public static method Main(): int
    Console.WriteLine(D2<string>.S());
    Console.WriteLine(D2<int>.S());
    Console.WriteLine(D2<string>.S());
    var d := new D2<string>();
    Console.WriteLine(d.I());
    Console.WriteLine(F.S());
    Console.WriteLine(B<string>._c);
    Console.WriteLine(B<int>._c);
    return 0;
  end Main;
end Program;
)BZL",
               "1\n1\n2\n3\n4\n4\n1\n");
}

// CONTROL row (passes pre-fix; guards the fix against over-substitution): an
// unqualified call from a STATIC GENERIC method of the derived generic, and
// from inside the declaring class itself (must keep its own substitution).
BEZEL_TEST(receiverless_inherited_static_generic_base_5215, static_generic_method_and_declaring_class) {
    expectBoth(R"BZL(
module M;
public class B<T>
  public static _c: int := 0;
  public constructor() end constructor;
  public static method Bump(): int
    B<T>._c := B<T>._c + 1;
    return B<T>._c;
  end Bump;
  public static method Twice(): int
    Bump();
    return Bump();
  end Twice;
end B;
public class D<T>: B<T>
  public constructor() base(); end constructor;
  public static method G<V>(): int
    return Bump();
  end G;
end D;
public class Program
  public static method Main(): int
    Console.WriteLine(D<string>.G<int>());
    Console.WriteLine(D<string>.G<long>());
    Console.WriteLine(B<string>.Twice());
    Console.WriteLine(B<string>._c);
    Console.WriteLine(B<int>._c);
    return 0;
  end Main;
end Program;
)BZL",
               "1\n2\n4\n4\n0\n");
}
