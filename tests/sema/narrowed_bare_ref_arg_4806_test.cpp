// Regression tests for issue #4806: a bare `ref o` / `out o` of a §12.7-narrowed
// variable was typed at the NARROWED type.
//
// MEASURED at ebf431b8 (r20-l3 base) with `bezelc -o`:
//
//   * `if o is int then var z := G(ref o); end` with `G(ref x: Object)` was
//     rejected: "the `ref` argument for parameter `x` to call to `G` does not
//     match the parameter's referent type". The same held for
//     `p := G(ref o)`, for `o := G(ref o)` (accepted before 787d5393 / #4711,
//     rejected since), for `F(out o)` at `out x: Object`, and for
//     `new R(ref o)`.
//   * The mirror image was ACCEPTED: `G(ref o)` with `G(ref x: int)`. Codegen's
//     #3090 lvalue arm handed the callee the SHARED §8.6 box's payload, so after
//     `var p: Object := o;` the callee's `x := x + 1` changed `p` from 5 to 6.
//     That is aliased mutation of a boxed value, which §12.7 / §8.6 exclude.
//
// Spec (v0.307):
//   * §12.7 rejects only a CHAIN extending the narrowed binding as a `ref`/`out`
//     argument (`ref o[0]`, `out o.X`, position (ii)). A bare-binding store
//     "targets the *declared* `Object`/interface location".
//   * §22.3: a `ref`/`out` argument aliases the caller's location, and its type
//     must match the referent type exactly.
//   * §7.4: passing the variable as `ref`/`out` invalidates the narrowing.
// So a bare `ref o` / `out o` names the declared location, typed at the
// declared type.
//
// The reject is not merely conservative. The reviewer MEASURED the admitted
// shape as unsound on base: `ref a` (a: Animal narrowed to Dog) at
// `ref x: Dog`, with a mutably-captured lambda storing an `Animal` into `a`
// during the call, gave an ASan heap-buffer-overflow in `Dog.Bark`. Spec
// clarification: #4851. The diagnostic therefore names the sound remedy, a
// copy-out into a fresh local of the parameter's type.
//
// ORACLE. The accept rows RUN the program and assert exact stdout, including
// the value the callee stored into the declared location. The reject row pins
// the §22.3 referent-type diagnostic. The chain row is a control: §12.7(ii)
// still rejects it, with its own wording.
//
// Cite: spec §12.7, §22.3, §7.4, §8.6, v0.307.

#include "support/aot_support.hpp"  // runAot
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;

namespace {
bool hasError(const bezel::test::JitResult& r, const std::string& a,
              const std::string& b) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(a) != std::string::npos &&
            d.message.find(b) != std::string::npos)
            return true;
    }
    return false;
}
}  // namespace

