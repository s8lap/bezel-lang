// Regression tests for issue #4915 (sema half; found under #4910) — the
// #4810 arm of `checkAssignable` (a concrete primitive SOURCE into a bare
// TYPE-PARAMETER TARGET) identified the target's type parameter BY NAME
// against the ENCLOSING scope's parameters. At a generic call's argument
// check the target is the CALLEE's own unsubstituted method parameter — an
// inference slot (§14.5) — so when the caller happened to be generic over a
// parameter of the same spelling (`Go<T>` calling `Id<T>`), the arm mistook
// the callee's `T` for the caller's and rejected a well-typed call:
//
//   `the argument `x` to call to `Id` has a type that is not implicitly
//    convertible to the target type (§5.1): a value of primitive type `int`
//    does not implicitly convert to the type parameter `T` …`
//
// MEASURED at 335e3849 (r20-c25 tip): all three accept rows below are
// rejected with that diagnostic; renaming the callee's parameter (`Id<V>`)
// or making the caller non-generic compiles and prints the value.
//
// Spec (v0.307): §14.5 — `Id(5)` infers the callee's `T = int`; the argument
// then converts by identity (§5.1). The caller's own `T` plays no part.
//
// The accept rows instantiate the caller's `T` at the SAME type the callee's
// `T` infers, so that this sema-only row runs independently of #4813/#4915's
// codegen half (a same-named callee parameter resolved through the caller's
// substitution); the mismatched instantiations (`Go<string>` calling `Id(5)`)
// are pinned by `generic_method_same_name_tparam_4813`, which needs both.
// The sema rejection fired in the generic BODY, independent of the
// instantiation, so these rows fail without the guard.
//
// ORACLE. Accept rows RUN and assert exact stdout. The reject controls assert
// the #4810 general arm's own wording, so the narrowing cannot have disabled
// the arm for the enclosing scope's own `T` (§5.1 / §3.6 rule 2).

#include "support/aot_support.hpp"  // runAot
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;

