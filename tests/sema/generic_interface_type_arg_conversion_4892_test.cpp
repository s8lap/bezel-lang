// fix #4892 / spec §5.1 *Reference conversions* ("a type to an interface it
// implements or conforms to" + "the variance-induced conversions of §12.9:
// `I<S…> → I<T…>` per the declared `in`/`out` markers, for reference-typed
// arguments only"), §11.2, §12.9 *Variance* (v0.307).
//
// What a class implements is an INSTANTIATION: `C: IEquatable<C>` implements
// `IEquatable<C>`, and `IEquatable<D>` is reachable from it only through a
// §12.9 variance conversion — which an invariant parameter (IEquatable's T,
// and every unmarked user parameter) never admits. Before the fix,
// `checkAssignable`'s cross-declaration Named→Named branch checked only the
// interface HEAD (`isSubtype(C, IEquatable)`) and accepted any type
// arguments, so `var e: IEquatable<D> := c` compiled and `e.Equals(d)`
// dispatched `C.Equals` with a `D` (runtime type confusion; the user
// `I<int>` → `I<string>` shape SIGSEGVs reading an int as a string). The
// builtin-interface branch (`A: IEnumerable<int>` → `IEnumerable<string>`)
// had the same residual inside `variantGenericAssignable`.
//
// Reject rows assert the fix's reason clause ("only at other type
// arguments"); every one compiled clean (and ran) on the pre-fix compiler.
// Accept rows RUN and assert the printed values, so an over-rejection or a
// wrong dispatch in a legal conversion fails them.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

bool hasInstantiationMismatch(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("not implicitly convertible") != std::string::npos &&
            d.message.find("only at other type arguments") !=
                std::string::npos)
            return true;
    return false;
}

const std::string kUserIface = R"(
public interface I<T>
  method Get(): T;
end I;
public class A: I<int>
  public constructor() end constructor;
  public method Get(): int => 42;
end A;
)";

}  // namespace

// ---- REJECT: the issue's repro (local initializer) ------------------------

