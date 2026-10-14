// Spec-derived regression tests for issue #6086 — the type of a `case`
// expression that has a bare `null` arm beside class, interface or built-in
// exception arms.
//
// THE RULE. Spec §19.1 "Expression-position type" (v0.338): "the construct's
// type is the left-fold of its arm-body types under the §20 common-type
// rule". §20 (v0.338): "A bare `null` branch … target-types to the nullable
// form of the other branches' common type". So `(case k of 0 => b, _ => null)`
// with `b: B` is a `B?`, exactly as `(if k = 0 then b else null)` already was.
//
// WHAT WAS BROKEN. `ExprTC::typeCase` folded the bare `null` arm's `Object?`
// placeholder through the common-type function, and `B` joined with `Object?`
// is `Object?`. MEASURED on 6d8c82eb with the real `bezelc`:
//   * `var r := (case k of 0 => b, _ => null); if r <> null then … r.y …`
//     printed `0` for a `B` whose `y` is 9 (with the internal "codegen cannot
//     read member 'y' on receiver of type ''" warning);
//   * `var c: C? := (case k of 0 => b, _ => null);` — a `B` into a sibling
//     `C?` — compiled.
// Rows marked FAILS-WITHOUT below are those two defects and their variants
// (measured: they fail on 6d8c82eb). The remaining accept rows compiled and
// ran correctly before the fix as well, but only because an `Object?` source
// was accepted at any reference target (#6023); they pin every spelling that
// must keep working once that unchecked conversion is removed, and are the
// reason this fix lands first.
//
// Every row is on `runAot` — the real driver — and an accept row compares
// stdout, taking each arm at least once.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

constexpr const char* kPrelude = R"bzl(module M;
public interface IFoo
  method F(): int;
end IFoo;
public class A: IFoo
  public constructor() end constructor;
  public x: int := 1;
  public method F(): int => x;
end A;
public class B: A
  public constructor() end constructor;
  public y: int := 9;
  public method Y(): int => y;
end B;
public class C: A
  public constructor() end constructor;
  public z: int := 7;
end C;
public enum E
  Red, Green;
end E;
)bzl";

}  // namespace

// THE ISSUE'S REPRO. The inferred local is `B?`, so after a null test `r.y` and
// `r.Y()` read `B`'s own member. Before the fix `r` was recorded `Object?`, the
// read went down codegen's permissive path, and the row printed `0 0` for the
// first call. Both arms are taken (`T(0)`, `T(1)`).
BEZEL_TEST(sema_case_null_arm_join_6086,
           inferred_local_from_class_and_null_arms_reads_member_spec_20) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method T(k: int): string
    var b := new B();
    var r := (case k of 0 => b, _ => null);
    if r <> null then return $"{r.y} {r.Y()}"; end if;
    return "null";
  end T;
  public static method Main(): int
    Console.WriteLine($"{T(0)}|{T(1)}");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("9 9|null\n"));
}

// The fold is order-independent: the bare `null` arm comes FIRST.
BEZEL_TEST(sema_case_null_arm_join_6086,
           null_arm_first_then_class_arm_reads_member_spec_20) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method T(k: int): string
    var b := new B();
    var r := (case k of 0 => null, _ => b);
    if r <> null then return $"{r.y}"; end if;
    return "null";
  end T;
  public static method Main(): int
    Console.WriteLine($"{T(0)}|{T(1)}");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("null|9\n"));
}

// REJECT. The join is `B?`, and `B?` does not convert to `C?` (§5.1: "`S?`
// implicitly converts to `T?` whenever `S` implicitly converts to `T`"; `B` and
// `C` are siblings). Before the fix the join was `Object?` and the initializer
// was accepted — a `B` stored in a `C?`.
BEZEL_TEST(sema_case_null_arm_join_6086,
           class_and_null_arms_rejected_at_sibling_nullable_target_spec_5_1) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method T(k: int): int
    var b := new B();
    var c: C? := (case k of 0 => b, _ => null);
    return (if c = null then -1 else c.z);
  end T;
  public static method Main(): int
    Console.WriteLine($"{T(0)} {T(1)}");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "not implicitly convertible to the target type (§5.1)"));
}