namespace {

// The #4810 general arm's diagnostic (the #4916 wording, unique in src/).
constexpr const char* kGeneralArmWording =
    "does not implicitly convert to the type parameter `";

bool rejectedByRule(const std::string& program, const char* paramName) {
    auto r = runAot(program);
    if (r.ran) return false;
    const std::string needle = std::string(kGeneralArmWording) + paramName + "`";
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

// ═════════ Accept rows (each rejected at 335e3849) ═══════════════════════

// The headline repro: an int literal to a generic callee whose parameter
// shares its NAME with the generic caller's parameter.
BEZEL_TEST(callee_type_param_name_collision_4915,
           int_literal_to_same_named_callee_param_accepted_spec_14_5) {
    auto r = runAot(R"BZ(
module Conf;
public class Util
  public static method Id<T>(x: T): T
    return x;
  end Id;
  public static method Go<T>(v: T): int
    return Id(5);
  end Go;
end Util;
public class Program
  public static method Main(): int
    Console.WriteLine(Util.Go<int>(1));
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5\n"));
}

// `string` is modelled as a primitive, so a string literal argument took the
// same path (the callee's `T` infers `string`).
BEZEL_TEST(callee_type_param_name_collision_4915,
           string_literal_to_same_named_callee_param_accepted_spec_14_5) {
    auto r = runAot(R"BZ(
module Conf;
public class Util
  public static method Id<T>(x: T): T
    return x;
  end Id;
  public static method Go<T>(v: T): string
    return Id("q");
  end Go;
end Util;
public class Program
  public static method Main(): int
    Console.WriteLine(Util.Go<string>("a"));
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("q\n"));
}

// A second argument beside a lambda: `x: T` is checked against the callee's
// unsubstituted `T` while the lambda fixes it at `int` (§14.5).
BEZEL_TEST(callee_type_param_name_collision_4915,
           literal_beside_lambda_to_same_named_callee_param_accepted_spec_14_5) {
    auto r = runAot(R"BZ(
module Conf;
public class Util
  public static method Apply<T>(f: (T) -> T, x: T): T
    return f(x);
  end Apply;
  public static method Go<T>(v: T): int
    return Apply(lambda(y: int) => y + 1, 5);
  end Go;
end Util;
public class Program
  public static method Main(): int
    Console.WriteLine(Util.Go<int>(1));
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("6\n"));
}

// Element site: a homogeneous collection literal whose only target is the
// callee's `Array<T>` (§14.5, as #4895 admits it). The per-element check
// (checkCollectionLiteralElementAssignability) took the callee's element `T`
// for the caller's `B<T>` by name and rejected each element — the same
// defect one level down, because it did not receive the callee's free set.
BEZEL_TEST(callee_type_param_name_collision_4915,
           homogeneous_literal_to_same_named_callee_array_param_accepted_spec_14_5) {
    auto r = runAot(R"BZ(
module Conf;
public class Util
  public static method C<T>(xs: Array<T>): string
    return $"{xs[0]},{xs[1]}";
  end C;
  public static method B<T>(x: T): string
    return C([11, 22]);
  end B;
end Util;
public class Program
  public static method Main(): int
    Console.WriteLine(Util.B<int>(1));
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("11,22\n"));
}

// ═════════ Reject controls (the #4810 arm must still fire) ════════════════

// Return site: the enclosing method's OWN `T` is the target — no conversion
// from `int` exists (§5.1, §3.6 rule 2).
BEZEL_TEST(callee_type_param_name_collision_4915,
           int_literal_returned_as_own_type_param_still_rejected_spec_5_1) {
    BEZEL_EXPECT_TRUE(rejectedByRule(R"BZ(
module Conf;
public class Util
  public static method Go<T>(v: T): T
    return 5;
  end Go;
end Util;
public class Program
  public static method Main(): int
    Console.WriteLine(Util.Go<string>("a"));
    return 0;
  end Main;
end Program;
)BZ", "T"));
}

// Argument site whose formal is the ENCLOSING CLASS's `T` (the callee is a
// non-generic method of the same generic class): the callee has no free
// parameter named `T`, so the arm must still reject.
BEZEL_TEST(callee_type_param_name_collision_4915,
           int_literal_to_enclosing_class_type_param_formal_still_rejected_spec_5_1) {
    BEZEL_EXPECT_TRUE(rejectedByRule(R"BZ(
module Conf;
public class Box<T>
  public method Put(x: T): int
    return 1;
  end Put;
  public method Go(): int
    return this.Put(5);
  end Go;
end Box;
public class Program
  public static method Main(): int
    var b := new Box<string>();
    Console.WriteLine(b.Go());
    return 0;
  end Main;
end Program;
)BZ", "T"));
}

// Argument site of a generic method whose OWN parameter is `U` but whose
// formal is the ENCLOSING CLASS's `T`: the callee's free set is `{U}`, so the
// `T` target is still the caller-scope parameter and the arm must reject.
BEZEL_TEST(callee_type_param_name_collision_4915,
           int_literal_to_class_type_param_formal_of_generic_method_still_rejected_spec_5_1) {
    BEZEL_EXPECT_TRUE(rejectedByRule(R"BZ(
module Conf;
public class Box<T>
  public method Put<U>(x: T, u: U): int
    return 1;
  end Put;
  public method Go(): int
    return this.Put(5, true);
  end Go;
end Box;
public class Program
  public static method Main(): int
    var b := new Box<string>();
    Console.WriteLine(b.Go());
    return 0;
  end Main;
end Program;
)BZ", "T"));
}

// Element site whose formal is the ENCLOSING CLASS's `Array<T>` (a
// non-generic method of the generic class): the callee's free set is empty,
// so each element still targets the caller-scope `T` and must be rejected.
BEZEL_TEST(callee_type_param_name_collision_4915,
           literal_element_to_enclosing_class_array_formal_still_rejected_spec_5_1) {
    BEZEL_EXPECT_TRUE(rejectedByRule(R"BZ(
module Conf;
public class Box<T>
  public method PutAll(xs: Array<T>): int
    return xs.Count;
  end PutAll;
  public method Go(): int
    return this.PutAll([5, 6]);
  end Go;
end Box;
public class Program
  public static method Main(): int
    var b := new Box<string>();
    Console.WriteLine(b.Go());
    return 0;
  end Main;
end Program;
)BZ", "T"));
}

// ═════════ Rework (rv-L29 review): names the call has already BOUND ═══════
//
// The exemption above is for the callee's INFERENCE slots only. Two kinds of
// same-spelled `T` reach the argument check with the callee's name still in
// `calleeMethodGenericParamNames` yet no longer the callee's slot:
//   * an EXPLICIT type argument — `explicitSubst` rewrites the formal to the
//     caller's type argument before the check (#4325), so `Id<T>(5)` inside
//     `Go<T>` checks `5` against the CALLER's `T`;
//   * a method `T` that SHADOWS its class's `T` — `substituteMemberType`
//     resolves class parameters by name, so the formal is the receiver's
//     class argument (the class `T` in scope for `this.Id(5)`).
// Exempting either admitted an ill-typed call (MEASURED on the first cut:
// `Get<T>(77, 1)` at `Go<A>` → ASan stack-buffer-overflow in `bz_array_add`;
// `C<T>([11, 22])` at `B<string>` → SIGSEGV; `this.Id(5)` → IR-verifier
// abort). Each row below is rejected by the #4810 arm, as at 335e3849
// (§5.1: no implicit conversion from a primitive to a type parameter;
// §14.5: an explicit list binds the parameters, no inference runs).

// Explicit same-name argument; the formal is the caller's `T` = class `A`.
BEZEL_TEST(callee_type_param_name_collision_4915,
           explicit_same_name_type_arg_int_to_class_instantiation_rejected_spec_14_5_5_1) {
    BEZEL_EXPECT_TRUE(rejectedByRule(R"BZ(
module Conf;
public class A
  public v: int := 3;
end A;
public class Util
  public static method Get<T>(x: T, n: int): string
    var a: Array<T> := [x];
    return $"{a.Count}:{n}";
  end Get;
  public static method Go<T>(v: T): string
    return Get<T>(77, 1);
  end Go;
end Util;
public class Program
  public static method Main(): int
    Console.WriteLine(Util.Go<A>(new A()));
    return 0;
  end Main;
end Program;
)BZ", "T"));
}

// Explicit same-name argument, the plain headline shape.
BEZEL_TEST(callee_type_param_name_collision_4915,
           explicit_same_name_type_arg_int_literal_rejected_spec_14_5_5_1) {
    BEZEL_EXPECT_TRUE(rejectedByRule(R"BZ(
module Conf;
public class Util
  public static method Id<T>(x: T): string
    return $"{x}";
  end Id;
  public static method Go<T>(v: T): string
    return Id<T>(5);
  end Go;
end Util;
public class Program
  public static method Main(): int
    Console.WriteLine(Util.Go<string>("a"));
    return 0;
  end Main;
end Program;
)BZ", "T"));
}

// Swapped explicit arguments: the callee's `x: T` becomes the caller's `U`.
BEZEL_TEST(callee_type_param_name_collision_4915,
           explicit_swapped_type_args_int_to_caller_param_rejected_spec_14_5_5_1) {
    BEZEL_EXPECT_TRUE(rejectedByRule(R"BZ(
module Conf;
public class Util
  public static method P<T, U>(x: T, y: U): string
    return $"{x}/{y}";
  end P;
  public static method Go<T, U>(a: T, b: U): string
    return P<U, T>(5, a);
  end Go;
end Util;
public class Program
  public static method Main(): int
    Console.WriteLine(Util.Go<int, string>(1, "b"));
    return 0;
  end Main;
end Program;
)BZ", "U"));
}

// Element site with an explicit argument: `C<T>` binds the element type to
// the caller's `T`, so each `int` element is rejected.
BEZEL_TEST(callee_type_param_name_collision_4915,
           explicit_same_name_type_arg_array_literal_elements_rejected_spec_14_5_5_1) {
    BEZEL_EXPECT_TRUE(rejectedByRule(R"BZ(
module Conf;
public class Util
  public static method C<T>(xs: Array<T>): string
    return $"{xs[0]},{xs[1]}";
  end C;
  public static method B<T>(x: T): string
    return C<T>([11, 22]);
  end B;
end Util;
public class Program
  public static method Main(): int
    Console.WriteLine(Util.B<string>("a"));
    return 0;
  end Main;
end Program;
)BZ", "T"));
}

// Explicit class AND method arguments on a static call of another generic
// class whose method shadows its class parameter.
BEZEL_TEST(callee_type_param_name_collision_4915,
           explicit_same_name_class_and_method_type_args_rejected_spec_14_5_5_1) {
    BEZEL_EXPECT_TRUE(rejectedByRule(R"BZ(
module Conf;
public class Box<T>
  public static method Wrap<T>(x: T): string
    return $"[{x}]";
  end Wrap;
end Box;
public class Util
  public static method Go<T>(v: T): string
    return Box<T>.Wrap<T>(5);
  end Go;
end Util;
public class Program
  public static method Main(): int
    Console.WriteLine(Util.Go<string>("a"));
    return 0;
  end Main;
end Program;
)BZ", "T"));
}

// Shadowing, inferred, instance receiver: the method's `T` has already been
// rewritten to the class's `T` (the receiver `this`), so it is not free.
// Kept rejected (a spurious reject for a shadowing method — see the filed
// follow-up — but never the unsound accept the first cut produced).
BEZEL_TEST(callee_type_param_name_collision_4915,
           shadowing_method_type_param_inferred_this_call_rejected_spec_14_5_5_1) {
    BEZEL_EXPECT_TRUE(rejectedByRule(R"BZ(
module Conf;
public class Box<T>
  public method Id<T>(x: T): T
    return x;
  end Id;
  public method Go(): int
    return this.Id(5) + 1;
  end Go;
end Box;
public class Program
  public static method Main(): int
    var b := new Box<string>();
    Console.WriteLine(b.Go());
    return 0;
  end Main;
end Program;
)BZ", "T"));
}

// Shadowing, inferred, static receiver `Box<T>` inside the class.
BEZEL_TEST(callee_type_param_name_collision_4915,
           shadowing_method_type_param_inferred_static_call_rejected_spec_14_5_5_1) {
    BEZEL_EXPECT_TRUE(rejectedByRule(R"BZ(
module Conf;
public class Box<T>
  public static method Id<T>(x: T): T
    return x;
  end Id;
  public method Go(): int
    return Box<T>.Id(5) + 1;
  end Go;
end Box;
public class Program
  public static method Main(): int
    var b := new Box<string>();
    Console.WriteLine(b.Go());
    return 0;
  end Main;
end Program;
)BZ", "T"));
}

// Accept control for the rework: an explicit same-name argument whose SOURCE
// is the caller's own `T` value is identity (§5.1) and must still compile
// and run — the narrowing removes the exemption, not the conversion.
BEZEL_TEST(callee_type_param_name_collision_4915,
           explicit_same_name_type_arg_with_caller_T_value_accepted_spec_14_5) {
    auto r = runAot(R"BZ(
module Conf;
public class Util
  public static method Id<T>(x: T): T
    return x;
  end Id;
  public static method Go<T>(v: T): string
    var w: T := Id<T>(v);
    return $"{w}";
  end Go;
end Util;
public class Program
  public static method Main(): int
    Console.WriteLine(Util.Go<string>("a"));
    Console.WriteLine(Util.Go<long>(9L));
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("a\n9\n"));
}

// Shadowing, inferred, NO receiver (`Pick(5)` inside `Box<T>`): the formal is
// the method's `T`, but codegen cannot yet bind a shadowing method `T` beside
// the class `T` (#4862). Accepting this shape MEASURED an IR-verifier abort
// for a `T`-typed local in the body and a silent wrong value for `Pick(5)` at
// `Box<long>`, so it stays rejected, as at 335e3849 (#4847).
BEZEL_TEST(callee_type_param_name_collision_4915,
           shadowing_method_type_param_inferred_unqualified_call_rejected_spec_14_5_5_1) {
    BEZEL_EXPECT_TRUE(rejectedByRule(R"BZ(
module Conf;
public class Box<T>
  private _v: T;
  public constructor(v: T)
    _v := v;
  end constructor;
  public method Pick<T>(x: T): T
    var t: T := x;
    return t;
  end Pick;
  public method Sum(): string
    var a: int := Pick(5);
    return $"{a} {_v}";
  end Sum;
end Box;
public class Program
  public static method Main()
    Console.WriteLine(new Box<string>("z").Sum());
  end Main;
end Program;
)BZ", "T"));
}
