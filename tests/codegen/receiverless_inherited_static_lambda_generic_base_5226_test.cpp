// Regression tests for issue #5226 - §22.12 (v0.313): "a static member declared
// on a base class is a member of every class derived from it ... one cell per
// declaring instantiation, whatever type name reaches it"; §14.1 (generic
// substitution). A bare `Bump()` names the declaring type's cell at the
// substitution the enclosing class's base clause supplies.
//
// THE DEFECT (silent wrong code). #5215 fixed the receiverless call to an
// inherited static of a generic base, but the enclosing class was recovered by
// reverse-looking `currentFn_` in `methodFns_`; inside a LAMBDA `currentFn_` is
// the lambda's own function, the lookup missed, and the static ran at an erased
// `T` against a cell the qualified reads `B<int>._c` / `B<string>._c` never see.

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

// Plain-AOT only: a lambda capturing a local inside a GENERIC class's method
// leaks its 4 B capture cell under the checked (LSan) leg - the unrelated
// defect #5228 - so the checked leg is not asserted for `D<T>` captures.
void expectPlain(const char* src, const std::string& expected) {
    JitResult plain = runAot(src);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 0);
    BEZEL_EXPECT_EQ(plain.stdoutText, expected);
}

}  // namespace

// The issue's repro: static-method lambda, instance-method lambda, and the
// non-generic-derived `E: B<int>` lambda. Pre-fix: "3 3 3 3 0 0".
BEZEL_TEST(receiverless_inherited_static_lambda_generic_base_5226, issue_5226_repro) {
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
  public static method Inner(): int
    var f := lambda(): int => Bump();
    return f() + f();
  end Inner;
  public method I(): int
    var f := lambda(): int => Bump();
    return f();
  end I;
end D;
public class E: B<int>
  public constructor() base(); end constructor;
  public static method Inner2(): int
    var f := lambda(): int => Bump();
    return f() + f();
  end Inner2;
end E;
public class Program
  public static method Main(): int
    Console.WriteLine(D<string>.Inner());
    Console.WriteLine(D<int>.Inner());
    var d := new D<string>();
    Console.WriteLine(d.I());
    Console.WriteLine(E.Inner2());
    Console.WriteLine(B<int>._c);
    Console.WriteLine(B<string>._c);
    return 0;
  end Main;
end Program;
)BZL", "3\n3\n3\n7\n4\n3\n");
}

// A lambda nested inside a lambda: the bare static call is two lambdas deep.
BEZEL_TEST(receiverless_inherited_static_lambda_generic_base_5226, nested_lambda) {
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
  public static method Nest(): int
    var f := lambda(): () -> int => lambda(): int => Bump();
    var g := f();
    return g() + g();
  end Nest;
  public method NestI(): int
    var f := lambda(): () -> int => lambda(): int => Bump();
    var g := f();
    return g();
  end NestI;
end D;
public class E: B<int>
  public constructor() base(); end constructor;
  public static method Nest2(): int
    var f := lambda(): () -> int => lambda(): int => Bump();
    var g := f();
    return g() + g();
  end Nest2;
end E;
public class Program
  public static method Main(): int
    Console.WriteLine(D<string>.Nest());
    Console.WriteLine(D<string>.Nest());
    var d := new D<string>();
    Console.WriteLine(d.NestI());
    Console.WriteLine(E.Nest2());
    Console.WriteLine(B<int>._c);
    Console.WriteLine(B<string>._c);
    return 0;
  end Main;
end Program;
)BZL", "3\n7\n5\n3\n2\n5\n");
}

// A lambda that captures a local and calls the bare static.
BEZEL_TEST(receiverless_inherited_static_lambda_generic_base_5226, capturing_lambda) {
    expectPlain(R"BZL(
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
  public static method Cap(): int
    var k := 10;
    var f := lambda(): int => Bump() + k;
    return f() + f();
  end Cap;
end D;
public class E: B<int>
  public constructor() base(); end constructor;
  public static method Cap2(): int
    var k := 10;
    var f := lambda(): int => Bump() + k;
    return f() + f();
  end Cap2;
end E;
public class Program
  public static method Main(): int
    Console.WriteLine(D<string>.Cap());
    Console.WriteLine(D<string>.Cap());
    Console.WriteLine(E.Cap2());
    Console.WriteLine(B<int>._c);
    Console.WriteLine(B<string>._c);
    return 0;
  end Main;
end Program;
)BZL", "23\n27\n23\n2\n4\n");
}

// The same capture through the non-generic derived `E: B<int>` only (no
// generic-class capture cell, so the checked leg applies too).
BEZEL_TEST(receiverless_inherited_static_lambda_generic_base_5226, capturing_lambda_nongeneric_derived) {
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
public class E: B<int>
  public constructor() base(); end constructor;
  public static method Cap2(): int
    var k := 10;
    var f := lambda(): int => Bump() + k;
    return f() + f();
  end Cap2;
end E;
public class Program
  public static method Main(): int
    Console.WriteLine(E.Cap2());
    Console.WriteLine(B<int>._c);
    Console.WriteLine(B<string>._c);
    return 0;
  end Main;
end Program;
)BZL", "23\n2\n0\n");
}