// An inferred local re-bound at its annotated type, `B?` → `B?` (identity).
BEZEL_TEST(sema_case_null_arm_join_6086,
           inferred_local_then_annotated_nullable_local_both_arms_spec_20) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method T(k: int): string
    var b := new B();
    var r := (case k of 0 => b, _ => null);
    var x: B? := r;
    if x <> null then return $"y={x.y}"; end if;
    return "null";
  end T;
  public static method Main(): int
    Console.WriteLine($"{T(0)}|{T(1)}");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("y=9|null\n"));
}

// `(case …)!` — §7.5 force-unwrap of the `B?` join yields `B`.
BEZEL_TEST(sema_case_null_arm_join_6086,
           force_unwrap_of_class_and_null_arms_spec_20) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method T(k: int): int
    var b := new B();
    var x: B := (case k of 0 => b, _ => null)!;
    return x.y;
  end T;
  public static method Main(): int
    Console.WriteLine($"{T(0)}");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("9\n"));
}

// `(case …) ?? b2` — `B? ?? B` is `B`; both sides are taken.
BEZEL_TEST(sema_case_null_arm_join_6086,
           null_coalesce_of_class_and_null_arms_spec_20) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method T(k: int): int
    var b := new B();
    var b2 := new B();
    b2.y := 5;
    var x: B := (case k of 0 => b, _ => null) ?? b2;
    return x.y;
  end T;
  public static method Main(): int
    Console.WriteLine($"{T(0)} {T(1)}");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("9 5\n"));
}

// A binding atom `(var t := 1; (case …))` carries the join's type out, and a
// binding atom whose value is the bare `null` still target-types.
BEZEL_TEST(sema_case_null_arm_join_6086,
           binding_atom_wrapping_class_and_null_arms_spec_20) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method T(k: int): string
    var b := new B();
    var x: B? := (var t := 1; (case k of 0 => b, _ => null));
    var n: B? := (var t := 1; null);
    if x <> null then return $"y={x.y} {n = null}"; end if;
    return $"null {n = null}";
  end T;
  public static method Main(): int
    Console.WriteLine($"{T(0)}|{T(1)}");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("y=9 True|null True\n"));
}

// The `case` is itself a branch of an `if` expression: `B?` joined with `B`.
BEZEL_TEST(sema_case_null_arm_join_6086,
           case_nested_in_if_branch_joins_to_nullable_class_spec_20) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method T(k: int): string
    var b := new B();
    var r := (if k < 2 then (case k of 0 => b, _ => null) else b);
    var x: B? := r;
    if x <> null then return $"y={x.y}"; end if;
    return "null";
  end T;
  public static method Main(): int
    Console.WriteLine($"{T(0)}|{T(1)}|{T(2)}");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("y=9|null|y=9\n"));
}

// Three arms: `B?`, `B`, and the bare `null`.
BEZEL_TEST(sema_case_null_arm_join_6086,
           three_arms_with_an_already_nullable_class_arm_spec_20) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method T(k: int): string
    var b := new B();
    var bn: B? := b;
    var r := (case k of 0 => bn, 1 => b, _ => null);
    var x: B? := r;
    if x <> null then return $"y={x.y}"; end if;
    return "null";
  end T;
  public static method Main(): int
    Console.WriteLine($"{T(0)}|{T(1)}|{T(2)}");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("y=9|y=9|null\n"));
}

// `B`, `A` and `null` arms: the common type of `B` and `A` is `A` (§20), so the
// join is `A?` and `A`'s member is read on the inferred local.
BEZEL_TEST(sema_case_null_arm_join_6086,
           derived_and_base_arms_with_null_join_to_nullable_base_spec_20) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method T(k: int): string
    var b := new B();
    var a := new A();
    a.x := 4;
    var r := (case k of 0 => b, 1 => a, _ => null);
    if r <> null then return $"x={r.x}"; end if;
    return "null";
  end T;
  public static method Main(): int
    Console.WriteLine($"{T(0)}|{T(1)}|{T(2)}");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("x=1|x=4|null\n"));
}