// The issue's repro: `var z := G(ref o)` inside `if o is int`. G stores a
// string into the declared `Object` location.
BEZEL_TEST(narrowed_bare_ref_arg_4806, bare_ref_at_declared_object_param_accepted_spec_12_7_22_3) {
    auto r = runAot(R"BZ(
module P;
public class Program
  public static method G(ref x: Object): int
    x := "str";
    return 1;
  end G;
  public static method Main()
    var o: Object := 5;
    if o is int then
      var z := G(ref o);
      Console.WriteLine(z);
    end if;
    Console.WriteLine(o.ToString());
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\nstr\n"));
}

// `p := G(ref o)`: the ref argument sits in another variable's assignment RHS.
BEZEL_TEST(narrowed_bare_ref_arg_4806, bare_ref_in_assignment_rhs_accepted_spec_12_7_22_3) {
    auto r = runAot(R"BZ(
module P;
public class Program
  public static method G(ref x: Object): int
    x := "str";
    return 1;
  end G;
  public static method Main()
    var o: Object := 5;
    var p: Object := 0;
    if o is int then
      p := G(ref o);
    end if;
    Console.WriteLine(o.ToString());
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("str\n"));
}

// `o := G(ref o)`: the #4711 (787d5393) accept->reject regression. The
// assignment stores the boxed `1` over the callee's "str".
BEZEL_TEST(narrowed_bare_ref_arg_4806, self_assignment_of_ref_call_accepted_spec_12_7_7_4) {
    auto r = runAot(R"BZ(
module P;
public class Program
  public static method G(ref x: Object): int
    x := "str";
    return 1;
  end G;
  public static method Main()
    var o: Object := 5;
    if o is int then
      o := G(ref o);
    end if;
    Console.WriteLine(o.ToString());
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n"));
}

// `out o` at `out x: Object`. The narrowed read `o + 1` before it still
// reads the int, and the alias `p` keeps the original box (5).
BEZEL_TEST(narrowed_bare_ref_arg_4806, bare_out_at_declared_object_param_accepted_spec_12_7_22_3) {
    auto r = runAot(R"BZ(
module P;
public class Program
  public static method F(out x: Object): int
    x := "out";
    return 2;
  end F;
  public static method Main()
    var o: Object := 5;
    var p: Object := o;
    if o is int then
      var y := o + 1;
      var z := F(out o);
      Console.WriteLine(y + z);
    end if;
    Console.WriteLine(o.ToString());
    Console.WriteLine(p.ToString());
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("8\nout\n5\n"));
}

// The constructor-argument path (`new R(ref o)`).
BEZEL_TEST(narrowed_bare_ref_arg_4806, bare_ref_constructor_argument_accepted_spec_12_7_22_3) {
    auto r = runAot(R"BZ(
module P;
public record R
  public N: int;
  public constructor(ref x: Object)
    x := "str";
    N := 1;
  end constructor;
end R;
public class Program
  public static method Main()
    var o: Object := 5;
    if o is int then
      var r := new R(ref o);
      Console.WriteLine(r.N);
    end if;
    Console.WriteLine(o.ToString());
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\nstr\n"));
}

// The mirror image. `ref o` at a `ref x: int` parameter names an `Object`
// location, whose storage is a §8.6 box, so §22.3 rejects it. Before the fix it
// was admitted and mutated the shared box (the alias `p` printed 6).
BEZEL_TEST(narrowed_bare_ref_arg_4806, bare_ref_at_narrowed_value_param_rejected_spec_22_3_8_6) {
    auto r = runAot(R"BZ(
module P;
public class Program
  public static method G(ref x: int): int
    x := x + 1;
    return x;
  end G;
  public static method Main()
    var o: Object := 5;
    var p: Object := o;
    if o is int then
      var z := G(ref o);
      Console.WriteLine(z);
    end if;
    Console.WriteLine(o.ToString());
    Console.WriteLine(p.ToString());
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r, "the `ref` argument for parameter `x` to call to `G`",
                               "does not match the parameter's referent type"));
    BEZEL_EXPECT_TRUE(hasError(r, "`o` is §12.7-narrowed here",
                               "(`var oCopy: int := o;` then `ref oCopy`)"));
}

// The reviewer's unsoundness witness (#4851). `a: Animal` narrowed to `Dog`,
// passed as `ref a` at `ref x: Dog`, while a lambda that captured `a` stores an
// `Animal` into it during the call. Admitting it read an Animal as a Dog
// (ASan heap-buffer-overflow in Dog.Bark). It must be rejected, and the
// diagnostic must name the copy-out remedy with the parameter's type.
BEZEL_TEST(narrowed_bare_ref_arg_4806, narrowed_class_ref_with_capturing_store_rejected_with_copy_out_hint_spec_22_3) {
    auto r = runAot(R"BZ(
module Conf;
public class Animal
  public constructor() end constructor;
  public virtual method Name(): string
    return "animal";
  end Name;
end Animal;
public class Dog: Animal
  public constructor() end constructor;
  public override method Name(): string
    return "dog";
  end Name;
  public _tag: string := "woof-field";
  public method Bark(): string
    return _tag;
  end Bark;
end Dog;
public class Program
  public static method G(ref x: Dog, f: () -> ()): string
    f();
    Console.WriteLine(x.Name());
    return x.Bark();
  end G;
  public static method Main()
    var d := new Dog();
    var c := new Animal();
    var a: Animal := d;
    var f: () -> () := lambda() => do a := c; end;
    if a is Dog then
      Console.WriteLine(G(ref a, f));
    end;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r, "the `ref` argument for parameter `x` to call to `G`",
                               "does not match the parameter's referent type"));
    BEZEL_EXPECT_TRUE(hasError(r, "`a` is §12.7-narrowed here",
                               "(`var aCopy: Dog := a;` then `ref aCopy`)"));
}

// The remedy the hint names compiles and runs: the callee aliases the fresh
// `Dog` local, so a later store into `a` cannot reach it.
BEZEL_TEST(narrowed_bare_ref_arg_4806, copy_out_remedy_accepted_and_runs_spec_22_3) {
    auto r = runAot(R"BZ(
module Conf;
public class Animal
  public constructor() end constructor;
  public virtual method Name(): string
    return "animal";
  end Name;
end Animal;
public class Dog: Animal
  public constructor() end constructor;
  public override method Name(): string
    return "dog";
  end Name;
  public _tag: string := "woof-field";
  public method Bark(): string
    return _tag;
  end Bark;
end Dog;
public class Program
  public static method G(ref x: Dog, f: () -> ()): string
    f();
    Console.WriteLine(x.Name());
    return x.Bark();
  end G;
  public static method Main()
    var d := new Dog();
    var c := new Animal();
    var a: Animal := d;
    var f: () -> () := lambda() => do a := c; end;
    if a is Dog then
      var aCopy: Dog := a;
      Console.WriteLine(G(ref aCopy, f));
    end;
    Console.WriteLine(a.Name());
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("dog\nwoof-field\nanimal\n"));
}

// CONTROL (§12.7(ii)): a CHAIN rooted at the narrowed binding is still a write
// into the box and still rejected with the box-write wording. The chain's root
// is a different node from the argument operand, so it keeps its narrowing.
BEZEL_TEST(narrowed_bare_ref_arg_4806, chain_ref_through_narrowed_binding_still_rejected_spec_12_7) {
    auto r = runAot(R"BZ(
module P;
public class Program
  public static method S(ref x: int): int
    x := 9;
    return 0;
  end S;
  public static method Main()
    var a: [3]int := [4, 5, 6];
    var o: Object := a;
    if o is [3]int then
      var z := S(ref o[0]);
    end if;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r, "cannot ref `o`", "is §12.7-narrowed to a value type"));
}