// The #1101-C bare INSTANCE arm shares the enclosing-class recovery: an
// inherited generic-base instance method `Get(): T?` called bare from inside a
// lambda must run at the derived class's substitution (T = string), not the
// erased i32 template (pre-fix: malformed IR / wrong value).
BEZEL_TEST(receiverless_inherited_static_lambda_generic_base_5226, bare_instance_arm_in_lambda) {
    expectBoth(R"BZL(
module M;
public class Factory<T>
  public _v: T?;
  public constructor(p: T?) _v := p; end constructor;
  public method Get(): T?
    return _v;
  end Get;
end Factory;
public class SF: Factory<string>
  public constructor(p: string?) base(p); end constructor;
  public method Run(): string
    var f := lambda(): string? => Get();
    var r := f();
    return (if r <> null then r! else "none");
  end Run;
end SF;
public class Program
  public static method Main(): int
    var s := new SF("hello");
    Console.WriteLine(s.Run());
    return 0;
  end Main;
end Program;
)BZL", "hello\n");
}

// NO-REGRESSION rows (fix-forward of #5226): a lambda in a member body that is
// emitted LAZILY from another member (here `class E: B<int>`'s `Run()` forces
// `D<string>`'s specialized ctor / field initializer) must resolve its enclosing
// class lexically (D<T>), never from a stale emitter-global "current method"
// (E). These pass on the base too; they pin that the fix does not break them.
BEZEL_TEST(receiverless_inherited_static_lambda_generic_base_5226, ctor_body_lambda_forced_from_other_class) {
    expectPlain(R"BZL(
module M;
public class B<T>
  public static _c: int := 0;
  public _i: int := 0;
  public constructor() end constructor;
  public static method Bump(): int
    B<T>._c := B<T>._c + 1;
    return B<T>._c;
  end Bump;
  public method IBump(): int
    B<T>._c := B<T>._c + 10;
    return B<T>._c;
  end IBump;
end B;
public class D<T>: B<T>
  public _r: int := 0;
  public constructor()
    base();
    var f := lambda(): int => Bump();
    _r := f();
  end constructor;
end D;
public class E: B<int>
  public constructor() base(); end constructor;
  public method Run(): int
    var d := new D<string>();
    return d._r;
  end Run;
end E;
public class Program
  public static method Main(): int
    var e := new E();
    Console.WriteLine(e.Run());
    Console.WriteLine(B<string>._c);
    Console.WriteLine(B<int>._c);
    return 0;
  end Main;
end Program;
)BZL", "1\n1\n0\n");
}

BEZEL_TEST(receiverless_inherited_static_lambda_generic_base_5226, field_init_lambda_forced_from_other_class) {
    expectPlain(R"BZL(
module M;
public class B<T>
  public static _c: int := 0;
  public _i: int := 0;
  public constructor() end constructor;
  public static method Bump(): int
    B<T>._c := B<T>._c + 1;
    return B<T>._c;
  end Bump;
  public method IBump(): int
    B<T>._c := B<T>._c + 10;
    return B<T>._c;
  end IBump;
end B;
public class D<T>: B<T>
  public _f: () -> int := lambda(): int => Bump();
  public constructor() base(); end constructor;
  public static method Inner(): int
    var f := lambda(): int => Bump();
    return f();
  end Inner;
  public method I(): int
    var f := lambda(): int => IBump();
    return f();
  end I;
end D;
public class E: B<int>
  public constructor() base(); end constructor;
  public method Run(): int
    var d := new D<string>();
    var a := d._f();
    Console.WriteLine($"f={a}");
    var b := D<string>.Inner();
    Console.WriteLine($"inner={b}");
    var c := d.I();
    Console.WriteLine($"I={c}");
    return 0;
  end Run;
end E;
public class Program
  public static method Main(): int
    var e := new E();
    Console.WriteLine(e.Run());
    Console.WriteLine(B<string>._c);
    Console.WriteLine(B<int>._c);
    return 0;
  end Main;
end Program;
)BZL", "f=1\ninner=2\nI=12\n0\n12\n0\n");
}

BEZEL_TEST(receiverless_inherited_static_lambda_generic_base_5226, static_field_init_lambda_forced_from_other_class) {
    expectBoth(R"BZL(
module M;
public class B<T>
  public static _c: int := 0;
  public _i: int := 0;
  public constructor() end constructor;
  public static method Bump(): int
    B<T>._c := B<T>._c + 1;
    return B<T>._c;
  end Bump;
  public method IBump(): int
    B<T>._c := B<T>._c + 10;
    return B<T>._c;
  end IBump;
end B;
public class F: B<int>
  public static readonly _sf: () -> int := lambda(): int => Bump();
  public constructor() base(); end constructor;
end F;
public class G: B<string>
  public constructor() base(); end constructor;
  public method Run(): int
    return F._sf();
  end Run;
end G;
public class Program
  public static method Main(): int
    var g := new G();
    Console.WriteLine(g.Run());
    Console.WriteLine(B<int>._c);
    Console.WriteLine(B<string>._c);
    return 0;
  end Main;
end Program;
)BZL", "1\n1\n0\n");
}