// Interface arms: the join is `IFoo?` and dispatches through the interface.
BEZEL_TEST(sema_case_null_arm_join_6086,
           interface_and_null_arms_join_to_nullable_interface_spec_20) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method T(k: int): string
    var b := new B();
    var f: IFoo := b;
    var r := (case k of 0 => f, _ => null);
    var x: IFoo? := r;
    if r <> null then return $"F={r.F()} {x <> null}"; end if;
    return "null";
  end T;
  public static method Main(): int
    Console.WriteLine($"{T(0)}|{T(1)}");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("F=1 True|null\n"));
}

// Built-in exception arms (a class with no user declaration, §31.4): the join
// is `IOException?`.
BEZEL_TEST(sema_case_null_arm_join_6086,
           builtin_exception_and_null_arms_join_to_nullable_exception_spec_20) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method T(k: int): string
    using var ex := new IOException("m");
    var r := (case k of 0 => ex, _ => null);
    var x: IOException? := r;
    if r <> null then return $"{r.Message} {x <> null}"; end if;
    return "null";
  end T;
  public static method Main(): int
    Console.WriteLine($"{T(0)}|{T(1)}");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("m True|null\n"));
}

// CONTROL — value-type arms are outside this fix (#573 keeps their fold): the
// annotated `E?` target is still reached and both arms run.
BEZEL_TEST(sema_case_null_arm_join_6086,
           enum_and_null_arms_still_reach_nullable_enum_target_spec_20) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method T(k: int): int
    var e: E? := (case k of 0 => E.Green, _ => null);
    return (if e = null then -1 else 5);
  end T;
  public static method Main(): int
    Console.WriteLine($"{T(0)} {T(1)}");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5 -1\n"));
}

// pending #6112. Two arms that are DIFFERENT instantiations of one generic
// class (`Bx<B>`, `Bx<A>`) plus a `null` arm: the result keeps the pre-#6086
// `Object?` fold, because typing it `Bx<B>?` made `r.V.y` read `B.y` out of the
// `A` inside a `Bx<A>` — a heap-buffer-overflow under the sanitizer (MEASURED on
// the unamended fix). The pair's common type is #6112's question (the same
// overflow exists with no `null` arm); this row pins only that the null-arm form
// is memory-clean, looped, on all three arms — not what `r.V.y` renders.
BEZEL_TEST(sema_case_null_arm_join_6086,
           mixed_generic_instantiation_arms_keep_the_object_fold_pending_6112) {
    auto r = runAotChecked(std::string(kPrelude) + R"bzl(public class Bx<T>
  public constructor(v: T) V := v; end constructor;
  public V: T;
end Bx;
public class P
  public static method T(k: int): int
    var a := new A(); var b := new B();
    var bx := new Bx<B>(b); var ba := new Bx<A>(a);
    var r := (case k of 0 => bx, 1 => ba, _ => null);
    if r <> null then Console.Write($"{r.V.y};"); return 1; end if;
    return 0;
  end T;
  public static method Main(): int
    var n := 0;
    for var i in 0..60 do n := n + T(0) + T(1) + T(2); end for;
    Console.WriteLine($"n={n}");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("n=122\n") != std::string::npos);
}