BEZEL_TEST(generic_interface_type_arg_conversion_4892,
           iequatable_other_type_arg_local_init_rejected_spec_5_1_12_9) {
    auto r = runAot(R"(
module M;
public class D: IEquatable<D>
  public constructor() end constructor;
  public method Equals(other: D): bool => false;
end D;
public class C: IEquatable<C>
  public constructor() end constructor;
  public method Equals(other: C): bool => true;
end C;
public class Program
  public static method Main(): int
    using var c := new C();
    using var d := new D();
    var e: IEquatable<D> := c;
    Console.WriteLine(e.Equals(d));
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasInstantiationMismatch(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ---- REJECT: user generic interface, every conversion site ----------------

BEZEL_TEST(generic_interface_type_arg_conversion_4892,
           user_iface_other_type_arg_local_init_rejected_spec_5_1_12_9) {
    auto r = runAot("module M;" + kUserIface + R"(
public class Program
  public static method Main(): int
    using var a := new A();
    var e: I<string> := a;
    Console.WriteLine(e.Get());
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasInstantiationMismatch(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

BEZEL_TEST(generic_interface_type_arg_conversion_4892,
           user_iface_other_type_arg_argument_rejected_spec_5_1_12_9) {
    auto r = runAot("module M;" + kUserIface + R"(
public class Program
  static method Use(x: I<string>): int
    return x.Get().Length;
  end Use;
  public static method Main(): int
    using var a := new A();
    Console.WriteLine(Use(a));
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasInstantiationMismatch(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

BEZEL_TEST(generic_interface_type_arg_conversion_4892,
           user_iface_other_type_arg_return_rejected_spec_5_1_12_9) {
    auto r = runAot("module M;" + kUserIface + R"(
public class Program
  static method Mk(): owned I<string>
    return new A();
  end Mk;
  public static method Main(): int
    using var x := Mk();
    Console.WriteLine(x.Get());
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasInstantiationMismatch(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

BEZEL_TEST(generic_interface_type_arg_conversion_4892,
           user_iface_other_type_arg_assignment_rejected_spec_5_1_12_9) {
    auto r = runAot("module M;" + kUserIface + R"(
public class B: I<string>
  public constructor() end constructor;
  public method Get(): string => "b";
end B;
public class Program
  public static method Main(): int
    using var a := new A();
    using var b := new B();
    var x: I<string> := b;
    x := a;
    Console.WriteLine(x.Get());
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasInstantiationMismatch(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

BEZEL_TEST(generic_interface_type_arg_conversion_4892,
           user_iface_other_type_arg_field_assignment_rejected_spec_5_1) {
    auto r = runAot("module M;" + kUserIface + R"(
public class H
  private _x: I<string>?;
  public constructor(a: A)
    _x := a;
  end constructor;
  public method Len(): int
    var x := _x;
    if x is null then return 0; end if;
    return x.Get().Length;
  end Len;
end H;
public class Program
  public static method Main(): int
    using var a := new A();
    using var h := new H(a);
    Console.WriteLine(h.Len());
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasInstantiationMismatch(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

BEZEL_TEST(generic_interface_type_arg_conversion_4892,
           user_iface_other_type_arg_collection_literal_element_rejected) {
    auto r = runAot("module M;" + kUserIface + R"(
public class Program
  public static method Main(): int
    using var a := new A();
    var xs: Array<I<string>> := [a];
    Console.WriteLine(xs[0].Get());
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasInstantiationMismatch(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// Through an interface-inheritance chain: `J: I<int>` does not make `J` an
// `I<string>`.
BEZEL_TEST(generic_interface_type_arg_conversion_4892,
           interface_chain_other_type_arg_rejected_spec_5_1_12_9) {
    auto r = runAot(R"(
module M;
public interface I<T>
  method Get(): T;
end I;
public interface J: I<int>
end J;
public class A: J
  public constructor() end constructor;
  public method Get(): int => 42;
end A;
public class Program
  public static method Main(): int
    using var a := new A();
    var j: J := a;
    var x: I<string> := j;
    Console.WriteLine(x.Get());
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasInstantiationMismatch(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// Builtin generic interface target: `A: IEnumerable<int>` is not an
// `IEnumerable<string>` (IEnumerable is `out T`, but int/string are not
// reference-related — and int is a value type, §12.9).
BEZEL_TEST(generic_interface_type_arg_conversion_4892,
           builtin_ienumerable_other_type_arg_rejected_spec_5_1_12_9) {
    auto r = runAot(R"(
module M;
public class A: IEnumerable<int>
  public constructor() end constructor;
  public method GetEnumerator(): owned IEnumerator<int>
    var xs: Array<int> := [1, 2];
    return xs.GetEnumerator();
  end GetEnumerator;
end A;
public class Program
  public static method Main(): int
    using var a := new A();
    var e: IEnumerable<string> := a;
    for var s in e do Console.WriteLine(s.Length); end for;
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasInstantiationMismatch(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// Record with a declared IEquatable<Self>, converted at the WRONG record.
BEZEL_TEST(generic_interface_type_arg_conversion_4892,
           record_declared_iequatable_other_type_arg_rejected_spec_5_1) {
    auto r = runAot(R"(
module M;
public record R(X: int): IEquatable<R>
  public method Equals(other: R): bool => X = other.X;
end R;
public record S(Y: int): IEquatable<S>
  public method Equals(other: S): bool => true;
end S;
public class Program
  public static method Main(): int
    var r := new R(1);
    var e: IEquatable<S> := r;
    Console.WriteLine(e.Equals(new S(1)));
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasInstantiationMismatch(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ---- ACCEPT: the legal conversions stay accepted and dispatch right -------

BEZEL_TEST(generic_interface_type_arg_conversion_4892,
           same_instantiation_generic_class_base_chain_accepted_and_run) {
    auto r = runAot(R"(
module M;
public interface I<T>
  method Get(): T;
end I;
public interface J<T>: I<T>
  method Other(): int;
end J;
public class G<T>: I<T>
  private _v: T;
  public constructor(v: T) _v := v; end constructor;
  public method Get(): T => _v;
end G;
public class K<U>: G<U>
  public constructor(v: U) base(v); end constructor;
end K;
public class Base: J<string>
  public constructor() end constructor;
  public method Get(): string => "base";
  public method Other(): int => 7;
end Base;
public class Derived: Base
  public constructor() base(); end constructor;
end Derived;
public class C: IEquatable<C>
  private _n: int;
  public constructor(n: int) _n := n; end constructor;
  public method Equals(other: C): bool => _n = other.N();
  public override method GetHashCode(): int => _n;
  public method N(): int => _n;
end C;
public class Program
  static method Show(x: I<int>): int => x.Get();
  static method Pick<T>(g: G<T>): T
    var i: I<T> := g;
    return i.Get();
  end Pick;
  public static method Main(): int
    using var c := new C(3);
    using var c3 := new C(3);
    using var c4 := new C(4);
    var e: IEquatable<C> := c;
    Console.WriteLine(e.Equals(c3));
    Console.WriteLine(e.Equals(c4));
    using var g := new G<int>(5);
    var gi: I<int> := g;
    Console.WriteLine(gi.Get());
    Console.WriteLine(Show(g));
    using var k := new K<string>("ks");
    var ki: I<string> := k;
    Console.WriteLine(ki.Get());
    Console.WriteLine(Pick(k));
    var arr: Array<I<string>> := [k];
    Console.WriteLine(arr[0].Get());
    using var dd := new Derived();
    var di: I<string> := dd;
    Console.WriteLine(di.Get());
    var dj: J<string> := dd;
    Console.WriteLine(dj.Other());
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("True\nFalse\n5\n5\nks\nks\nks\nbase\n7\n"));
}

// §12.9's declared variance is still honoured across declarations: a class
// implementing `IProducer<Dog>` (out T) converts to `IProducer<Animal>`, and
// one implementing `IConsumer<Animal>` (in T) converts to `IConsumer<Dog>`.
BEZEL_TEST(generic_interface_type_arg_conversion_4892,
           declared_variance_cross_declaration_accepted_and_run_spec_12_9) {
    auto r = runAot(R"(
module M;
public class Animal
  public constructor() end constructor;
  public virtual method Name(): string => "animal";
end Animal;
public class Dog: Animal
  public constructor() base(); end constructor;
  public override method Name(): string => "dog";
end Dog;
public interface IProducer<out T>
  method Make(): owned T;
end IProducer;
public interface IConsumer<in T>
  method Take(x: T): string;
end IConsumer;
public class DogShop: IProducer<Dog>, IConsumer<Animal>
  public constructor() end constructor;
  public method Make(): owned Dog => new Dog();
  public method Take(x: Animal): string => "took " + x.Name();
end DogShop;
public class Program
  public static method Main(): int
    using var s := new DogShop();
    var p: IProducer<Animal> := s;
    using var a := p.Make();
    Console.WriteLine(a.Name());
    var k: IConsumer<Dog> := s;
    using var d := new Dog();
    Console.WriteLine(k.Take(d));
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("dog\ntook dog\n"));
}

// A record's declared IEquatable<Self> and a user class's IEnumerable<int>
// still convert at their own instantiation. (The IEnumerable view is observed
// through §8.5's dynamic ToString dispatch rather than by iterating: the
// enumerator would borrow a local Array that GetEnumerator's return outlives.)
BEZEL_TEST(generic_interface_type_arg_conversion_4892,
           record_iequatable_self_and_user_ienumerable_accepted_and_run) {
    auto r = runAot(R"(
module M;
public record R(X: int): IEquatable<R>
  public method Equals(other: R): bool => X = other.X;
end R;
public class A: IEnumerable<int>
  private owned _xs: Array<int>;
  public constructor() _xs := new Array<int>(); _xs.Add(1); _xs.Add(2); end constructor;
  public method GetEnumerator(): owned IEnumerator<int>
    return _xs.GetEnumerator();
  end GetEnumerator;
end A;
public class Program
  public static method Main(): int
    var r := new R(1);
    var e: IEquatable<R> := r;
    Console.WriteLine(e.Equals(new R(1)));
    Console.WriteLine(e.Equals(new R(2)));
    using var a := new A();
    var en: IEnumerable<int> := a;
    Console.WriteLine($"{en}");
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\nFalse\nM.A\n"));
}