// pending #6112. `Bx<int>` and `Bx<int?>` are different instantiations
// too (§7.1: `int?` carries a presence flag, so `V` has another layout), and
// the comparison that finds the mixed pair must see the nullability of a type
// ARGUMENT. With the `Bx<int?>` arm first the result was typed `Bx<int?>?` and
// `r.V` read past the end of a `Bx<int>` — heap-buffer-overflow, MEASURED on
// the fix before this row. As for the row above, only memory-cleanliness is
// pinned, not what `r.V` renders.
BEZEL_TEST(sema_case_null_arm_join_6086,
           instantiations_differing_in_argument_nullability_int_keep_the_object_fold_pending_6112) {
    auto r = runAotChecked(std::string(kPrelude) + R"bzl(public class Bx<T>
  public constructor(v: T) V := v; end constructor;
  public V: T;
end Bx;
public class P
  public static method T(k: int): int
    var b := new B();
    var bi := new Bx<int>(3); var biq := new Bx<int?>(4);
    var r := (case k of 0 => biq, 1 => bi, _ => null);
    if r <> null then Console.Write($"{r.V};"); return 1; end if;
    return 0;
  end T;
  public static method Main(): int
    var n := 0;
    for var i in 0..60 do n := n + T(0) + T(1) + T(2); end for;
    Console.WriteLine($"n={n}");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("n=122\n") != std::string::npos);
}

// pending #6112. `Bx<B>` and `Bx<B?>`: a class argument differing only
// in nullability. Typed `Bx<B>?`, `r.V.y` dereferenced the `null` a
// `Bx<B?>` holds — SEGV, MEASURED on the fix before this row.
BEZEL_TEST(sema_case_null_arm_join_6086,
           instantiations_differing_in_argument_nullability_class_pending_6112) {
    auto r = runAotChecked(std::string(kPrelude) + R"bzl(public class Bx<T>
  public constructor(v: T) V := v; end constructor;
  public V: T;
end Bx;
public class P
  public static method T(k: int): int
    var b := new B();
    var bb := new Bx<B>(b); var bbq := new Bx<B?>(null);
    var r := (case k of 0 => bb, 1 => bbq, _ => null);
    if r <> null then Console.Write($"{r.V.y};"); return 1; end if;
    return 0;
  end T;
  public static method Main(): int
    var n := 0;
    for var i in 0..60 do n := n + T(0) + T(1) + T(2); end for;
    Console.WriteLine($"n={n}");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("n=122\n") != std::string::npos);
}

// pending #6112. `Bx<string>` and `Bx<string?>` holding `null`: the
// same SEGV through `r.V.Length`, MEASURED on the fix before this row.
BEZEL_TEST(sema_case_null_arm_join_6086,
           instantiations_differing_in_argument_nullability_string_pending_6112) {
    auto r = runAotChecked(std::string(kPrelude) + R"bzl(public class Bx<T>
  public constructor(v: T) V := v; end constructor;
  public V: T;
end Bx;
public class P
  public static method T(k: int): int
    var b := new B();
    var b1 := new Bx<string>("zz"); var b2 := new Bx<string?>(null);
    var r := (case k of 0 => b1, 1 => b2, _ => null);
    if r <> null then Console.Write($"{r.V.Length};"); return 1; end if;
    return 0;
  end T;
  public static method Main(): int
    var n := 0;
    for var i in 0..60 do n := n + T(0) + T(1) + T(2); end for;
    Console.WriteLine($"n={n}");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("n=122\n") != std::string::npos);
}

// pending #6112. The mixed pair inside a nested `case` whose own result
// meets an outer `null` arm.
BEZEL_TEST(sema_case_null_arm_join_6086,
           instantiations_differing_in_argument_nullability_nested_case_pending_6112) {
    auto r = runAotChecked(std::string(kPrelude) + R"bzl(public class Bx<T>
  public constructor(v: T) V := v; end constructor;
  public V: T;
end Bx;
public class P
  public static method T(k: int): int
    var b := new B();
    var bi := new Bx<int>(3); var biq := new Bx<int?>(4);
    var r := (case k of 2 => null, _ => (case k of 0 => biq, 1 => bi, _ => null));
    if r <> null then Console.Write($"{r.V};"); return 1; end if;
    return 0;
  end T;
  public static method Main(): int
    var n := 0;
    for var i in 0..60 do n := n + T(0) + T(1) + T(2); end for;
    Console.WriteLine($"n={n}");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("n=122\n") != std::string::npos);
}
